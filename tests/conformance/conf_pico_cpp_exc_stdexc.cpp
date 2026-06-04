/* conf_pico_cpp_exc_stdexc — catch a derived throw by the LIBRARY base
 * std::exception, mirroring Exult's EXACT exception hierarchy + idiom.
 *
 * The Exult runtime blocker ([[cronovm-eh-stdexc-catch-bug]]): a missing
 * optional file makes U7open_in throw `file_open_exception : file_exception :
 * exult_exception : std::exception`, and U7exists wraps it in
 * `try { U7open_in(f); } catch (std::exception&) { return false; }`. On the VM
 * that catch was reported as SKIPPED, so the exception escaped uncaught.
 *
 * This reproduces that shape faithfully: a 3-level single-inheritance chain
 * rooted at the libc++ std::exception, the base carrying a std::string message
 * with a what() override (exactly exult_exception), thrown across a no-try frame
 * and caught by std::exception& as well as by the intermediate bases. A MISS
 * lands in catch(...) -> a distinct value -> differential checksum mismatch vs a
 * native clang++ oracle. (pico fixture: picolibc headers/bitcode supply the C
 * types <exception>/<string> need; std::exception's out-of-line ABI comes from
 * runtime/lib/cvm_cxxstl.cpp, auto-linked by cvm-cc.) */
#include <exception>
#include <string>
#include <stdint.h>

namespace {

/* == exult_exception: holds a std::string message, overrides what(). */
class exult_exc : public std::exception {
    std::string what_;
public:
    explicit exult_exc(std::string w) : what_(std::move(w)) {}
    const char *what() const noexcept override { return what_.c_str(); }
};
class file_exc : public exult_exc {
public:
    explicit file_exc(std::string w) : exult_exc(std::move(w)) {}
};
class file_open_exc : public file_exc {
public:
    explicit file_open_exc(const std::string &f) : file_exc("Error opening file " + f) {}
};

/* == U7open_in: throws for a "missing" file. */
[[gnu::noinline]] static void u7open(const std::string &f) { throw file_open_exc(f); }

/* == U7exists: must turn the throw into `false` via catch(std::exception&). */
[[gnu::noinline]] static bool u7exists(const std::string &f) {
    try {
        u7open(f);
        return true;
    } catch (std::exception &) {   /* the catch that was skipped on the VM */
        return false;
    }
}

}  /* anon */

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    /* 1: the exact Exult idiom — every probe of a "missing" file must return
     * false (caught). If the catch is skipped, the exception escapes. */
    for (int32_t i = 0; i < 6; ++i) {
        std::string name = "endshape";
        name += (char)('0' + i);
        name += ".flx";
        bool ok = u7exists(name);          /* expect false for all */
        MIX(ok ? (1 + i) : (1000 + i));
    }

    /* 2: catch file_open_exc by each base level explicitly (1/2/3 levels up). */
    for (int32_t i = 0; i < 3; ++i) {
        int32_t r;
        try { u7open("x"); r = -1; }
        catch (file_exc &)        { r = 100 + i; }   /* 1 level up */
        catch (std::exception &)  { r = -2; }
        MIX(r);

        try { u7open("y"); r = -1; }
        catch (exult_exc &)       { r = 200 + i; }   /* 2 levels up */
        catch (std::exception &)  { r = -3; }
        MIX(r);

        try { u7open("z"); r = -1; }
        catch (std::exception &)  { r = 300 + i; }   /* 3 levels up (the bug) */
        catch (...)               { r = -4; }        /* a MISS lands here */
        MIX(r);
    }

    #undef MIX
    return (int)h;
}
