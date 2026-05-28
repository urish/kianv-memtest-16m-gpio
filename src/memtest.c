/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * KianV uLinux SoC v2 POST: 16 MiB QSPI PSRAM memtest.
 *
 * Sequence:
 *   1. UART init @ 115200 8N1, banner.
 *   2. Read ui_in[1] for test depth: 0 = FAST (single stride-64 sweep,
 *      ~2 s @ 30 MHz), 1 = THOROUGH (three full-coverage sweeps:
 *      addr-in-data, inverse-addr, xorshift32 LFSR; ~96 s @ 30 MHz).
 *   3. Probe phase (always): write+read 2 patterns at 8 sparse addresses
 *      with uart_drain() bracketing every store/load, so the LAST line
 *      on the wire pinpoints any hung-bus access.
 *   4. Selected sweep(s) with a per-tick (~100 KiB) progress event.
 *      ui_in[0] = 0 -> '.' to UART (throttled to ~1 per MiB).
 *      ui_in[0] = 1 -> outer-ring 7-seg chase on uo_out[6:0] at ~10 Hz
 *                      (segment A on bit 0, G on bit 6, common cathode).
 *   5. PASS  -> "PASS" to UART (drained), then SEG_P latched on the
 *               7-segment, then syscon halt.
 *      FAIL  -> "FAIL test=N addr=0x.. exp=0x.. got=0x.." to UART,
 *               then SEG_F latched, then syscon halt.
 */

#include <stdint.h>
#include <stddef.h>

/* ---- MMIO addresses ----------------------------------------------------- */
#define UART_DATA       0x10000000u
#define UART_LSR        0x10000005u
#define UART_DIV        0x10000010u

#define GPIO_UO_EN      0x10600000u
#define GPIO_UO_OUT     0x10600004u
#define GPIO_UI_IN      0x10600008u

#define SYSCON          0x11100000u
#define SYSCON_HALT     0x5555u

#define PSRAM_BASE      0x80000000u
#define PSRAM_TEST_END  0x80FFF000u   /* top 4 KiB reserved for stack */

/* ---- Tunables ----------------------------------------------------------- */
#ifndef CPU_FREQ
#define CPU_FREQ        30000000u
#endif
#ifndef BAUDRATE
#define BAUDRATE        115200u
#endif

/* Inner-loop tick fires every SPIN_BYTES of memory traffic. In GPIO mode
 * (ui_in[0]=1) each tick advances the 7-segment spinner by one step, so
 * 100 KiB at ~30 MHz with ~120 cycles per QSPI word lands the spinner
 * at roughly 10 Hz (0.1 s per step). In UART mode (ui_in[0]=0) the
 * tick is throttled by DOT_PER_SPIN so a '.' only goes out every Nth
 * tick, giving the operator a per-MiB cadence on the console. */
#define SPIN_BYTES    (100u * 1024u)
#define DOT_PER_SPIN  10u

/* LSR bits (8250-compatible). */
#define LSR_THRE        0x20u   /* THR empty - ok to write next byte        */
#define LSR_TEMT        0x40u   /* TX shift register empty - safe to halt   */

