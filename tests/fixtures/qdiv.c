#include "cvm_intrin.h"
#include <stdint.h>

/* Exercises the QDIV1616 opcode (cvm_qdiv_16_16) end-to-end:
 *   result = ((uint64_t)a << 16) / b, unsigned, truncating toward zero.
 * Each phase uses inputs whose 48-bit numerator has a known quotient so the
 * test catches a truncated numerator or a wrong shift. Returns 0 on success,
 * otherwise a non-zero code identifying which phase regressed. */
int qdiv_main(int n) {
    (void)n;

    /* Phase 1 — Q16.16 divide: 3.0 (0x00030000) / 2.0 (0x00020000) = 1.5
     * (0x00018000). ((3<<16)<<16)/(2<<16) = (3<<16)/2 = 0x18000. */
    if (cvm_qdiv_16_16(0x00030000u, 0x00020000u) != 0x00018000u) return 1;

    /* Phase 2 — exact 64-bit numerator: (0x10000 << 16) / 3 = 2^32 / 3 =
     * 0x55555555 (1431655765, since 1431655765*3 = 2^32 - 1). Proves the
     * full 64-bit dividend, not a 32-bit one (which would divide 0/3 = 0). */
    if (cvm_qdiv_16_16(0x00010000u, 3u) != 0x55555555u) return 2;

    /* Phase 3 — numerator that overflows 32 bits: (0xFFFFFFFF << 16) /
     * 0xFFFFFFFF = 0x10000. A 32-bit-only numerator would mangle this. */
    if (cvm_qdiv_16_16(0xFFFFFFFFu, 0xFFFFFFFFu) != 0x00010000u) return 3;

    /* Phase 4 — identity: x / 1.0 in Q16.16 returns x. (1 << 16) / 0x10000 = 1. */
    if (cvm_qdiv_16_16(1u, 0x00010000u) != 1u) return 4;

    /* Phase 5 — truncation toward zero: (5 << 16) / 0x30000 = (5<<16)/3
     * = 0x50000/3 = 0x1AAAA (109226, since 109226*3 = 327678 < 327680). */
    if (cvm_qdiv_16_16(0x00050000u, 0x00030000u) != 0x0001AAAAu) return 5;

    return 0;
}
