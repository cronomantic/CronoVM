/* conf_sat.c — conformance slice: saturating add/sub (llvm.{u}{add,sub}.sat)
 * across widths. UQM emits uadd.sat.i8 + usub.sat.i8/i16, so the narrow forms
 * matter. The wrap-detect idioms below fold to the sat intrinsics; the seeds
 * include values that actually saturate (sums > 2^N-1, diffs < 0). Differential
 * vs native catches a non-width-aware lowering (e.g. clamping i8 add to
 * 0xFFFFFFFF instead of 0xFF, or never detecting narrow overflow). */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

static uint8_t  NOINLINE uaddsat8 (uint8_t  a, uint8_t  b) { uint8_t  s=a+b; return s<a?(uint8_t)0xFF:s; }
static uint16_t NOINLINE uaddsat16(uint16_t a, uint16_t b) { uint16_t s=a+b; return s<a?(uint16_t)0xFFFF:s; }
static uint32_t NOINLINE uaddsat32(uint32_t a, uint32_t b) { uint32_t s=a+b; return s<a?0xFFFFFFFFu:s; }
static uint8_t  NOINLINE usubsat8 (uint8_t  a, uint8_t  b) { uint8_t  s=a-b; return s>a?(uint8_t)0:s; }
static uint16_t NOINLINE usubsat16(uint16_t a, uint16_t b) { uint16_t s=a-b; return s>a?(uint16_t)0:s; }
static uint32_t NOINLINE usubsat32(uint32_t a, uint32_t b) { uint32_t s=a-b; return s>a?0u:s; }

/* Mix of small, mid and near-max values so add saturates and sub underflows. */
static volatile uint32_t seeds[8] = { 0, 1, 100, 200, 255, 32000, 65000, 0xFFFFFF00u };

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(v)) * 16777619u; } while (0)

    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            uint32_t a = seeds[i], b = seeds[j];
            MIX(uaddsat8 ((uint8_t)a,  (uint8_t)b));
            MIX(uaddsat16((uint16_t)a, (uint16_t)b));
            MIX(uaddsat32(a, b));
            MIX(usubsat8 ((uint8_t)a,  (uint8_t)b));
            MIX(usubsat16((uint16_t)a, (uint16_t)b));
            MIX(usubsat32(a, b));
        }
    }

    #undef MIX
    return (int)h;
}
