#include <stdint.h>
#include "cvm_float64.h"

/* cvm_float64.h end-to-end. Returns 0 on success, otherwise a small
 * non-zero code identifying which check failed. `n` is the R0 seed
 * (always 0 from `test_e2e` here).
 *
 * Each phase is split into its own noinline function so a single-function
 * register count never blows past the translator's 254-register budget.
 * The classification + sign + pack helpers in cvm_float64.h are static
 * inline and get inlined into each phase; with one phase per function
 * the inlined-helper SSA load stays manageable. */

#define KEEP_OPAQUE(x, n) ((x) + (n) - (n))

static inline float bits2f(uint32_t b) { float f; __builtin_memcpy(&f, &b, sizeof f); return f; }

/* `x == x` to detect non-NaN gets folded to fcmp ord by clang -O1, which
 * the translator rejects. Use a bit-pattern check instead. */
static inline int is_nan_f(float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, sizeof b);
    return ((b & 0x7F800000u) == 0x7F800000u) && ((b & 0x007FFFFFu) != 0u);
}

/* --- Phase 1: constants ---------------------------------------------- */
__attribute__((noinline))
static int phase1_constants(int n) {
    (void)n;
    if (CVM_D_ZERO.lo  != 0u || CVM_D_ZERO.hi  != 0x00000000u) return 1;
    if (CVM_D_ONE.lo   != 0u || CVM_D_ONE.hi   != 0x3FF00000u) return 2;
    if (CVM_D_INF.lo   != 0u || CVM_D_INF.hi   != 0x7FF00000u) return 3;
    if (CVM_D_NAN.hi   != 0x7FF80000u)                         return 4;
    return 0;
}

/* --- Phase 2: classification ----------------------------------------- */
__attribute__((noinline))
static int phase2_classify(int n) {
    uint32_t z = (uint32_t)n;
    cvm_f64 zero = cvm_d_pack(0u, 0u, 0u + z, 0u);
    cvm_f64 one  = cvm_d_pack(0u, 1023u + z, 0u, 0u);
    cvm_f64 inf  = cvm_d_pack(0u, 0x7FFu + z, 0u, 0u);
    cvm_f64 nan  = cvm_d_pack(0u, 0x7FFu + z, 0x80000u, 0u);

    if (!cvm_d_iszero(zero))  return 10;
    if (cvm_d_iszero(one))    return 11;
    if (!cvm_d_isinf(inf))    return 12;
    if (cvm_d_isinf(nan))     return 13;
    if (!cvm_d_isnan(nan))    return 14;
    if (cvm_d_isnan(inf))     return 15;
    if (!cvm_d_isfinite(one)) return 16;
    if (cvm_d_isfinite(inf))  return 17;
    if (cvm_d_isfinite(nan))  return 18;
    return 0;
}

/* --- Phase 3: sign ops ----------------------------------------------- */
__attribute__((noinline))
static int phase3_sign(int n) {
    (void)n;
    cvm_f64 one = CVM_D_ONE;
    cvm_f64 ng = cvm_d_neg(one);
    if (ng.hi != 0xBFF00000u || ng.lo != 0u) return 20;
    if (cvm_d_neg(ng).hi != 0x3FF00000u)     return 21;
    cvm_f64 ab = cvm_d_abs(ng);
    if (ab.hi != 0x3FF00000u || ab.lo != 0u) return 22;
    return 0;
}