/* ---- Tiny helpers ------------------------------------------------------- */
static inline void mmio_w32(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t mmio_r32(uint32_t a)         { return *(volatile uint32_t *)a; }
static inline void mmio_w8 (uint32_t a, uint8_t v)  { *(volatile uint8_t  *)a = v; }
static inline uint8_t  mmio_r8 (uint32_t a)         { return *(volatile uint8_t  *)a; }

/* ---- UART --------------------------------------------------------------- */
static void uart_init(void) {
    uint32_t f = CPU_FREQ;
    mmio_w32(UART_DIV, ((f / 10000u) << 16) | (f / BAUDRATE));
}

static void uart_putc(char c) {
    while ((mmio_r8(UART_LSR) & LSR_THRE) == 0) { }
    mmio_w8(UART_DATA, (uint8_t)c);
}

static void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

static void uart_putx32(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    int i;
    uart_puts("0x");
    for (i = 28; i >= 0; i -= 4) uart_putc(hex[(v >> i) & 0xfu]);
}

/* Block until the shift register is fully empty, so the last visible byte
 * really did make it out before whatever follows (a HALT, or a PSRAM
 * access that may hang the bus). */
static void uart_drain(void) {
    while ((mmio_r8(UART_LSR) & LSR_TEMT) == 0) { }
}

/* ---- uo / GPIO routing --------------------------------------------------
 * The SoC default maps UART TX to uo_out[0]. Setting GPIO_UO_EN bits steals
 * those pins for GPIO_UO_OUT. We use this to drive a 7-segment spinner
 * during the bulk sweeps, but only when ui_in[0] is held high -- otherwise
 * we stay UART-only so the operator can capture clean dots on the console.
 * Before any final UART message (PASS / FAIL / halt) we always release
 * uo by clearing GPIO_UO_EN so the bytes are visible. */
static inline void uo_release(void) {
    mmio_w32(GPIO_UO_EN, 0u);
}
static inline int ui_switch_on(void) {
    return (int)(mmio_r32(GPIO_UI_IN) & 1u);
}
static inline int ui_thorough(void) {
    return (int)((mmio_r32(GPIO_UI_IN) >> 1) & 1u);
}

/* Outer-ring chase: a, b, c, d, e, f. const -> .rodata, lives in flash. */
static const uint8_t spinner_glyphs[6] = { 0x01u, 0x02u, 0x04u, 0x08u, 0x10u, 0x20u };

/* Final-state glyphs (bit0=A .. bit6=G). The GPIO output register keeps
 * driving the pins after SYSCON_HALT gates the CPU clock, so the glyph
 * stays lit until reset. */
#define SEG_P   0x73u   /* PASS */
#define SEG_F   0x71u   /* FAIL */

/* ---- Terminal states ---------------------------------------------------- */
static void halt_with_glyph(uint8_t glyph) {
    uart_drain();
    mmio_w32(GPIO_UO_EN, 0xFFu);
    mmio_w32(GPIO_UO_OUT, (uint32_t)glyph);
    mmio_w32(SYSCON, SYSCON_HALT);
    for (;;) { }
}

static void fail(int test, uint32_t addr, uint32_t exp, uint32_t got) {
    uo_release();
    uart_puts("\nFAIL test=");
    uart_putc((char)('0' + test));
    uart_puts(" addr=");
    uart_putx32(addr);
    uart_puts(" exp=");
    uart_putx32(exp);
    uart_puts(" got=");
    uart_putx32(got);
    uart_puts("\n");
    halt_with_glyph(SEG_F);
}

/* ---- Probe phase -------------------------------------------------------- */
/*
 * Spot-check a handful of addresses spread across the PSRAM window. Every
 * MMIO access is bracketed by uart_drain() so that, if the bus hangs, the
 * very last line printed identifies the offending step exactly:
 *
 *   "0x80000000 wr 0xdeadbeef ..."   --> hang was on the WRITE
 *   "0x80000000 wr 0xdeadbeef rd"    --> hang was on the READ
 *   "0x80000000 wr 0xdeadbeef rd 0xdeadbeef OK"   --> step survived
 */
static int probe_addr(uint32_t addr) {
    static const uint32_t patterns[2] = { 0xDEADBEEFu, 0x12345678u };
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        uint32_t pat = patterns[i];
        uart_puts("  ");
        uart_putx32(addr);
        uart_puts(" wr ");
        uart_putx32(pat);
        uart_drain();

        mmio_w32(addr, pat);
        uart_puts(" rd");
        uart_drain();

        uint32_t v = mmio_r32(addr);
        uart_putc(' ');
        uart_putx32(v);
        if (v == pat) {
            uart_puts(" OK\n");
        } else {
            uart_puts(" MISMATCH\n");
            ok = 0;
        }
    }
    return ok;
}

static int probe_psram(void) {
    /* Sparse fan-out across the 16 MiB window. The top-most probe sits
     * one page below the stack guard so it never collides with sp. */
    static const uint32_t addrs[] = {
        0x80000000u,
        0x80000004u,    /* second word at base catches address-line[0..1] */
        0x80010000u,    /* 64 KiB in                                       */
        0x80100000u,    /* 1 MiB                                           */
        0x80400000u,    /* 4 MiB                                           */
        0x80800000u,    /* 8 MiB (likely bank boundary on 2x8 MiB layouts) */
        0x80C00000u,    /* 12 MiB                                          */
        0x80FFE000u,    /* near top, well below stack                      */
    };
    uart_puts("\n--- probe phase (write+read 2 patterns at 8 addresses) ---\n");
    int all_ok = 1;
    for (size_t i = 0; i < sizeof(addrs)/sizeof(addrs[0]); i++) {
        if (!probe_addr(addrs[i])) all_ok = 0;
    }
    return all_ok;
}

