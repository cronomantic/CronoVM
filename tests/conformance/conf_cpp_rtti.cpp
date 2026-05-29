/* conf_cpp_rtti.cpp — conformance slice: RTTI (dynamic_cast). Compiled WITH
 * RTTI (cvm-cc no longer forces -fno-rtti). Exercises the cast outcomes a real
 * engine relies on (e.g. Exult's gump/UI hierarchy): successful downcast,
 * failed downcast (wrong dynamic type -> null), cast through single and
 * multiple inheritance, and cross-cast (sidecast) between sibling bases. The
 * differential oracle is the host's real libc++abi __dynamic_cast, so any
 * divergence in our implementation is caught. typeid is intentionally NOT used
 * (unsupported; no in-scope engine needs it). -fno-exceptions. */
#include <stdint.h>

namespace {
struct Base { int32_t b; Base() : b(1) {} virtual int32_t id() const { return 0; } virtual ~Base() {} };
struct D1 : Base { int32_t x; D1() : x(10) {} int32_t id() const override { return 1; } };
struct D2 : Base { int32_t y; D2() : y(20) {} int32_t id() const override { return 2; } };
/* multiple inheritance + a cross-cast target */
struct Iface { int32_t i; Iface() : i(100) {} virtual int32_t tag() const { return 7; } virtual ~Iface() {} };
struct Multi : Base, Iface { int32_t z; Multi() : z(30) {} int32_t id() const override { return 3; } };

/* return a checksum of dynamic_cast results so the differential compares one int */
static int32_t probe(Base *p) {
    uint32_t h = 0;
    #define ADD(v) h = h * 31u + (uint32_t)(int32_t)(v)
    D1 *d1 = dynamic_cast<D1 *>(p);   ADD(d1 ? d1->x : -1);
    D2 *d2 = dynamic_cast<D2 *>(p);   ADD(d2 ? d2->y : -1);
    Multi *m = dynamic_cast<Multi *>(p);  ADD(m ? m->z : -1);
    /* cross-cast: Base* -> Iface* (sibling base of Multi), only valid if the
     * dynamic type actually derives from Iface */
    Iface *f = dynamic_cast<Iface *>(p);  ADD(f ? f->tag() + f->i : -1);
    ADD(p->id());
    #undef ADD
    return (int32_t)h;
}
} // namespace

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    /* exercise each dynamic type through a Base* (built at runtime so the
     * dynamic type isn't constant-folded away) */
    for (int32_t i = 0; i < 8; ++i) {
        Base *p;
        switch (i & 3) {
            case 0: p = new Base(); break;
            case 1: p = new D1();   break;
            case 2: p = new D2();   break;
            default: p = new Multi(); break;
        }
        MIX(probe(p));
        delete p;
    }

    #undef MIX
    return (int)h;
}
