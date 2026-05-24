/**
 * n64-hardware-test
 * Hardware revision characterization ROM for Nintendo 64.
 *
 * Reads processor and FPU revision identifiers, runs targeted bug probes,
 * and reports raw results to both the console OSD and USB/isviewer debug log.
 *
 * Adding a new probe:
 *   1. Write a probe_*() function returning probe_result_t.
 *   2. Add an entry to the probes[] table in main().
 */

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
 * Affects VR4300 versions 1.x, 2.0, 2.1. Fixed in 2.2+.
 * A single intervening instruction (e.g. NOP) clears the hazard.
 *
 * Probe: compare results of the hazard sequence vs. the NOP-separated
 * sequence on fixed inputs (0 * inf, 2 * 3). PASS = results match.
 * ---------------------------------------------------------------------- */

static probe_result_t probe_mulmul(void) {
    float broken, working;
    const float zero = 0.0f;
    const float inf  = __builtin_inff();
    const float a    = 2.0f;
    const float b    = 3.0f;

    uint32_t fcr31_saved = C1_FCR31();
    C1_WRITE_FCR31(fcr31_saved & ~(C1_ENABLE_OVERFLOW | C1_ENABLE_DIV_BY_0 | C1_ENABLE_INVALID_OP));

    __asm__ volatile (
        "mul.s $f0, %1, %2\n"
        "mul.s %0, %3, %4\n"
        : "=f"(broken)
        : "f"(zero), "f"(inf), "f"(a), "f"(b)
        : "f0"
    );
    __asm__ volatile (
        "mul.s $f0, %1, %2\n"
        "nop\n"
        "mul.s %0, %3, %4\n"
        : "=f"(working)
        : "f"(zero), "f"(inf), "f"(a), "f"(b)
        : "f0"
    );

    C1_WRITE_FCR31(fcr31_saved);

    if (F32I(broken) != F32I(working))
        return FAIL((uint64_t)F32I(broken) << 32 | (uint64_t)F32I(working));
    return PASS();
}

/* -------------------------------------------------------------------------
 * BUG: sra — 32-bit arithmetic right shift leaks 64-bit state
 *
 * STUB: requires construction of 64-bit register state in inline asm.
 * Deferred pending test vector validation on hardware.
 * ---------------------------------------------------------------------- */

static probe_result_t probe_sra(void) {
    return STUB();
}

/* -------------------------------------------------------------------------
 * BUG: mult — 32-bit signed multiply sign-extension anomaly
 *
 * STUB: requires construction of non-sign-extended 64-bit input state.
 * Deferred pending test vector validation on hardware.
 * ---------------------------------------------------------------------- */

static probe_result_t probe_mult(void) {
    return STUB();
}

/* -------------------------------------------------------------------------
 * BUG: div — 32-bit signed divide sign-extension anomaly
 *
 * STUB: requires construction of non-sign-extended 64-bit input state.
 * Deferred pending test vector validation on hardware.
 * ---------------------------------------------------------------------- */

static probe_result_t probe_div(void) {
    return STUB();
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
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void) {
    debug_init_isviewer();
    debug_init_usblog();

    console_init();
    console_set_render_mode(RENDER_MANUAL);
    console_clear();

    uint32_t prid = read_prid();
    uint32_t fcr0 = read_fcr0();

    /* Console output */
    printf("=== n64-hardware-test ===\n\n");

    printf("PRId  0x%08lX\n", (unsigned long)prid);
    printf("  impl  0x%02X\n", (unsigned)(prid >> 8) & 0xFF);
    printf("  rev   0x%02X\n\n", (unsigned)(prid >> 0) & 0xFF);

    printf("FCR0  0x%08lX\n", (unsigned long)fcr0);
    printf("  impl  0x%02X\n", (unsigned)(fcr0 >> 8) & 0xFF);
    printf("  rev   0x%02X\n\n", (unsigned)(fcr0 >> 0) & 0xFF);

    for (size_t i = 0; i < NUM_PROBES; i++) {
        const probe_entry_t *p = &probes[i];
        probe_result_t r = p->fn();
        printf("%-8s  %s", p->tag, status_str(r.status));
        if (r.status == RESULT_FAIL && r.detail != 0) {
            printf("  got=0x%08lX  ref=0x%08lX",
                (unsigned long)(r.detail >> 32),
                (unsigned long)(r.detail & 0xFFFFFFFF));
        }
        printf("\n");
    }

    /* Debug log */
    debugf("=== n64-hardware-test ===\n");
    debugf("PRId=0x%08lX impl=0x%02X rev=0x%02X\n",
        (unsigned long)prid,
        (unsigned)(prid >> 8) & 0xFF,
        (unsigned)(prid >> 0) & 0xFF);
    debugf("FCR0=0x%08lX impl=0x%02X rev=0x%02X\n",
        (unsigned long)fcr0,
        (unsigned)(fcr0 >> 8) & 0xFF,
        (unsigned)(fcr0 >> 0) & 0xFF);

    for (size_t i = 0; i < NUM_PROBES; i++) {
        const probe_entry_t *p = &probes[i];
        probe_result_t r = p->fn();
        debugf("%s: %s", p->tag, status_str(r.status));
        if (r.status == RESULT_FAIL && r.detail != 0) {
            debugf("  got=0x%08lX ref=0x%08lX",
                (unsigned long)(r.detail >> 32),
                (unsigned long)(r.detail & 0xFFFFFFFF));
        }
        debugf("\n");
    }

    console_render();

    while (1) {}
}