/* ---- Bulk memory tests -------------------------------------------------- *
 * NOTE on globals: the linker script (kianv.ld) only defines a FLASH MEMORY
 * region; there is no RAM section and crt0 does not zero .bss. Any
 * non-const static here would land in flash (or worse), and writes to it
 * would silently no-op or hang the bus. So all per-test state lives on
 * the stack (top-of-PSRAM, known good because the banner prints).
 * The const tables in probe_psram() / uart_putx32() / patterns[] are
 * .rodata-in-flash and read-only, which is fine.
 */

/* Live feedback state. Polled at every SPIN_BYTES of test traffic so the
 * operator can toggle ui_in[0] mid-sweep and the next tick reflects it.
 * Stack-allocated in main() (no .bss). */
typedef struct {
    uint32_t spin_idx;       /* 0..5 -- next outer-ring chase step          */
    uint32_t dot_subcount;   /* 0..DOT_PER_SPIN-1 in UART mode              */
    int      was_gpio;       /* -1 cold start, 0 last tick UART, 1 last GPIO */
} tick_state_t;

static void tick_event(tick_state_t *st) {
    int cur = ui_switch_on();

    /* Mode transition: announce on UART before silencing it (going to
     * GPIO) or after restoring it (coming back). Drain so the line is
     * fully out before the uo handover. Reset the dot sub-counter so
     * the first dot after coming back to UART aligns on a fresh MiB. */
    if (cur != st->was_gpio) {
        if (cur) {
            uart_puts("\n[ui_in[0]=1] -> 7-seg outer-ring spinner; UART silent\n");
            uart_drain();
            mmio_w32(GPIO_UO_EN, 0xFFu);
        } else {
            uo_release();
            uart_puts("\n[ui_in[0]=0] -> back to UART dots\n");
        }
        st->was_gpio = cur;
        st->dot_subcount = 0;
    }

    if (cur) {
        /* GPIO mode: advance the chase glyph on every tick (~10 Hz). */
        mmio_w32(GPIO_UO_OUT, (uint32_t)spinner_glyphs[st->spin_idx]);
        st->spin_idx = (st->spin_idx + 1u) % 6u;
    } else {
        /* UART mode: emit '.' once per DOT_PER_SPIN ticks (~per MiB). */
        st->dot_subcount++;
        if (st->dot_subcount >= DOT_PER_SPIN) {
            st->dot_subcount = 0;
            uart_putc('.');
        }
    }
}

#define ON_TICK(p, st) do { \
        (p) += 4; \
        if ((p) >= SPIN_BYTES) { (p) = 0; tick_event(st); } \
    } while (0)

