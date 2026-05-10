#include <stdint.h>
#include "cvm_int64.h"

/* cvm_int64.h end-to-end. Returns 0 on success, otherwise a small
 * non-zero code identifying which check failed. `n` is the R0 seed
 * (always 0 from `test_e2e` here). We thread it through every input
 * so clang -O1 can't fold the calls away.
 *
 * Each phase exercises one cluster of behaviour so a regression points
 * at a specific section. */

int i64_basic_main(int n) {
    uint32_t z = (uint32_t)n;                  /* opaque zero */

    /* Phase 1 — construction + truncation round trip. */
    {
        cvm_i64 a = cvm_i64_from_i32(42 + (int32_t)z);
        if (a.lo != 42u)               return 1;
        if (a.hi != 0u)                return 2;
        if (cvm_i64_to_i32(a) != 42)   return 3;

        cvm_i64 neg = cvm_i64_from_i32(-1 - (int32_t)z);
        if (neg.lo != 0xFFFFFFFFu)     return 4;
        if (neg.hi != 0xFFFFFFFFu)     return 5;          /* sign-extended */
        if (cvm_i64_to_i32(neg) != -1) return 6;

        cvm_i64 from_u = cvm_i64_from_u32(0xDEADBEEFu + z);
        if (from_u.lo != 0xDEADBEEFu)  return 7;
        if (from_u.hi != 0u)           return 8;          /* zero-extended */

        cvm_i64 parts = cvm_i64_from_parts(0x12345678u + z, 0x9ABCDEF0u);
        if (parts.lo != 0x12345678u)   return 9;
        if (parts.hi != 0x9ABCDEF0u)   return 10;
    }

    /* Phase 2 — add, with and without carry across the 32-bit boundary. */
    {
        cvm_i64 a = cvm_i64_from_parts(0x10u + z, 0u);
        cvm_i64 b = cvm_i64_from_parts(0x20u, 0u);
        cvm_i64 r = cvm_i64_add(a, b);
        if (r.lo != 0x30u)             return 20;
        if (r.hi != 0u)                return 21;

        /* Carry into hi. */
        a = cvm_i64_from_parts(0xFFFFFFFFu + z, 0u);
        b = cvm_i64_from_parts(1u, 0u);
        r = cvm_i64_add(a, b);
        if (r.lo != 0u)                return 22;
        if (r.hi != 1u)                return 23;

        /* Carry plus existing hi. */
        a = cvm_i64_from_parts(0xFFFFFFFFu + z, 5u);
        b = cvm_i64_from_parts(2u, 3u);
        r = cvm_i64_add(a, b);
        if (r.lo != 1u)                return 24;
        if (r.hi != 9u)                return 25;
    }

    /* Phase 3 — sub, with and without borrow. */
    {
        cvm_i64 a = cvm_i64_from_parts(0x100u + z, 5u);
        cvm_i64 b = cvm_i64_from_parts(0x040u, 2u);
        cvm_i64 r = cvm_i64_sub(a, b);
        if (r.lo != 0x0C0u)            return 30;
        if (r.hi != 3u)                return 31;

        /* Borrow from hi. */
        a = cvm_i64_from_parts(0u + z, 5u);
        b = cvm_i64_from_parts(1u, 2u);
        r = cvm_i64_sub(a, b);
        if (r.lo != 0xFFFFFFFFu)       return 32;
        if (r.hi != 2u)                return 33;
    }

    /* Phase 4 — negation. */
    {
        cvm_i64 a = cvm_i64_from_i32(7 + (int32_t)z);
        cvm_i64 r = cvm_i64_neg(a);
        if (cvm_i64_to_i32(r) != -7)   return 40;
        /* -7 in 64-bit is 0xFFFFFFFFFFFFFFF9 */
        if (r.lo != 0xFFFFFFF9u)       return 41;
        if (r.hi != 0xFFFFFFFFu)       return 42;

        /* neg(neg(x)) == x */
        cvm_i64 rr = cvm_i64_neg(r);
        if (rr.lo != 7u || rr.hi != 0u) return 43;

        /* neg(0) == 0 */
        cvm_i64 zero = cvm_i64_from_i32(0 + (int32_t)z);
        cvm_i64 nz = cvm_i64_neg(zero);
        if (nz.lo != 0u || nz.hi != 0u) return 44;
    }

    /* Phase 5 — multiplication. The cross-product paths (lo*hi terms)
     * exercise MULHU; verify a known full 64-bit result. */
    {
        /* 0x100000000 * 1 == 0x100000000 (just the hi half) */
        cvm_i64 a = cvm_i64_from_parts(0u + z, 1u);   /* 2^32 */
        cvm_i64 b = cvm_i64_from_i32(1 + (int32_t)z);
        cvm_i64 r = cvm_u64_mul(a, b);
        if (r.lo != 0u)                return 50;
        if (r.hi != 1u)                return 51;

        /* 100000 * 100000 = 10000000000 = 0x2_540BE400 */
        a = cvm_i64_from_u32(100000u + z);
        b = cvm_i64_from_u32(100000u);
        r = cvm_u64_mul(a, b);
        if (r.lo != 0x540BE400u)       return 52;
        if (r.hi != 2u)                return 53;

        /* (2^32 + 5) * 7 = 7*2^32 + 35 */
        a = cvm_i64_from_parts(5u + z, 1u);
        b = cvm_i64_from_u32(7u);
        r = cvm_u64_mul(a, b);
        if (r.lo != 35u)               return 54;
        if (r.hi != 7u)                return 55;

        /* (2^32 + 0xFFFFFFFF) * 2 = 0x2_FFFFFFFE — exercises carry from
         * lo*lo into the cross terms. */
        a = cvm_i64_from_parts(0xFFFFFFFFu + z, 1u);
        b = cvm_i64_from_u32(2u);
        r = cvm_u64_mul(a, b);
        if (r.lo != 0xFFFFFFFEu)       return 56;
        if (r.hi != 3u)                return 57;
    }

    /* Phase 6 — shifts. */
    {
        cvm_i64 a = cvm_i64_from_parts(0x12345678u + z, 0x9ABCDEF0u);

        /* shl 0 == identity */
        cvm_i64 r = cvm_u64_shl(a, 0u);
        if (r.lo != a.lo || r.hi != a.hi) return 60;

        /* shl 4 — within low half. */
        r = cvm_u64_shl(a, 4u);
        if (r.lo != 0x23456780u)       return 61;
        if (r.hi != 0xABCDEF01u)       return 62;          /* top 4 of lo into hi */

        /* shl 32 — entire lo moves to hi. */
        r = cvm_u64_shl(a, 32u);
        if (r.lo != 0u)                return 63;
        if (r.hi != 0x12345678u)       return 64;

        /* shl 33 — same but plus extra shift. */
        r = cvm_u64_shl(a, 33u);
        if (r.lo != 0u)                return 65;
        if (r.hi != 0x2468ACF0u)       return 66;

        /* shr 4 — within hi half down. */
        r = cvm_u64_shr(a, 4u);
        if (r.lo != 0x01234567u)       return 67;
        if (r.hi != 0x09ABCDEFu)       return 68;          /* original hi >> 4 */
        /* lo gets bottom-4 of original hi mixed in: */
        /* expected lo = (a.lo >> 4) | (a.hi << 28) */
        uint32_t expected_lo = (0x12345678u >> 4) | (0x9ABCDEF0u << 28);
        if (r.lo != expected_lo)       return 69;

        /* shr 32 — hi moves to lo, hi cleared. */
        r = cvm_u64_shr(a, 32u);
        if (r.lo != 0x9ABCDEF0u)       return 70;
        if (r.hi != 0u)                return 71;

        /* sar 4 on negative — sign-fills. */
        cvm_i64 neg = cvm_i64_from_i32(-256 + (int32_t)z); /* 0xFFFFFFFFFFFFFF00 */
        r = cvm_i64_sar(neg, 4u);                          /* expect 0xFFFFFFFFFFFFFFF0 */
        if (r.lo != 0xFFFFFFF0u)       return 72;
        if (r.hi != 0xFFFFFFFFu)       return 73;

        /* sar 32 on negative — hi rotates into lo, sign-fill at top. */
        r = cvm_i64_sar(neg, 32u);
        if (r.lo != 0xFFFFFFFFu)       return 74;          /* original hi */
        if (r.hi != 0xFFFFFFFFu)       return 75;          /* sign-fill */
    }

    /* Phase 7 — comparisons (signed and unsigned). */
    {
        cvm_i64 a = cvm_i64_from_parts(5u + z, 0u);
        cvm_i64 b = cvm_i64_from_parts(5u, 0u);
        cvm_i64 c = cvm_i64_from_parts(0u, 1u);            /* 2^32 */

        if (!cvm_i64_eq(a, b))         return 80;
        if (cvm_i64_ne(a, b))          return 81;
        if (!cvm_u64_lt(a, c))         return 82;          /* 5 < 2^32 */
        if (cvm_u64_lt(c, a))          return 83;
        if (!cvm_u64_le(a, b))         return 84;
        if (!cvm_u64_le(a, c))         return 85;

        /* Signed: -1 < 1 always (lex would say no since hi=0xFFF... > 0) */
        cvm_i64 neg1 = cvm_i64_from_i32(-1 - (int32_t)z);  /* {lo,hi}={-1,-1} */
        cvm_i64 pos1 = cvm_i64_from_i32(1 + (int32_t)z);
        if (!cvm_i64_lt(neg1, pos1))   return 86;
        if (cvm_u64_lt(neg1, pos1))    return 87;          /* unsigned: -1 > 1 */
        if (!cvm_u64_lt(pos1, neg1))   return 88;
    }

    /* Phase 8 — bitwise. */
    {
        cvm_i64 a = cvm_i64_from_parts(0xFF00FF00u + z, 0x0F0F0F0Fu);
        cvm_i64 b = cvm_i64_from_parts(0x0FF00FF0u, 0xF0F0F0F0u);

        cvm_i64 r;
        r = cvm_i64_and(a, b);
        if (r.lo != (0xFF00FF00u & 0x0FF00FF0u)) return 90;
        if (r.hi != (0x0F0F0F0Fu & 0xF0F0F0F0u)) return 91;

        r = cvm_i64_or (a, b);
        if (r.lo != (0xFF00FF00u | 0x0FF00FF0u)) return 92;
        if (r.hi != (0x0F0F0F0Fu | 0xF0F0F0F0u)) return 93;

        r = cvm_i64_xor(a, b);
        if (r.lo != (0xFF00FF00u ^ 0x0FF00FF0u)) return 94;
        if (r.hi != (0x0F0F0F0Fu ^ 0xF0F0F0F0u)) return 95;

        r = cvm_i64_not(a);
        if (r.lo != ~0xFF00FF00u)      return 96;
        if (r.hi != ~0x0F0F0F0Fu)      return 97;
    }

    return 0;
}
