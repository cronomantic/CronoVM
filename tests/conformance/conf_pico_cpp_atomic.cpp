/* conf_pico_cpp_atomic.cpp — conformance slice: std::atomic (phase 4, cmpxchg).
 * Exercises atomic load/store, fetch_add (atomicrmw) and compare_exchange
 * (cmpxchg) success + failure, for both int and pointer. Under the cooperative
 * VM these lower to plain load/op/store + a branchless conditional store.
 * int32 checksum, platform-stable. */
#include <atomic>
#include <cstdint>

extern "C" int conf_main() {
    int32_t acc = 0;

    std::atomic<int> a{10};
    acc += a.load();                       /* 10 */
    a.store(20);
    acc += a.load();                       /* + 20 = 30 */
    acc += a.fetch_add(5);                 /* returns old 20 -> + 20 = 50; a = 25 */
    acc += a.load();                       /* + 25 = 75 */

    int expected = 25;
    bool ok = a.compare_exchange_strong(expected, 100);  /* a==25 -> true, a=100 */
    acc += ok ? 3 : 0;                     /* + 3 = 78 */
    acc += a.load();                       /* + 100 = 178 */

    int wrong = 999;
    bool ok2 = a.compare_exchange_strong(wrong, 200);    /* a!=999 -> false */
    acc += ok2 ? 0 : 7;                    /* + 7 = 185 */
    acc += (wrong == 100) ? 2 : 0;         /* CAS failure loads actual -> + 2 = 187 */

    /* pointer compare-exchange */
    int x = 1, y = 2;
    std::atomic<int*> p{&x};
    int* ep = &x;
    acc += p.compare_exchange_strong(ep, &y) ? 5 : 0;    /* &x -> true, p=&y, + 5 = 192 */
    acc += (p.load() == &y) ? 8 : 0;       /* + 8 = 200 */

    return acc;                            /* 200 */
}
