/* CronoVM soft int64 runtime — linked counterpart to the translator's i64
 * legaliser, for the ops too large to open-code.
 *
 * Most i64 operations (add/sub/mul/and/or/xor/shifts/cmp/sext/zext/trunc) are
 * lowered inline by the translator. Only 64-bit DIVIDE and REMAINDER are
 * lowered to a CALL into one of the `__cvm_*div64`/`__cvm_*mod64` functions
 * below — a 64/64 division is a ~64-iteration loop, not worth inlining at
 * every use. cvm-cc auto-links this TU into any module that uses i64 div/rem
 * (detected via `cvm-translate --probe-runtime`).
 *
 * ABI matches what clang's i386-elf lowering produces for a `cvm_i64`
 * argument/return — two i32 words per arg in (lo,hi) order, an `sret` hidden
 * pointer for the 64-bit return — which the translator already handles and the
 * legaliser emits the matching CALL for (the same path as the f64 helpers).
 *
 * Divide-by-zero: C division by zero is UB, and the VM's 32-bit DIV opcode
 * traps. The soft 64-bit path can't trap from inside a helper, so it is
 * DEFINED here as the natural fall-out of the long-division loop with b==0:
 * the quotient becomes all-ones (~0) and the remainder becomes the dividend.
 * Callers must not divide by zero (matching C's contract). */

#include "cvm_int64.h"

/* Unsigned 64/64 long division. Returns the quotient; writes the remainder
 * through *rem. Restoring division, MSB-first, 64 iterations. */
static cvm_i64 cvm__udivmod64(cvm_i64 a, cvm_i64 b, cvm_i64 *rem) {
    cvm_i64 q = CVM_I64_ZERO;
    cvm_i64 r = CVM_I64_ZERO;
    for (int i = 63; i >= 0; --i) {
        /* r = (r << 1) | bit i of a */
        r = cvm_u64_shl(r, 1u);
        uint32_t bit = (i >= 32) ? ((a.hi >> (i - 32)) & 1u)
                                 : ((a.lo >> i) & 1u);
        r.lo |= bit;
        /* if b <= r: r -= b; set bit i of q */
        if (cvm_u64_le(b, r)) {
            r = cvm_i64_sub(r, b);
            if (i >= 32) q.hi |= (1u << (i - 32));
            else         q.lo |= (1u << i);
        }
    }
    *rem = r;
    return q;
}

/* |x|, recording whether x was negative. INT64_MIN maps to itself (its
 * 2's-complement negation), whose unsigned value is the correct magnitude. */
static cvm_i64 cvm__i64_abs(cvm_i64 x, int *neg) {
    *neg = (int)((x.hi >> 31) & 1u);
    return *neg ? cvm_i64_neg(x) : x;
}

/* --- unsigned ---------------------------------------------------------- */
cvm_i64 __cvm_udiv64(cvm_i64 a, cvm_i64 b) {
    cvm_i64 r;
    return cvm__udivmod64(a, b, &r);
}
cvm_i64 __cvm_umod64(cvm_i64 a, cvm_i64 b) {
    cvm_i64 r;
    cvm__udivmod64(a, b, &r);
    return r;
}

/* --- signed (truncating toward zero; rem takes the dividend's sign) ---- */
cvm_i64 __cvm_sdiv64(cvm_i64 a, cvm_i64 b) {
    int na, nb;
    cvm_i64 ua = cvm__i64_abs(a, &na);
    cvm_i64 ub = cvm__i64_abs(b, &nb);
    cvm_i64 r;
    cvm_i64 q = cvm__udivmod64(ua, ub, &r);
    return (na ^ nb) ? cvm_i64_neg(q) : q;
}
cvm_i64 __cvm_smod64(cvm_i64 a, cvm_i64 b) {
    int na, nb;
    cvm_i64 ua = cvm__i64_abs(a, &na);
    cvm_i64 ub = cvm__i64_abs(b, &nb);
    cvm_i64 r;
    cvm__udivmod64(ua, ub, &r);
    return na ? cvm_i64_neg(r) : r;   /* remainder sign follows the dividend */
}

/* --- variable-amount shifts -------------------------------------------- *
 * The translator lowers a CONSTANT i64 shift inline; a variable amount goes
 * through these. The amount is passed as a cvm_i64 (LLVM shift operands share
 * the value's type) — only its low word matters (shifts are mod 64). */
cvm_i64 __cvm_shl64(cvm_i64 x, cvm_i64 n) { return cvm_u64_shl(x, n.lo); }
cvm_i64 __cvm_shr64(cvm_i64 x, cvm_i64 n) { return cvm_u64_shr(x, n.lo); }
cvm_i64 __cvm_sar64(cvm_i64 x, cvm_i64 n) { return cvm_i64_sar(x, n.lo); }
