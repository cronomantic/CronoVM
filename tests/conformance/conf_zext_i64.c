/* Differential conformance slice: NARROW zext/sext to i64.
 *
 * Guards the fix for a translator miscompile found by the differential corpus
 * (tools/cvm-fuzz): `zext iN->i64` / `sext iN->i64` (N<32) wrote the i64 LOW word
 * straight from the source register WITHOUT masking it to N bits. A narrow value
 * can carry garbage above bit N-1 (the kept-zero/sign-extended invariant only
 * holds straight out of a load or trunc — and the translator materialises a
 * negative narrow constant sign-extended, so a prior `xor iN` with such a
 * constant leaves 1s in the high bits). That garbage landed in the i64 low word
 * and, in a subsequent `add i64`, fabricated a spurious carry into the high word
 * (off-by-one) — while the low word, masked later, looked correct, hiding it.
 * This whole class was a SINGLE root cause behind ~10 corpus seeds.
 *
 * To exercise it at the conformance -O1 build level: the narrow value flows
 * IN-REGISTER (no store/reload to mask it) straight into `(uint64_t)`/`(int64_t)`
 * and then an i64 add whose other operand has a non-zero high word, so the carry
 * is observed. The xor constants are chosen negative-as-iN to force the garbage.
 *
 * UB-free: all sums are unsigned (signed values only as sign-extending casts,
 * reinterpreted before the add); int32 FNV checksum, fixed-width. */
#include <stdint.h>

int conf_main(void) {
    static const uint64_t W[6] = {
        UINT64_C(0x57DB60C68D497273), UINT64_C(0x123456789ABCDEF0),
        UINT64_C(0xFFFFFFFF00000001), UINT64_C(0x00000001FFFFFFFF),
        UINT64_C(0x80000000FFFF0001), UINT64_C(0x0F0F0F0FF0F0F0F0),
    };
    static const uint32_t X[6] = {
        59931u, 0x0000DEADu, 0x00008001u, 12345u, 0x0000FFFFu, 0x000000ABu,
    };
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(v)) * 16777619u; } while (0)

    for (int k = 0; k < 6; k++) {
        volatile uint32_t vx = X[k];
        volatile uint64_t vw = W[k];
        /* narrow values with garbage-prone high bits (xor with negative-as-iN
         * constants) flowing in-register into the i64 widenings. */
        uint16_t n16 = (uint16_t)(vx ^ 0xEA1Bu);   /* 0xEA1B < 0 as int16_t */
        uint8_t  n8  = (uint8_t)(vx ^ 0x80u);      /* 0x80   < 0 as int8_t  */

        uint64_t zu16 = vw + (uint64_t)n16;                 /* zext i16->i64 */
        uint64_t zu8  = vw + (uint64_t)n8;                  /* zext i8 ->i64 */
        uint64_t ss16 = vw + (uint64_t)(int64_t)(int16_t)n16; /* sext i16->i64 */
        uint64_t ss8  = vw + (uint64_t)(int64_t)(int8_t)n8;   /* sext i8 ->i64 */

        MIX((uint32_t)zu16); MIX((uint32_t)(zu16 >> 32));
        MIX((uint32_t)zu8);  MIX((uint32_t)(zu8  >> 32));
        MIX((uint32_t)ss16); MIX((uint32_t)(ss16 >> 32));
        MIX((uint32_t)ss8);  MIX((uint32_t)(ss8  >> 32));
    }
    #undef MIX
    return (int)h;
}
