#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <libdragon.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static inline uint32_t f32_to_bits(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}
#define F32I(x) f32_to_bits(x)

/* -------------------------------------------------------------------------
 * Probe result type
 * ---------------------------------------------------------------------- */

typedef enum {
    RESULT_PASS,
    RESULT_FAIL,
    RESULT_STUB,
} probe_status_t;

typedef struct {
    probe_status_t status;
    uint64_t detail;   /* optional; 0 if unused */
} probe_result_t;

static probe_result_t PASS(void)       { return (probe_result_t){ RESULT_PASS, 0 }; }
static probe_result_t FAIL(uint64_t d) { return (probe_result_t){ RESULT_FAIL, d }; }
static probe_result_t STUB(void)       { return (probe_result_t){ RESULT_STUB, 0 }; }

static const char *status_str(probe_status_t s) {
    switch (s) {
        case RESULT_PASS: return "PASS";
        case RESULT_FAIL: return "FAIL";
        case RESULT_STUB: return "STUB";
    }
    return "????";
}

/* -------------------------------------------------------------------------
 * Region / console type
 *
 * libdragon caches boot information from RSP DMEM during startup:
 *
 *   0xA4000009 -> __boot_tvtype      -> get_tv_type()
 *   0xA400000A -> __boot_resettype
 *   0xA400000B -> __boot_consoletype -> sys_bbplayer()
 *
 * These bytes originate from the IPL boot process. The corresponding
 * PIF-RAM boot word at 0xBFC007E4 appears to be cleared before main()
 * executes, so the preserved DMEM copies are used instead.
 * ---------------------------------------------------------------------- */

static uint8_t read_dmem_tvtype(void) {
    return *((volatile uint8_t *)0xA4000009);
}

static uint8_t read_dmem_consoletype(void) {
    return *((volatile uint8_t *)0xA400000B);
}

static const char *tv_type_str(int t) {
    switch (t) {
        case TV_PAL:  return "PAL";
        case TV_NTSC: return "NTSC";
        case TV_MPAL: return "MPAL";
    }
    return "unknown";
}

/* -------------------------------------------------------------------------
 * Identifier reads
 * ---------------------------------------------------------------------- */

static uint32_t read_prid(void) {
    uint32_t v;
    __asm__ volatile ("mfc0 %0, $15" : "=r"(v));
    return v;
}

static uint32_t read_fcr0(void) {
    uint32_t v;
    __asm__ volatile ("cfc1 %0, $0" : "=r"(v));
    return v;
}

/* -------------------------------------------------------------------------
 * MI_VERSION
 *
 * 0xA4300004. Bits [7:0] = IO version.
 * 0x02 = retail RCP, 0x03 = Analogue 3D.
 * ---------------------------------------------------------------------- */

static uint32_t read_mi_version(void) {
    return *((volatile uint32_t *)0xA4300004);
}

/* -------------------------------------------------------------------------
 * RDRAM manufacturer
 *
 * RDRAM_REG_DEVICE_MANUFACTURER (reg 9) at chip 0.
 * Base: 0xA3F00000, stride shift 8 (RI v2), reg 9 is odd so MI upper
 * mode is required to access it (shifts 32-bit transfer to upper bus half).
 *
 * Address: 0xA3F00000 + (0 << 8 + 9) * 4 = 0xA3F00024
 *
 * Registers are physically little-endian; byteswap after reading.
 * bits [31:16] = manufacturer code, bits [15:0] = product code.
 *
 * Known manufacturer codes:
 *   0x0002 Toshiba   0x0003 Fujitsu   0x0005 NEC
 *   0x0007 Hitachi   0x0009 OKI       0x000A LG
 *   0x0010 Samsung   0x0013 Hyundai
 * ---------------------------------------------------------------------- */

#define MI_MODE_REG         ((volatile uint32_t *)0xA4300000)
#define MI_WMODE_SET_UPPER  0x00002000
#define MI_WMODE_CLR_UPPER  0x00001000
#define RDRAM_REGS          ((volatile uint32_t *)0xA3F00000)

static inline uint32_t byteswap32(uint32_t v) {
    return ((v & 0xFF000000) >> 24) |
           ((v & 0x00FF0000) >>  8) |
           ((v & 0x0000FF00) <<  8) |
           ((v & 0x000000FF) << 24);
}

typedef struct {
    uint16_t manu;
    uint16_t code;
} rdram_manufacturer_t;

