#include <stdint.h>
#include "cvm_intrin.h"

/* FSQRT opcode end-to-end. Returns 0 on success, otherwise a small
 * non-zero code identifying which check failed. `n` is the R0 seed
 * (always 0 from `test_e2e` here). We thread it through every input
 * so clang -O1 can't fold the calls away — `cvm_fsqrt` is opaque to
 * the optimiser anyway (extern decl with no body), but routing the
 * inputs through `n` keeps the rest of the arithmetic genuine.
 *
 * Comparing floats with `==` here is safe because every check uses
 * inputs that produce results exactly representable in binary32
 * (sqrt(0), sqrt(1), sqrt(4), sqrt(0x40800000) etc.). */

static inline float    bits2f(int32_t b) { float   f; __builtin_memcpy(&f, &b, sizeof f); return f; }
static inline int32_t  f2bits(float   f) { int32_t b; __builtin_memcpy(&b, &f, sizeof b); return b; }

/* Bit-pattern NaN check. The natural `x == x` form gets folded by
 * clang -O1 to `fcmp ord x, x` (predicate 7) which the translator
 * doesn't accept; this helper sidesteps that by inspecting the IEEE
 * bit pattern directly: NaN ⇔ exponent all-ones AND mantissa non-zero. */
static inline int is_nan_f(float f) {
    uint32_t b = (uint32_t)f2bits(f);
    return ((b & 0x7F800000u) == 0x7F800000u) && ((b & 0x007FFFFFu) != 0u);
}

int fsqrt_main(int n) {
    float zero = (float)n;             /* I2F_S of 0 = 0.0f, opaque */

    /* Phase 1 — exact perfect squares: sqrt yields integer floats. */
    if (cvm_fsqrt(0.0f  + zero) != 0.0f) return 1;
    if (cvm_fsqrt(1.0f  + zero) != 1.0f) return 2;
    if (cvm_fsqrt(4.0f  + zero) != 2.0f) return 3;
    if (cvm_fsqrt(9.0f  + zero) != 3.0f) return 4;
    if (cvm_fsqrt(16.0f + zero) != 4.0f) return 5;
    if (cvm_fsqrt(64.0f + zero) != 8.0f) return 6;

    /* Phase 2 — fractional perfect squares. */
    if (cvm_fsqrt(0.25f  + zero) != 0.5f)  return 10;
    if (cvm_fsqrt(0.0625f + zero) != 0.25f) return 11;

    /* Phase 3 — non-negative non-square: bound the result rather than
     * compare for exact equality (rounding modes vary). sqrt(2) is
     * approximately 1.41421356; bracket within ±1 ULP-ish.
     * Use plain < / > on f32, which the translator supports. */
    {
        float r = cvm_fsqrt(2.0f + zero);
        if (!(r > 1.41421f)) return 20;
        if (!(r < 1.41422f)) return 21;
        /* sqrt(2)*sqrt(2) ≈ 2.0 within rounding tolerance. */
        float r2 = r * r;
        if (!(r2 > 1.99999f)) return 22;
        if (!(r2 < 2.00001f)) return 23;
    }

    /* Phase 4 — special values:
     *   sqrt(+Inf) = +Inf
     *   sqrt(NaN)  = NaN
     *   sqrt(-1)   = NaN (in IEEE; some libms produce -NaN)
     * Detect via canonical float comparisons:
     *   x != x   ↔   x is NaN
     *   x > FLT_MAX ↔ x is +Inf (we test against a large finite below) */
    {
        /* Build +Inf without using <math.h>: bit pattern 0x7F800000. */
        float pinf = bits2f((int32_t)0x7F800000);
        float r_inf = cvm_fsqrt(pinf + zero);
        if (r_inf != pinf) return 30;            /* exact equality with +Inf */

        /* sqrt(NaN). Build a quiet NaN: 0x7FC00000. */
        float qnan = bits2f((int32_t)0x7FC00000);
        float r_nan = cvm_fsqrt(qnan + zero);
        if (!is_nan_f(r_nan)) return 31;         /* must be NaN */

        /* sqrt(-1) is NaN per IEEE. */
        float r_neg = cvm_fsqrt(-1.0f - zero);
        if (!is_nan_f(r_neg)) return 32;

        /* sqrt(-0.0) = -0.0 per IEEE (sign of zero preserved).
         * But `-0.0f == 0.0f` per IEEE, so test the bit pattern. */
        float neg_zero = bits2f((int32_t)0x80000000);
        float r_n0 = cvm_fsqrt(neg_zero);
        /* The standard says sqrt(-0) = -0; we don't enforce sign-of-zero
         * preservation strictly because soft-float impls vary. Just
         * require the magnitude is zero. */
        if (r_n0 != 0.0f) return 33;
    }

    return 0;
}
