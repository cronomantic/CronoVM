/* CronoVM software-emulated IEEE 754 binary64 (`double`) — user-side
 * library, zero VM changes.
 *
 * The VM has no f64 register class. The translator rejects every
 * appearance of `double` in LLVM IR, so user code that needs 64-bit
 * floats can't use C's `double`. Instead, this header exposes a
 * `cvm_f64` struct of two `uint32_t` halves (the canonical IEEE 754
 * binary64 bit pattern) and a working arithmetic surface implemented
 * entirely in i32 land. The mantissa arithmetic for add/mul piggybacks
 * on `cvm_int64.h`'s primitives.
 *
 * ---- Scope ----
 *
 *   * Construction, classification (isnan / isinf / iszero / isfinite)
 *   * Sign ops (neg / abs / copysign)
 *   * Comparisons (eq / ne / lt / le / gt / ge) with NaN unordered
 *     semantics — eq / lt return 0 if either operand is NaN; ne returns 1.
 *   * Conversions: f32 ↔ f64, i32 → f64, u32 → f64, f64 → i32 (saturating),
 *     f64 → u32 (saturating).
 *   * Arithmetic: add, sub, mul, neg, div.
 *
 * ---- Trade-offs ----
 *
 *   * **Rounding mode is per-op.** `cvm_d_div` rounds to nearest even
 *     (the IEEE 754 default) — bit-exact against hardware for ratios
 *     that fit in 53 bits. `cvm_d_add` / `cvm_d_sub` / `cvm_d_mul`
 *     still truncate (round-toward-zero) — within 1 ULP of IEEE, not
 *     bit-exact. Mixed because the div hot loop already needed extra
 *     state to handle the guard bit while the add/sub/mul paths
 *     would each grow noticeably for full IEEE rounding. Game math
 *     (physics integration, scaling, accumulation) tolerates the
 *     1-ULP slack; division is the operation where bit-exactness
 *     mattered most for the fixtures we care about.
 *   * **Flush-to-zero subnormals**: numbers in (0, 2^-1022) are treated as
 *     zero on input AND output. Standard for game-grade math; saves
 *     significant complexity in the alignment shifts and renormalisation
 *     paths. Sign of the resulting zero is preserved when meaningful
 *     (e.g. (-1) * tiny → -0).
 *   * **NaN propagation**: any operation with a NaN input returns the
 *     canonical quiet NaN (`CVM_D_NAN`). NaN payloads are NOT preserved.
 *
 * ---- Layout ----
 *
 *   `cvm_f64.hi` holds the top 32 bits (sign:1 + exp:11 + top 20 of
 *   mantissa). `cvm_f64.lo` holds the bottom 32 bits of the mantissa.
 *   Same arrangement the host CPU's `double` uses on little-endian
 *   targets, so a `__builtin_memcpy` between `double` and `cvm_f64` would
 *   be a no-op on the host (but the host language can't see f64 inside
 *   the VM, so this property is informational only). */

#ifndef CVM_FLOAT64_H
#define CVM_FLOAT64_H

#include <stdint.h>
#include "cvm_intrin.h"
#include "cvm_int64.h"

typedef struct cvm_f64 { uint32_t lo, hi; } cvm_f64;

/* --- Bit-field accessors --------------------------------------------- */

#define CVM_D_SIGN(x)   (((x).hi >> 31) & 1u)
#define CVM_D_EXP(x)    (((x).hi >> 20) & 0x7FFu)
#define CVM_D_MHI(x)    ((x).hi & 0x000FFFFFu)
#define CVM_D_MLO(x)    ((x).lo)
#define CVM_D_BIAS      1023

static inline cvm_f64 cvm_d_pack(uint32_t sign, uint32_t exp,
                                 uint32_t mhi20, uint32_t mlo32) {
    cvm_f64 r;
    r.lo = mlo32;
    r.hi = (sign << 31) | ((exp & 0x7FFu) << 20) | (mhi20 & 0xFFFFFu);
    return r;
}

/* --- Constants ------------------------------------------------------- */

