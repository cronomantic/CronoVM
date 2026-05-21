/* Exercises translator intrinsic lowerings added for the DOOM port:
 * unordered float compare (fcmp uge), narrow signed min/max (llvm.smax.i16),
 * and count-leading-zeros (llvm.ctlz.i32).
 * vm_main(7,5): uge(7,5)=1; smax_i16(7,5)=7; clz(7|1=7)=29 -> 1000+7+29 = 1036. */
int vm_main(int a, int b)
{
    float fa = (float)a, fb = (float)b;
    int   uge = !(fa < fb);                 /* fcmp uge */
    short sa = (short)a, sb = (short)b;
    int   mx = (sa > sb) ? sa : sb;          /* llvm.smax.i16 */
    int   cz = __builtin_clz((unsigned)(a | 1));  /* llvm.ctlz.i32, nonzero */
    return uge * 1000 + mx + cz;
}
