/* conf_pico_cpp_map.cpp — conformance slice: libc++ std::map (phase 4d).
 * Red-black tree: insert, operator[], find, iteration order, count, at() throw.
 * int32 checksum, platform-stable. */
#include <cstdint>
#include <map>
#include <stdexcept>

extern "C" int conf_main() {
    int32_t acc = 0;

    std::map<int, int> m;
    for (int i = 0; i < 6; ++i)
        m[i] = i * i;             /* 0,1,4,9,16,25 */

    for (auto& kv : m)            /* in-order traversal */
        acc += kv.second;         /* sum = 55 */
    acc += (int32_t)m.size();     /* + 6 = 61 */

    acc += (m.find(3) != m.end()) ? m.find(3)->second : 0;   /* 9 -> 70 */
    acc += (int32_t)m.count(10);  /* 0 */
    acc += m.at(4);               /* 16 -> 86 */

    bool caught = false;
    try {
        (void)m.at(99);           /* missing key -> std::out_of_range */
    } catch (const std::out_of_range&) {
        caught = true;
    }
    acc += caught ? 9 : 0;        /* + 9 = 95 */

    return acc;                   /* 95 */
}
