/* CronoVM soft-float64 runtime — linked counterpart to the translator's
 * f64 legaliser.
 *
 * The VM has no f64 register class. Rather than reject `double`, the
 * translator legalises every native f64 operation: an f64 value lives in two
 * 32-bit frame slots (lo/hi words) and each arithmetic / comparison /
 * conversion op is lowered to a CALL into one of the `__cvm_f*` functions
 * below. (Trivial ops — fneg/fabs/copysign, constants, load/store — are
 * lowered inline and never call here.)
 *
 * These wrappers are deliberately EXTERNAL (the header's `cvm_d_*` are
 * `static`): the legaliser references them by name, so they must survive
 * llvm-link as stable external symbols. Their bodies are pure 32-bit word
 * arithmetic (cvm_float64.h composes i32 ops), so this TU never itself
 * contains a native f64 value — no bootstrap problem.
 *
 * ABI (what clang's i386-elf lowering produces, and what the legaliser emits
 * the matching CALL for): a `cvm_f64` argument is passed as two i32 words in
 * (lo, hi) order; a `cvm_f64` return uses an `sret` hidden pointer as the
 * first argument (the function returns void and writes through it). So
 * `__cvm_fadd(&r, a.lo, a.hi, b.lo, b.hi)` and `__cvm_flt(a.lo,a.hi,b.lo,b.hi)
 * -> i32`. cvm-cc links this TU into any module that uses `double`. */

#include "cvm_float64.h"

/* --- arithmetic (sret return) --------------------------------------- */
cvm_f64 __cvm_fadd(cvm_f64 a, cvm_f64 b) { return cvm_d_add(a, b); }
cvm_f64 __cvm_fsub(cvm_f64 a, cvm_f64 b) { return cvm_d_sub(a, b); }
cvm_f64 __cvm_fmul(cvm_f64 a, cvm_f64 b) { return cvm_d_mul(a, b); }
cvm_f64 __cvm_fdiv(cvm_f64 a, cvm_f64 b) { return cvm_d_div(a, b); }

/* --- comparisons (i32 return) --------------------------------------- */
int __cvm_feq(cvm_f64 a, cvm_f64 b) { return cvm_d_eq(a, b); }
int __cvm_fne(cvm_f64 a, cvm_f64 b) { return cvm_d_ne(a, b); }
int __cvm_flt(cvm_f64 a, cvm_f64 b) { return cvm_d_lt(a, b); }
int __cvm_fle(cvm_f64 a, cvm_f64 b) { return cvm_d_le(a, b); }
int __cvm_fgt(cvm_f64 a, cvm_f64 b) { return cvm_d_gt(a, b); }
int __cvm_fge(cvm_f64 a, cvm_f64 b) { return cvm_d_ge(a, b); }
/* Compound predicates, so the legaliser emits ONE call per fcmp (a multi-call
 * lowering would need to preserve the first result across the second CALL,
 * which clobbers the transient registers). ord = both non-NaN; one = ordered
 * not-equal. uno/ueq are these negated (xor 1) by the caller. */
int __cvm_ford(cvm_f64 a, cvm_f64 b) { return !cvm_d_isnan(a) && !cvm_d_isnan(b); }
int __cvm_fone(cvm_f64 a, cvm_f64 b) { return cvm_d_lt(a, b) || cvm_d_lt(b, a); }

/* --- conversions ---------------------------------------------------- */
cvm_f64  __cvm_f_from_i32(int32_t v)  { return cvm_d_from_i32(v); }   /* sret */
cvm_f64  __cvm_f_from_u32(uint32_t v) { return cvm_d_from_u32(v); }   /* sret */
int32_t  __cvm_f_to_i32(cvm_f64 x)    { return cvm_d_to_i32(x); }
uint32_t __cvm_f_to_u32(cvm_f64 x)    { return cvm_d_to_u32(x); }
cvm_f64  __cvm_f_from_f32(float f)    { return cvm_d_from_f32(f); }   /* sret */
float    __cvm_f_to_f32(cvm_f64 x)    { return cvm_d_to_f32(x); }

/* 64-bit integer -> double (sret). Composed from the 32-bit primitives so this
 * TU never forms a native f64 (no bootstrap) and never re-invokes the i64->f64
 * conversion it backs. Split into two 32-bit halves and combine as
 *     hi * 2^32 + lo.
 * `cvm_d_from_u32(hi/lo)` is exact (both < 2^32 <= 2^53) and multiplying by a
 * power of two only adjusts the exponent, so `hi * 2^32` is exact too — only
 * the final add rounds, giving a SINGLE correctly-rounded 64-bit result (no
 * double rounding). Used by `(double)(u)int64`, e.g. picolibc strtod's
 * `__atod_engine` (it accumulates the mantissa in a uint64_t). */
cvm_f64 __cvm_f_from_u64(uint64_t v) {
    uint32_t lo = (uint32_t)v;
    uint32_t hi = (uint32_t)(v >> 32);
    cvm_f64 dlo = cvm_d_from_u32(lo);
    if (hi == 0u) return dlo;                  /* value fits in 32 bits */
    cvm_f64 k16 = cvm_d_from_u32(0x10000u);    /* 2^16 */
    cvm_f64 thi = cvm_d_mul(cvm_d_mul(cvm_d_from_u32(hi), k16), k16); /* hi*2^32 (exact) */
    return cvm_d_add(thi, dlo);
}
cvm_f64 __cvm_f_from_i64(int64_t v) {
    if (v < 0) return cvm_d_neg(__cvm_f_from_u64((uint64_t)(-(uint64_t)v)));
    return __cvm_f_from_u64((uint64_t)v);
}

/* --- sqrt (llvm.sqrt.f64) ---------------------------------------------- *
 * Newton–Raphson y' = (y + x/y)/2 on a seed that halves x's exponent (so it
 * is within ~a factor of sqrt(2) of the answer for any magnitude). From a
 * worst-case relative error ~0.5 the quadratic iteration reaches < 2^-53 in
 * five steps; six are used for margin. Each step costs one soft f64 divide,
 * so this is intentionally for the rare double-sqrt — game code uses sqrtf
 * (the single FSQRT opcode). Special cases match IEEE: sqrt(NaN)=NaN,
 * sqrt(-0)=-0, sqrt(x<0)=NaN, sqrt(+Inf)=+Inf. */
cvm_f64 __cvm_fsqrt(cvm_f64 x) {
    if (cvm_d_isnan(x))  return CVM_D_NAN;
    if (cvm_d_iszero(x)) return x;                  /* +0 -> +0, -0 -> -0 */
    if (CVM_D_SIGN(x))   return CVM_D_NAN;          /* negative -> NaN    */
    if (cvm_d_isinf(x))  return x;                  /* +Inf -> +Inf       */

    int32_t se = ((int32_t)CVM_D_EXP(x) - 1023) >> 1;     /* halved exponent */
    cvm_f64 y    = cvm_d_pack(0u, (uint32_t)(se + 1023), 0u, 0u);
    cvm_f64 half = cvm_d_pack(0u, 1022u, 0u, 0u);         /* 0.5 */
    for (int it = 0; it < 6; ++it) {
        cvm_f64 q = cvm_d_div(x, y);
        y = cvm_d_mul(cvm_d_add(y, q), half);
    }
    return y;
}
