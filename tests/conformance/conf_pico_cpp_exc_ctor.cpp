/* conf_pico_cpp_exc_ctor — throw FROM a constructor; verify partial-member cleanup.
 *
 * The Exult bring-up fault is a recursive ~std::set<unsigned> (__tree node deleter)
 * reading a GARBAGE node pointer during an EH unwind — the value differs per host
 * run, i.e. it destroys UNINITIALISED memory as if it were a populated tree. The
 * prime suspect: an exception thrown from a CONSTRUCTOR, where the EH cleanup must
 * destroy ONLY the already-constructed members/bases (in reverse order) and must
 * NOT touch members whose construction had not started. If the landingpad cleanup
 * destroys a not-yet-constructed std::set member, it runs ~__tree on raw memory ->
 * wild node pointer -> BAD_ADDR. conf_cpp_exc / conf_pico_cpp_exc_unwind throw from
 * ordinary functions, so this construction-cleanup path was never exercised.
 *
 * Each fixture object holds several std::set<unsigned> members; its constructor
 * builds them in order and throws after a chosen one. The dtor trace records every
 * destruction; the differential checksum (vs native clang++) catches a missed,
 * doubled, or spurious member destruction (the last would read garbage on the VM).
 *
 * conf_pico* => picolibc + vendored libc++; int32 checksum is platform-independent. */
#include <cstdint>
#include <exception>
#include <set>
#include <string>

namespace {

volatile int32_t g_trace = 0;
inline void note(int32_t tag) { g_trace = g_trace * 31 + tag; }

struct FileExc : std::exception {
    int32_t code;
    explicit FileExc(int32_t c) : code(c) {}
    const char* what() const noexcept override { return "ctor-throw"; }
};

/* A non-trivial member: a populated std::set<unsigned> + a string. Its dtor
 * notes its tag (so we can see exactly which members were destroyed). */
struct Field {
    int32_t            tag;
    std::set<unsigned> s;
    std::string        name;
    explicit Field(int32_t t) : tag(t), name("field") {
        for (unsigned k = 0; k <= (unsigned)(t % 5 + 1); ++k) s.insert(k * 5u + (unsigned)t);
    }
    ~Field() { note(1000 + tag + (int32_t)s.size() + (int32_t)name.size()); }
};

/* Five Field members built in order; throw after member index `throw_after`
 * (0..4) -> members [0..throw_after] are fully constructed, the throwing one is
 * partial, members above are untouched. Reverse-order cleanup must run dtors for
 * indices [0..throw_after-1] only (the thrower's own already-built sub-objects
 * are unwound by Field's ctor, not by Box's cleanup). */
struct Box {
    Field a, b, c, d, e;
    Box(int32_t base, int32_t throw_after)
            : a((throw_after >= 0) ? (base + 0) : throw FileExc(base)),
              b((throw_after >= 1) ? (base + 1) : throw FileExc(base + 1)),
              c((throw_after >= 2) ? (base + 2) : throw FileExc(base + 2)),
              d((throw_after >= 3) ? (base + 3) : throw FileExc(base + 3)),
              e((throw_after >= 4) ? (base + 4) : throw FileExc(base + 4)) {
        /* If we got here nothing threw (throw_after >= 4). Throw from the body so
         * ALL five members are fully constructed and must all be cleaned up. */
        throw FileExc(base + 100);
    }
    ~Box() { note(7); }   /* never runs: ctor always throws */
};

}   /* namespace */

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
#define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    for (int32_t throw_after = 0; throw_after <= 4; ++throw_after) {
        g_trace = 0;
        int32_t caught = 0, code = -1;
        try {
            Box box(throw_after * 10 + 1, throw_after);
            (void)box;
        } catch (const std::exception& ex) {
            caught = 1;
            const FileExc* fe = dynamic_cast<const FileExc*>(&ex);
            code = fe ? fe->code : -2;
        } catch (...) {
            caught = 2;
        }
        MIX(throw_after);
        MIX(caught);
        MIX(code);
        MIX(g_trace);    /* exact set of member dtors that ran */
    }

#undef MIX
    return (int)h;
}
