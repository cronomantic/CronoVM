/* conf_cpp_ptr.cpp — conformance slice: pointers to members.
 *   - Pointer-to-data-member, incl. an array of them: o.*pmd.
 *   - Member-function-pointer TABLE built at runtime and called through a
 *     helper: non-virtual entries + a virtual entry. This is the pattern that
 *     regressed before the function-pointer 2-alignment fix (an odd function
 *     index was misread as the Itanium pmf virtual flag — see CVM_FN_SLOT).
 * Differential vs native; -fno-exceptions -fno-rtti.
 *
 * TOOLCHAIN NOTE: kept deliberately conservative. clang folds pmf *constants*
 * differently across versions — a const-global pmf array, or a ternary SELECT
 * between two pmf constants, can emit IR shapes the translator doesn't
 * materialise ("operand has no register assigned") on some clang releases. So
 * this fixture only uses explicit runtime stores + helper calls, which are
 * stable across toolchains. (The translator does handle those folded shapes on
 * the clang it was developed against; they are simply not portable enough to
 * assert in CI.) */
#include <stdint.h>

namespace {
struct Obj {
    int32_t x, y;
    Obj(int32_t a, int32_t b) : x(a), y(b) {}
    int32_t add() const { return x + y; }
    int32_t sub() const { return x - y; }
    int32_t mul() const { return x * y; }
    virtual int32_t scale() const { return x * 3; }
    virtual ~Obj() {}
};
typedef int32_t (Obj::*Pmf)() const;
typedef int32_t Obj::*Pmd;
static int32_t call_pmf(const Obj &o, Pmf f) { return (o.*f)(); }
static int32_t get_pmd(const Obj &o, Pmd m) { return o.*m; }
} // namespace

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    Pmd md[2]; md[0] = &Obj::x; md[1] = &Obj::y;
    Pmf nv[3]; nv[0] = &Obj::add; nv[1] = &Obj::sub; nv[2] = &Obj::mul;  /* non-virtual table */
    Pmf vf = &Obj::scale;                                                /* virtual scalar */

    for (int32_t i = 0; i < 12; ++i) {
        Obj o(i * 3, i - 4);
        for (int k = 0; k < 2; ++k) MIX(get_pmd(o, md[k]));   /* o.*pmd */
        for (int k = 0; k < 3; ++k) MIX(call_pmf(o, nv[k]));  /* non-virtual pmf table (the fix) */
        MIX(call_pmf(o, vf));                                 /* virtual pmf */
    }

    #undef MIX
    return (int)h;
}