static const char *rdram_manu_str(uint16_t manu) {
    switch (manu) {
        case 0x0002: return "Toshiba";
        case 0x0003: return "Fujitsu";
        case 0x0005: return "NEC";
        case 0x0007: return "Hitachi";
        case 0x0009: return "OKI";
        case 0x000A: return "LG";
        case 0x0010: return "Samsung";
        case 0x0013: return "Hyundai";
        default:     return "unknown";
    }
}

static rdram_manufacturer_t read_rdram_manufacturer(int chip_id) {
    /* reg 9 is odd: set MI upper mode before read, clear after */
    *MI_MODE_REG = MI_WMODE_SET_UPPER;
    uint32_t raw = RDRAM_REGS[(chip_id << 8) + 9];
    *MI_MODE_REG = MI_WMODE_CLR_UPPER;
    uint32_t value = byteswap32(raw);
    return (rdram_manufacturer_t){
        .manu = (value >> 16) & 0xFFFF,
        .code = (value >>  0) & 0xFFFF,
    };
}



/* -------------------------------------------------------------------------
 * RDRAM register dump
 * ---------------------------------------------------------------------- */

static uint32_t rdram_read_reg(int chip_id, int reg)
{
    if (reg & 1) *MI_MODE_REG = MI_WMODE_SET_UPPER;
    uint32_t raw = RDRAM_REGS[(chip_id << 8) + reg];
    if (reg & 1) *MI_MODE_REG = MI_WMODE_CLR_UPPER;
    return byteswap32(raw);
}

static void dump_rdram_regs(int chip_id)
{
    uint32_t dt = rdram_read_reg(chip_id, 0);

    debugf("RDRAM chip_id=%d\n", chip_id);
    debugf("  r00 DeviceType         0x%08lX"
           "  ColBits=%u BankBits=%u RowBits=%u Bn=%u En=%u Ver=%u Type=%u\n",
        (unsigned long)dt,
        (unsigned int)((dt >> 28) & 0xF),
        (unsigned int)((dt >> 20) & 0xF),
        (unsigned int)((dt >> 16) & 0xF),
        (unsigned int)((dt >> 26) & 0x1),
        (unsigned int)((dt >> 24) & 0x1),
        (unsigned int)((dt >>  4) & 0xF),
        (unsigned int)((dt >>  0) & 0xF));
    debugf("  r01 DeviceId           0x%08lX\n", (unsigned long)rdram_read_reg(chip_id, 1));
    debugf("  r02 Delay              0x%08lX\n", (unsigned long)rdram_read_reg(chip_id, 2));
    debugf("  r03 Mode               0x%08lX\n", (unsigned long)rdram_read_reg(chip_id, 3));
    debugf("  r04 RefInterval        0x%08lX\n", (unsigned long)rdram_read_reg(chip_id, 4));
    debugf("  r05 RefRow             0x%08lX\n", (unsigned long)rdram_read_reg(chip_id, 5));
    debugf("  r06 RasInterval        0x%08lX\n", (unsigned long)rdram_read_reg(chip_id, 6));
    debugf("  r07 MinInterval        0x%08lX\n", (unsigned long)rdram_read_reg(chip_id, 7));
    debugf("  r08 AddressSelect      0x%08lX\n", (unsigned long)rdram_read_reg(chip_id, 8));
    debugf("  r09 DeviceManufacturer 0x%08lX\n", (unsigned long)rdram_read_reg(chip_id, 9));
    debugf("\n");
}

/* -------------------------------------------------------------------------
 * BUG: mulmul — FP double-multiply hazard
 *
 * Back-to-back mul.s may produce incorrect results for the second multiply
 * when the first multiply's operands include sNaN, Zero, or Infinity.
 * Affects VR4300 versions 1.x, 2.0, 2.1.
 * A single intervening instruction (e.g. NOP) clears the hazard.
 *
 * Probe: compare results of the hazard sequence vs. the NOP-separated
 * sequence on fixed inputs (0 * inf, 2 * 3). PASS = results match.
 * ---------------------------------------------------------------------- */

