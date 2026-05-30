/* conf_pico_cpp_vector.cpp — conformance slice: libc++ std::vector on the VM.
 *
 * libc/libc++ phase 4b. Exercises a real STL container end-to-end on CronoVM:
 *   - std::vector<int> growth (push_back -> reallocation via operator new ->
 *     the cart allocator) + range-for iteration,
 *   - the std exception path: vector::at() out of range throws std::out_of_range,
 *     caught BY BASE reference (std::exception&). This validates that the
 *     out-of-line std exception hierarchy in cvm_cxxstl.cpp (auto-linked by
 *     cvm-cc) links AND that throw -> unwind -> catch-by-base works through
 *     cvm_cxxrt's setjmp/longjmp unwinder + RTTI.
 *
 * conf_pico* name => the runner links picolibc.bc + pico_machine.c (malloc/
 * memcpy/strlen/free) with picolibc's C headers below libc++ (-idirafter).
 * Native oracle uses host libc++; the int32 checksum is platform-independent. */
#include <cstdint>
#include <stdexcept>
#include <vector>

extern "C" int conf_main() {
    int32_t acc = 0;

    std::vector<int> v;
    for (int i = 1; i <= 10; ++i)
        v.push_back(i);

    for (int x : v)
        acc += x;                 /* 1..10 = 55 */
    acc += (int32_t)v.size();     /* + 10 = 65 */

    /* Force a reallocation past the initial capacity and read back. */
    v.reserve(64);
    acc += (int32_t)(v.capacity() >= 64 ? 3 : 0);   /* + 3 = 68 */

    /* The std exception path, end-to-end: at() out of range -> std::out_of_range
     * -> caught by std::exception& (base). */
    bool caught = false;
    try {
        acc += v.at(99);          /* throws; not added */
        acc += 1000;              /* unreachable */
    } catch (const std::exception&) {
        caught = true;
    }
    acc += caught ? 7 : 0;        /* + 7 = 75 */

    return acc;                   /* 75 */
}
