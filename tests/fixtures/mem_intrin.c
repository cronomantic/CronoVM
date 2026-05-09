/* Exercise the llvm.mem{cpy,set,move} intrinsics end-to-end.
 *
 * vm_main(seed) returns:
 *   sum(buf[0..15]) + dst.tag + dst.payload[0] + dst.payload[2]
 *
 * Where:
 *   - buf is a 16-byte array first memset to (seed & 0xFF), so
 *     sum(buf) == 16 * (seed & 0xFF).
 *   - src is a struct {tag=42, payload={seed, seed+1, seed+2, seed+3}}.
 *   - dst is memcpy'd from src.
 *   - We then memmove dst.payload[1..3] one slot left so:
 *     payload becomes {seed+1, seed+2, seed+3, seed+3}.
 *     This is a true forward overlap, exactly the case where memcpy would
 *     be undefined behaviour.
 *
 * Final result for seed=10:
 *   sum(buf) = 16 * 10 = 160
 *   dst.tag           = 42
 *   dst.payload[0]    = seed+1 = 11
 *   dst.payload[2]    = seed+3 = 13
 *   total             = 160 + 42 + 11 + 13 = 226
 */

#include <stddef.h>

typedef struct {
    int tag;
    int payload[4];
} record_t;

int vm_main(int seed) {
    unsigned char buf[16];
    record_t src;
    record_t dst;

    /* memset over a fixed-size array. Clang lowers this to llvm.memset. */
    __builtin_memset(buf, seed & 0xFF, sizeof(buf));

    /* Build src by hand. */
    src.tag = 42;
    src.payload[0] = seed;
    src.payload[1] = seed + 1;
    src.payload[2] = seed + 2;
    src.payload[3] = seed + 3;

    /* Struct copy — Clang lowers to llvm.memcpy. */
    __builtin_memcpy(&dst, &src, sizeof(dst));

    /* In-buffer overlap shifted by one int. memmove must handle it. */
    __builtin_memmove(&dst.payload[0], &dst.payload[1], 3 * sizeof(int));

    int sum = 0;
    for (int i = 0; i < 16; ++i) sum += buf[i];

    return sum + dst.tag + dst.payload[0] + dst.payload[2];
}