#define CVM_D_ZERO       ((cvm_f64){0u,          0x00000000u})
#define CVM_D_NEG_ZERO   ((cvm_f64){0u,          0x80000000u})
#define CVM_D_ONE        ((cvm_f64){0u,          0x3FF00000u})
#define CVM_D_NEG_ONE    ((cvm_f64){0u,          0xBFF00000u})
#define CVM_D_INF        ((cvm_f64){0u,          0x7FF00000u})
#define CVM_D_NEG_INF    ((cvm_f64){0u,          0xFFF00000u})
#define CVM_D_NAN        ((cvm_f64){0u,          0x7FF80000u})  /* quiet */

/* --- Classification -------------------------------------------------- */

static inline int cvm_d_iszero(cvm_f64 x) {
    return CVM_D_EXP(x) == 0u && CVM_D_MHI(x) == 0u && x.lo == 0u;
}
static inline int cvm_d_isinf(cvm_f64 x) {
    return CVM_D_EXP(x) == 0x7FFu && CVM_D_MHI(x) == 0u && x.lo == 0u;
}
static inline int cvm_d_isnan(cvm_f64 x) {
    return CVM_D_EXP(x) == 0x7FFu && (CVM_D_MHI(x) != 0u || x.lo != 0u);
}
static inline int cvm_d_isfinite(cvm_f64 x) {
    return CVM_D_EXP(x) != 0x7FFu;
}

/* --- Sign ops -------------------------------------------------------- */

static inline cvm_f64 cvm_d_neg(cvm_f64 x) {
    cvm_f64 r = x; r.hi ^= 0x80000000u; return r;
}
static inline cvm_f64 cvm_d_abs(cvm_f64 x) {
    cvm_f64 r = x; r.hi &= 0x7FFFFFFFu; return r;
}
static inline cvm_f64 cvm_d_copysign(cvm_f64 mag, cvm_f64 sgn) {
    cvm_f64 r = mag;
    r.hi = (r.hi & 0x7FFFFFFFu) | (sgn.hi & 0x80000000u);
    return r;
}

/* --- Comparisons (NaN-unordered) ------------------------------------- */

__attribute__((noinline))
static int cvm_d_eq(cvm_f64 a, cvm_f64 b) {
    if (cvm_d_isnan(a) || cvm_d_isnan(b)) return 0;
    if (cvm_d_iszero(a) && cvm_d_iszero(b)) return 1;   /* +0 == -0 */
    return a.lo == b.lo && a.hi == b.hi;
}

__attribute__((noinline))
static int cvm_d_ne(cvm_f64 a, cvm_f64 b) {
    if (cvm_d_isnan(a) || cvm_d_isnan(b)) return 1;     /* unordered → !=  */
    if (cvm_d_iszero(a) && cvm_d_iszero(b)) return 0;
    return a.lo != b.lo || a.hi != b.hi;
}

__attribute__((noinline))
static int cvm_d_lt(cvm_f64 a, cvm_f64 b) {
    if (cvm_d_isnan(a) || cvm_d_isnan(b)) return 0;
    if (cvm_d_iszero(a) && cvm_d_iszero(b)) return 0;
    uint32_t sa = CVM_D_SIGN(a), sb = CVM_D_SIGN(b);
    if (sa != sb) return sa > sb;       /* negative < positive */
    /* Same sign: positives lex-compare ascending; negatives reverse. */
    if (sa == 0u) {
        if (a.hi != b.hi) return a.hi < b.hi;
        return a.lo < b.lo;
    } else {
        if (a.hi != b.hi) return a.hi > b.hi;
        return a.lo > b.lo;
    }
}

static inline int cvm_d_le(cvm_f64 a, cvm_f64 b) { return cvm_d_lt(a, b) || cvm_d_eq(a, b); }
static inline int cvm_d_gt(cvm_f64 a, cvm_f64 b) { return cvm_d_lt(b, a); }
static inline int cvm_d_ge(cvm_f64 a, cvm_f64 b) { return cvm_d_le(b, a); }

/* --- Conversions: f32 ↔ f64 ------------------------------------------- */

