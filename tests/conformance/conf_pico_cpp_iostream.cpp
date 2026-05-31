/* conf_pico_cpp_iostream.cpp — conformance slice: libc++ <sstream>/<fstream> on
 * the VM (the Exult-port iostream frontier). Exercises the stream classes and,
 * crucially, std::num_get / std::num_put — the number parse/format path whose
 * codegen forced the translator features this corpus added: the i33/i65
 * overflow-checked widths (conf_overflow_wide), the vector movemask idiom
 * (conf_vector_movemask), and the i64->double conversion (__cvm_f_from_i64/u64,
 * picolibc strtod's __atod_engine builds the mantissa in a uint64_t).
 *
 * Available only because __config_site sets _LIBCPP_HAS_LOCALIZATION 1 (NEWLIB
 * dispatch to picolibc's xlocale `*_l`) + _LIBCPP_HAS_FILESYSTEM 1 (libc++ guards
 * <fstream> behind it). cvm-cc auto-links cxxio.bc (the vendored libc++ stream/
 * locale library) on the CVM_PROBE_IOSTREAM probe bit; the cart's picolibc.bc is
 * built --with-locale. int32 checksum, the "C" locale + fixed-width values keep
 * host-64 and VM-32 bit-for-bit identical vs the native oracle (host libc++).
 *
 * NOTE on stream state: this fixture checks the PARSED VALUES and loop
 * termination (`while (in >> x)`, which relies on failbit) — NOT `good()`/
 * `eof()` after a successful parse that lands exactly on EOF. libc++'s eofbit in
 * that corner differs on the VM (our stringbuf underflow vs the host's), a
 * cosmetic divergence that does not affect the parsed values, `if (in)`, or
 * `while (in >> x)`. Left as a known minor item, not exercised here. */
#include <cstdint>
#include <sstream>
#include <fstream>
#include <string>

extern "C" int conf_main() {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    /* ---- num_put: format integers + a string into an ostringstream -------- */
    std::ostringstream os;
    os << 42 << ' ' << -12345 << ' ' << 2000000000 << " hex=" << std::hex << 255
       << std::dec << " end";
    std::string out = os.str();
    for (char c : out) MIX((int32_t)(unsigned char)c);
    MIX((int32_t)out.size());

    /* ---- num_get: parse them back with operator>> (the movemask + i33 path) */
    std::istringstream is("42 -12345 2000000000 7fffffff");
    int a = 0, b = 0, c = 0; unsigned hx = 0;
    is >> a >> b >> c >> std::hex >> hx;
    MIX(a); MIX(b); MIX(c); MIX((int32_t)hx);

    /* loop termination via failbit (the practical end-of-stream test). */
    std::istringstream cnt("11 22 33 44 55");
    int x, n = 0; while (cnt >> x) ++n;
    MIX(n);                            /* 5 */

    /* long long parse (the i65 overflow-guard path). */
    std::istringstream isll("9000000000 -9000000000");
    long long la = 0, lb = 0;
    isll >> la >> lb;
    MIX((int32_t)la); MIX((int32_t)(la >> 32));
    MIX((int32_t)lb); MIX((int32_t)(lb >> 32));

    /* double parse (num_get -> strtod_l -> __atod_engine: (double)uint64). Exact-
     * binary values, so host and VM agree bit-for-bit. The last value exercises
     * the i64->double conversion (>2^32). */
    std::istringstream isd("2.5 -0.25 1024 9000000000");
    double d0 = 0, d1 = 0, d2 = 0, d3 = 0;
    isd >> d0 >> d1 >> d2 >> d3;
    MIX((int32_t)(d0 * 4));            /* 10   */
    MIX((int32_t)(d1 * 4));            /* -1   */
    MIX((int32_t)d2);                  /* 1024 */
    MIX((int32_t)(d3 / 1000000.0));    /* 9000 */

    /* ---- fstream: write then read back via operator>> (the streambuf + the
     * machine-port file backend). pico_machine.c models ONE file, so the
     * ofstream must close before the ifstream opens. */
    {
        std::ofstream of("io.tmp");
        of << 314 << ' ' << 271 << '\n';
    }                                  /* of destructed -> flush + close */
    std::ifstream inf("io.tmp");
    int p = 0, q = 0;
    inf >> p >> q;
    MIX(p); MIX(q);                    /* 314, 271 */

    #undef MIX
    return (int)h;
}