/* --- Phase 4: comparisons -------------------------------------------- */
__attribute__((noinline))
static int phase4_compare(int n) {
    cvm_f64 one  = CVM_D_ONE;
    cvm_f64 two  = cvm_d_from_i32(2 + n);
    cvm_f64 nan  = CVM_D_NAN;

    if (!cvm_d_eq(one, one))  return 30;
    if (cvm_d_eq(one, two))   return 31;
    if (cvm_d_eq(one, nan))   return 32;
    if (!cvm_d_ne(one, nan))  return 33;
    if (!cvm_d_lt(one, two))  return 34;
    if (cvm_d_lt(two, one))   return 35;
    if (cvm_d_lt(one, nan))   return 36;
    if (!cvm_d_le(one, one))  return 37;
    if (!cvm_d_gt(two, one))  return 38;
    if (!cvm_d_ge(two, two))  return 39;
    if (!cvm_d_eq(CVM_D_ZERO, CVM_D_NEG_ZERO)) return 40;
    if (!cvm_d_lt(cvm_d_neg(two), cvm_d_neg(one))) return 41;
    return 0;
}

/* --- Phase 5: f32 round-trip ----------------------------------------- */
__attribute__((noinline))
static int phase5_f32_roundtrip(int n) {
    cvm_f64 d = cvm_d_from_f32(1.5f + (float)n);
    if (cvm_d_to_f32(d) != 1.5f) return 50;
    if (d.hi != 0x3FF80000u)     return 51;
    if (d.lo != 0u)              return 52;

    d = cvm_d_from_f32(0.0f + (float)n);
    if (!cvm_d_iszero(d))        return 53;
    if (cvm_d_to_f32(d) != 0.0f) return 54;

    d = cvm_d_from_f32(-2.0f - (float)n);
    if (cvm_d_to_f32(d) != -2.0f) return 55;
    if (CVM_D_SIGN(d) != 1u)      return 56;

    float pinf = bits2f(0x7F800000u);
    d = cvm_d_from_f32(pinf + (float)n);
    if (!cvm_d_isinf(d))         return (int)(d.hi - 0x40000000u);
    if (CVM_D_SIGN(d) != 0u)     return 58;
    if (cvm_d_to_f32(d) != pinf) return 59;

    float qnan = bits2f(0x7FC00000u);
    d = cvm_d_from_f32(qnan + (float)n);
    if (!cvm_d_isnan(d))         return 60;
    float back = cvm_d_to_f32(d);
    if (!is_nan_f(back))         return 61;
    return 0;
}

/* --- Phase 6: int conversions --------------------------------------- */
__attribute__((noinline))
static int phase6_int_convert(int n) {
    cvm_f64 d = cvm_d_from_i32(7 + n);
    if (cvm_d_to_i32(d) != 7)        return 70;
    if (cvm_d_to_f32(d) != 7.0f)     return 71;

    d = cvm_d_from_i32(-100 - n);
    if (cvm_d_to_i32(d) != -100)     return 72;
    if (cvm_d_to_f32(d) != -100.0f)  return 73;

    d = cvm_d_from_i32(0 + n);
    if (!cvm_d_iszero(d))            return 74;

    d = cvm_d_from_i32(INT32_MAX - n);
    if (cvm_d_to_i32(d) != INT32_MAX) return 75;

    d = cvm_d_from_i32(INT32_MIN + n);
    if (cvm_d_to_i32(d) != INT32_MIN) return 76;

    cvm_f64 du = cvm_d_from_u32(0xFFFFFFFFu + (uint32_t)n);
    if (cvm_d_to_u32(du) != 0xFFFFFFFFu) return 77;
    if (cvm_d_to_i32(du) != INT32_MAX)   return 78;

    cvm_f64 neg = cvm_d_from_i32(-5 - n);
    if (cvm_d_to_u32(neg) != 0u)     return 79;

    if (cvm_d_to_i32(CVM_D_NAN) != 0)   return 80;
    if (cvm_d_to_u32(CVM_D_NAN) != 0u)  return 81;
    return 0;
}

