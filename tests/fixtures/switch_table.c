/* Exercises the dense-switch lowering (jump-table form).
 *
 * Eight contiguous cases (0..7) with a default. n_cases = 8, range = 8,
 * density = 1.0 — well above the 0.5 threshold, so the translator
 * picks the table form (BEQ/CMP_LTU + LDW + JMPR) instead of the
 * chained CMP_EQ + BNE per case.
 *
 * Each arm depends on runtime inputs (x, y), preventing clang -O1
 * from folding the switch to its own lookup table — the IR keeps the
 * `switch` instruction so our codegen actually has to lower it.
 *
 * Returned values picked so each arm is distinguishable:
 *   op 0 → x + y      op 4 → x | y
 *   op 1 → x - y      op 5 → x & y
 *   op 2 → x * y      op 6 → x ^ y
 *   op 3 → x << y     op 7 → x >> y      default → -1
 */

int vm_main(int op, int x, int y) {
    int r;
    switch (op) {
        case 0: r = x + y;  break;
        case 1: r = x - y;  break;
        case 2: r = x * y;  break;
        case 3: r = x << y; break;
        case 4: r = x | y;  break;
        case 5: r = x & y;  break;
        case 6: r = x ^ y;  break;
        case 7: r = x >> y; break;
        default: r = -1;    break;
    }
    return r;
}
