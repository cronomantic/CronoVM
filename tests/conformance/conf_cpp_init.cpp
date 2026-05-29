/* conf_cpp_init.cpp — conformance slice: C++ static initialisation. Exercises
 * the parts that need the translator's global-ctor support + the C++ ABI
 * runtime: a NON-foldable global object (its ctor reads a volatile, so it
 * can't be constant-folded -> a real llvm.global_ctors entry that must run
 * before main) and a function-local static (-> __cxa_guard_acquire/release).
 * Differential vs native proves the ctors actually ran (else the global reads
 * back zero and the checksum diverges). Built -fno-exceptions -fno-rtti. */
#include <stdint.h>

extern "C" volatile int32_t g_seed;   /* volatile -> ctor can't fold */

namespace {
struct Counter {
    int32_t v;
    Counter(int32_t s) : v(s * 7 + 1) {}
    int32_t get() const { return v; }
};
Counter g_global((int32_t)g_seed);     /* global object -> global ctor */

int32_t with_static(int32_t x) {
    static Counter local(x + (int32_t)g_seed);   /* local static -> __cxa_guard */
    return local.get();
}
} // namespace

extern "C" volatile int32_t g_seed = 10;

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)
    MIX(g_global.get());                 /* 71 iff the global ctor ran */
    for (int32_t i = 1; i <= 8; ++i)
        MIX(with_static(i));             /* first call inits the static */
    #undef MIX
    return (int)h;
}
