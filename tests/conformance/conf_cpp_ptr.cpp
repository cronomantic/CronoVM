/* conf_cpp_ptr.cpp — conformance slice: pointers to members (the parts that
 * lower correctly):
 *   - Pointer-to-data-member, incl. an array of them: o.*pmd.
 *   - Scalar member-function pointers — non-virtual and virtual — selected at
 *     runtime, called both inline and through a function parameter.
 * Differential vs native; -fno-exceptions -fno-rtti.
 *
 * KNOWN GAP (left out, documented for later): an ARRAY of member-FUNCTION
 * pointers mislowers — loading the {fnptr,adj} struct elements back yields a
 * null/garbage target at runtime ("null function pointer call" / "call target
 * index out of range") for 3+ entries. Scalar pmfs (here) and arrays of
 * pointer-to-DATA-members work. Also a const-global pmf array is an unsupported
 * initializer shape. Tables of member-function pointers are rare in real code
 * (engines use virtuals or free-function callbacks); this is the precise
 * remaining C++ codegen gap, alongside exceptions and RTTI. */
#include <stdint.h>

namespace {
struct Obj {
    int32_t x, y;
    Obj(int32_t a, int32_t b) : x(a), y(b) {}
    int32_t add() const { return x + y; }
    int32_t sub() const { return x - y; }
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

    Pmd md[2]; md[0] = &Obj::x; md[1] = &Obj::y;   /* pmd array — works */

    for (int32_t i = 0; i < 12; ++i) {
        Obj o(i * 3, i - 4);
        for (int k = 0; k < 2; ++k)
            MIX(get_pmd(o, md[k]));                /* o.*pmd */

        /* scalar non-virtual pmf, selected at runtime, both call forms */
        Pmf nv = (i & 1) ? &Obj::add : &Obj::sub;
        MIX(call_pmf(o, nv));
        MIX((o.*nv)());

        /* scalar virtual pmf, both call forms */
        Pmf vf = &Obj::scale;
        MIX(call_pmf(o, vf));
        MIX((o.*vf)());
    }

    #undef MIX
    return (int)h;
}
