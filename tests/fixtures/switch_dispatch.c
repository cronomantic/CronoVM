/* Exercise the LLVMSwitch lowering.
 *
 * Each arm performs a different binary operation on (x, y), which prevents
 * Clang's -O1 "switch to lookup table" optimization (the optimizer can
 * only fold the switch away when every arm yields a value Clang can
 * precompute and tabulate; arms that depend on the runtime inputs keep
 * the switch as a real `switch` IR instruction).
 *
 * Mix of consecutive (0,1,2) and gappy (5, 10) case constants plus a
 * default arm.
 *
 * vm_main(op, x, y) returns:
 *   op == 0  → x + y
 *   op == 1  → x - y
 *   op == 2  → x * y
 *   op == 5  → x ^ y
 *   op == 10 → x & y
 *   anything else → -1
 */

int vm_main(int op, int x, int y) {
    int r;
    switch (op) {
        case 0:  r = x + y;  break;
        case 1:  r = x - y;  break;
        case 2:  r = x * y;  break;
        case 5:  r = x ^ y;  break;
        case 10: r = x & y;  break;
        default: r = -1;     break;
    }
    return r;
}
