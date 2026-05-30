/* conf_cxx_config.cpp — conformance slice: libc++ HEADERS are wired in.
 *
 * This is the libc/libc++ phase-4a smoke test. It proves the toolchain wiring
 * end-to-end: cvm-cc compiles a C++ TU with the toolchain's libc++ headers
 * (-stdlib=libc++) under CronoVM's freestanding <__config_site> +
 * <__external_threading> overrides (in runtime/lib), and the result translates
 * and runs on the VM with the SAME value as the native oracle.
 *
 * Deliberately CONFIG-ONLY: it includes the libc++ headers that depend only on
 * <__config_site> (no C library, no STL containers, no exceptions, no
 * allocation). The STL containers (vector/string/map) + smart pointers need
 * the std exception runtime in cvm_cxxrt (std::__throw_*) — that is phase 4b/4c
 * and gets its own fixtures. Here we only assert that the headers PARSE under
 * our config and that compile-time facts evaluate identically on VM and host.
 *
 * extern "C" conf_main (unmangled entry); fixed-width int32 checksum so the
 * 32-bit VM and 64-bit host agree bit-for-bit. */
#include <cstddef>
#include <type_traits>
#include <version>
#include <stdint.h>

namespace {

/* A tiny compile-time computation routed through libc++'s type traits, so the
 * checksum actually exercises the headers (not just their presence). */
template <typename T>
struct width_score : std::integral_constant<int32_t, (int32_t)sizeof(T) * 8> {};

} // namespace

extern "C" int conf_main() {
    int32_t acc = 0;

    /* Trait results are platform-independent booleans. */
    acc += std::is_integral<int32_t>::value          ? 1  : 0;
    acc += std::is_floating_point<double>::value      ? 2  : 0;
    acc += std::is_signed<int32_t>::value             ? 4  : 0;
    acc += std::is_unsigned<uint32_t>::value          ? 8  : 0;
    acc += std::is_pointer<void*>::value              ? 16 : 0;
    acc += std::is_const<const int>::value            ? 32 : 0;
    acc += std::is_same<std::size_t, std::size_t>::value ? 64 : 0;
    acc += std::is_same<int, long>::value             ? 128 : 0;  /* false */

    /* std::conditional / remove_cv exercised at compile time. */
    using picked = std::conditional<true, int32_t, double>::type;
    acc += (int32_t)sizeof(picked);                              /* 4 */

    /* integral_constant arithmetic through the trait above. */
    acc += width_score<int32_t>::value;                          /* 32 */
    acc += width_score<uint16_t>::value;                         /* 16 */

    /* __cpp_* feature-test macros from <version> are compile-time constants;
     * fold one deterministically (presence -> fixed contribution, not its
     * value, so it stays identical across libc++ versions). */
#ifdef __cpp_lib_integer_sequence
    acc += 256;
#endif

    return acc;
}
