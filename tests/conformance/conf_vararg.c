/* conf_vararg.c — conformance slice: varargs (llvm.va_start/va_arg/va_end).
 * Pulls int / int64 / double through one va_list. The vararg ABI differs
 * between host-x64 and VM-i386, but each target uses its own correct ABI and
 * the LOGICAL result is identical, so the differential is valid. (No pointers
 * or narrow types in va_arg — those promote / are pointer-size-dependent.) */
#include <stdint.h>
#include <stdarg.h>

#define NOINLINE __attribute__((noinline))

/* sum `n` ints */
static int32_t NOINLINE vsum_i(int32_t n, ...) {
    va_list ap; va_start(ap, n);
    int32_t s = 0;
    for (int32_t i = 0; i < n; ++i) s += va_arg(ap, int32_t);
    va_end(ap);
    return s;
}
/* interpret a tiny tag string: 'i' int32, 'l' int64, 'd' double(*1000) */
static int32_t NOINLINE vmix(const char *tags, ...) {
    va_list ap; va_start(ap, tags);
    int32_t acc = 0;
    for (const char *t = tags; *t; ++t) {
        if (*t == 'i')      acc = acc * 3 + va_arg(ap, int32_t);
        else if (*t == 'l') { int64_t v = va_arg(ap, int64_t); acc = acc * 3 + (int32_t)(v ^ (v >> 32)); }
        else if (*t == 'd') { double v = va_arg(ap, double); acc = acc * 3 + (int32_t)(v * 1000.0); }
    }
    va_end(ap);
    return acc;
}

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    MIX(vsum_i(0));
    MIX(vsum_i(1, 42));
    MIX(vsum_i(5, 1, 2, 3, 4, 5));
    MIX(vsum_i(8, -1, -2, -3, -4, 100, 200, 300, 400));
    MIX(vmix("iii", 7, -8, 9));
    MIX(vmix("ldl", (int64_t)0x1122334455667788LL, 2.5, (int64_t)-1));
    MIX(vmix("didi", 1.5, 10, -3.25, -20));
    MIX(vmix("llll", (int64_t)1, (int64_t)-1, (int64_t)0x7fffffffffffffffLL, (int64_t)0x8000000000000000LL));

    #undef MIX
    return (int)h;
}
