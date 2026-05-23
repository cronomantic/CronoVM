/* f64 intrinsic coverage: clang's default -ffp-contract=on contracts `a*b±c`
 * into llvm.fmuladd.f64, which the translator lowers to fmul+fadd (using the
 * result slot as the intermediate). __builtin_fabs lowers to llvm.fabs.f64
 * (inline sign-bit clear). Returns 0 on success, else the first failed code.
 * n-neutral (constants kept exact) so one `test_e2e f64_fma.bin 0 <n>` works. */
int f64_fma_main(int n) {
    double a = 2.0 + (double)n - (double)n;
    double b = 3.0 + (double)n - (double)n;
    double c = 4.0 + (double)n - (double)n;

    double r = a * b + c;                 /* fmuladd -> 10.0 */
    if (r != 10.0) return 1;

    double s = a * b - c;                 /* fmuladd (neg c) -> 2.0 */
    if (s != 2.0) return 2;

    double m = -5.0 + (double)n - (double)n;
    if (__builtin_fabs(m) != 5.0) return 3;          /* llvm.fabs.f64 */
    if (__builtin_fabs(c) != 4.0) return 4;

    double cs = __builtin_copysign(c, m);            /* llvm.copysign.f64 */
    if (cs != -4.0) return 5;

    return 0;
}
