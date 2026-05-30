/* conf_pico_cpp_string.cpp — conformance slice: libc++ std::string (phase 4c).
 * Exercises SSO + heap growth, concat, find/substr/compare, and the throwing
 * path (substr out_of_range caught by base). int32 checksum, platform-stable. */
#include <cstdint>
#include <stdexcept>
#include <string>

extern "C" int conf_main() {
    int32_t acc = 0;

    std::string s = "abc";        /* small (SSO) */
    s += "defg";                  /* "abcdefg" */
    acc += (int32_t)s.size();     /* 7 */

    std::string big(64, 'x');     /* forces heap allocation */
    acc += (int32_t)big.size();   /* + 64 = 71 */
    acc += (big == std::string(64, 'x')) ? 5 : 0;   /* + 5 = 76 */

    acc += (int32_t)s.find('d');  /* 'd' at index 3 -> + 3 = 79 */
    acc += (int32_t)s.substr(1, 3).size();          /* "bcd" -> + 3 = 82 */
    acc += (s.compare("abcdefg") == 0) ? 4 : 0;     /* + 4 = 86 */

    bool caught = false;
    try {
        (void)s.substr(100);      /* pos > size -> std::out_of_range */
    } catch (const std::exception&) {
        caught = true;
    }
    acc += caught ? 9 : 0;        /* + 9 = 95 */

    return acc;                   /* 95 */
}
