#include <stdint.h>
#include "cvm_intrin.h"

/* Exercises the f32 opcodes end-to-end. Returns 0 on success, otherwise
 * a small non-zero code identifying which check failed. Each phase stays
 * focused on one cluster of behaviour so a regression points at one
 * file:line.
 *
 * `n` is the R0 seed (always 0 from `test_e2e` here). We thread it
 * through every input so clang -O1 can't constant-fold the comparisons
 * away — `float_basic_main` has external linkage, so the optimiser
 * can't see what value the caller passes and has to keep the work
 * runtime. The inputs are still bit-for-bit deterministic at n=0. */

static inline float    bits2f(int32_t b) { float   f; __builtin_memcpy(&f, &b, sizeof f); return f; }
static inline int32_t  f2bits(float   f) { int32_t b; __builtin_memcpy(&b, &f, sizeof b); return b; }

int float_basic_main(int n) {
    float zero = (float)n;             /* I2F_S of 0 = 0.0f, opaque to optimiser */

    /* Phase 1 — basic arithmetic. Inputs chosen so the results are exact
     * in binary32 (no rounding); compare with == and trust IEEE. */
    float a = 1.5f + zero;
    float b = 2.5f + zero;
    if (a + b != 4.0f)        return 1;
    if (b - a != 1.0f)        return 2;
    if (a * b != 3.75f)       return 3;
    if (b / (0.5f + zero) != 5.0f) return 4;     /* exact: 2.5 / 0.5 = 5 */
    if (-a != -1.5f)          return 5;

    /* Phase 2 — every C comparison operator (the six the translator
     * supports: ==, !=, <, <=, >, >=). */
    if (!(a <  b))            return 10;
    if (!(a <= a))            return 11;
    if (!(b >  a))            return 12;
    if (!(b >= b))            return 13;
    if (  a == b)             return 14;
    if (!(a != b))            return 15;

    /* Phase 3 — int ↔ float conversions, ordinary range. */
    if ((int32_t)a != 1)               return 20;   /* truncates toward zero */
    if ((int32_t)b != 2)               return 21;
    if ((int32_t)(-2.7f - zero) != -2) return 22;   /* trunc, not floor */
    if ((float)(5 + n) != 5.0f)        return 23;
    if ((float)((int32_t)-1 + n) != -1.0f) return 24;
    /* (float)(uint32_t)-1 = 4294967295 ≈ rounds to 4294967296 = 2^32
     * (nearest float, ties-to-even). Distinguishes I2F_S from I2F_U. */
    if ((float)(uint32_t)((int32_t)-1 + n) != 4294967296.0f) return 25;

    /* Phase 4 — saturating F2I_S edge cases. The plain `(int32_t)f` cast
     * is UB in C for NaN / out-of-range inputs, and clang -O1 may fold
     * those calls to any value (typically 0) before the F2I_S opcode is
     * ever emitted. To pin the saturating semantics that the VM
     * guarantees, use the runtime intrinsics from cvm_intrin.h —
     * extern calls are opaque to the optimiser, so the opcode reaches
     * the interpreter unmodified. Construct ±Inf / NaN by bitcast;
     * thread `n` through the bit pattern to keep them opaque. */
    float pos_inf = bits2f((int32_t)0x7F800000 + n);
    float neg_inf = bits2f((int32_t)0xFF800000 + n);
    float nan_v   = bits2f((int32_t)0x7FC00000 + n);
    if (cvm_f2i_sat_s(pos_inf) != INT32_MAX) return 30;
    if (cvm_f2i_sat_s(neg_inf) != INT32_MIN) return 31;
    if (cvm_f2i_sat_s(nan_v)   != 0)         return 32;

    /* Saturating beyond the i32 range without going to ±Inf. */
    if (cvm_f2i_sat_s(1.0e10f  + zero) != INT32_MAX) return 33;
    if (cvm_f2i_sat_s(-1.0e10f + zero) != INT32_MIN) return 34;

    /* F2I_U: NaN→0, negatives→0, > 2^32 → UINT32_MAX. */
    if (cvm_f2i_sat_u(nan_v)         != 0u)          return 35;
    if (cvm_f2i_sat_u(-1.0f - zero)  != 0u)          return 36;
    if (cvm_f2i_sat_u(pos_inf)       != 0xFFFFFFFFu) return 37;

    /* Phase 5 — NaN ordered/unordered distinction. The translator maps
     * `==` to OEQ and `!=` to UNE, which is the only place the two
     * predicates' difference shows. */
    if (  nan_v == nan_v)      return 40;   /* OEQ: false (NaN ≠ self) */
    if (!(nan_v != nan_v))     return 41;   /* UNE: true  (NaN ≠ self) */
    if (  nan_v <  (1.0f + zero))      return 42;   /* OLT: false on NaN */
    if (  (1.0f + zero) <  nan_v)      return 43;

    /* Phase 6 — repeated FMUL stays bit-exact. 1.5^8 = 25.62890625
     * exactly in binary32 (each intermediate fits the 24-bit mantissa). */
    float t = 1.0f + zero;
    for (int i = 0; i < 8; i++) t = t * 1.5f;
    if (t != 25.62890625f)     return 50;

    /* Phase 7 — FDIV by zero produces ±Inf (no trap). */
    float div0_pos = (1.0f  + zero) / zero;
    float div0_neg = (-1.0f - zero) / zero;
    if (f2bits(div0_pos) != (int32_t)0x7F800000) return 60;
    if (f2bits(div0_neg) != (int32_t)0xFF800000) return 61;

    return 0;
}
