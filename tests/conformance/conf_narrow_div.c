/* Differential conformance slice: NARROW integer division / remainder.
 *
 * Guards the fix for a translator miscompile found by the differential corpus
 * (tools/cvm-fuzz): a narrow `udiv`/`urem`/`sdiv`/`srem iN` (N<32) was lowered to
 * the VM's 32-bit DIV/MOD without first normalising its operands to N bits. A
 * narrow value can carry garbage above bit N-1 (the kept-zero/sign-extended
 * invariant only holds straight out of a load or trunc), and — unlike
 * add/mul/and/xor — division and remainder depend on the FULL operand, so the
 * result was wrong. clang narrows a 32-bit divide to `udiv iN` at -O2 once it
 * proves the operands fit (`(uint16_t)((uint32_t)a / ((uint32_t)b|1)) -> udiv
 * i16`); `_BitInt(N)` forces the same narrow op already at -O1 (the conformance
 * build level), and the operands here come from `trunc`s of wider volatile-seeded
 * values, so their register high bits are non-zero — the exact trigger.
 *
 * UB-free: unsigned divisors forced nonzero with `| 1`; signed divisors masked
 * into [1, 2^(N-1)-1] (positive, nonzero) so no INT_MIN/-1 overflow; signed
 * dividends are reinterpreted bit patterns (implementation-defined, consistent).
 * int32 FNV-1a checksum, fixed-width — host-64 and VM-32 agree bit-for-bit. */
#include <stdint.h>

int conf_main(void) {
    static const uint32_t SEED[8] = {
        0x22E2F8B7u, 0x9C3A17E5u, 0x0F1E2D3Cu, 0xDEADBEEFu,
        0x13579BDFu, 0xFEDCBA98u, 0xA5A5A5A5u, 0x0BADF00Du,
    };
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    for (int k = 0; k < 8; k++) {
        volatile uint32_t vx = SEED[k];
        volatile uint32_t vy = SEED[(k * 3 + 1) & 7];

        /* unsigned narrow div/rem — operands trunc'd from wide (garbage high). */
        unsigned _BitInt(8)  ua8 = (unsigned _BitInt(8))(vx ^ 0xABCDu);
        unsigned _BitInt(8)  ub8 = (unsigned _BitInt(8))(vy ^ 0x1234u);
        MIX((uint32_t)(ua8 / (ub8 | (unsigned _BitInt(8))1)));
        MIX((uint32_t)(ua8 % (ub8 | (unsigned _BitInt(8))1)));

        unsigned _BitInt(16) ua16 = (unsigned _BitInt(16))(vx ^ 0xB590u);
        unsigned _BitInt(16) ub16 = (unsigned _BitInt(16))(vy ^ 0x3278u);
        MIX((uint32_t)(ua16 / (ub16 | (unsigned _BitInt(16))1)));
        MIX((uint32_t)(ua16 % (ub16 | (unsigned _BitInt(16))1)));

        /* signed narrow div/rem — divisor masked positive in [1, 2^(N-1)-1]. */
        signed _BitInt(8)  sa8 = (signed _BitInt(8))(vx ^ 0x55AAu);
        signed _BitInt(8)  sd8 = (signed _BitInt(8))((vy & 0x3Fu) | 1u);
        MIX((uint32_t)(uint8_t)(sa8 / sd8));
        MIX((uint32_t)(uint8_t)(sa8 % sd8));

        signed _BitInt(16) sa16 = (signed _BitInt(16))(vx ^ 0x9E37u);
        signed _BitInt(16) sd16 = (signed _BitInt(16))((vy & 0x3FFFu) | 1u);
        MIX((uint32_t)(uint16_t)(sa16 / sd16));
        MIX((uint32_t)(uint16_t)(sa16 % sd16));

        /* the plain-C pattern clang narrows to udiv iN at -O2 (corpus level). */
        uint16_t p = (uint16_t)(vx ^ 0x4D27u);
        uint16_t q = (uint16_t)(vy ^ 0xCACFu);
        MIX((uint16_t)((uint32_t)p / ((uint32_t)q | 1u)));
        MIX((uint16_t)((uint32_t)p % ((uint32_t)q | 1u)));
    }
    #undef MIX
    return (int)h;
}
