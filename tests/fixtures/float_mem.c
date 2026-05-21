/* Regression fixture for float load/store and llvm.fmuladd.f32 lowering.
 *
 * Storing/loading float to memory (a global array) was rejected by the
 * translator ("store: unsupported value type") — f32 is a 32-bit word and
 * must use STW/LDW. And `a*b + c` on floats lowers to llvm.fmuladd.f32 under
 * clang's default fp-contract, which had no lowering. Both are essential for
 * the 3D maths header. vm_main(n) computes 2*n + 3 through float memory. */

float fm[4];

int vm_main(int n) {
    fm[0] = (float)n;     /* SIToFP + float store */
    fm[1] = 2.0f;
    fm[2] = 3.0f;
    float r = fm[0] * fm[1] + fm[2] + fm[3]; /* loads + fmuladd; fm[3]=0 */
    return (int)r;                     /* FPToSI -> 2*n + 3 */
}
