/* conf_float.c — conformance slice: float/double arithmetic, comparisons,
 * conversions, and the math intrinsics clang folds. Emphasis on the paths that
 * have miscompiled before (sitofp/fptosi at narrow widths) and the f32<->f64 +
 * soft-f64 runtime surface. IEEE-754 round-to-nearest is identical on host and
 * VM, so the differential oracle is valid; results are quantised into an int32
 * checksum (multiply + truncate) so tiny ULP-free ops compare exactly. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

static float  NOINLINE i2f (int32_t v)  { return (float)v; }
static float  NOINLINE i2f16(int16_t v) { return (float)v; }   /* narrow sitofp */
static float  NOINLINE u2f (uint32_t v) { return (float)v; }
static int32_t NOINLINE f2i (float f)   { return (int32_t)f; }
static double NOINLINE i2d (int32_t v)  { return (double)v; }
static int32_t NOINLINE d2i (double d)  { return (int32_t)d; }
static double NOINLINE f2d (float f)    { return (double)f; }  /* fpext  */
static float  NOINLINE d2f (double d)   { return (float)d; }   /* fptrunc */

/* Small bounded seeds: every float below stays well within int32 range after
 * the *1000 quantisation, so no float->int conversion is out-of-range (which
 * would be UB and differ host-vs-VM — a false MISCOMPILE). */
static volatile int32_t seeds[8] = { 0, 1, -1, 3, -3, 17, -17, 255 };

int conf_main(void) {
    uint32_t h = 2166136261u;
    /* quantise a float/double to a stable int32 for hashing */
    #define MIXF(f) do { float _q=(f); MIX((int32_t)(_q * 1000.0f)); } while (0)
    #define MIXD(d) do { double _q=(d); MIX((int32_t)(_q * 1000.0)); } while (0)
    #define MIX(v)  do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    for (int i = 0; i < 8; ++i) {
        int32_t x = seeds[i];

        /* int->float conversions (sitofp/uitofp), incl. narrow source */
        MIXF(i2f(x));
        MIXF(i2f16((int16_t)x));
        MIXF(u2f((uint32_t)(uint8_t)x));   /* bounded unsigned source */
        MIXD(i2d(x));

        /* float->int (fptosi) */
        MIX(f2i((float)x * 1.5f));
        MIX(d2i((double)x * 1.25));

        /* f32<->f64 (fpext/fptrunc) */
        MIXD(f2d((float)x * 0.5f));
        MIXF(d2f((double)x * 0.5));

        /* f32 arithmetic + intrinsics */
        float fx = (float)x * 0.5f - 3.0f;
        MIXF(fx + 2.0f);
        MIXF(fx * 1.5f);
        MIXF(fx - 0.25f);
        if (fx != 0.0f) MIXF(10.0f / fx);
        MIXF(__builtin_fabsf(fx));            /* llvm.fabs.f32 (inlined) */
        MIXF(__builtin_copysignf(2.0f, fx));  /* llvm.copysign.f32 (inlined) */
        /* NOTE: sqrtf/fmaf lower to libcalls (math runtime), not translator
         * intrinsics — out of scope for translator conformance; covered by the
         * f64 e2e suite + cvm-cc's auto-linked soft runtime. */
        MIX(fx > 0.0f);            /* fcmp */
        MIX(fx <= 1.0f);

        /* f64 arithmetic + intrinsics (soft-f64 runtime) */
        double dx = (double)x * 0.25 + 1.0;
        MIXD(dx + 2.0);
        MIXD(dx * 1.5);
        if (dx != 0.0) MIXD(100.0 / dx);
        MIXD(__builtin_fabs(dx));             /* llvm.fabs.f64 (inlined) */
        MIX(dx >= 0.0);
    }

    #undef MIX
    #undef MIXF
    #undef MIXD
    return (int)h;
}
