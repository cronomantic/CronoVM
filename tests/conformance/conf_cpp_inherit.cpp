/* conf_cpp_inherit.cpp — conformance slice: C++ inheritance codegen beyond the
 * single-base case. Multiple inheritance (this-pointer adjustment when casting
 * to a non-primary base + calling its virtuals), and virtual (diamond)
 * inheritance (shared virtual base, vbase offsets, adjustor thunks). These are
 * the parts of the Itanium C++ ABI that the single-inheritance OO fixture does
 * NOT exercise. Differential vs native; -fno-exceptions -fno-rtti. */
#include <stdint.h>

namespace {

struct A { int32_t a; A(int32_t x):a(x){} virtual int32_t fa() const { return a; } virtual ~A(){} };
struct B { int32_t b; B(int32_t x):b(x){} virtual int32_t fb() const { return b * 2; } virtual ~B(){} };

/* multiple inheritance: a D* must be adjusted to a B* before B's vtable call */
struct D : A, B {
    int32_t d;
    D(int32_t x):A(x), B(x+1), d(x+2) {}
    int32_t fa() const override { return a + d; }
    int32_t fb() const override { return b + d; }
};

/* virtual (diamond) inheritance: V is a shared virtual base of L and R, joined
 * in Dia — one V subobject, reached through vbase offsets. */
struct V { int32_t v; V(int32_t x):v(x){} virtual int32_t fv() const { return v; } virtual ~V(){} };
struct L : virtual V { int32_t l; L(int32_t x):V(x), l(x+10){} int32_t fv() const override { return v + l; } };
struct R : virtual V { int32_t r; R(int32_t x):V(x), r(x+20){} };
struct Dia : L, R { int32_t k; Dia(int32_t x):V(x), L(x), R(x), k(x+30){} int32_t fv() const override { return v + l + r + k; } };

static int32_t call_a(const A &x) { return x.fa(); }
static int32_t call_b(const B &x) { return x.fb(); }
static int32_t call_v(const V &x) { return x.fv(); }

} // namespace

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    for (int32_t i = 1; i <= 10; ++i) {
        D dd(i);
        /* call via each base subobject — exercises this-adjustment */
        MIX(call_a(dd));            /* A* == D* (primary)  */
        MIX(call_b(dd));            /* B* needs +offsetof(B) adjustment */
        A *pa = &dd; B *pb = &dd;
        MIX(pa->fa()); MIX(pb->fb());
        /* The B-base byte offset is pointer-size dependent (4-byte vptr on the
         * i386 VM vs 8-byte on the x64 oracle), so it is NOT differential-safe;
         * the *adjustment having happened* is proven by pb->fb() returning b+d
         * (reads B's fields through the adjusted this), not by the raw offset. */
        MIX((int32_t)(pb != 0 && (void *)pb != (void *)pa));  /* adjusted -> distinct, 1 */

        Dia x(i);
        MIX(call_v(x));             /* V reached via virtual-base offset */
        L *pl = &x; R *pr = &x;
        MIX(pl->fv());              /* L's override of the shared V */
        MIX((int32_t)(pl->v == pr->v));  /* same single V subobject -> 1 */
    }

    #undef MIX
    return (int)h;
}
