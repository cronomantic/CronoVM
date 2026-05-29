/* conf_overflow.c — conformance slice: overflow-checked arithmetic, the
 * llvm.{uadd,umul}.with.overflow.{i32,i64} intrinsics clang emits for
 * __builtin_{add,mul}_overflow (and inside picolibc's strtoul/strtoull). Each
 * call returns a {value, overflow-flag} pair via extractvalue; this folds both
 * fields for every operand pairing into one int32 checksum. Differential vs the
 * host. Also covers abs.i64 (llvm.abs.i64). */
#include <stdint.h>

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    /* i32: add + mul overflow across a spread incl. the 2^31 / 2^32 boundaries */
    static const uint32_t a32[] = {
        0u, 1u, 2u, 1000u, 0x7fffffffu, 0x80000000u, 0xfffffff0u, 0xffffffffu
    };
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) {
            uint32_t r;  int o  = __builtin_add_overflow(a32[i], a32[j], &r);
            uint32_t r2; int o2 = __builtin_mul_overflow(a32[i], a32[j], &r2);
            MIX(r); MIX(o); MIX(r2); MIX(o2);
        }

    /* i64: add + mul overflow incl. the 2^32 / 2^63 / 2^64 boundaries (the i64
     * umul overflow exercises the schoolbook 64x64 high-half detection) */
    static const uint64_t a64[] = {
        0ull, 1ull, 0xffffffffull, 0x100000000ull, 0x7fffffffffffffffull,
        0x8000000000000000ull, 0xffffffffffffffffull, 0x123456789ull
    };
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) {
            uint64_t r;  int o  = __builtin_add_overflow(a64[i], a64[j], &r);
            uint64_t r2; int o2 = __builtin_mul_overflow(a64[i], a64[j], &r2);
            MIX((int32_t)r);  MIX((int32_t)(r  >> 32)); MIX(o);
            MIX((int32_t)r2); MIX((int32_t)(r2 >> 32)); MIX(o2);
        }

    /* abs.i64 incl. INT64_MIN (well-defined wrap; host and VM agree) */
    static const long long s64[] = {
        0, 1, -1, 5, -5, 2147483648LL, -2147483648LL,
        9223372036854775807LL, (-9223372036854775807LL - 1), -1000000000000LL
    };
    for (int i = 0; i < 10; ++i) {
        long long x = s64[i];
        long long r = x < 0 ? -x : x;     /* clang -> llvm.abs.i64 */
        MIX((int32_t)r); MIX((int32_t)(r >> 32));
    }

    #undef MIX
    return (int)h;
}