/* --- Phase 7: addition ----------------------------------------------- */
__attribute__((noinline))
static int phase7_add(int n) {
    uint32_t z = (uint32_t)n;
    cvm_f64 one  = CVM_D_ONE;
    cvm_f64 two  = cvm_d_from_i32(2 + n);
    if (!cvm_d_eq(cvm_d_add(one, one), two)) return 90;

    cvm_f64 zero = cvm_d_add(one, cvm_d_neg(one));
    if (!cvm_d_iszero(zero))                 return 91;

    cvm_f64 half = cvm_d_pack(0u, 1022u + z, 0u, 0u);
    if (!cvm_d_eq(cvm_d_add(half, half), one)) return 92;

    cvm_f64 onehalf = cvm_d_add(one, half);
    if (onehalf.hi != 0x3FF80000u)           return 93;
    if (onehalf.lo != 0u)                    return 94;

    cvm_f64 a = cvm_d_from_i32(100 + n);
    cvm_f64 b = cvm_d_from_i32(200 + n);
    cvm_f64 c = cvm_d_from_i32(300 + n);
    if (!cvm_d_eq(cvm_d_add(a, b), c))       return 95;

    cvm_f64 inf_one = cvm_d_add(CVM_D_INF, one);
    if (!cvm_d_isinf(inf_one))               return 96;
    if (CVM_D_SIGN(inf_one) != 0u)           return 97;

    if (!cvm_d_isnan(cvm_d_add(CVM_D_INF, CVM_D_NEG_INF))) return 98;
    if (!cvm_d_isnan(cvm_d_add(one, CVM_D_NAN)))           return 99;
    return 0;
}

/* --- Phase 8: subtraction -------------------------------------------- */
__attribute__((noinline))
static int phase8_sub(int n) {
    cvm_f64 a = cvm_d_from_i32(10 + n);
    cvm_f64 b = cvm_d_from_i32(3  + n);
    cvm_f64 c = cvm_d_from_i32(7  + n);
    if (!cvm_d_eq(cvm_d_sub(a, b), c))       return 110;
    if (!cvm_d_iszero(cvm_d_sub(c, c)))      return 111;
    return 0;
}

/* --- Phase 9: multiplication ----------------------------------------- */
__attribute__((noinline))
static int phase9_mul(int n) {
    uint32_t z = (uint32_t)n;
    cvm_f64 two   = cvm_d_from_i32(2 + n);
    cvm_f64 three = cvm_d_from_i32(3 + n);
    cvm_f64 six   = cvm_d_from_i32(6 + n);
    if (!cvm_d_eq(cvm_d_mul(two, three), six)) return 120;

    if (!cvm_d_eq(cvm_d_mul(CVM_D_ONE, three), three)) return 121;

    if (!cvm_d_iszero(cvm_d_mul(CVM_D_ZERO, three))) return 122;

    cvm_f64 neg6 = cvm_d_mul(cvm_d_neg(two), three);
    if (!cvm_d_eq(neg6, cvm_d_neg(six))) return 123;

    if (!cvm_d_isnan(cvm_d_mul(CVM_D_INF, CVM_D_ZERO))) return 124;
    if (!cvm_d_isinf(cvm_d_mul(CVM_D_INF, two)))         return 125;
    if (!cvm_d_isnan(cvm_d_mul(two, CVM_D_NAN)))         return 126;

    cvm_f64 half = cvm_d_pack(0u, 1022u + z, 0u, 0u);
    cvm_f64 four = cvm_d_from_i32(4 + n);
    if (!cvm_d_eq(cvm_d_mul(half, four), two)) return 127;
    return 0;
}

/* --- Phase 10: division special cases ------------------------------- */
__attribute__((noinline))
static int phase10_div_special(int n) {
    cvm_f64 one = CVM_D_ONE;
    cvm_f64 two = cvm_d_from_i32(2 + n);
    cvm_f64 zero = CVM_D_ZERO;

    if (!cvm_d_isnan(cvm_d_div(zero, zero)))                return 130;
    if (!cvm_d_isnan(cvm_d_div(CVM_D_INF, CVM_D_INF)))      return 131;
    if (!cvm_d_isnan(cvm_d_div(one, CVM_D_NAN)))            return 132;
    if (!cvm_d_isinf(cvm_d_div(one, zero)))                 return 133;
    if (!cvm_d_iszero(cvm_d_div(one, CVM_D_INF)))           return 134;
    cvm_f64 ninf = cvm_d_div(cvm_d_neg(one), zero);
    if (!cvm_d_isinf(ninf) || CVM_D_SIGN(ninf) != 1u)       return 135;
    if (!cvm_d_iszero(cvm_d_div(zero, two)))                return 136;
    return 0;
}