static probe_result_t probe_mulmul(void) {
    /*
     * Single asm block containing both sequences so the compiler cannot
     * insert anything between the two mul.s instructions.
     * Operands loaded explicitly via mtc1 from integer bit patterns.
     *
     * Broken:  mul.s (0*inf), mul.s (2*3) back-to-back
     * Working: mul.s (0*inf), nop, mul.s (2*3)
     *
     * PASS = results match (bug absent).
     * FAIL = results differ (bug fires). Detail = broken:working.
     * 
     * Original ctest.z64 mulmul test by HailtoDodongo
     * Run on hardware known to have the bug by Buu42
     * Known-bad input pattern provided from log by Buu42:
     * (7F800000 * 37BAD25F, 38978B5D * 0C50A394): 05770421 != 05770422
     * 
     * test fixed by Jhynjhiruu
     */
    uint32_t bits_1 = 0x7F800000;
    uint32_t bits_2 = 0x37BAD25F;
    uint32_t bits_3 = 0x38978B5D;
    uint32_t bits_4 = 0x0C50A394;
    uint32_t broken, working;

    uint32_t fcr31_saved = C1_FCR31();
    C1_WRITE_FCR31(fcr31_saved & ~(C1_ENABLE_OVERFLOW | C1_ENABLE_DIV_BY_0 | C1_ENABLE_INVALID_OP));

    __asm__ volatile (
        "mtc1   %2, $f12\n"
        "mtc1   %3, $f13\n"
        "mtc1   %4, $f14\n"
        "mtc1   %5, $f15\n"
        /* broken: back-to-back mul.s */
        "mul.s  $f0, $f12, $f13\n"
        "mul.s  $f1, $f14, $f15\n"
        "mfc1   %0, $f1\n"
        /* working: nop between mul.s */
        "mul.s  $f0, $f12, $f13\n"
        "nop\n"
        "mul.s  $f1, $f14, $f15\n"
        "mfc1   %1, $f1\n"
        : "=r"(broken), "=r"(working)
        : "r"(bits_1), "r"(bits_2), "r"(bits_3), "r"(bits_4)
        : "$f0", "$f1", "$f12", "$f13", "$f14", "$f15"
    );

    C1_WRITE_FCR31(fcr31_saved);

    if (broken != working)
        return FAIL((uint64_t)broken << 32 | (uint64_t)working);
    return PASS();
}

/* -------------------------------------------------------------------------
 * BUG: sra — 32-bit arithmetic right shift leaks 64-bit state
 * ---------------------------------------------------------------------- */

static probe_result_t probe_sra(void) {
    /*
     * Load 0x0123456789ABCDEF into a register, execute sra rd, rt, 16.
     *
     * VR4300 manual says result should be 0xFFFFFFFFFFFF89AB
     * (sign-extended from bit 31 of the lower word, which is 1).
     *
     * Hardware produces 0x00000000456789AB
     * (upper 32 bits filled from upper word of input, new bit 31 = 0).
     *
     * PASS = manual behavior (bug absent).
     * FAIL = hardware behavior observed (bug fires).
     */
    uint64_t result;

    __asm__ volatile (
        "lui    $t0, 0x0123\n"
        "dsll   $t0, $t0, 16\n"
        "ori    $t0, $t0, 0x4567\n"
        "dsll   $t0, $t0, 16\n"
        "ori    $t0, $t0, 0x89AB\n"
        "dsll   $t0, $t0, 16\n"
        "ori    $t0, $t0, 0xCDEF\n"
        "sra    $t1, $t0, 16\n"
        "sd     $t1, %0\n"
        : "=m"(result)
        :
        : "$t0", "$t1"
    );

    const uint64_t man_expected = 0xFFFFFFFFFFFF89ABULL;
    const uint64_t hw_observed  = 0x00000000456789ABULL;

    if (result == man_expected) return PASS();
    if (result == hw_observed)  return FAIL(result);
    return FAIL(result);
}

/* -------------------------------------------------------------------------
 * BUG: mult — 32-bit signed multiply sign-extension anomaly
 * ---------------------------------------------------------------------- */

static probe_result_t probe_mult(void) {
    /*
     * The bug: mult is supposed to sign-extend both 32-bit operands to 64
     * bits before multiplying (sign from bit 31). In practice the second
     * operand is sign-extended from bit 34, making it a 64x35-bit multiply.
     *
     * To expose this, we construct a second operand where bit 34 differs
     * from bit 31, so the two interpretations produce different results.
     *
     * Input:
     *   rs = 2 (clean)
     *   rt_clean = 1  (bit31=0, bit34=0 -> +1, result HI=0 LO=2)
     *   rt_dirty = 0x0000000700000001  (bit31=0, bit34=1)
     *
     * PASS = clean and dirty results match (bug not triggered or not present).
     * FAIL = results differ (bug fires). Detail = dirty HI:LO.
     */
    uint32_t hi_clean, lo_clean;
    uint32_t hi_dirty, lo_dirty;

    __asm__ volatile (
        "li     $t0, 2\n"
        "li     $t1, 1\n"
        "mult   $t0, $t1\n"
        "mfhi   %0\n"
        "mflo   %1\n"
        : "=r"(hi_clean), "=r"(lo_clean)
        :
        : "$t0", "$t1", "hi", "lo"
    );

    __asm__ volatile (
        "li     $t0, 2\n"
        "lui    $t1, 0x0000\n"
        "dsll   $t1, $t1, 16\n"
        "ori    $t1, $t1, 0x0007\n"
        "dsll   $t1, $t1, 16\n"
        "ori    $t1, $t1, 0x0000\n"
        "dsll   $t1, $t1, 16\n"
        "ori    $t1, $t1, 0x0001\n"
        "mult   $t0, $t1\n"
        "mfhi   %0\n"
        "mflo   %1\n"
        : "=r"(hi_dirty), "=r"(lo_dirty)
        :
        : "$t0", "$t1", "hi", "lo"
    );

    if (hi_clean != hi_dirty || lo_clean != lo_dirty)
        return FAIL((uint64_t)hi_dirty << 32 | (uint64_t)lo_dirty);
    return PASS();
}

