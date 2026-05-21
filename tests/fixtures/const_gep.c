/* Regression fixture for constant-expression GEP operands.
 *
 * Storing to a global array at a fixed index makes clang -O1 emit a
 * ConstantExpr getelementptr (`store i32 v, ptr getelementptr(@g, k)`) as
 * the pointer operand. Before the fix, cg_reg_for had no case for constant
 * GEP/bitcast expressions and bailed with "operand has no register
 * assigned". The array has external linkage so the stores can't be
 * optimised away. Returns 2*n + 3 (= n + (n+3)). */

int cg_probe[4];

int vm_main(int n) {
    cg_probe[0] = n;
    cg_probe[1] = n + 1;   /* store via constexpr GEP &cg_probe[1] */
    cg_probe[2] = n + 2;   /* &cg_probe[2] */
    cg_probe[3] = n + 3;   /* &cg_probe[3] */
    return cg_probe[0] + cg_probe[3];
}