/* --- Phase 11: division exact ratios -------------------------------- */
__attribute__((noinline))
static int phase11_div_exact(int n) {
    cvm_f64 one  = CVM_D_ONE;
    cvm_f64 two  = cvm_d_from_i32(2 + n);
    cvm_f64 four = cvm_d_from_i32(4 + n);
    cvm_f64 six  = cvm_d_from_i32(6 + n);

    /* 6 / 2 = 3 */
    cvm_f64 three = cvm_d_from_i32(3 + n);
    if (!cvm_d_eq(cvm_d_div(six, two), three)) return 140;
    /* 4 / 2 = 2 */
    if (!cvm_d_eq(cvm_d_div(four, two), two))  return 141;
    /* 1 / 1 = 1 */
    if (!cvm_d_eq(cvm_d_div(one, one), one))   return 142;
    /* 4 / 1 = 4 */
    if (!cvm_d_eq(cvm_d_div(four, one), four)) return 143;
    /* (-2) / 2 = -1 */
    cvm_f64 neg_one = cvm_d_div(cvm_d_neg(two), two);
    if (!cvm_d_eq(neg_one, cvm_d_neg(one)))    return 144;
    return 0;
}

/* --- Phase 12: division inexact (1/3) ------------------------------- */
/* This is the case that historically surfaced a translator bug —
 * `if (!cvm_d_lt(...))` was observing the call result as 0 even though
 * `return lt;` returned 1. Kept here as a regression catch. */
__attribute__((noinline))
static int phase12_div_inexact(int n) {
    cvm_f64 one   = CVM_D_ONE;
    cvm_f64 three = cvm_d_from_i32(3 + n);
    cvm_f64 q     = cvm_d_div(one, three);

    /* Expected truncate-rounded value of 1/3 in IEEE 754 binary64:
     *   0x3FD5555555555555. */
    if (q.hi != 0x3FD55555u) return 150;
    if (q.lo != 0x55555555u) return 151;

    /* Bound check: 1/3 must lie in (0.333, 0.334). Use the comparison
     * surface that earlier exposed the translator bug. */
    cvm_f64 lo_v = cvm_d_pack(0u, 1021u, 0x55555u, 0x55555550u);  /* < 1/3 */
    cvm_f64 hi_v = cvm_d_pack(0u, 1021u, 0x55556u, 0x00000000u);  /* > 1/3 */

    int lt_lo = cvm_d_lt(lo_v, q);
    if (!lt_lo) return 152;
    int lt_hi = cvm_d_lt(q, hi_v);
    if (!lt_hi) return 153;
    return 0;
}

/* --- Top-level driver. Each phase is its own function, so the budget
 *     of 254 SSA registers is per-phase rather than per-fixture. ---- */
int f64_basic_main(int n) {
    int r;
    if ((r = phase1_constants(n))    != 0) return r;
    if ((r = phase2_classify(n))     != 0) return r;
    if ((r = phase3_sign(n))         != 0) return r;
    if ((r = phase4_compare(n))      != 0) return r;
    if ((r = phase5_f32_roundtrip(n))!= 0) return r;
    if ((r = phase6_int_convert(n))  != 0) return r;
    if ((r = phase7_add(n))          != 0) return r;
    if ((r = phase8_sub(n))          != 0) return r;
    if ((r = phase9_mul(n))          != 0) return r;
    if ((r = phase10_div_special(n)) != 0) return r;
    if ((r = phase11_div_exact(n))   != 0) return r;
    if ((r = phase12_div_inexact(n)) != 0) return r;
    return 0;
}