/* -------------------------------------------------------------------------
 * BUG: div — 32-bit signed divide sign-extension anomaly
 * ---------------------------------------------------------------------- */

static probe_result_t probe_div(void) {
    /*
     * The bug: div sign-extends the divisor from bit 34 rather than bit 31.
     * Additionally, when bits 63 and 31 of the divisor are not equal, the
     * quotient in LO is wrong in a way that is not fully understood.
     * The remainder in HI is at least consistent with:
     *   remainder = (int32_t)(dividend - quotient * divisor)
     *
     * We target the anomalous case: bit 63 = 0, bit 31 = 1 in the divisor.
     *
     * Inputs:
     *   dividend = 10 (clean)
     *   divisor_clean = 2  -> expect LO=5, HI=0
     *   divisor_dirty = 0x0000000080000002 (bit31=1, bit63=0)
     *
     * PASS = clean and dirty results match (bug not triggered or not present).
     * FAIL = results differ (bug fires). Detail = dirty HI:LO.
     */
    uint32_t hi_clean, lo_clean;
    uint32_t hi_dirty, lo_dirty;

    __asm__ volatile (
        "li     $t0, 10\n"
        "li     $t1, 2\n"
        "div    $t0, $t1\n"
        "mfhi   %0\n"
        "mflo   %1\n"
        : "=r"(hi_clean), "=r"(lo_clean)
        :
        : "$t0", "$t1", "hi", "lo"
    );

    __asm__ volatile (
        "li     $t0, 10\n"
        "lui    $t1, 0x0000\n"
        "dsll   $t1, $t1, 16\n"
        "ori    $t1, $t1, 0x0000\n"
        "dsll   $t1, $t1, 16\n"
        "ori    $t1, $t1, 0x8000\n"
        "dsll   $t1, $t1, 16\n"
        "ori    $t1, $t1, 0x0002\n"
        "div    $t0, $t1\n"
        "mfhi   %0\n"
        "mflo   %1\n"
        : "=r"(hi_dirty), "=r"(lo_dirty)
        :
        : "$t0", "$t1", "hi", "lo"
    );

    if (hi_clean != hi_dirty || lo_clean != lo_dirty)
        return FAIL((uint64_t)hi_dirty << 32 | (uint64_t)lo_dirty);
    return PASS();
}

/* -------------------------------------------------------------------------
 * Probe table
 * ---------------------------------------------------------------------- */

typedef probe_result_t (*probe_fn_t)(void);

typedef struct {
    const char *tag;
    probe_fn_t  fn;
} probe_entry_t;

static const probe_entry_t probes[] = {
    { "mulmul", probe_mulmul },
    { "sra",    probe_sra    },
    { "mult",   probe_mult   },
    { "div",    probe_div    },
};

#define NUM_PROBES (sizeof(probes) / sizeof(probes[0]))

/* -------------------------------------------------------------------------
 * Reporting
 * ---------------------------------------------------------------------- */

static void report(uint8_t dmem_tvtype, int tv_type,
                   uint8_t dmem_consoletype, bool is_ique,
                   uint32_t prid, uint32_t fcr0,
                   uint32_t mi_version, bool has_expak,
                   bool base_single_chip, bool expak_single_chip,
                   rdram_manufacturer_t rdram0, rdram_manufacturer_t rdram1,
                   rdram_manufacturer_t rdram2, rdram_manufacturer_t rdram3)

