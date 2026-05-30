/* conf_pico_cpp_atomic64.cpp — conformance slice: 64-bit std::atomic (phase 4).
 * Exercises i64 atomicrmw (fetch_add/sub/or/and) and i64 compare_exchange
 * (cmpxchg) success + failure. Values kept small so the int32 checksum is
 * platform-stable after truncation. */
#include <atomic>
#include <cstdint>

extern "C" int conf_main() {
    int32_t acc = 0;

    std::atomic<int64_t> b{100};
    acc += (int32_t)b.load();                       /* 100 */
    acc += (int32_t)b.fetch_add(50);                /* old 100 -> 200; b=150 */
    acc += (int32_t)b.fetch_sub(30);                /* old 150 -> 350; b=120 */
    acc += (int32_t)b.load();                        /* + 120 = 470 */

    int64_t e = 120;
    acc += b.compare_exchange_strong(e, 1000) ? 7 : 0;  /* b==120 -> +7=477; b=1000 */
    acc += (int32_t)b.load();                        /* + 1000 = 1477 */
    int64_t w = 5;
    acc += b.compare_exchange_strong(w, 2000) ? 0 : 9;  /* b!=5 -> +9 = 1486 */
    acc += (w == 1000) ? 3 : 0;                      /* CAS failure loads actual -> 1489 */

    std::atomic<uint64_t> m{0xF0};
    acc += (int32_t)m.fetch_or(0x0F);                /* old 0xF0=240 -> 1729; m=0xFF */
    acc += (int32_t)m.fetch_and(0x3C);               /* old 0xFF=255 -> 1984; m=0x3C */
    acc += (int32_t)m.load();                         /* + 60 = 2044 */

    return acc;                                       /* 2044 */
}
