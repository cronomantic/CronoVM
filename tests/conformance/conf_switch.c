/* conf_switch.c — conformance slice: switch lowering. Dense ranges (jump
 * table), sparse/scattered cases (compare chain or sparse table), default,
 * negative case values, and fallthrough. Differential vs native. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* dense 0..7 -> jump table */
static int32_t NOINLINE dense(int32_t x) {
    switch (x) {
        case 0: return 11;
        case 1: return 22;
        case 2: return 33;
        case 3: return 44;
        case 4: return 55;
        case 5: return 66;
        case 6: return 77;
        case 7: return 88;
        default: return -1;
    }
}
/* sparse + negative + fallthrough */
static int32_t NOINLINE sparse(int32_t x) {
    int32_t acc = 0;
    switch (x) {
        case -100: acc += 1;          /* fallthrough */
        case 5:    acc += 2; break;
        case 1000: acc += 4; break;
        case -7:   acc += 8; break;
        case 65536: acc += 16; break;
        default:   acc += 32; break;
    }
    return acc;
}

/* dense range WITH GAPS + an unreachable default. clang -O1 builds a lookup
 * table indexed by (sel) and fills the absent in-range slots (1,2) with POISON
 * (reaching them is UB). This reproduces the @switch.table.* shape that UQM
 * comm dialogue emits (e.g. AngryHomeArilou = [i32 40, poison, poison, ...]),
 * exercising the translator's poison-in-initialiser serialisation. The caller
 * passes ONLY covered values, so the poison slots are never read. */
static int32_t NOINLINE poison_tab(unsigned sel) {
    switch (sel) {
        case 0: return 40;
        case 3: return 46;
        case 4: return 48;
        case 5: return 50;
        case 6: return 52;
        case 7: return 54;
        default: __builtin_unreachable();
    }
}

static volatile int32_t probes[16] = {
    -100, -7, 0, 1, 2, 3, 5, 6, 7, 8, 42, 1000, 65536, 65537, -1, 99
};
/* only the covered cases of poison_tab (never the poison slots 1,2) */
static volatile unsigned ptab_sel[6] = { 0, 3, 4, 5, 6, 7 };

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)
    for (int i = 0; i < 16; ++i) {
        MIX(dense(probes[i]));
        MIX(sparse(probes[i]));
    }
    for (int i = 0; i < 6; ++i)
        MIX(poison_tab(ptab_sel[i]));
    #undef MIX
    return (int)h;
}
