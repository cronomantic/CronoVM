/* Differential conformance slice: 64-bit funnel shift / rotate (llvm.fshl.i64 /
 * llvm.fshr.i64).
 *
 * clang canonicalises a 64-bit rotate `(x<<n)|(x>>(64-n))` to llvm.fshl.i64 (or
 * fshr) already at -O1. The translator previously rejected it (`width 64
 * unsupported`); it now lowers it to a cvm_int64_rt soft call
 * (__cvm_fshl64/__cvm_fshr64), the probe links the i64 runtime, and cvm-cc
 * compiles that runtime scalar so it survives -O2/-O3 (auto-vectorising it
 * produced vector phi/select the legaliser rejects). This guards the whole path.
 *
 * Rotate amounts span the <32 / ==32 / >32 word boundaries (and the s==0 / s!=0
 * runtime fixup). int32 FNV checksum, fixed-width; rotates are UB-free (all shift
 * counts are constants in 1..63). */
#include <stdint.h>

static uint64_t rotl64(uint64_t x, unsigned n) { return (x << n) | (x >> (64u - n)); }
static uint64_t rotr64(uint64_t x, unsigned n) { return (x >> n) | (x << (64u - n)); }

int conf_main(void) {
    static const uint64_t V[4] = {
        UINT64_C(0x0123456789ABCDEF), UINT64_C(0xFEDCBA9876543210),
        UINT64_C(0x8000000000000001), UINT64_C(0x00000000FFFFFFFF),
    };
    static const unsigned N[8] = { 1u, 7u, 13u, 31u, 32u, 33u, 47u, 63u };
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(v)) * 16777619u; } while (0)

    for (int k = 0; k < 4; k++) {
        volatile uint64_t vv = V[k];
        /* CONSTANT amounts -> llvm.fshl.i64 / fshr.i64 (the new lowering). The
         * amounts span the <32 / ==32 / >32 word boundaries. */
        uint64_t c[8] = {
            rotl64(vv, 1),  rotl64(vv, 13), rotl64(vv, 31), rotl64(vv, 32),
            rotl64(vv, 47), rotr64(vv, 7),  rotr64(vv, 33), rotr64(vv, 63),
        };
        for (int j = 0; j < 8; j++) { MIX((uint32_t)c[j]); MIX((uint32_t)(c[j] >> 32)); }
        /* VARIABLE amounts -> __cvm_shl64/__cvm_shr64 (variable i64 shift; the
         * probe must link the i64 runtime for these too). N[j] is in 1..63. */
        for (int j = 0; j < 8; j++) {
            uint64_t a = rotl64(vv, N[j]);
            uint64_t b = rotr64(vv, N[j]);
            MIX((uint32_t)a); MIX((uint32_t)(a >> 32));
            MIX((uint32_t)b); MIX((uint32_t)(b >> 32));
        }
    }
    #undef MIX
    return (int)h;
}
