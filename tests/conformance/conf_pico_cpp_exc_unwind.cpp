/* conf_pico_cpp_exc_unwind — deep EH unwind through STL-destructor cleanups.
 *
 * conf_cpp_exc covers the EH surface at SHALLOW depth with a trivial (int-tag)
 * cleanup dtor. The Exult bring-up hit a fault that looks like a BROKEN unwind in
 * a DIFFERENT shape: an exception (a class derived from std::exception, like
 * file_open_exception) thrown DEEP in a call chain, where every intermediate
 * frame owns NON-TRIVIAL STL locals (std::set<unsigned> -> recursive ~__tree,
 * std::string) so each frame has a real cleanup landingpad, and some intermediate
 * frames have a try/catch for a DIFFERENT type so the exception propagates THROUGH
 * their landingpad. The catch is `catch(const std::exception&)` at the very top —
 * exactly the gamewin_probe setup() pattern where the unwind failed to reach the
 * handler and instead re-entered the function.
 *
 * Differential oracle (native clang++): the checksum folds, for each unwind depth,
 * whether the base catch fired, the recovered code, and the dtor-execution trace
 * (so a skipped catch, a re-entry, a missed/double dtor, or a crash all diverge).
 *
 * conf_pico* => links picolibc + the vendored libc++; int32 checksum is
 * platform-independent. */
#include <cstdint>
#include <exception>
#include <set>
#include <stdexcept>
#include <string>

namespace {

volatile int32_t g_dtor_trace = 0;

/* A cleanup object with NON-TRIVIAL members: destroying it during unwind runs
 * std::string's dtor and std::set's recursive ~__tree node deleter — the exact
 * destructor shape that faulted in the Exult run. */
struct Guard {
    uint32_t          tag;
    std::string       name;
    std::set<unsigned> s;
    explicit Guard(uint32_t t) : tag(t), name("guard-cleanup") {
        for (unsigned k = 0; k <= (t % 6u); ++k) s.insert(k * 7u + t);
    }
    ~Guard() {
        g_dtor_trace = g_dtor_trace * 31
                     + (int32_t)(tag + (uint32_t)name.size() + (uint32_t)s.size());
    }
};

/* Thrown type derived from std::exception, like Exult's file_open_exception. */
struct FileExc : std::exception {
    int32_t code;
    explicit FileExc(int32_t c) : code(c) {}
    const char* what() const noexcept override { return "file-open"; }
};

/* Recurse `maxlevel` deep; each level holds a Guard (cleanup landingpad). At the
 * bottom, throw FileExc. Every other intermediate frame wraps the recursive call
 * in a try/catch for an UNRELATED type, so FileExc propagates through a landingpad
 * that evaluates a non-matching clause (the most fragile unwind path). */
int32_t deep(int32_t level, int32_t maxlevel) {
    Guard g((uint32_t)(level + 1));
    if (level >= maxlevel) {
        throw FileExc(level * 100 + 7);
    }
    if (level & 1) {
        try {
            return deep(level + 1, maxlevel) + level;
        } catch (const std::range_error&) {   /* never matches FileExc */
            return -1;
        }
    }
    return deep(level + 1, maxlevel) + level;
}

/* The gamewin_probe setup() shape: the SAME function owns local objects with
 * non-trivial dtors in its try body (built at staggered points before the throw)
 * AND the catch(std::exception&). Its landingpad must run the cleanups (destroy
 * the constructed locals) THEN dispatch the catch — the combined cleanup+catch
 * landingpad that the Exult unwind appeared to mis-handle. After the catch it
 * keeps using locals declared OUTSIDE the try (which must survive the longjmp). */
int32_t catcher_with_cleanup(int32_t depth) {
    int32_t outer_a = depth * 3 + 1;       /* live across the try -> must survive */
    std::string outer_s("outer-state");    /* used AFTER the catch */
    int32_t outer_b = depth * 7 + 2;
    int32_t caught = 0, code = -1;
    try {
        Guard g1((uint32_t)(depth + 50));  /* constructed before the throw */
        std::string mid("mid-state");
        Guard g2((uint32_t)(depth + 60));
        (void)deep(0, depth);              /* throws FileExc from the bottom */
        return -777;                       /* unreachable */
    } catch (const std::exception& e) {
        caught = 1;
        const FileExc* fe = dynamic_cast<const FileExc*>(&e);
        code = fe ? fe->code : -2;
    } catch (...) {
        caught = 2;
    }
    /* Post-catch: combine values live across the EH boundary. */
    return outer_a * 100000 + outer_b * 1000 + caught * 100
         + (int32_t)outer_s.size() * 10 + (code & 0xff);
}

}   /* namespace */

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
#define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    for (int32_t depth = 1; depth <= 12; ++depth) {
        g_dtor_trace = 0;
        int32_t caught = 0, code = -1;
        try {
            (void)deep(0, depth);
        } catch (const std::exception& e) {       /* base catch, like setup() */
            caught = 1;
            const FileExc* fe = dynamic_cast<const FileExc*>(&e);
            code = fe ? fe->code : -2;            /* RTTI in the handler */
        } catch (...) {
            caught = 2;
        }
        MIX(depth);
        MIX(caught);
        MIX(code);
        MIX(g_dtor_trace);
    }

    /* setup()-shaped: cleanup-locals + catch in the same frame, deep throw. */
    for (int32_t depth = 1; depth <= 12; ++depth) {
        g_dtor_trace = 0;
        MIX(catcher_with_cleanup(depth));
        MIX(g_dtor_trace);
    }

#undef MIX
    return (int)h;
}