__attribute__((noinline))
static cvm_f64 cvm_d_from_f32(float f) {
    int32_t bits;
    __builtin_memcpy(&bits, &f, sizeof bits);
    uint32_t sign  = (uint32_t)bits >> 31;
    uint32_t exp32 = ((uint32_t)bits >> 23) & 0xFFu;
    uint32_t m32   = (uint32_t)bits & 0x7FFFFFu;
    if (exp32 == 0u) {
        /* Zero or subnormal — flush. */
        return sign ? CVM_D_NEG_ZERO : CVM_D_ZERO;
    }
    if (exp32 == 0xFFu) {
        if (m32 == 0u) return sign ? CVM_D_NEG_INF : CVM_D_INF;
        return CVM_D_NAN;
    }
    /* Normal: rebias, shift mantissa from 23 bits up to 52 bits. */
    uint32_t exp64 = exp32 - 127u + 1023u;
    uint32_t mhi = m32 >> 3;             /* top 20 of 23 → m_hi[19:0] */
    uint32_t mlo = (m32 & 0x7u) << 29;   /* bottom 3   → m_lo[31:29] */
    return cvm_d_pack(sign, exp64, mhi, mlo);
}

__attribute__((noinline))
static float cvm_d_to_f32(cvm_f64 x) {
    uint32_t sign  = CVM_D_SIGN(x);
    uint32_t exp64 = CVM_D_EXP(x);
    uint32_t bits;
    if (exp64 == 0u) {
        bits = sign << 31;                                  /* zero / FTZ */
    } else if (exp64 == 0x7FFu) {
        if (CVM_D_MHI(x) == 0u && x.lo == 0u)
            bits = (sign << 31) | 0x7F800000u;              /* ±Inf */
        else
            bits = 0x7FC00000u;                             /* canonical NaN */
    } else {
        int32_t e32 = (int32_t)exp64 - 1023 + 127;
        if (e32 >= 0xFF) {
            bits = (sign << 31) | 0x7F800000u;              /* overflow → ±Inf */
        } else if (e32 <= 0) {
            bits = sign << 31;                              /* underflow → ±0 */
        } else {
            /* Top 23 bits of 52-bit mantissa = top 20 of MHI shifted left
             * 3, OR'd with top 3 of MLO. Truncate (no rounding). */
            uint32_t m23 = (CVM_D_MHI(x) << 3) | (x.lo >> 29);
            bits = (sign << 31) | ((uint32_t)e32 << 23) | (m23 & 0x7FFFFFu);
        }
    }
    float f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}

/* --- Conversions: int ↔ f64 ------------------------------------------ */

/* Find the index of the most-significant set bit (0..31). UB for x==0;
 * callers check first. Binary search avoids using a CLZ opcode the VM
 * doesn't have, and avoids a loop clang might fold to __builtin_clz. */
static inline int cvm__top_bit_u32(uint32_t x) {
    int n = 0;
    if (x & 0xFFFF0000u) { n += 16; x >>= 16; }
    if (x & 0x0000FF00u) { n +=  8; x >>=  8; }
    if (x & 0x000000F0u) { n +=  4; x >>=  4; }
    if (x & 0x0000000Cu) { n +=  2; x >>=  2; }
    if (x & 0x00000002u) { n +=  1; }
    return n;
}

__attribute__((noinline))
static cvm_f64 cvm_d_from_u32(uint32_t v) {
    if (v == 0u) return CVM_D_ZERO;
    int top = cvm__top_bit_u32(v);          /* 0..31 */
    uint32_t exp = (uint32_t)(1023 + top);
    uint32_t mant_no_lead = v ^ (1u << top);
    /* 52-bit mantissa = mant_no_lead << (52 - top). */
    int shl = 52 - top;
    cvm_i64 m;
    if (shl >= 32) {
        m.lo = 0u;
        m.hi = mant_no_lead << (shl - 32);
    } else if (shl == 0) {
        m.lo = mant_no_lead;
        m.hi = 0u;
    } else {
        m.lo = mant_no_lead << shl;
        m.hi = mant_no_lead >> (32 - shl);
    }
    return cvm_d_pack(0u, exp, m.hi & 0xFFFFFu, m.lo);
}

