/* conf_mem.c — conformance slice: the memory intrinsics clang emits
 * (llvm.memcpy/memset/memmove) at a range of sizes + alignments, plus the
 * overlapping case that distinguishes memmove from memcpy. Hashes the whole
 * resulting buffer. Differential vs native. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

static void NOINLINE do_memset(uint8_t *p, int v, int n) { __builtin_memset(p, v, n); }
static void NOINLINE do_memcpy(uint8_t *d, const uint8_t *s, int n) { __builtin_memcpy(d, s, n); }
static void NOINLINE do_memmove(uint8_t *d, const uint8_t *s, int n) { __builtin_memmove(d, s, n); }

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    uint8_t buf[64];

    /* memset across sizes 0..40 with varying fill */
    for (int n = 0; n <= 40; ++n) {
        for (int i = 0; i < 64; ++i) buf[i] = (uint8_t)(i + 1);
        do_memset(buf, 0xA5 + n, n);
        for (int i = 0; i < 64; ++i) MIX(buf[i]);
    }

    /* memcpy across sizes + a non-zero source offset (alignment variety) */
    uint8_t src[64];
    for (int i = 0; i < 64; ++i) src[i] = (uint8_t)(i * 7 + 3);
    for (int n = 0; n <= 32; ++n) {
        for (int off = 0; off < 4; ++off) {
            for (int i = 0; i < 64; ++i) buf[i] = 0;
            do_memcpy(buf + off, src + off, n);
            for (int i = 0; i < 64; ++i) MIX(buf[i]);
        }
    }

    /* memmove with forward + backward overlap */
    for (int i = 0; i < 64; ++i) buf[i] = (uint8_t)(i + 100);
    do_memmove(buf + 4, buf, 32);          /* dst > src, overlapping */
    for (int i = 0; i < 64; ++i) MIX(buf[i]);
    for (int i = 0; i < 64; ++i) buf[i] = (uint8_t)(i + 100);
    do_memmove(buf, buf + 4, 32);          /* dst < src, overlapping */
    for (int i = 0; i < 64; ++i) MIX(buf[i]);

    #undef MIX
    return (int)h;
}
