/* conf_int_width.c — conformance slice: integer arithmetic / comparison /
 * shift / conversion across widths, with emphasis on the paths that have
 * historically miscompiled or gone unlowered:
 *   - i64 soft arithmetic (mul/div/rem/shifts)        -> the i64 runtime
 *   - narrow signed/unsigned icmp                      -> the narrow-icmp bug
 *   - sext/zext/trunc round-trips                      -> width legalisation
 *   - rotates (funnel shift, llvm.fshl/fshr)           -> idiom fold
 * Differential vs the native oracle; fixed-width types + int32 checksum. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* noinline so the narrow compares aren't folded; type-pinned signatures. */
static int NOINLINE lt_s8 (int8_t  a, int8_t  b) { return a < b; }
static int NOINLINE lt_u8 (uint8_t a, uint8_t b) { return a < b; }
static int NOINLINE lt_s16(int16_t a, int16_t b) { return a < b; }
static int NOINLINE lt_u16(uint16_t a,uint16_t b) { return a < b; }
static uint32_t NOINLINE rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t NOINLINE rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static int64_t  NOINLINE mul64(int64_t a, int64_t b) { return a * b; }
static int64_t  NOINLINE div64(int64_t a, int64_t b) { return a / b; }
static int64_t  NOINLINE rem64(int64_t a, int64_t b) { return a % b; }
static uint64_t NOINLINE shl64(uint64_t a, int n) { return a << n; }
static uint64_t NOINLINE shr64(uint64_t a, int n) { return a >> n; }
static int64_t  NOINLINE sar64(int64_t a, int n)  { return a >> n; }

/* Arbitrary (non-power-of-2) integer widths via _BitInt, kept as REGISTER SSA
 * (no address taken, so no iN load/store — matching how clang -O1 narrows a
 * value into an odd width, e.g. the i14 it derives in UQM comm.c text layout).
 * Exercises trunc i32->iN, iN add/mul/ashr/lshr, signed + unsigned iN compares,
 * and sext/zext iN->i32 — all the width-generic codegen paths. The values are
 * N-bit so host-64 and VM-32 agree bit-for-bit. */
typedef          _BitInt(14) bi14;
typedef unsigned _BitInt(14) bu14;
typedef          _BitInt(7)  bi7;
typedef          _BitInt(13) bi13;

static int32_t NOINLINE w14(int32_t a, int32_t b, int n) {
    bi14 x = (bi14)a, y = (bi14)b;   /* trunc i32 -> i14 (signed) */
    x = x + y;                       /* add  i14 */
    x = x * (bi14)3;                 /* mul  i14 */
    x = x >> (n & 13);               /* ashr i14 (arithmetic) */
    bu14 u = (bu14)a;                /* trunc i32 -> i14 (unsigned) */
    u = u >> (n & 13);               /* lshr i14 (logical) */
    int cs = (x < y);                /* signed   icmp i14 */
    int cu = (u < (bu14)b);          /* unsigned icmp i14 */
    return (int32_t)x + (int32_t)u * 7 + cs * 101 + cu * 1009; /* sext/zext i14->i32 */
}
static int32_t NOINLINE w7(int32_t a, int n) {
    bi7 x = (bi7)a; x = x + (bi7)5; x = x >> (n & 6);
    return (int32_t)x;               /* sext i7->i32 */
}
static int32_t NOINLINE w13(int32_t a, int32_t b) {
    bi13 x = (bi13)a, y = (bi13)b; x = x * y;
    return (int32_t)x;               /* mul i13 + sext */
}

static volatile int32_t seeds[8] = { 0, 1, -1, 7, -7, 30000, -30000, 1234567 };

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)
    #define MIX64(v) do { uint64_t _t=(uint64_t)(v); MIX((uint32_t)_t); MIX((uint32_t)(_t>>32)); } while (0)

    for (int i = 0; i < 8; ++i) {
        int32_t x = seeds[i];

        /* narrow signed/unsigned compares */
        for (int j = 0; j < 8; ++j) {
            int32_t y = seeds[j];
            MIX(lt_s8 ((int8_t)x,  (int8_t)y));
            MIX(lt_u8 ((uint8_t)x, (uint8_t)y));
            MIX(lt_s16((int16_t)x, (int16_t)y));
            MIX(lt_u16((uint16_t)x,(uint16_t)y));
        }

        /* sext/zext/trunc round-trips */
        MIX((int32_t)(int8_t)x);
        MIX((int32_t)(uint8_t)x);
        MIX((int32_t)(int16_t)x);
        MIX((int32_t)(uint16_t)x);

        /* rotates (funnel shift) — n in 1..31 */
        int n = (i & 7) + 1;
        MIX(rotl32((uint32_t)x, n));
        MIX(rotr32((uint32_t)x, n));

        /* i64 soft arithmetic */
        int64_t a = (int64_t)x * 0x100000001LL + 0x123456789ABCDEF0LL;
        for (int j = 1; j < 8; ++j) {
            int64_t b = (int64_t)seeds[j] * 0x9E3779B97F4A7C15LL + 1;
            MIX64(mul64(a, b));
            if (b != 0) { MIX64(div64(a, b)); MIX64(rem64(a, b)); }
            MIX64(shl64((uint64_t)a, j));
            MIX64(shr64((uint64_t)a, j));
            MIX64(sar64(a, j));
        }

        /* arbitrary (non-power-of-2) widths (reuses `n` from the rotates above) */
        for (int j = 0; j < 8; ++j) {
            MIX(w14(x, seeds[j], n));
            MIX(w13(x, seeds[j]));
        }
        MIX(w7(x, n));
    }

    #undef MIX
    #undef MIX64
    return (int)h;
}