__attribute__((noinline))
static cvm_f64 cvm_d_from_i32(int32_t v) {
    if (v == 0) return CVM_D_ZERO;
    uint32_t sign;
    uint32_t mag;
    if (v < 0) {
        sign = 1u;
        /* Magnitude via two's-complement; works for INT32_MIN
         * (yields 0x80000000 = 2^31, the correct mathematical
         * absolute value, fits in u32). */
        mag = ~(uint32_t)v + 1u;
    } else {
        sign = 0u;
        mag = (uint32_t)v;
    }
    cvm_f64 r = cvm_d_from_u32(mag);
    if (sign) r.hi |= 0x80000000u;
    return r;
}

__attribute__((noinline))
static int32_t cvm_d_to_i32(cvm_f64 x) {
    if (cvm_d_isnan(x)) return 0;
    uint32_t sign = CVM_D_SIGN(x);
    uint32_t exp  = CVM_D_EXP(x);
    if (exp == 0u) return 0;                               /* ±0 / subnormal → 0 */
    int32_t e = (int32_t)exp - 1023;
    if (e < 0) return 0;                                   /* |x| < 1 */
    if (e >= 31) {
        if (sign) {
            /* x == INT32_MIN exactly: sign=1, exp=158 (e=31), mant=0. */
            if (e == 31 && CVM_D_MHI(x) == 0u && x.lo == 0u) return INT32_MIN;
            return INT32_MIN;                              /* underflow saturate */
        }
        return INT32_MAX;                                  /* overflow saturate */
    }
    /* 53-bit mantissa = (1 << 52) | (mhi << 32) | mlo, then >> (52-e)
     * gives integer part. Always fits in u32 because e <= 30. */
    uint32_t mhi_full = CVM_D_MHI(x) | 0x100000u;          /* implicit 1 at bit 52 */
    int sh = 52 - e;
    uint32_t result;
    if (sh >= 32) {
        result = mhi_full >> (sh - 32);
    } else {
        result = (mhi_full << (32 - sh)) | (x.lo >> sh);
    }
    return sign ? -(int32_t)result : (int32_t)result;
}

__attribute__((noinline))
static uint32_t cvm_d_to_u32(cvm_f64 x) {
    if (cvm_d_isnan(x)) return 0u;
    if (CVM_D_SIGN(x)) return 0u;                          /* negatives → 0 */
    uint32_t exp = CVM_D_EXP(x);
    if (exp == 0u) return 0u;
    int32_t e = (int32_t)exp - 1023;
    if (e < 0) return 0u;
    if (e >= 32) return UINT32_MAX;
    uint32_t mhi_full = CVM_D_MHI(x) | 0x100000u;
    int sh = 52 - e;
    if (sh >= 32) return mhi_full >> (sh - 32);
    return (mhi_full << (32 - sh)) | (x.lo >> sh);
}

/* --- Arithmetic: add / sub ------------------------------------------- */
/* Align mantissas to the larger exponent, add or subtract, renormalise.
 * Truncate rounding (drop bits below position 0 of the aligned mantissa).
 * FTZ on output.
 *
 * Same shape as cvm_d_div below: every 64-bit operation is open-coded
 * on two `uint32_t` halves, and `cvm_i64_add/sub/cvm_u64_shr/shl` are
 * NOT called from this body. Going through the struct helpers was
 * portable enough under clang 22 but inflates the SSA register count
 * by ~10% under clang 18-21 — `cvm_d_add` would then bust the
 * translator's 245-slot SSA register budget. Scalarising drops the
 * function back into the budget and makes the IR shape stable across
 * clang versions. */
