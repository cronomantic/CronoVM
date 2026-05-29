/* conf_ctrl.c — conformance slice: control flow that stresses the register
 * allocator + phi handling (the lost-copy / phi-isolation bug class): nested
 * loops, phi nodes live across loop exits, early break/continue, short-circuit
 * && / ||, and a goto-based state loop. Differential vs native. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* nested loops with a value carried across the exit edge (phi at loop exit) */
static int32_t NOINLINE nested(int32_t n) {
    int32_t acc = 0, last = -1;
    for (int32_t i = 0; i < n; ++i) {
        for (int32_t j = 0; j <= i; ++j) {
            if (((i + j) & 3) == 0) continue;
            acc += i * j - j;
            last = i + j;            /* `last` is a phi live after both loops */
            if (acc > 100000) break;
        }
        if (last > 50) acc -= 1;
    }
    return acc + last;               /* uses the loop-exit phi */
}

/* short-circuit && / || with side-effecting accumulation */
static int32_t NOINLINE shortcircuit(int32_t a, int32_t b, int32_t c) {
    int32_t s = 0;
    if (a > 0 && b > 0 && (s += 1, c > 0)) s += 10;
    if (a < 0 || b < 0 || (s += 2, c < 0)) s += 20;
    return s;
}

/* goto-driven state machine */
static int32_t NOINLINE statem(int32_t x) {
    int32_t s = 0, steps = 0;
s0: if (steps++ > 40) return s;
    s += 1; if (x & 1) { x >>= 1; goto s1; } else goto s2;
s1: s += 3; x += 1; if (x > 3) goto s2; else goto s0;
s2: s += 7; x -= 2; if (x <= 0) return s; goto s0;
}

static volatile int32_t seeds[8] = { 0, 1, 2, 5, 13, 31, -9, 100 };

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)
    for (int i = 0; i < 8; ++i) {
        int32_t s = seeds[i];
        MIX(nested(s & 63));
        for (int j = 0; j < 8; ++j) MIX(shortcircuit(s, seeds[j], seeds[(j+3)&7]));
        MIX(statem(s));
    }
    #undef MIX
    return (int)h;
}
