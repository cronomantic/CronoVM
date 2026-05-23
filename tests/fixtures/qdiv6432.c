#include "cvm_intrin.h"
#include <stdint.h>

/* Exercises the QDIV6432 opcode (cvm_qdiv_64_32) end-to-end:
 *   result = (uint32_t)((((uint64_t)hi << 32) | lo) / divisor)
 * The dividend-high is tied to the destination register, so each phase also
 * checks the translator's MOV/QDIV6432/MOV staging didn't drop or alias an
 * operand. Returns 0 on success, else a code identifying the phase. */
int qdiv6432_main(int n) {
    (void)n;

    /* Phase 1 — low half only: 0x100 / 2 = 0x80. */
    if (cvm_qdiv_64_32(0u, 0x100u, 2u) != 0x80u) return 1;

    /* Phase 2 — high half contributes: (1 << 32) / 2 = 2^31 = 0x80000000.
     * A 32-bit-only numerator would divide 0/2 = 0. */
    if (cvm_qdiv_64_32(1u, 0u, 2u) != 0x80000000u) return 2;

    /* Phase 3 — low / divisor with no high part: 0xFFFFFFFF / 0xFFFFFFFF = 1. */
    if (cvm_qdiv_64_32(0u, 0xFFFFFFFFu, 0xFFFFFFFFu) != 1u) return 3;

    /* Phase 4 — full 64-bit numerator: (2 << 32) / 3 = 0x2_00000000 / 3 =
     * 0xAAAAAAAA (2863311530, since 0xAAAAAAAA*3 = 0x1FFFFFFFE, rem 2). */
    if (cvm_qdiv_64_32(2u, 0u, 3u) != 0xAAAAAAAAu) return 4;

    /* Phase 5 — identity: x / 1 = x. */
    if (cvm_qdiv_64_32(0u, 7u, 1u) != 7u) return 5;

    /* Phase 6 — mixed 64-bit numerator, divisor 2^16, with a quotient that
     * overflows 32 bits so the result is truncated to its low 32 bits:
     * 0x123456789ABCDEF0 / 0x10000 = 0x123456789ABC, (u32) = 0x56789ABC. */
    if (cvm_qdiv_64_32(0x12345678u, 0x9ABCDEF0u, 0x10000u) != 0x56789ABCu) return 6;

    return 0;
}
