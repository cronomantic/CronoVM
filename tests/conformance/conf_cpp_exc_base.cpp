/* conf_cpp_exc_base — catch-BY-BASE across a multi-level hierarchy.
 *
 * Guards the CronoVM EH unwinder's type matcher (cvm_cxxrt.cpp ti_search):
 * `catch (Base&)` must catch a thrown object whose dynamic type DERIVES from
 * Base through one OR MORE intermediate single-inheritance levels, when no
 * closer clause exists. The pre-existing conf_cpp_exc only ever catches by the
 * EXACT thrown type (its catch(E&)/catch(Derived&) are exact matches), so the
 * multi-level base walk went untested — and missed the real bug: a derived
 * throw caught only by a LIBRARY/grand-base (Exult's `file_open_exception :
 * file_exception : exult_exception : std::exception` caught by `std::exception&`)
 * was NOT matched on the VM and went uncaught.
 *
 * Mirrors that shape with a LOCAL hierarchy (no <exception> needed, so it runs
 * in the libc-less conformance harness): a POLYMORPHIC chain (like std::exception
 * which has virtuals) and a NON-polymorphic chain, each caught one and two levels
 * up by base, with a catch(...) sentinel so a miss is observable as a distinct
 * value. Differential vs a native clang++ oracle; the throw depends on the loop
 * variable (no constant-folded throw) and uses int32 + an int32 checksum so
 * host-64 and VM-32 agree. */
#include <stdint.h>

namespace {

/* Polymorphic single-inheritance chain (like std::exception <- ... ). */
struct PolyBase { virtual ~PolyBase() {} int32_t b; };
struct PolyMid  : PolyBase { int32_t m; };
struct PolyLeaf : PolyMid  { int32_t l; };

/* Non-polymorphic single-inheritance chain (no vtables). */
struct FlatBase { int32_t b; };
struct FlatMid  : FlatBase { int32_t m; };
struct FlatLeaf : FlatBase { int32_t m; int32_t l; };  /* 1-level for variety */

/* throw a value whose DYNAMIC type sits at the requested depth below its base.
 * d==1 -> PolyMid (1 level), d==2 -> PolyLeaf (2 levels), d==3 -> FlatLeaf
 * (non-poly, 1 level), else no throw. All payloads depend on `seed`. */
[[gnu::noinline]] static int32_t emit(int32_t d, int32_t seed) {
    if (d == 1) { PolyMid  x; x.b = seed; x.m = seed + 1;                 throw x; }
    if (d == 2) { PolyLeaf x; x.b = seed; x.m = seed + 1; x.l = seed + 2; throw x; }
    if (d == 3) { FlatLeaf x; x.b = seed; x.m = seed + 1; x.l = seed + 2; throw x; }
    return seed * 2;
}

/* propagate THROUGH a frame with no try of its own (the Exult member-init case:
 * the throw crosses frames before any handler). */
[[gnu::noinline]] static int32_t relay(int32_t d, int32_t seed) {
    return emit(d, seed) + 1;
}

}  /* anon */

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    /* 1: catch a derived throw ONLY by its (grand)base — the bug. */
    for (int32_t i = 0; i <= 4; ++i) {
        int32_t d = i % 4;            /* 0,1,2,3,0 */
        int32_t r;
        try { r = emit(d, i + 10); }
        catch (PolyBase& e) { r = 1000 + e.b; }   /* catches PolyMid (1) & PolyLeaf (2) by base */
        catch (FlatBase& e) { r = 2000 + e.b; }   /* catches FlatLeaf by base */
        catch (...)         { r = -777; }         /* a MISS lands here */
        MIX(r);
    }

    /* 2: same, but the exception crosses a frame with no try (member-init shape). */
    for (int32_t i = 0; i <= 4; ++i) {
        int32_t d = i % 4;
        int32_t r;
        try { r = relay(d, i + 100); }
        catch (PolyBase& e) { r = 3000 + e.b; }
        catch (FlatBase& e) { r = 4000 + e.b; }
        catch (...)         { r = -888; }
        MIX(r);
    }

    /* 3: most-derived-first clause order still works WITH a base clause present:
     * a closer clause must win over the base clause. */
    for (int32_t i = 1; i <= 2; ++i) {
        int32_t r;
        try { r = emit(i, i + 200); }             /* i=1 PolyMid, i=2 PolyLeaf */
        catch (PolyLeaf& e) { r = 5000 + e.l; }   /* exact for depth 2 */
        catch (PolyBase& e) { r = 6000 + e.b; }   /* base for depth 1 */
        MIX(r);
    }

    #undef MIX
    return (int)h;
}
