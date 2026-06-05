/* conf_pico_cpp_align — allocator / container alignment.
 *
 * The Exult egg-bug investigation found vector<Usecode_value> element addresses
 * consistently MISALIGNED by 3 (addr & 3 == 3) with a CORRECT element stride (20),
 * i.e. the buffer BASE is misaligned — a systematic off-by-3, not random heap
 * corruption. If `operator new` / malloc (and thus a vector's data buffer) returns
 * non-4-aligned storage, every non-char object placed there is misaligned. This
 * fixture checks that directly: it counts how many allocations / vector buffers are
 * 4-aligned. Native: ALL of them. A VM allocator that returns misaligned storage
 * scores lower -> MISCOMPILE.
 *
 * Only the low alignment BITS are folded (not the absolute pointer), so host-64 and
 * VM-32 agree. conf_pico* => picolibc malloc + vendored libc++. */
#include <cstdint>
#include <new>
#include <vector>

namespace {
/* ~20 bytes, 4-aligned — same shape class as Usecode_value (tag + 12-byte union
 * + bool). */
struct Big {
    long  a;
    char  buf[12];
    bool  b;
};
}

extern "C" int conf_main(void) {
    int32_t acc = 0;

    /* (1) raw operator new across a range of sizes: every result must be at least
     * 4-aligned (in practice libc++ guarantees max_align_t / __STDCPP_DEFAULT_NEW
     * _ALIGNMENT__, but 4 is the minimum we rely on). */
    for (int i = 1; i <= 40; ++i) {
        void* p = ::operator new((size_t)i * 3u + 1u);   /* odd-ish sizes */
        acc += (((uintptr_t)p & 3u) == 0u) ? 1 : 0;
        ::operator delete(p);
    }

    /* (2) a std::vector<Big> buffer must stay aligned across reallocation growth —
     * this is the exact construct that misbehaved (vector of a ~20-byte object). */
    std::vector<Big> v;
    for (int i = 0; i < 60; ++i) {
        v.push_back(Big{(long)i, {}, (i & 1) != 0});
        acc += (((uintptr_t)(const void*)v.data() & 3u) == 0u) ? 1 : 0;
        /* element addresses must also be 4-aligned (base + i*sizeof) */
        acc += (((uintptr_t)(const void*)&v[(size_t)i] & 3u) == 0u) ? 1 : 0;
    }

    /* (3) array new of the 20-byte type. */
    Big* arr = new Big[7];
    for (int i = 0; i < 7; ++i)
        acc += (((uintptr_t)(const void*)&arr[i] & 3u) == 0u) ? 1 : 0;
    delete[] arr;

    return acc;   /* native: 40 + 60*2 + 7 = 167 */
}
