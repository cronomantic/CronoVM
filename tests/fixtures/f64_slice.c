/* f64-legalisation derisk slice. Uses NATIVE `double`: the translator must
 * legalise each op to a soft-float runtime call (fadd/fsub/fmul/fdiv/fcmp/
 * conversions) or an inline word op (fneg). Returns 0 on success, else a
 * small code identifying the first failed check.
 *
 * Every value is kept n-neutral via `+ (double)n - (double)n`: that pair of
 * fadd/fsub is value-preserving but NOT foldable under default (non-fast)
 * FP semantics, so clang can't constant-fold the arithmetic away — the ops
 * really reach codegen — yet the result is independent of n, so a single
 * `test_e2e f64_slice.bin 0 <n>` check works for any seed.
 *
 * All constants here are exactly representable in binary64, so the equality
 * checks are bit-exact.
 *
 *   a = 0; b = a+1.5 = 1.5; c = b*2 = 3.0; d = c-a = 3.0; e = d/4 = 0.75
 *   (int)(e*100) = 75; -e = -0.75; sqrt-free, all exact.
 */
int f64_slice_main(int n) {
    double z  = (double)n - (double)n;          /* fsub -> 0.0, n-neutral */
    double a  = (double)n + z - (double)n;      /* sitofp + fadd + fsub -> 0 */

    double b  = a + 1.5;                         /* fadd const */
    if (!(b == 1.5)) return 1;                   /* fcmp oeq */

    double c  = b * 2.0;                         /* fmul */
    if (c != 3.0) return 2;                      /* fcmp one */

    double d  = c - a;                           /* fsub */
    if (d <= b) return 3;                         /* fcmp ole (3.0 <= 1.5 false) */
    if (!(d > b)) return 4;                       /* fcmp ogt */

    double e  = d / 4.0;                          /* fdiv -> 0.75 */
    if (!(e < b)) return 5;                        /* fcmp olt */
    if (e >= b)  return 6;                         /* fcmp oge */

    double f  = -e;                               /* fneg (inline) -> -0.75 */
    if (!(f < e)) return 7;
    if (f + e != 0.0) return 8;                   /* -0.75 + 0.75 == 0 */

    int  r  = (int)(e * 100.0);                   /* fmul + fptosi */
    if (r != 75) return 9;

    unsigned u = (unsigned)(e * 100.0);           /* fptoui */
    if (u != 75u) return 10;

    double g = (double)r;                         /* sitofp 75 */
    if (g != 75.0) return 11;

    return 0;
}
