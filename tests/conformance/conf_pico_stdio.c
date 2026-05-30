/* conf_pico_stdio.c — conformance slice: picolibc tinystdio formatting.
 *
 * Links the real picolibc bitcode (built --with-stdio) on the VM side and the
 * host libc on the native side, then checks that snprintf produces byte-
 * identical output for a spread of conversions. This is both a smoke test that
 * tinystdio translates+runs and a differential check of its formatter — the
 * varargs walk + the integer engine + the CLASSIC dtoa float path (f64 soft).
 *
 * Discipline: fold the formatted bytes + the return length into one int32
 * checksum. Differential caveats avoided on purpose:
 *   - %p (pointer) — addresses differ VM vs host, skipped.
 *   - %g/%e and inexact %f — picolibc's classic dtoa and the host libc are
 *     DIFFERENT implementations; only EXACT-binary values at fixed precision
 *     are compared, where every correct implementation must agree digit-for-
 *     digit (no rounding ambiguity).
 *
 * The VM build gets snprintf from picolibc.bc, main + weak malloc from
 * vm_entry.c, and errno/sbrk/posix-stubs/__isnand from pico_machine.c.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)
    char b[128];
    int  n;
    /* fold the whole buffer (len bytes) + the return value */
    #define DO(...) do { n = snprintf(b, sizeof b, __VA_ARGS__); \
                         MIX(n); for (int _i = 0; _i < n && _i < (int)sizeof b; ++_i) MIX(b[_i]); } while (0)

    /* ---- integers: bases, signs, widths, precision, flags ---- */
    DO("%d", 0);
    DO("%d", 42);
    DO("%d", -42);
    DO("%d", 2147483647);
    DO("%d", (int)(-2147483647 - 1));      /* INT_MIN */
    DO("%u", 4000000000u);
    DO("%x %X", 0xDEADBEEFu, 0xDEADBEEFu);
    DO("%o", 0755u);
    DO("[%5d][%-5d][%05d][%+d][% d]", 42, 42, 42, 42, 42);
    DO("[%8.3d][%.0d]", 7, 0);
    DO("%ld %lu", -123456789L, 123456789UL);
    DO("hex ptr-ish %08x", 0x1234u);

    /* ---- chars + strings: widths, precision, truncation ---- */
    DO("%c%c%c", 'A', 'b', '!');
    DO("[%s]", "hello");
    DO("[%10s][%-10s]", "hi", "hi");
    DO("[%.3s]", "truncated");
    DO("%s and %d and %c", "mix", 7, 'Z');
    DO("%%literal%%");

    /* ---- floats: EXACT-binary values only, fixed precision (no rounding) --- */
    DO("%f", 0.0);
    DO("%f", 1.0);
    DO("%f", -2.5);
    DO("%f", 3.25);
    DO("%.2f", 0.25);
    DO("%.1f", 100.5);
    DO("%8.2f", -7.75);
    DO("%.3f", 0.125);
    DO("%f", 12345.5);

    /* ---- return value when truncated (snprintf returns would-be length) ---- */
    char tiny[4];
    int wlen = snprintf(tiny, sizeof tiny, "%d", 999999);   /* needs 6, buf 4 */
    MIX(wlen);                       /* must be 6 on both */
    for (int i = 0; i < (int)sizeof tiny; ++i) MIX(tiny[i]);

    #undef DO
    #undef MIX
    return (int)h;
}