__attribute__((noinline))
static cvm_f64 cvm_d_add(cvm_f64 a, cvm_f64 b) {
    if (cvm_d_isnan(a) || cvm_d_isnan(b)) return CVM_D_NAN;

    if (cvm_d_isinf(a)) {
        if (cvm_d_isinf(b) && CVM_D_SIGN(a) != CVM_D_SIGN(b)) return CVM_D_NAN;
        return a;
    }
    if (cvm_d_isinf(b)) return b;

    if (cvm_d_iszero(a)) return b;
    if (cvm_d_iszero(b)) return a;

    /* Order so |a| >= |b|. Compare magnitude bits ignoring the sign. */
    uint32_t a_mag_hi = a.hi & 0x7FFFFFFFu;
    uint32_t b_mag_hi = b.hi & 0x7FFFFFFFu;
    int swap = (a_mag_hi < b_mag_hi) ||
               (a_mag_hi == b_mag_hi && a.lo < b.lo);
    if (swap) { cvm_f64 t = a; a = b; b = t; }

    uint32_t r_sign = CVM_D_SIGN(a);
    int eff_sub = (CVM_D_SIGN(a) != CVM_D_SIGN(b));

    uint32_t a_exp = CVM_D_EXP(a);
    uint32_t b_exp = CVM_D_EXP(b);
    uint32_t a_m_lo = a.lo;
    uint32_t a_m_hi = CVM_D_MHI(a) | 0x100000u;         /* implicit 1 */
    uint32_t b_m_lo = b.lo;
    uint32_t b_m_hi = CVM_D_MHI(b) | 0x100000u;

    /* Align b to a's exponent (a is the larger). Inline 64-bit right
     * shift over the two halves. */
    uint32_t exp_diff = a_exp - b_exp;
    if (exp_diff >= 64u) return a;                       /* b lost in the noise */
    if (exp_diff >= 32u) {
        b_m_lo = b_m_hi >> (exp_diff - 32u);
        b_m_hi = 0u;
    } else if (exp_diff > 0u) {
        b_m_lo = (b_m_lo >> exp_diff) | (b_m_hi << (32u - exp_diff));
        b_m_hi = b_m_hi >> exp_diff;
    }

    uint32_t r_lo, r_hi;
    uint32_t r_exp = a_exp;
    if (eff_sub) {
        /* r = a_m - b_m (two halves with borrow). */
        r_lo = a_m_lo - b_m_lo;
        r_hi = a_m_hi - b_m_hi - (a_m_lo < b_m_lo ? 1u : 0u);
        if (r_lo == 0u && r_hi == 0u) return CVM_D_ZERO;
        /* Renormalise: leading 1 lives somewhere in bits 0..52. Find
         * it and shift left so it lands at bit 52. After subtraction
         * of two 53-bit normalised values, leading is <= 52, so the
         * shift is non-negative. */
        int leading;
        if (r_hi != 0u) leading = 32 + cvm__top_bit_u32(r_hi);
        else            leading =      cvm__top_bit_u32(r_lo);
        int shl = 52 - leading;
        if (shl > 0) {
            uint32_t n = (uint32_t)shl;
            if (n >= 32u) {
                r_hi = r_lo << (n - 32u);
                r_lo = 0u;
            } else {
                r_hi = (r_hi << n) | (r_lo >> (32u - n));
                r_lo = r_lo << n;
            }
            int32_t r_exp_signed = (int32_t)r_exp - shl;
            if (r_exp_signed <= 0) return r_sign ? CVM_D_NEG_ZERO : CVM_D_ZERO;
            r_exp = (uint32_t)r_exp_signed;
        }
    } else {
        /* r = a_m + b_m (two halves with carry). */
        r_lo = a_m_lo + b_m_lo;
        r_hi = a_m_hi + b_m_hi + (r_lo < a_m_lo ? 1u : 0u);
        /* Possible overflow into bit 53. Right-shift by 1 and bump exp. */
        if (r_hi & 0x200000u) {
            r_lo = (r_lo >> 1) | (r_hi << 31);
            r_hi = r_hi >> 1;
            r_exp++;
            if (r_exp >= 0x7FFu) return r_sign ? CVM_D_NEG_INF : CVM_D_INF;
        }
    }

    return cvm_d_pack(r_sign, r_exp, r_hi & 0xFFFFFu, r_lo);
}

static inline cvm_f64 cvm_d_sub(cvm_f64 a, cvm_f64 b) {
    return cvm_d_add(a, cvm_d_neg(b));
}

/* --- Arithmetic: mul ------------------------------------------------- */
/* Schoolbook 53×53 → 106-bit product computed as 4 partial 32×32 products.
 * Truncate rounding, FTZ. */
