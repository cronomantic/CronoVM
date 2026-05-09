/* Force the translator to relax a conditional branch.
 *
 * Strategy: a forward goto over a huge volatile-arithmetic body to a
 * block placed at the *end* of the function. Source layout is:
 *
 *     if (x == 0) goto end_short;
 *     ...long arithmetic body...
 *     return r;
 *   end_short:
 *     return 999;
 *
 * Clang preserves source order for the basic block list, so layout is
 * [entry, body, end_short]. Entry's `br cond, end_short, body` lowers
 * to `BNE cond, zero, +K` where K spans the entire body — well past
 * imm8 reach. Without `cg_relax_branches`, the translator errors out
 * with "branch offset out of range". With it, the BNE is rewritten to
 * `BEQ +1; JMP end_short` (imm24) and the binary translates cleanly.
 *
 * Each `r OP= K` lowers to LOAD + MOVI/MOVHI + binop + STORE
 * (5 instructions) thanks to the volatile qualifier; ~30 lines × 5 ≈
 * 150 emitted instructions in the body block. */

int vm_main(int x) {
    if (x == 0) goto end_short;

    volatile int r = x;
    r ^= 0x10001; r += 0x20002;
    r ^= 0x30003; r += 0x40004;
    r ^= 0x50005; r += 0x60006;
    r ^= 0x70007; r += 0x80008;
    r ^= 0x90009; r += 0xA000A;
    r ^= 0xB000B; r += 0xC000C;
    r ^= 0xD000D; r += 0xE000E;
    r ^= 0xF000F; r += 0x11111;
    r ^= 0x22222; r += 0x33333;
    r ^= 0x44444; r += 0x55555;
    r ^= 0x66666; r += 0x77777;
    r ^= 0x88888; r += 0x99999;
    r ^= 0xAAAAA; r += 0xBBBBB;
    r ^= 0xCCCCC; r += 0xDDDDD;
    return r;

end_short:
    return 999;
}
