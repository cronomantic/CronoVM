/* conf_overflow_wide.c — conformance slice: SIGNED overflow-checked arithmetic
 * at the odd wide widths i33 and i65 that libc++'s std::num_get emits when it
 * parses an integer from a stream. num_get range-checks each accumulation step
 * in a type ONE BIT WIDER than the result, so the check itself cannot overflow:
 * an i32 result is checked in i33, an i64 (`long long`) result in i65. The
 * telltale IR (verified against the vendored libc++ locale.cpp) is, per step:
 *
 *     %v  = zext i32 <acc-as-unsigned> to i33   ; (i65: zext i64 -> i65)
 *     %d  = sext i32 <signed delta>    to i33   ; (i65: sext i32 -> i65)
 *     %r  = call { i33, i1 } @llvm.sadd.with.overflow.i33(%v, %d)
 *     %of = extractvalue { i33, i1 } %r, 1      ; the overflow flag
 *     %s  = extractvalue { i33, i1 } %r, 0      ; the iN value
 *           trunc i33 %s to i32   /   icmp slt i33 %s, 0
 *
 * The add does NOT fold (the reason clang keeps the intrinsic) because one
 * operand is ZERO-extended (range [0, 2^32-1]) and the other SIGN-extended
 * ([-2^31, 2^31-1]) — their sum can exceed the i33/i65 range, so the overflow
 * really can fire. The translator legalises a 33..63-bit value as a "wide2"
 * 2-slot value kept canonically sign-extended to 64 bits, and i65 as a "wide3"
 * 3-slot value {w0,w1,sign}; sadd.with.overflow flags overflow when the raw sum
 * doesn't fit the width (a canonicalise-mismatch). Fixed-width + int32 checksum,
 * so host-64 and VM-32 agree bit-for-bit; differential vs the native oracle. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

typedef _BitInt(33) i33;
typedef _BitInt(65) i65;

/* ---- i33 (wide2): the `int` parse guard --------------------------------- */

/* acc carried UNSIGNED (zext i32->i33), signed delta (sext i32->i33); the
 * sadd.with.overflow.i33 + trunc i33->i32 path. */
static int32_t NOINLINE parse_i32(const uint8_t *d, int n, int *ov) {
    uint32_t acc = 0; int of = 0;
    for (int i = 0; i < n; ++i) {
        int32_t delta = (int32_t)d[i] - 64;
        i33 r;
        if (__builtin_add_overflow((i33)(uint32_t)acc, (i33)delta, &r)) { of = 1; break; }
        acc = (uint32_t)(int32_t)r;                 /* trunc i33 -> i32 */
        acc = acc * 1000003u + (uint32_t)d[i];
    }
    *ov = of; return (int32_t)acc;
}

/* Forces `icmp slt i33 %r, 0` (the sign test) by branching on the sign of a
 * sadd.with.overflow.i33 result against a runtime threshold. */
static uint32_t NOINLINE sign_i33(uint32_t acc, int32_t delta, int32_t thr, int *ovf) {
    i33 r;
    if (__builtin_add_overflow((i33)(uint32_t)acc, (i33)delta, &r)) { *ovf = 1; return acc; }
    if (r < (i33)thr) return (uint32_t)(int32_t)r ^ 0xAAAAAAAAu;   /* icmp slt i33 */
    return (uint32_t)(int32_t)r * 2654435761u;
}

/* ---- i65 (wide3): the `long long` parse guard ---------------------------- */

/* acc carried UNSIGNED i64 (zext i64->i65), signed delta (sext i32->i65); the
 * sadd.with.overflow.i65 + trunc i65->i64 path. */
static int64_t NOINLINE parse_i64(const uint8_t *d, int n, int *ov) {
    uint64_t acc = 0; int of = 0;
    for (int i = 0; i < n; ++i) {
        int32_t delta = (int32_t)d[i] - 64;
        i65 r;
        if (__builtin_add_overflow((i65)(uint64_t)acc, (i65)delta, &r)) { of = 1; break; }
        acc = (uint64_t)(int64_t)r;                 /* trunc i65 -> i64 */
        acc = acc * 1000000007ull + (uint64_t)d[i];
    }
    *ov = of; return (int64_t)acc;
}

/* Forces `icmp slt i65 %r, 0` by branching on the sign of a sadd.with.overflow.i65
 * result against the literal 0. */
static int64_t NOINLINE sign_i65(uint64_t acc, int32_t delta, int *ovf) {
    i65 r;
    if (__builtin_add_overflow((i65)(uint64_t)acc, (i65)delta, &r)) { *ovf = 1; return -1; }
    if (r < (i65)0) { *ovf = 2; return (int64_t)(uint64_t)(int64_t)r; }   /* icmp slt i65, 0 */
    return (int64_t)((uint64_t)(int64_t)r * 31u);
}

/* Digit strings engineered to drive the accumulators near / past the i33 and
 * i65 boundaries (so both the overflow flag and the sign test fire). */
static volatile uint8_t seeds[8][12] = {
    { 0 },
    { 1, 2, 3, 0 },
    { 255, 255, 255, 255, 255, 255, 255, 255, 0 },
    { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 0 },
    { 4, 4, 4, 4, 4, 4, 0 },
    { 200, 1, 200, 1, 200, 1, 0 },
    { 7, 0 },
    { 128, 64, 32, 16, 8, 4, 2, 1, 0 },
};
static volatile int32_t deltas[8] = { 1, -100, 2000000000, -2000000000, 5, -7, 1000000, -3 };
static volatile int32_t thrs[8]   = { 0, 10, -1000000000, 1500000000, -50, 100, 0, 7 };

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v)   do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)
    #define MIX64(v) do { uint64_t _t=(uint64_t)(v); MIX((uint32_t)_t); MIX((uint32_t)(_t>>32)); } while (0)

    uint32_t acc32 = 12345u;
    uint64_t acc64 = 0xFFFFFFFF00000000ull;
    for (int s = 0; s < 8; ++s) {
        int n = 0; while (n < 12 && seeds[s][n] != 0) ++n;
        uint8_t buf[12]; for (int i = 0; i < 12; ++i) buf[i] = seeds[s][i];

        int o32, o64, sv32 = 0, sv64 = 0;
        int32_t r32 = parse_i32(buf, n, &o32);
        int64_t r64 = parse_i64(buf, n, &o64);
        MIX(r32);   MIX(o32);
        MIX64(r64); MIX(o64);

        acc32 = sign_i33(acc32, deltas[s], thrs[s], &sv32); MIX(acc32);   MIX(sv32);
        acc64 = (uint64_t)sign_i65(acc64, deltas[s], &sv64); MIX64(acc64); MIX(sv64);
    }

    #undef MIX
    #undef MIX64
    return (int)h;
}
