/* llvm.ctpop.i32 — population count. clang folds power-of-two tests like
 * `x & (x-1)` into a ctpop comparison (e.g. DOOM's texture-height pow2 check in
 * R_DrawColumn), so the translator must lower llvm.ctpop even when the source
 * never calls __builtin_popcount directly. Lowered with Kernighan's loop.
 *
 * ctpop_main(183): popcount(0xB7 = 1011_0111) = 6.
 */
int ctpop_main(int n)
{
    return __builtin_popcount((unsigned)n);
}
