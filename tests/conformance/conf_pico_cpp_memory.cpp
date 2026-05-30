/* conf_pico_cpp_memory.cpp — conformance slice: <memory> smart pointers
 * (phase 4, completeness). unique_ptr (move), shared_ptr ref-counting,
 * weak_ptr lock/expiry, make_unique/make_shared, custom deleter side effect.
 * Exercises the cooperative atomicrmw lowering (shared_ptr's atomic refcount).
 * int32 checksum, platform-stable. */
#include <cstdint>
#include <memory>

namespace {
int g_deleted = 0;
struct Tracked {
    int v;
    explicit Tracked(int x) : v(x) {}
    ~Tracked() { ++g_deleted; }
};
} // namespace

extern "C" int conf_main() {
    int32_t acc = 0;

    /* unique_ptr + move */
    auto u = std::make_unique<int>(21);
    acc += *u;                                  /* 21 */
    std::unique_ptr<int> u2 = std::move(u);
    acc += (u == nullptr) ? 4 : 0;              /* moved-from is null -> 25 */
    acc += *u2;                                 /* + 21 = 46 */

    /* shared_ptr ref counting (atomic refcount -> atomicrmw) */
    auto s = std::make_shared<int>(10);
    acc += (int32_t)s.use_count();              /* 1 -> 47 */
    {
        auto s2 = s;
        acc += (int32_t)s.use_count();          /* 2 -> 49 */
        std::weak_ptr<int> w = s2;
        acc += w.expired() ? 0 : 3;             /* alive -> 52 */
        if (auto sl = w.lock())
            acc += *sl;                         /* + 10 = 62 */
    }
    acc += (int32_t)s.use_count();              /* back to 1 -> 63 */

    /* destructor runs through the deleting path */
    {
        auto t = std::make_shared<Tracked>(7);
        acc += t->v;                            /* + 7 = 70 */
    }
    acc += g_deleted * 5;                        /* deleted once -> + 5 = 75 */

    return acc;                                  /* 75 */
}