__attribute__((noinline))
static cvm_f64 cvm_d_mul(cvm_f64 a, cvm_f64 b) {
    if (cvm_d_isnan(a) || cvm_d_isnan(b)) return CVM_D_NAN;

    uint32_t r_sign = CVM_D_SIGN(a) ^ CVM_D_SIGN(b);

    if (cvm_d_isinf(a)) {
        if (cvm_d_iszero(b)) return CVM_D_NAN;
        return r_sign ? CVM_D_NEG_INF : CVM_D_INF;
    }
    if (cvm_d_isinf(b)) {
        if (cvm_d_iszero(a)) return CVM_D_NAN;
        return r_sign ? CVM_D_NEG_INF : CVM_D_INF;
    }
    if (cvm_d_iszero(a) || cvm_d_iszero(b)) {
        return r_sign ? CVM_D_NEG_ZERO : CVM_D_ZERO;
    }

    /* Reconstruct 53-bit mantissas (with implicit leading 1 at bit 52). */
    uint32_t a_mhi_full = CVM_D_MHI(a) | 0x100000u;        /* bits 52..32 */
    uint32_t a_mlo      = a.lo;                            /* bits 31..0  */
    uint32_t b_mhi_full = CVM_D_MHI(b) | 0x100000u;
    uint32_t b_mlo      = b.lo;

    /* 53*53 = 106 bits; compute via four 32x32 partial products and
     * accumulate into w3:w2:w1:w0 (128 bits). */
    uint32_t p0_lo = a_mlo * b_mlo;
    uint32_t p0_hi = cvm_mulhu(a_mlo, b_mlo);
    uint32_t p1_lo = a_mlo * b_mhi_full;
    uint32_t p1_hi = cvm_mulhu(a_mlo, b_mhi_full);
    uint32_t p2_lo = a_mhi_full * b_mlo;
    uint32_t p2_hi = cvm_mulhu(a_mhi_full, b_mlo);
    uint32_t p3_lo = a_mhi_full * b_mhi_full;
    uint32_t p3_hi = cvm_mulhu(a_mhi_full, b_mhi_full);

    /* w0 = p0_lo */
    uint32_t w0 = p0_lo;

    /* w1 = p0_hi + p1_lo + p2_lo + carry_in(0) */
    uint32_t s1a = p0_hi + p1_lo;
    uint32_t c1a = (s1a < p0_hi) ? 1u : 0u;
    uint32_t w1  = s1a + p2_lo;
    uint32_t c1b = (w1  < s1a) ? 1u : 0u;
    uint32_t carry_to_w2 = c1a + c1b;

    /* w2 = p1_hi + p2_hi + p3_lo + carry_to_w2 */
    uint32_t s2a = p1_hi + p2_hi;
    uint32_t c2a = (s2a < p1_hi) ? 1u : 0u;
    uint32_t s2b = s2a + p3_lo;
    uint32_t c2b = (s2b < s2a) ? 1u : 0u;
    uint32_t w2  = s2b + carry_to_w2;
    uint32_t c2c = (w2  < s2b) ? 1u : 0u;
    uint32_t carry_to_w3 = c2a + c2b + c2c;

    /* w3 = p3_hi + carry_to_w3 (no overflow possible: p3_hi <= 0xFFFFFFFD) */
    uint32_t w3 = p3_hi + carry_to_w3;

    /* The 106-bit product lives in bits [0, 106); the mantissas were both
     * in [2^52, 2^53), so the product is in [2^104, 2^106). The leading
     * 1 is at bit 105 if the high bit of w3 (bit 9 of w3 == bit 105 of
     * the 128-bit value) is set, else at bit 104. */
    int extra = ((w3 >> 9) & 1u) ? 1 : 0;

    /* Extract bits [shift, shift+52] as the 53-bit result mantissa. */
    int shift = 52 + extra;                                /* 52 or 53 */
    int s = shift - 32;                                    /* 20 or 21 */
    /* Result top half  = (w3 << (32-s)) | (w2 >> s) */
    /* Result bottom half = (w2 << (32-s)) | (w1 >> s) */
    uint32_t out_hi = (w3 << (32u - (uint32_t)s)) | (w2 >> (uint32_t)s);
    uint32_t out_lo = (w2 << (32u - (uint32_t)s)) | (w1 >> (uint32_t)s);

    int32_t r_exp_signed = (int32_t)CVM_D_EXP(a) + (int32_t)CVM_D_EXP(b)
                         - 1023 + extra;
    if (r_exp_signed >= 0x7FF) return r_sign ? CVM_D_NEG_INF : CVM_D_INF;
    if (r_exp_signed <= 0)     return r_sign ? CVM_D_NEG_ZERO : CVM_D_ZERO;

    return cvm_d_pack(r_sign, (uint32_t)r_exp_signed,
                      out_hi & 0xFFFFFu,                   /* drop implicit 1 */
                      out_lo);
}

