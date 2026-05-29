/* conf_cpp_array.cpp — conformance slice: array new/delete + placement new.
 * new[] of a type with a non-trivial destructor stores an array "cookie" (the
 * element count) ahead of the returned pointer so delete[] can run each dtor;
 * this is a distinct codegen path from scalar new. Also covers placement new
 * (construct in provided storage; no allocation). Differential; -fno-exc/-rtti. */
#include <stdint.h>

/* placement new without <new>: declare the placement operator ourselves.
 * __SIZE_TYPE__ is the target's size_t (4 bytes on i386-VM, 8 on the x64 host
 * oracle) so the signature matches the real operator new on both. */
inline void *operator new(__SIZE_TYPE__, void *p) noexcept { return p; }

namespace {
static volatile int32_t g_dtor_sum;     /* observable dtor side effect */

struct Item {
    int32_t v;
    Item() : v(0) {}
    void set(int32_t x) { v = x; }
    ~Item() { g_dtor_sum += v; }         /* non-trivial dtor -> array cookie */
};
} // namespace

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    for (int32_t n = 1; n <= 8; ++n) {
        g_dtor_sum = 0;
        Item *arr = new Item[n];          /* array new (with cookie) */
        for (int32_t i = 0; i < n; ++i) arr[i].set(i * 3 + n);
        int32_t s = 0;
        for (int32_t i = 0; i < n; ++i) s += arr[i].v;
        MIX(s);
        delete[] arr;                     /* runs n dtors -> g_dtor_sum */
        MIX(g_dtor_sum);

        /* placement new into stack storage (no allocation) */
        alignas(Item) unsigned char buf[sizeof(Item)];
        Item *p = new (buf) Item();
        p->set(n * 100);
        MIX(p->v);
        p->~Item();                       /* explicit dtor for placement */
    }

    #undef MIX
    return (int)h;
}
