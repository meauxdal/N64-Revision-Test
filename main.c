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
 * libdragon reads these from RSP DMEM at boot (crt0.S):
 *   0xA4000009 -> __boot_tvtype      -> get_tv_type()
 *   0xA400000A -> __boot_resettype
 *   0xA400000B -> __boot_consoletype -> sys_bbplayer()
 *
 * 0xBFC007E4 is the PIF-RAM boot info word written by IPL2,
 * source of the above values before libdragon parses them.
 * ---------------------------------------------------------------------- */

static uint32_t read_pif_boot_word(void) {
    return *((volatile uint32_t *)0xBFC007E4);
}

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
     */
    uint32_t bits_zero = 0x00000000UL;          /* 0.0f */
    uint32_t bits_inf  = 0x7F800000UL;          /* +inf */
    uint32_t bits_two  = 0x40000000UL;          /* 2.0f */
    uint32_t bits_thr  = 0x40400000UL;          /* 3.0f */
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
        : "r"(bits_zero), "r"(bits_inf), "r"(bits_two), "r"(bits_thr)
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
     *
     * NOTE: exact expected output for the dirty case is not yet known.
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

static void report(int tv_type, uint32_t pif_boot_word,
                   uint8_t dmem_tvtype, uint8_t dmem_consoletype,
                   uint32_t prid, uint32_t fcr0, bool is_ique)
{
    probe_result_t results[NUM_PROBES];
    for (size_t i = 0; i < NUM_PROBES; i++)
        results[i] = probes[i].fn();

    printf("=== n64-revision-test ===\n\n");

    printf("libdragon\n");
    printf("  region  get_tv_type()     %s\n", tv_type_str(tv_type));
    printf("  iQue    sys_bbplayer()    %s\n\n", is_ique ? "yes" : "no");

    printf("PIF\n");
    printf("  boot word     0xBFC007E4  0x%08lX\n", (unsigned long)pif_boot_word);
    printf("  tv type       0xA4000009  0x%02X\n",  (unsigned)dmem_tvtype);
    printf("  console type  0xA400000B  0x%02X\n\n",  (unsigned)dmem_consoletype);

    printf("CP0 PRId  0x%08lX\n", (unsigned long)prid);
    printf("  [15:8] ID                 0x%02X\n", (unsigned)(prid >> 8) & 0xFF);
    printf("  [7:0]  revision           0x%02X\n\n", (unsigned)(prid >> 0) & 0xFF);

    printf("CP1 FCR0  0x%08lX\n", (unsigned long)fcr0);
    printf("  [15:8] implementation     0x%02X\n", (unsigned)(fcr0 >> 8) & 0xFF);
    printf("  [7:0]  revision           0x%02X\n\n", (unsigned)(fcr0 >> 0) & 0xFF);

    printf("hardware bugs\n");
    for (size_t i = 0; i < NUM_PROBES; i++) {
        printf("  %-8s  %s", probes[i].tag, status_str(results[i].status));
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

    int      tv_type          = get_tv_type();
    uint32_t pif_boot_word    = read_pif_boot_word();
    uint8_t  dmem_tvtype      = read_dmem_tvtype();
    uint8_t  dmem_consoletype = read_dmem_consoletype();
    uint32_t prid             = read_prid();
    uint32_t fcr0             = read_fcr0();
    bool     is_ique          = sys_bbplayer();

    report(tv_type, pif_boot_word, dmem_tvtype, dmem_consoletype,
           prid, fcr0, is_ique);

    console_render();

    while (1) {}
}
