/* Regression: i1 (boolean) constant materialization.
 *
 * clang lowers `cond ? 0 : 1` on a reused i1 to `zext(xor i1 %cond, true)`.
 * The translator materialized constant operands sign-extended, so `i1 true`
 * became -1 and the xor turned into a full-width NOT: !1 -> -2, !0 -> -1
 * instead of 0 / 1. This silently corrupted the boolean (it broke DOOM's
 * keyboard event type `down ? ev_keydown : ev_keyup`). i1 constants must
 * materialize zero-extended (0/1). Returns 0 on success. */

__attribute__((noinline)) static int sink2(int a, int b) { return a * 10 + b; }

/* `down` (i1) is reused — compared against prev AND flipped for `type` — so
 * clang keeps it as i1 and emits `xor i1 %down, true` for the flip. */
__attribute__((noinline)) static int repro(int x, int prev) {
    int down = (x != 0);
    if (down != prev) {
        int type = down ? 0 : 1;          /* xor i1 down, true ; zext */
        return sink2(type, down);
    }
    return 999;
}

int main(int n) {
    (void)n;
    if (repro(5, 0) != 1)  return 1;      /* down=1 -> type 0 -> 0*10+1 = 1  */
    if (repro(0, 1) != 10) return 2;      /* down=0 -> type 1 -> 1*10+0 = 10 */
    /* also exercise the bare value, not just through a struct/sink */
    int b = (n + 1 != 0);                 /* n=0 -> b=1 */
    if ((b ? 0 : 1) != 0)  return 3;      /* !b must be 0, not -2 */
    return 0;
}
