/* conf_int_intrin.c — conformance slice: integer LLVM intrinsics that clang
 * folds from ordinary C idioms, across widths. Results fold into a rolling
 * checksum; the differential runner compares the VM checksum against the
 * native one, so an unlowered intrinsic (translate gap) OR a mislowered one
 * (wrong value — e.g. a missing narrow-width sign-extension) is caught.
 *
 * Design notes:
 *  - conf_main is defined FIRST so it is the VM entry point.
 *  - Inputs come from a `volatile` array so clang can't constant-fold the ops
 *    away at -O1 (which would leave the intrinsics unexercised at runtime).
 *  - Narrow-width intrinsics (abs.i8/i16, ctpop.i8/i16, ...) are forced via
 *    `noinline` helpers with type-pinned signatures — otherwise clang widens
 *    the result to i32 and the narrow form never appears (that is exactly the
 *    bug class that bit the UQM port: abs.i16 / ctpop.i8 sign-extension).
 *  - Fixed-width types only, int32 checksum: host-64 and VM-32 agree exactly. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

static int8_t  NOINLINE abs8 (int8_t  v) { return (int8_t)(v < 0 ? -v : v); }
static int16_t NOINLINE abs16(int16_t v) { return (int16_t)(v < 0 ? -v : v); }
static int32_t NOINLINE abs32(int32_t v) { return v < 0 ? -v : v; }
static int     NOINLINE pop8 (uint8_t  v) { return __builtin_popcount(v); }
static int     NOINLINE pop16(uint16_t v) { return __builtin_popcount(v); }
static int     NOINLINE pop32(uint32_t v) { return __builtin_popcount(v); }
static int16_t NOINLINE bswap16i(uint16_t v) { return (int16_t)__builtin_bswap16(v); }
static int8_t  NOINLINE bitrev8 (uint8_t  v) { return (int8_t)__builtin_bitreverse8(v); }
static int16_t NOINLINE bitrev16(uint16_t v) { return (int16_t)__builtin_bitreverse16(v); }

static volatile int32_t seeds[8] = { 0, 1, -1, 7, -7, 30000, -30000, 1234567 };

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    for (int i = 0; i < 8; ++i) {
        int32_t  x  = seeds[i];
        uint32_t ux = (uint32_t)x;

        /* narrow + wide abs (llvm.abs.i8/i16/i32) */
        MIX(abs8 ((int8_t)x));
        MIX(abs16((int16_t)x));
        MIX(abs32(x));

        /* popcount (llvm.ctpop.i8/i16/i32) */
        MIX(pop8 ((uint8_t)ux));
        MIX(pop16((uint16_t)ux));
        MIX(pop32(ux));

        /* leading/trailing zeros (llvm.ctlz/cttz.i32) — avoid 0 (UB) */
        uint32_t nz = ux ? ux : 1u;
        MIX(__builtin_clz(nz));
        MIX(__builtin_ctz(nz));

        /* byte swaps (llvm.bswap.i16/i32) */
        MIX(bswap16i((uint16_t)ux));
        MIX(__builtin_bswap32(ux));

        /* bit reverse (llvm.bitreverse.i8/i16/i32) */
        MIX(bitrev8 ((uint8_t)ux));
        MIX(bitrev16((uint16_t)ux));
        MIX((int32_t)__builtin_bitreverse32(ux));

        /* signed/unsigned min/max (llvm.smin/smax/umin/umax.i32) */
        for (int j = 0; j < 8; ++j) {
            int32_t  y  = seeds[j];
            uint32_t uy = (uint32_t)y;
            MIX(x < y ? x : y);
            MIX(x > y ? x : y);
            MIX(ux < uy ? ux : uy);
            MIX(ux > uy ? ux : uy);
        }
    }

    #undef MIX
    return (int)h;
}
