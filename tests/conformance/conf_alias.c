/* conf_alias.c — conformance slice: GlobalAlias resolution.
 *
 * A real libc (picolibc) defines many entry points as aliases via
 * __strong_reference / __weak_reference — e.g. `free` aliases `__malloc_free`,
 * `sbrk` aliases `__fallback_sbrk`. In LLVM IR that is a GlobalAlias, and a
 * direct `call @free` carries the alias (not a Function) as its callee; taking
 * `&free` yields an alias operand. The translator must peel a GlobalAlias to
 * its aliasee so the normal direct-call / function-address paths apply.
 *
 * This fixture builds the same shapes with __attribute__((alias(...))), with no
 * libc dependency: a called alias, an alias-to-alias chain, and an alias taken
 * as a function pointer. Differential vs the host (clang resolves aliases too).
 */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* The real implementations. */
static int NOINLINE impl_add(int a, int b) { return a + b; }
static int NOINLINE impl_mul(int a, int b) { return a * b; }

/* Direct aliases to them (-> GlobalAlias in the IR). The aliasee must have a
 * matching prototype; the alias is declared with the same signature. */
extern int alias_add(int, int) __attribute__((alias("impl_add")));
extern int alias_mul(int, int) __attribute__((alias("impl_mul")));
/* An alias OF an alias — exercises chain peeling. */
extern int alias_add2(int, int) __attribute__((alias("alias_add")));

typedef int (*binop)(int, int);

/* Opaque so clang can't fold the indirect call back to a direct one. */
static binop NOINLINE pick(int which) {
    return which ? alias_mul : alias_add2;   /* function-pointer to aliases */
}

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    /* direct calls through aliases */
    MIX(alias_add(3, 4));        /* 7  via impl_add */
    MIX(alias_mul(6, 7));        /* 42 via impl_mul */
    MIX(alias_add2(10, 20));     /* 30 via the alias-to-alias chain */

    /* alias used as a function-pointer value, then called indirectly */
    binop f = pick(0);
    binop g = pick(1);
    MIX(f(100, 23));             /* 123 */
    MIX(g(9, 9));                /* 81  */

    /* fold a small loop so the pointers genuinely flow at runtime */
    int acc = 0;
    for (int i = 0; i < 5; ++i) {
        binop op = pick(i & 1);
        acc = op(acc + 1, i + 2);
    }
    MIX(acc);

    #undef MIX
    return (int)h;
}
