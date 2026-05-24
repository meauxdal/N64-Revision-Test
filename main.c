/**
 * n64-hardware-test
 * Hardware revision characterization ROM for Nintendo 64.
 *
 * Reads processor and FPU revision identifiers, runs targeted bug probes,
 * and reports results to both the console OSD and USB/isviewer debug log.
 *
 * Probes implemented:
 *   [CPU] PRId  — VR4300 processor id and revision
 *   [FPU] FCR0  — FPU implementation and revision
 *   [BUG] mulmul — FP double-mul hazard (fixed in later steppings)
 *   [BUG] sra   — 32-bit arithmetic right-shift 64-bit state leak (stub)
 *   [BUG] mult  — 32-bit signed multiply sign-extension (stub)
 *   [BUG] div   — 32-bit signed divide sign-extension (stub)
 *
 * Adding a new probe:
 *   1. Write a probe_*() function returning probe_result_t.
 *   2. Add an entry to the probes[] table in main().
 *   3. Add an interpret_*() entry if you have known mappings.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <libdragon.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Type-pun float -> uint32_t without UB. */
static inline uint32_t f32_to_bits(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}
#define F32I(x) f32_to_bits(x)

static inline bool is_denormal(float f) {
    uint32_t b = F32I(f);
    return ((b >> 23) & 0xFF) == 0 && (b & 0x7FFFFF) != 0;
}

static inline bool is_nan(float f) {
    uint32_t b = F32I(f);
    return ((b >> 23) & 0xFF) == 0xFF && (b & 0x7FFFFF) != 0;
}

/* -------------------------------------------------------------------------
 * Probe result type
 * ---------------------------------------------------------------------- */

typedef enum {
    RESULT_PASS,
    RESULT_FAIL,
    RESULT_STUB,   /* probe scaffolded but asm not yet validated */
} probe_status_t;

typedef struct {
    probe_status_t status;
    /* Optional extra detail word for logging; 0 if unused. */
    uint64_t detail;
} probe_result_t;

static probe_result_t PASS(void)         { return (probe_result_t){ RESULT_PASS, 0 }; }
static probe_result_t FAIL(uint64_t d)   { return (probe_result_t){ RESULT_FAIL, d }; }
static probe_result_t FAIL0(void)        { return (probe_result_t){ RESULT_FAIL, 0 }; }
static probe_result_t STUB(void)         { return (probe_result_t){ RESULT_STUB, 0 }; }