/* Apply IEEE 754 round-to-nearest-even to a 54-bit divider quotient.
 *
 * Inputs are the post-loop state of `cvm_d_div` (below). On entry
 * `*q_hi:*q_lo` holds the 54-bit quotient with leading 1 at bit 53,
 * bits 52..1 the significand fraction, bit 0 the guard bit. `r_lo`
 * and `r_hi` carry the residual remainder — used only to derive the
 * sticky bit. On return `*q_hi:*q_lo` is the post-rounded 53-bit
 * significand (leading 1 at bit 52, fraction in bits 51..0) and
 * `*r_exp_signed` may have incremented by one if rounding overflowed.
 *
 * Split out of cvm_d_div as a noinline helper for SSA-pressure
 * reasons — clang's IR for the merged function fits LLVM 22 but
 * crosses the translator's 245-slot pre-allocator ceiling under
 * LLVM 18's inflated IR shape. The CALL+spill across this helper
 * lets the loop-locals (a_m_*, b_m_*, r_*) go dead before the
 * rounding-locals (guard, sticky, lsb, new_lo, carry) come live. */
__attribute__((noinline))
static void cvm_d_div_round_rne(uint32_t *q_hi, uint32_t *q_lo,
                                int32_t *r_exp_signed,
                                uint32_t r_lo, uint32_t r_hi)
{
    uint32_t qh = *q_hi;
    uint32_t ql = *q_lo;

    uint32_t guard  = ql & 1u;
    uint32_t sticky = (r_lo | r_hi) ? 1u : 0u;

    /* Drop guard bit. Significand now leads at bit 52. */
    ql = (ql >> 1) | (qh << 31);
    qh = qh >> 1;

    uint32_t lsb = ql & 1u;
    if (guard && (sticky || lsb)) {
        uint32_t new_lo = ql + 1u;
        uint32_t carry  = (new_lo == 0u) ? 1u : 0u;
        ql = new_lo;
        qh = qh + carry;
        /* Significand overflow into bit 53: shift right and bump
         * exponent. The bit shifted out is always 0 (we just
         * incremented an all-ones significand). */
        if (qh & 0x200000u) {
            ql = (ql >> 1) | ((qh & 1u) << 31);
            qh = qh >> 1;
            *r_exp_signed = *r_exp_signed + 1;
        }
    }

    *q_hi = qh;
    *q_lo = ql;
}

