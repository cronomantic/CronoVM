#include "cvm_intrin.h"
#include <stdint.h>

/* Exercises MULH / MULHU end-to-end. Each phase uses inputs whose
 * 32x32→64 product has known low and high halves so the test catches
 * silent precision loss. Returns 0 on success, otherwise a non-zero
 * code identifying which phase regressed. */
int mulh_main(int n) {
    (void)n;

    /* Phase 1 — signed: 100000 * 100000 = 10_000_000_000 = 0x2_540B_E400.
     * Low 32 = 0x540B_E400, high 32 = 0x2. cvm_mulh must report 2. */
    int32_t a = 100000, b = 100000;
    int32_t lo  = (int32_t)((uint32_t)a * (uint32_t)b);
    int32_t hi  = cvm_mulh(a, b);
    if (lo != (int32_t)0x540BE400) return 1;
    if (hi != 2)                   return 2;

    /* Phase 2 — signed negative: -1 * -1 = 1. The unsigned u32x u32 of
     * 0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE_00000001, but the SIGNED
     * product is +1 which is 0x00000000_00000001, so MULH must yield 0,
     * MULHU 0xFFFFFFFE. Distinguishes the two opcodes. */
    int32_t s_hi = cvm_mulh(-1, -1);
    if (s_hi != 0) return 3;
    uint32_t u_hi = cvm_mulhu(0xFFFFFFFFu, 0xFFFFFFFFu);
    if (u_hi != 0xFFFFFFFEu) return 4;

    /* Phase 3 — Q16.16 multiply: 1.5 (0x00018000) × 2.0 (0x00020000)
     * should give 3.0 (0x00030000). The wrapper composes one MUL and
     * one MULH plus two shifts. */
    int32_t q = cvm_qmul_16_16(0x00018000, 0x00020000);
    if (q != 0x00030000) return 5;

    /* Phase 4 — Q16.16 with negatives: -1.5 × 2.0 = -3.0 (0xFFFD0000). */
    int32_t qn = cvm_qmul_16_16(-0x00018000, 0x00020000);
    if (qn != (int32_t)0xFFFD0000) return 6;

    return 0;
}
