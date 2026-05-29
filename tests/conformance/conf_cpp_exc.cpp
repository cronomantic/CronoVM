/* conf_cpp_exc — C++ exception handling conformance fixture.
 *
 * Exercises the Itanium EH surface the translator lowers onto setjmp/longjmp
 * (invoke / landingpad / resume / __cxa_throw / eh.typeid.for) and the runtime
 * unwinder in runtime/lib/cvm_cxxrt.cpp:
 *   - basic typed catch + catch(...)
 *   - catch-by-base + most-derived-first clause order
 *   - a non-trivial destructor run as a cleanup while unwinding (+ resume)
 *   - rethrow (`throw;`)
 *   - exception propagation ACROSS a function with no try of its own
 *
 * All thrown values depend on the loop variable (no constant-folded throws), so
 * the EH machinery actually runs and the fixture stays toolchain-conservative
 * (see the corpus's pmf-fragility lesson). Differential vs a native clang++
 * oracle; fixed-width types + int32 checksum so host-64 and VM-32 agree. */
#include <stdint.h>

namespace {
struct E       { int32_t code; };
struct Derived : E { int32_t extra; };
struct A       { int32_t v; };
struct B       { int32_t v; };

/* throws E (x<0) or Derived (x==0), else returns. */
static int32_t may_throw(int32_t x) {
    if (x < 0)  throw E{ x * 10 };
    if (x == 0) throw Derived{ {7}, 99 };
    return x * 2;
}

/* a destructor with a visible side effect, run as a cleanup while unwinding */
static volatile int32_t g_dtor_trace = 0;
struct Guard { int32_t tag; ~Guard() { g_dtor_trace = g_dtor_trace * 31 + tag; } };
static int32_t with_cleanup(int32_t x) {
    Guard g{ x + 100 };          /* dtor runs on BOTH normal and unwind exit */
    return may_throw(x) + 1;
}

/* catches then rethrows; the rethrown exception is caught by the caller */
static int32_t rethrower(int32_t x) {
    try { return may_throw(x); }
    catch (E&) { throw; }        /* __cxa_rethrow */
}

/* no try here — A/B propagate THROUGH this frame to the caller's handlers */
static int32_t leaf(int32_t x) { if (x < 0) throw A{ x }; if (x == 0) throw B{ x + 5 }; return x; }
static int32_t mid(int32_t x)  { return leaf(x) * 3; }
}

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    /* 1+2: basic + catch-by-base + clause order + catch(...) */
    for (int32_t i = -3; i <= 5; ++i) {
        int32_t r;
        try { r = may_throw(i); }
        catch (Derived& d) { r = d.code + d.extra; }   /* most-derived first */
        catch (E& e)       { r = e.code - 1; }
        catch (...)        { r = -999; }
        MIX(r);
    }

    /* 3: cleanup dtor + resume */
    for (int32_t i = -2; i <= 2; ++i) {
        int32_t r;
        try { r = with_cleanup(i); }
        catch (E& e) { r = e.code - 7; }
        MIX(r);
    }
    MIX(g_dtor_trace);

    /* 4: rethrow */
    for (int32_t i = -3; i <= 3; ++i) {
        int32_t r;
        try { r = rethrower(i); }
        catch (E& e) { r = 1000 + e.code; }
        MIX(r);
    }

    /* 5: nested try + cross-function propagation */
    for (int32_t i = -2; i <= 3; ++i) {
        int32_t r;
        try {
            try { r = mid(i); }
            catch (A& a) { r = 100 + a.v; }            /* inner catches A */
        } catch (B& b) { r = 200 + b.v; }              /* outer catches B */
        MIX(r);
    }

    #undef MIX
    return (int)h;
}