/* --- Division: restoring long-division on 53-bit mantissas ----------- */
__attribute__((noinline))
static cvm_f64 cvm_d_div(cvm_f64 a, cvm_f64 b) {
    if (cvm_d_isnan(a) || cvm_d_isnan(b)) return CVM_D_NAN;

    uint32_t r_sign = CVM_D_SIGN(a) ^ CVM_D_SIGN(b);

    if (cvm_d_isinf(a)) {
        if (cvm_d_isinf(b)) return CVM_D_NAN;
        return r_sign ? CVM_D_NEG_INF : CVM_D_INF;
    }
    if (cvm_d_isinf(b)) {
        return r_sign ? CVM_D_NEG_ZERO : CVM_D_ZERO;
    }
    if (cvm_d_iszero(b)) {
        if (cvm_d_iszero(a)) return CVM_D_NAN;
        return r_sign ? CVM_D_NEG_INF : CVM_D_INF;
    }
    if (cvm_d_iszero(a)) return r_sign ? CVM_D_NEG_ZERO : CVM_D_ZERO;

    /* Reconstruct 53-bit mantissas (implicit leading 1 at bit 52). To
     * keep clang -O1 from folding the struct halves into an i64 load
     * (which the translator rejects), keep the components as separate
     * i32 locals and only assemble cvm_i64 right at the cvm_u64_shl
     * call boundary. */
    uint32_t a_m_lo = a.lo;
    uint32_t a_m_hi = CVM_D_MHI(a) | 0x100000u;
    uint32_t b_m_lo = b.lo;
    uint32_t b_m_hi = CVM_D_MHI(b) | 0x100000u;
    int32_t exp_adjust = 0;

    /* Pre-shift a_m so a_m / b_m ∈ [1, 2). Both inputs are 53-bit
     * normalised, so the ratio is in (1/2, 2) and one left-shift on
     * a_m covers the sub-1 case. */
    int a_lt_b = (a_m_hi != b_m_hi) ? (a_m_hi < b_m_hi)
                                    : (a_m_lo < b_m_lo);
    if (a_lt_b) {
        a_m_hi = (a_m_hi << 1) | (a_m_lo >> 31);
        a_m_lo = a_m_lo << 1;
        exp_adjust = -1;
    }
    /* Now a_m ∈ [b_m, 2*b_m). Restoring long division with R ∈ [0, b_m)
     * (R never overflows). 53 iterations produce a 54-bit quotient: bit
     * 53 is the leading 1 from the pre-loop seed, bits 52..1 form the
     * 53-bit IEEE significand candidate (after shift-right by 1), and
     * bit 0 is the guard bit G for round-to-nearest-even. The final
     * non-zero remainder (if any) supplies the sticky bit S — would-be
     * quotient bits below G that we never materialised. */
    uint32_t r_lo = a_m_lo - b_m_lo;
    uint32_t r_hi = a_m_hi - b_m_hi - (a_m_lo < b_m_lo ? 1u : 0u);
    uint32_t q_lo = 1u;
    uint32_t q_hi = 0u;
    for (int i = 0; i < 53; ++i) {
        /* R << 1 */
        r_hi = (r_hi << 1) | (r_lo >> 31);
        r_lo = r_lo << 1;
        /* q << 1 */
        q_hi = (q_hi << 1) | (q_lo >> 31);
        q_lo = q_lo << 1;
        /* if R >= b_m */
        int r_ge_b = (r_hi != b_m_hi) ? (r_hi > b_m_hi)
                                      : (r_lo >= b_m_lo);
        if (r_ge_b) {
            uint32_t new_lo = r_lo - b_m_lo;
            uint32_t borrow = (r_lo < b_m_lo) ? 1u : 0u;
            r_hi = r_hi - b_m_hi - borrow;
            r_lo = new_lo;
            q_lo |= 1u;
        }
    }

    int32_t r_exp_signed = (int32_t)CVM_D_EXP(a) - (int32_t)CVM_D_EXP(b)
                         + 1023 + exp_adjust;

    /* Hand the post-loop state off to the rounding helper. The CALL
     * lets the loop locals (a_m_*, b_m_*) and r_lo/r_hi go dead
     * after the helper returns — they're spilled across the call
     * via the standard liveness path but don't need permanent regs
     * any more, which keeps cvm_d_div under the translator's 245-
     * slot pre-allocator ceiling on LLVM 18's slightly inflated IR. */
    cvm_d_div_round_rne(&q_hi, &q_lo, &r_exp_signed, r_lo, r_hi);

    if (r_exp_signed >= 0x7FF) return r_sign ? CVM_D_NEG_INF : CVM_D_INF;
    if (r_exp_signed <= 0)     return r_sign ? CVM_D_NEG_ZERO : CVM_D_ZERO;

    return cvm_d_pack(r_sign, (uint32_t)r_exp_signed,
                      q_hi & 0xFFFFFu, q_lo);
}

#endif /* CVM_FLOAT64_H */