static inline uint32_t xs32(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* Stride-64 address-in-data sweep. Touches ~1/16 of words but spans the
 * full address range, so address-line opens/shorts and per-region stuck
 * cells are caught. ~2 s @ 30 MHz. */
static void test_fast(int idx, tick_state_t *st) {
    uint32_t a;
    uint32_t progress;

    uart_puts("\n[fast] stride-64 address-in-data  write");
    progress = 0;
    for (a = PSRAM_BASE; a < PSRAM_TEST_END; a += 64) {
        *(volatile uint32_t *)a = a;
        ON_TICK(progress, st);
    }

    uart_puts(" verify");
    progress = 0;
    for (a = PSRAM_BASE; a < PSRAM_TEST_END; a += 64) {
        uint32_t v = *(volatile uint32_t *)a;
        if (v != a) fail(idx, a, a, v);
        ON_TICK(progress, st);
    }
    uart_puts(" OK");
}

static void test_addr_in_data(int idx, tick_state_t *st) {
    volatile uint32_t *p;
    uint32_t a;
    uint32_t progress;

    uart_puts("\n[1/3] address-in-data  write");
    progress = 0;
    p = (volatile uint32_t *)PSRAM_BASE;
    for (a = PSRAM_BASE; a < PSRAM_TEST_END; a += 4) { *p++ = a; ON_TICK(progress, st); }

    uart_puts(" verify");
    progress = 0;
    p = (volatile uint32_t *)PSRAM_BASE;
    for (a = PSRAM_BASE; a < PSRAM_TEST_END; a += 4) {
        uint32_t v = *p++;
        if (v != a) fail(idx, a, a, v);
        ON_TICK(progress, st);
    }
    uart_puts(" OK");
}

static void test_inverse_addr(int idx, tick_state_t *st) {
    volatile uint32_t *p;
    uint32_t a;
    uint32_t progress;

    uart_puts("\n[2/3] inverse-address  write");
    progress = 0;
    p = (volatile uint32_t *)PSRAM_BASE;
    for (a = PSRAM_BASE; a < PSRAM_TEST_END; a += 4) { *p++ = ~a; ON_TICK(progress, st); }

    uart_puts(" verify");
    progress = 0;
    p = (volatile uint32_t *)PSRAM_BASE;
    for (a = PSRAM_BASE; a < PSRAM_TEST_END; a += 4) {
        uint32_t v = *p++;
        if (v != ~a) fail(idx, a, ~a, v);
        ON_TICK(progress, st);
    }
    uart_puts(" OK");
}

static void test_lfsr(int idx, tick_state_t *st) {
    volatile uint32_t *p;
    uint32_t a, s;
    uint32_t progress;

    uart_puts("\n[3/3] xorshift32 LFSR  write");
    progress = 0;
    s = 0xCAFEBABEu;
    p = (volatile uint32_t *)PSRAM_BASE;
    for (a = PSRAM_BASE; a < PSRAM_TEST_END; a += 4) {
        s = xs32(s);
        *p++ = s;
        ON_TICK(progress, st);
    }

    uart_puts(" verify");
    progress = 0;
    s = 0xCAFEBABEu;
    p = (volatile uint32_t *)PSRAM_BASE;
    for (a = PSRAM_BASE; a < PSRAM_TEST_END; a += 4) {
        s = xs32(s);
        uint32_t v = *p++;
        if (v != s) fail(idx, a, s, v);
        ON_TICK(progress, st);
    }
    uart_puts(" OK");
}

/* ---- Entry point -------------------------------------------------------- */
int main(void) {
    uart_init();
    uart_puts("\nKianV uLinux SoC v2: 16 MiB QSPI PSRAM memtest\n");
    uart_puts("  region = ");
    uart_putx32(PSRAM_BASE);
    uart_puts(" .. ");
    uart_putx32(PSRAM_TEST_END);
    uart_puts(" (16 MiB - 4 KiB top reserved for stack)\n");

    int thorough = ui_thorough();
    if (thorough) {
        uart_puts("  mode   = thorough (ui_in[1]=1) -- 3 full-coverage sweeps, ~96 s @ 30 MHz\n");
    } else {
        uart_puts("  mode   = fast (ui_in[1]=0)     -- stride-64 single sweep, ~2 s @ 30 MHz\n");
        uart_puts("                                    (set ui_in[1]=1 before reset for thorough)\n");
    }

    if (!probe_psram()) {
        uart_puts("\nprobe phase reported MISMATCH(es) - skipping bulk sweep\n");
        halt_with_glyph(SEG_F);
    }

    uart_puts("\nprobe phase clean. starting bulk ");
    uart_puts(thorough ? "sweeps\n" : "sweep\n");
    uart_puts("  ui_in[0]=0 -> UART dots, one per ~MiB of traffic\n");
    uart_puts("  ui_in[0]=1 -> 7-seg outer-ring spinner on uo_out (~10 Hz; UART silent)\n");

    tick_state_t st = { .spin_idx = 0, .dot_subcount = 0, .was_gpio = -1 };
    if (thorough) {
        test_addr_in_data(1, &st);
        test_inverse_addr(2, &st);
        test_lfsr(3, &st);
    } else {
        test_fast(1, &st);
    }

    uo_release();
    uart_puts("\n\nPASS - all PSRAM tests succeeded\n");
    halt_with_glyph(SEG_P);
    return 0;
}
