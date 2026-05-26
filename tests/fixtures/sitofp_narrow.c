#include <stdint.h>

/* Regression: `sitofp iN` for N < 32 must sign-extend the operand first.
 * Narrow loads (LDB/LDH) and trunc leave the value zero-extended in its
 * 32-bit register, so a raw signed int->float conversion read e.g. an
 * int16 -2008 as +63528. In Quake this frustum-culled the entire world:
 * mnode_t bounding boxes are int16[6] and `(float)minmaxs[i]` flipped every
 * negative coordinate huge-positive, so R_RecursiveWorldNode rejected the
 * root node and nothing but the background colour drew.
 *
 * Exercises the f32 I2F_S opcode for both int16 and int8 sources. (The f64
 * conversion path — __cvm_f_from_i32 — got the same sign-extension fix, but a
 * double here would pull in the f64 runtime that raw cvm-translate doesn't
 * link; this fixture stays f32-only. The f32 path is the one Quake's frustum
 * cull actually hit.)
 *
 * Globals are non-const and indexed by the runtime count `n`, so the loads
 * (and thus the sitofp opcodes) survive the optimiser; nothing folds. The
 * `* 0.5f` keeps a real float multiply in the path so the cast can't collapse
 * to an integer round-trip. Each term halves to an exact binary value (all
 * inputs are even), and (int) truncates toward zero.
 *
 * sitofp_narrow_main(4), summed per term:
 *   g16/2: -1004 + -2 + 688 + 50 = -268
 *   g8 /2:   -50 + -1 +  25 +  4 =  -22
 *   total = -290
 * With the bug every negative input reads huge-positive, so the result is a
 * large positive number — a clean mismatch.
 */
int16_t g16[4] = {-2008, -4, 1376, 100};
int8_t  g8[4]  = {-100, -2, 50, 8};

int sitofp_narrow_main(int n) {
    int acc = 0;
    for (int i = 0; i < n; i++) {
        acc += (int)((float)g16[i] * 0.5f);    /* I2F_S of i16 */
        acc += (int)((float)g8[i] * 0.5f);     /* I2F_S of i8  */
    }
    return acc;
}
