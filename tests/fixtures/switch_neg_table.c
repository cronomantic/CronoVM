/* Exercises the dense-switch table lowering with **negative iN case**
 * constants — the gap left by switch_table.c (positive i32 cases only).
 *
 * Target IR shape (mirrors cvm_d_from_f32 in cvm_float64.h):
 *
 *   %t = lshr i32 %op, 24
 *   %s = trunc i32 %t to i8
 *   switch i8 %s, label %default [
 *     i8 -4, ...; i8 -3, ...; i8 -2, ...; i8 -1, ...
 *   ]
 *
 * n_cases = 4, range = 4, density = 1.0 — over threshold → table form.
 *
 * Latent bug class this guards against: the table form previously read
 * case constants via `LLVMConstIntGetSExtValue`, which sign-extended
 * `i8 -1` to 0xFFFFFFFF before computing `lo`. The Trunc lowering
 * (session-5 fix) leaves cond_reg zero-extended to 0xFF, so
 * `SUB off, cond=0xFF, lo=0xFFFFFFFE` yielded 0x103 and
 * `CMP_LTU off, n_range=4` missed — every input fell through to default.
 * The chain form was patched in session 5; the table form needed the
 * same ZExt + cond_w mask treatment.
 *
 * Caller passes the test code as the low byte of `op`. Clang -O1
 * narrows `op & 0xff` to an i8 and keeps `switch i8` because the case
 * literals are written as the unsigned bit patterns 0xfc..0xff —
 * exactly the trunc-i32-to-i8 + switch-i8 shape that triggered the
 * latent bug. Each arm depends on runtime (x, y) so the switch
 * survives -O1 instead of being folded to a constant.
 */

int vm_main(int op, int x, int y) {
    /* op is interpreted as a byte-packed payload: the top byte selects
     * the case, lower bytes are ignored. lshr-then-trunc is the IR
     * pattern that survives -O1 with a `switch i8`. */
    unsigned char b = (unsigned char)op;
    int r;
    /* Cases are written as the unsigned bit patterns 0xfc..0xff so
     * clang -O1 keeps the IR as `switch i8 %b` (with `i8 -4..-1`)
     * rather than collapsing to `switch i32` — the same shape that
     * cvm_d_from_f32 produces in cvm_float64.h. The translator sees
     * each case as `i8 -1` etc., which is exactly the negative-iN
     * path the table form previously botched. */
    switch (b) {
        case 0xfc: r = x + y;  break;
        case 0xfd: r = x - y;  break;
        case 0xfe: r = x * y;  break;
        case 0xff: r = x ^ y;  break;
        default:   r = -1;     break;
    }
    return r;
}
