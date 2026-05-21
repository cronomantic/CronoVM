/* Regression fixture for `store i64` (8-byte struct zero-init/copy chunks).
 *
 * clang lowers struct/array zero-init and small copies with i64 stores even
 * on a 32-bit target. The VM has no 64-bit register; a constant i64 store
 * splits into two 32-bit word stores. This mirrors the cron_cvert array
 * construction in the Cronopio 3D pipeline that first hit it.
 * vm_main(n) returns n. */

struct P  { float x, y, z, w; };
struct CV { struct P pos; float u, v, l; };
struct CV sink[3];

int vm_main(int n) {
    struct CV a = {{0,0,0,0}, 0, 0, 0};   /* zero-init -> store i64 chunks */
    a.u = (float)n;
    struct CV arr[3] = { a, a, a };
    sink[0] = arr[0]; sink[1] = arr[1]; sink[2] = arr[2];
    return (int)sink[2].u;
}
