/* Differential conformance slice: NAMED float libm libcalls -> VM opcodes.
 *
 * Guards the translator lowering of the bare `sqrtf`/`fabsf`/`floorf`/`ceilf`/
 * `truncf` calls (not just the `llvm.*.f32` intrinsics) to the FSQRT / fabs-AND /
 * FFLOOR / FCEIL / FTRUNC opcodes. libc++ <cmath> on float (e.g. std::sqrt(float)
 * at -O1, with no math-errno folding) emits these as NAMED libcalls; without the
 * lowering the translator rejected them ("callee has no definition" — it only
 * knew the intrinsics, and there was no f32 sqrt path at all). The extern decls
 * below force the named form regardless of clang's builtin folding.
 *
 * Float results are compared BIT-EXACT (the opcode evaluates the same host
 * sqrtf/floorf/... so the bits match); only non-negative args to sqrtf (no NaN
 * payload ambiguity). int32 FNV checksum. (sinf/cosf/powf/logf are transcendental
 * — no single opcode; they need picolibc's float libm, added separately.) */
#include <stdint.h>

extern float sqrtf(float);
extern float fabsf(float);
extern float floorf(float);
extern float ceilf(float);
extern float truncf(float);

static uint32_t fbits(float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    return b;
}

int conf_main(void) {
    static const float P[4] = { 42.0f, 2.0f, 1000.49f, 0.25f };          /* >= 0 */
    static const float S[6] = { 7.3f, -2.8f, -0.5f, 3.5f, -1000.49f, 0.0f };
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(v)) * 16777619u; } while (0)

    for (int k = 0; k < 4; k++) {
        volatile float x = P[k];
        MIX(fbits(sqrtf(x)));
    }
    for (int k = 0; k < 6; k++) {
        volatile float x = S[k];
        MIX(fbits(fabsf(x)));
        MIX(fbits(floorf(x)));
        MIX(fbits(ceilf(x)));
        MIX(fbits(truncf(x)));
    }
    #undef MIX
    return (int)h;
}
