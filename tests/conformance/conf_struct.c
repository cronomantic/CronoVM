/* conf_struct.c — conformance slice: aggregates. Struct pass-by-value (small =
 * register-coerced, large = byval/memory), return-by-value (sret), nested
 * structs, array members, and per-field access. Only explicit fields are
 * hashed (never raw struct bytes — padding is indeterminate and would differ
 * host-vs-VM as a false miscompile). Differential vs native. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

typedef struct { int32_t x, y; } P2;          /* small: 8 bytes */
typedef struct { int32_t a, b, c, d, e; } Big; /* larger: 20 bytes -> byval/memory */
typedef struct { P2 lo, hi; int32_t tag; } Nest;
typedef struct { int16_t v[6]; } Arr;

static P2  NOINLINE p2_add(P2 a, P2 b) { P2 r = { a.x + b.x, a.y + b.y }; return r; }
static int32_t NOINLINE big_sum(Big g) { return g.a + g.b + g.c + g.d + g.e; }
static Big NOINLINE big_scale(Big g, int32_t k) {
    Big r = { g.a*k, g.b*k, g.c*k, g.d*k, g.e*k }; return r;
}
static int32_t NOINLINE nest_reduce(Nest n) {
    return (n.lo.x - n.lo.y) * n.tag + (n.hi.x + n.hi.y);
}
static int32_t NOINLINE arr_sum(Arr a) {
    int32_t s = 0; for (int i = 0; i < 6; ++i) s += a.v[i]; return s;
}

static volatile int32_t seeds[8] = { 0, 1, -1, 3, -7, 100, -250, 4096 };

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    for (int i = 0; i < 8; ++i) {
        int32_t s = seeds[i];

        P2 a = { s, s + 1 }, b = { 10, -3 };
        P2 c = p2_add(a, b);
        MIX(c.x); MIX(c.y);

        Big g = { s, s+1, s+2, s+3, s+4 };
        MIX(big_sum(g));
        Big sg = big_scale(g, 3);
        MIX(sg.a); MIX(sg.c); MIX(sg.e);

        Nest n = { { s, s/2 }, { 7, -7 }, (int16_t)(i + 1) };
        MIX(nest_reduce(n));

        Arr ar; for (int k = 0; k < 6; ++k) ar.v[k] = (int16_t)(s + k * 11);
        MIX(arr_sum(ar));
    }

    #undef MIX
    return (int)h;
}
