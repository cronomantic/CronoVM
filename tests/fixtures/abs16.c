/* abs16.c — exercises llvm.abs.i16 lowering.
 *
 * Mirrors UQM grpinfo.c's BuildGroups distance math: dx = a - b; if (dx < 0)
 * dx = -dx; with dx an i16. clang -O1 folds the i16 select into llvm.abs.i16.
 *
 * The discriminating case abs16(30000, -30000): a-b = 60000, truncated to a
 * NEGATIVE short (-5536). A lowering that skips sign-extension would read the
 * 32-bit reg as positive 60000 and mis-handle it (same bug class as the
 * sitofp-narrow miscompile) — so the abs.i16 path MUST sign-extend first.
 */
short abs16(short a, short b) {
    short dx = (short)(a - b);
    if (dx < 0)
        dx = -dx;
    return dx;
}
