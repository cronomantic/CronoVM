#include <stdint.h>

/* Minimal i64-legalisation derisk slice (route A: auto-lower native 64-bit to
 * the soft runtime / 2-word frame slots).
 *
 * Proves a 64-bit value can live in a frame slot, be produced by an add that
 * carries lo->hi, and be read back. `step` is volatile so the optimiser can't
 * fold the i64 arithmetic down to i32 (it otherwise reduces (sext(n)+K)>>32 to
 * a plain i32 compare and no i64 reaches codegen).
 *
 * Exercises: i64 alloca + i64 store(const) + volatile i64 load, sext i32->i64,
 * i64 add (lo ADD + carry into hi), i64 lshr by 32, trunc i64->i32.
 *
 *   n=0 -> c = 0 + 0xFFFFFFFF = 0x00000000FFFFFFFF, (c>>32) = 0
 *   n=1 -> c = 1 + 0xFFFFFFFF = 0x0000000100000000, (c>>32) = 1
 * Run: test_e2e i64_slice.bin 0 0   (n=0, expect 0)
 *      test_e2e i64_slice.bin 1 1   (n=1, expect 1)
 */
int i64_slice_main(int n) {
    volatile long long step = 0xFFFFFFFFLL;     /* frame slot; blocks folding */
    long long a = (long long)n;                 /* sext i32 -> i64           */
    long long c = a + step;                     /* i64 load + i64 add (carry) */
    return (int)((unsigned long long)c >> 32);  /* lshr 32 + trunc -> hi word */
}