static const char *status_str(probe_status_t s) {
    switch (s) {
        case RESULT_PASS: return "PASS";
        case RESULT_FAIL: return "FAIL";
        case RESULT_STUB: return "STUB";
    }
    return "????";
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
 * Identifier interpretation
 * ---------------------------------------------------------------------- */

static const char *interpret_prid_rev(uint8_t rev) {
    switch (rev) {
        case 0x10: return "1.0 (early retail; mulmul affected)";
        case 0x22: return "2.2 (later retail)";
        case 0x40: return "4.0 (iQue Player)";
        default:   return "unknown";
    }
}

static const char *interpret_fcr0_rev(uint8_t rev) {
    switch (rev) {
        case 0x00: return "0x00 (all known retail + iQue)";
        default:   return "unknown";
    }
}

/* -------------------------------------------------------------------------
 * BUG: mulmul — FP double-multiply hazard
 *
 * A consecutive pair of mul.s instructions may produce incorrect results
 * for the second multiply when operands include NaN, Zero, or Infinity,
 * on early VR4300 steppings. A NOP between the two clears the hazard.
 *
 * Probe: run a known-bad input pair with and without the intervening NOP
 * and compare results. PASS = no discrepancy (bug absent or not triggered).
 *
 * Known-bad class: (0 * inf) followed by any mul.s.
 * We use fixed inputs so the test is deterministic.
 * ---------------------------------------------------------------------- */

static probe_result_t probe_mulmul(void) {
    float broken, working;
    const float a1 = 0.0f;
    const float b1 = __builtin_inff();
    const float a2 = 2.0f;
    const float b2 = 3.0f;

    __asm__ volatile (
        "mul.s $f0, %2, %3\n"
        "mul.s %0, %4, %5\n"
        : "=f"(broken)
        : "f"(a1), "f"(a1), "f"(b1), "f"(a2), "f"(b2)   /* dummy use of a1 */
        : "f0"
    );
    /* Clang/GCC: separate asm block guarantees the NOP version is not
       merged or reordered with the above. */
    __asm__ volatile (
        "mul.s $f0, %2, %3\n"
        "nop\n"
        "mul.s %0, %4, %5\n"
        : "=f"(working)
        : "f"(a1), "f"(a1), "f"(b1), "f"(a2), "f"(b2)
        : "f0"
    );

    if (F32I(broken) != F32I(working))
        return FAIL((uint64_t)F32I(broken) << 32 | F32I(working));
    return PASS();
}

/* -------------------------------------------------------------------------
 * BUG: sra — 32-bit arithmetic right shift leaks 64-bit state
 *
 * Per VR4300 manual: sra rd, rt, sa should sign-extend from bit 31 of the
 * lower 32 bits. In practice on all known N64 hardware, bits shifted in from
 * the top come from the *upper* 32 bits of the 64-bit register, and only the
 * *new* bit 31 is used for sign-extension into the upper word.
 *
 * Example: input 0x0123456789ABCDEF, sa=16
 *   Manual result:  0xFFFFFFFFFFFF89AB  (sign from original bit 31 = 1)
 *   Hardware result: 0x00000000456789AB  (sign from new bit 31 = 0)
 *
 * STUB: asm needs validation on hardware before promoting to PASS/FAIL.
 * ---------------------------------------------------------------------- */

static probe_result_t probe_sra(void) {
    /* Load a 64-bit value into a register using DMTC1 as scratch,
       then read it back via DMFC1 — or better, use integer registers
       directly with 64-bit loads. This requires -mabi=64 or inline asm
       that manually constructs the 64-bit value. Deferred pending asm
       review. */
    (void)0;
    return STUB();
}

/* -------------------------------------------------------------------------
 * BUG: mult — 32-bit signed multiply sign-extension
 *
 * mult is supposed to sign-extend both 32-bit operands to 64 bits before
 * multiplying. In practice the second operand is sign-extended only from
 * bit 34, not bit 31, making it a 64×35-bit multiply.
 *
 * STUB: characterization inputs need hardware validation.
 * ---------------------------------------------------------------------- */

static probe_result_t probe_mult(void) {
    (void)0;
    return STUB();
}

/* -------------------------------------------------------------------------
 * BUG: div — 32-bit signed divide sign-extension
 *
 * Similar to mult, but with an additional anomalous case when bits 63 and
 * 31 of the divisor are not equal. Remainder behavior is at least consistent
 * with: remainder = (int32_t)(dividend - quotient * divisor).
 *
 * STUB: requires careful construction of 64-bit register state.
 * ---------------------------------------------------------------------- */

static probe_result_t probe_div(void) {
    (void)0;
    return STUB();
}

/* -------------------------------------------------------------------------
 * Probe table
 * ---------------------------------------------------------------------- */

typedef probe_result_t (*probe_fn_t)(void);

typedef struct {
    const char *tag;          /* printed label, e.g. "[BUG] mulmul" */
    const char *pass_note;    /* brief note shown on PASS */
    const char *fail_note;    /* brief note shown on FAIL */
    probe_fn_t  fn;
} probe_entry_t;

static const probe_entry_t probes[] = {
    {
        "[BUG] mulmul",
        "not present",
        "FP hazard; early stepping",
        probe_mulmul,
    },
    {
        "[BUG] sra",
        "manual behavior",
        "64-bit state leak (expected on all N64)",
        probe_sra,
    },
    {
        "[BUG] mult",
        "manual behavior",
        "sign-ext anomaly (expected on all N64)",
        probe_mult,
    },
    {
        "[BUG] div",
        "manual behavior",
        "sign-ext anomaly (expected on all N64)",
        probe_div,
    },
};

#define NUM_PROBES (sizeof(probes) / sizeof(probes[0]))

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void) {
    debug_init_isviewer();
    debug_init_usblog();

    console_init();
    console_set_render_mode(RENDER_MANUAL);
    console_clear();

    /* -- Identifier reads ------------------------------------------------ */

    uint32_t prid = read_prid();
    uint32_t fcr0 = read_fcr0();

    uint8_t cpu_impl = (prid >> 8) & 0xFF;
    uint8_t cpu_rev  = (prid >> 0) & 0xFF;
    uint8_t fpu_impl = (fcr0 >> 8) & 0xFF;
    uint8_t fpu_rev  = (fcr0 >> 0) & 0xFF;

    /* -- Console header -------------------------------------------------- */

    printf("=== N64 hardware revision report ===\n\n");

    printf("[CPU] PRId:  0x%08lX\n", (unsigned long)prid);
    printf("      impl:  0x%02X (expect 0x0B)\n", cpu_impl);
    printf("      rev:   0x%02X => %s\n", cpu_rev, interpret_prid_rev(cpu_rev));

    printf("[FPU] FCR0:  0x%08lX\n", (unsigned long)fcr0);
    printf("      impl:  0x%02X (expect 0x0B)\n", fpu_impl);
    printf("      rev:   0x%02X => %s\n", fpu_rev, interpret_fcr0_rev(fpu_rev));

    printf("\n");

    /* -- Probes ---------------------------------------------------------- */

    for (size_t i = 0; i < NUM_PROBES; i++) {
        const probe_entry_t *p = &probes[i];
        probe_result_t r = p->fn();

        const char *note = "";
        if (r.status == RESULT_PASS) note = p->pass_note;
        if (r.status == RESULT_FAIL) note = p->fail_note;

        printf("%-16s %s  %s\n", p->tag, status_str(r.status), note);

        if (r.status == RESULT_FAIL && r.detail != 0) {
            printf("                 got=0x%08lX ref=0x%08lX\n",
                (unsigned long)(r.detail >> 32),
                (unsigned long)(r.detail & 0xFFFFFFFF));
        }
    }

    printf("\n");

    /* -- Debug log (mirrors console output with more detail) ------------ */

    debugf("=== N64 hardware revision report ===\n");
    debugf("PRId=0x%08lX impl=0x%02X rev=0x%02X (%s)\n",
        (unsigned long)prid, cpu_impl, cpu_rev, interpret_prid_rev(cpu_rev));
    debugf("FCR0=0x%08lX impl=0x%02X rev=0x%02X (%s)\n",
        (unsigned long)fcr0, fpu_impl, fpu_rev, interpret_fcr0_rev(fpu_rev));

    for (size_t i = 0; i < NUM_PROBES; i++) {
        const probe_entry_t *p = &probes[i];
        probe_result_t r = p->fn();  /* run again for log — probes must be pure */
        const char *note = (r.status == RESULT_FAIL) ? p->fail_note : p->pass_note;
        debugf("%s: %s  %s", p->tag, status_str(r.status), note);
        if (r.status == RESULT_FAIL && r.detail != 0) {
            debugf("  [got=0x%08lX ref=0x%08lX]",
                (unsigned long)(r.detail >> 32),
                (unsigned long)(r.detail & 0xFFFFFFFF));
        }
        debugf("\n");
    }

    console_render();

    /* Hold forever — this is a reporting ROM, not a game. */
    while (1) {}
}