{
    probe_result_t results[NUM_PROBES];
    for (size_t i = 0; i < NUM_PROBES; i++)
        results[i] = probes[i].fn();

    printf("====================== n64-revision-test ======================\n");
    printf("tvtype  0x%02X %-4s    iQue?  0x%02X %s\n",
        (unsigned)dmem_tvtype,     tv_type_str(tv_type),
        (unsigned)dmem_consoletype, is_ique ? "yes" : "no");
    printf("\n");

    printf("CP0 PRId    (reg 15)        0x%08lX\n", (unsigned long)prid);
    printf("  [15:8] ID                 0x%02X\n", (unsigned)(prid >> 8) & 0xFF);
    printf("  [7:0]  revision           0x%02X\n", (unsigned)(prid >> 0) & 0xFF);
    printf("\n");

    printf("CP1 FCR0    (reg 0)         0x%08lX\n", (unsigned long)fcr0);
    printf("  [15:8] implementation     0x%02X\n", (unsigned)(fcr0 >> 8) & 0xFF);
    printf("  [7:0]  revision           0x%02X\n", (unsigned)(fcr0 >> 0) & 0xFF);
    printf("\n");

    printf("MI_VERSION  (0xA4300004)    0x%08lX\n", (unsigned long)mi_version);
    printf("  IO version                0x%02X\n", (unsigned)(mi_version & 0xFF));
    printf("\n");

    printf("RDRAM  %uMB\n", has_expak ? 8 : 4);
    printf("  base  %s\n", base_single_chip ? "1x36Mbit" : "2x18Mbit");
    printf("  ID=0  manu=0x%04X (%s)  code=0x%04X\n",
        rdram0.manu, rdram_manu_str(rdram0.manu), rdram0.code);
    printf("  ID=2  manu=0x%04X (%s)  code=0x%04X\n",
        rdram1.manu, rdram_manu_str(rdram1.manu), rdram1.code);

    if (has_expak) {
        printf("  expak %s\n", expak_single_chip ? "1x36Mbit" : "2x18Mbit");
        printf("  ID=4  manu=0x%04X (%s)  code=0x%04X\n",
            rdram2.manu, rdram_manu_str(rdram2.manu), rdram2.code);
        printf("  ID=6  manu=0x%04X (%s)  code=0x%04X\n",
            rdram3.manu, rdram_manu_str(rdram3.manu), rdram3.code);
    }
    printf("\n");

    printf("VR4300 bugs\n");
    for (size_t i = 0; i < NUM_PROBES; i++) {
        printf("  %-6s  %s", probes[i].tag, status_str(results[i].status));
        if (results[i].status == RESULT_FAIL && results[i].detail != 0) {
            printf("  got=0x%08lX_%08lX",
                (unsigned long)(results[i].detail >> 32),
                (unsigned long)(results[i].detail & 0xFFFFFFFF));
        }
    printf("\n");
    }    
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void) {
    debug_init_isviewer();
    debug_init_usblog();

    console_init();
    console_set_render_mode(RENDER_MANUAL);
    console_clear();

    uint8_t  dmem_tvtype        = read_dmem_tvtype();
    int      tv_type            = get_tv_type();
    
    uint8_t  dmem_consoletype   = read_dmem_consoletype();
    bool     is_ique            = sys_bbplayer();

    uint32_t prid               = read_prid();
    uint32_t fcr0               = read_fcr0();
    
    uint32_t mi_version         = read_mi_version();

    rdram_manufacturer_t rdram0 = read_rdram_manufacturer(0);
    rdram_manufacturer_t rdram1 = read_rdram_manufacturer(2);

    bool base_single_chip  = (rdram_read_reg(0, 1) == rdram_read_reg(2, 1));    
    
    uint32_t memsize = get_memory_size();
    bool has_expak   = (memsize > 4*1024*1024);

    rdram_manufacturer_t rdram2 = {0}, rdram3 = {0};

    bool expak_single_chip = false;
    if (has_expak) {
        rdram2 = read_rdram_manufacturer(4);
        rdram3 = read_rdram_manufacturer(6);
        expak_single_chip = (rdram_read_reg(4, 1) == rdram_read_reg(6, 1));
    }
       
    dump_rdram_regs(0);
    dump_rdram_regs(2);
    if (has_expak) {
        dump_rdram_regs(4);
        dump_rdram_regs(6);
    }    

    report(dmem_tvtype, tv_type,
        dmem_consoletype, is_ique,
        prid, fcr0,
        mi_version, has_expak,
        base_single_chip, expak_single_chip,
        rdram0, rdram1,
        rdram2, rdram3);

    console_render();

    while (1) {}
}
