/* conf_cpp_ptr.cpp — conformance slice: pointers to members (full surface).
 *   - Pointer-to-data-member, incl. an array of them: o.*pmd.
 *   - Member-function-pointer tables: non-virtual, virtual, and MIXED, called
 *     through a helper and inline; selected at runtime.
 *   - A const-global pmf array initializer.
 * Differential vs native; -fno-exceptions -fno-rtti.
 *
 * The member-function-pointer paths exercise the fix that made function-pointer
 * values 2-aligned (even): the Itanium pmf ABI reserves the low bit of the
 * pointer field as the virtual flag, so an odd function index was misread as a
 * virtual member pointer. See CVM_FN_SLOT in the translator. */
#include <stdint.h>

namespace {
struct Obj {
    int32_t x, y;
    Obj(int32_t a, int32_t b) : x(a), y(b) {}
    int32_t add() const { return x + y; }
    int32_t sub() const { return x - y; }
    int32_t mul() const { return x * y; }
    virtual int32_t scale() const { return x * 3; }
    virtual int32_t neg() const { return -y; }
    virtual ~Obj() {}
};
typedef int32_t (Obj::*Pmf)() const;
typedef int32_t Obj::*Pmd;
static int32_t call_pmf(const Obj &o, Pmf f) { return (o.*f)(); }
static int32_t get_pmd(const Obj &o, Pmd m) { return o.*m; }

/* const-global pmf array (mixed virtual + non-virtual) */
static const Pmf kTable[4] = { &Obj::add, &Obj::scale, &Obj::mul, &Obj::neg };
} // namespace

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    Pmd md[2]; md[0] = &Obj::x; md[1] = &Obj::y;
    /* runtime-built mixed table */
    Pmf rt[5]; rt[0]=&Obj::add; rt[1]=&Obj::sub; rt[2]=&Obj::mul; rt[3]=&Obj::scale; rt[4]=&Obj::neg;

    for (int32_t i = 0; i < 12; ++i) {
        Obj o(i * 3, i - 4);
        for (int k = 0; k < 2; ++k) MIX(get_pmd(o, md[k]));        /* pmd */
        for (int k = 0; k < 5; ++k) MIX(call_pmf(o, rt[k]));       /* runtime mixed table */
        for (int k = 0; k < 4; ++k) MIX((o.*kTable[k])());         /* const-global mixed table, inline */
        Pmf sel = (i & 1) ? &Obj::scale : &Obj::add;               /* runtime select, mixed v/nv */
        MIX(call_pmf(o, sel));
        MIX((o.*sel)());
    }

    #undef MIX
    return (int)h;
}
