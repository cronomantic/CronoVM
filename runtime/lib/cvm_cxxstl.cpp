/* cvm_cxxstl.cpp — the out-of-line std exception hierarchy for CronoVM.
 *
 * Auto-linked by cvm-cc (alongside cvm_cxxrt.cpp) whenever a translation unit
 * is C++. It supplies the parts of the standard library exception classes that
 * libc++ leaves to its shared library / libc++abi — the bits the STL headers
 * reference but do NOT emit inline: the out-of-line constructors, destructors,
 * what() overrides, and (via the Itanium "key function" rule) the matching
 * vtables + type_info objects.
 *
 * WHY a separate TU and not cvm_cxxrt: this file is compiled as C++ AGAINST THE
 * TOOLCHAIN'S OWN libc++ HEADERS (<stdexcept>/<new>/<exception>), so the class
 * layouts, mangled names, vtable shapes and type_info objects are exactly
 * libc++'s — we don't hand-build any ABI table. Defining each class's key
 * function (its first out-of-line virtual — here the destructor / what()) makes
 * clang emit `_ZTV*` (vtable) and `_ZTI*` (type_info) for that class. Those
 * type_info objects are __cxxabiv1::__class_type_info / __si_class_type_info
 * instances whose vtables are the `_ZTVN10__cxxabiv1...E` symbols cvm_cxxrt
 * already provides — so catch-by-base over std exceptions flows through the
 * existing setjmp/longjmp unwinder + __dynamic_cast RTTI in cvm_cxxrt.
 *
 * std::logic_error / runtime_error hold a std::__libcpp_refstring (a single
 * `const char*`). libc++'s real refstring is reference-counted for cheap copies;
 * ours is a plain strdup/free of the message — ABI-compatible because the layout
 * is identical (one pointer) and we define every refstring member here, so the
 * representation is entirely ours.
 *
 * operator new/delete, __cxa_*, the EH unwinder and the RTTI abi vtables live in
 * cvm_cxxrt.cpp; this file depends on them. malloc/free/str* come from the
 * cart's C library (picolibc / SDK). */
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <typeinfo>

/* ---- std::__libcpp_refstring (versioned namespace std::__1) -------------- *
 * Plain owning C string: ctor/copy strdup, dtor frees. free(nullptr) is a no-op
 * so an OOM (malloc -> nullptr) degrades to a null message rather than a crash
 * in these noexcept paths. */
std::__libcpp_refstring::__libcpp_refstring(const char* __msg) {
    std::size_t __n = __msg ? std::strlen(__msg) : 0;
    char* __p = static_cast<char*>(std::malloc(__n + 1));
    if (__p)
        std::memcpy(__p, __msg ? __msg : "", __n + 1);
    __imp_ = __p;
}

std::__libcpp_refstring::__libcpp_refstring(const __libcpp_refstring& __s) noexcept {
    const char* __m = __s.__imp_;
    std::size_t __n = __m ? std::strlen(__m) : 0;
    char* __p = static_cast<char*>(std::malloc(__n + 1));
    if (__p)
        std::memcpy(__p, __m ? __m : "", __n + 1);
    __imp_ = __p;
}

std::__libcpp_refstring& std::__libcpp_refstring::operator=(const __libcpp_refstring& __s) noexcept {
    if (this != &__s) {
        const char* __m = __s.__imp_;
        std::size_t __n = __m ? std::strlen(__m) : 0;
        char* __p = static_cast<char*>(std::malloc(__n + 1));
        if (__p)
            std::memcpy(__p, __m ? __m : "", __n + 1);
        std::free(const_cast<char*>(__imp_));
        __imp_ = __p;
    }
    return *this;
}

std::__libcpp_refstring::~__libcpp_refstring() { std::free(const_cast<char*>(__imp_)); }

bool std::__libcpp_refstring::__uses_refcount() const { return false; }

/* ---- std::exception (base of all library exceptions) -------------------- */
std::exception::~exception() noexcept {}
const char* std::exception::what() const noexcept { return "std::exception"; }

/* ---- std::logic_error / runtime_error (carry the message) --------------- */
std::logic_error::logic_error(const std::string& __s) : __imp_(__s.c_str()) {}
std::logic_error::logic_error(const char* __s) : __imp_(__s) {}
std::logic_error::logic_error(const logic_error& __o) noexcept : exception(__o), __imp_(__o.__imp_) {}
std::logic_error& std::logic_error::operator=(const logic_error& __o) noexcept {
    __imp_ = __o.__imp_;
    return *this;
}
std::logic_error::~logic_error() noexcept {}
const char* std::logic_error::what() const noexcept { return __imp_.c_str(); }

std::runtime_error::runtime_error(const std::string& __s) : __imp_(__s.c_str()) {}
std::runtime_error::runtime_error(const char* __s) : __imp_(__s) {}
std::runtime_error::runtime_error(const runtime_error& __o) noexcept : exception(__o), __imp_(__o.__imp_) {}
std::runtime_error& std::runtime_error::operator=(const runtime_error& __o) noexcept {
    __imp_ = __o.__imp_;
    return *this;
}
std::runtime_error::~runtime_error() noexcept {}
const char* std::runtime_error::what() const noexcept { return __imp_.c_str(); }

/* ---- The derived *_error classes: ctors are inline (forward to the base),
 * only their out-of-line destructors (key functions) live here. ----------- */
std::domain_error::~domain_error() noexcept {}
std::invalid_argument::~invalid_argument() noexcept {}
std::length_error::~length_error() noexcept {}
std::out_of_range::~out_of_range() noexcept {}
std::range_error::~range_error() noexcept {}
std::overflow_error::~overflow_error() noexcept {}
std::underflow_error::~underflow_error() noexcept {}

/* ---- <new> exceptions --------------------------------------------------- */
/* copy ctor + operator= are in-class defaulted in libc++ — do NOT redefine.
 * The destructor is the key function (first out-of-line virtual): defining it
 * is what emits each class's vtable + type_info. */
std::bad_alloc::bad_alloc() noexcept {}
std::bad_alloc::~bad_alloc() noexcept {}
const char* std::bad_alloc::what() const noexcept { return "std::bad_alloc"; }

std::bad_array_new_length::bad_array_new_length() noexcept {}
std::bad_array_new_length::~bad_array_new_length() noexcept {}
const char* std::bad_array_new_length::what() const noexcept { return "std::bad_array_new_length"; }

/* ---- std::shared_ptr control block (libc++ src/memory.cpp) --------------- *
 * __shared_count / __shared_weak_count are libc++'s reference-count base
 * classes; their out-of-line members (and key-function vtables/type_info) live
 * in the dylib. We provide them here. The refcounts use atomicrmw (now lowered
 * by the translator under the cooperative model); lock() is written WITHOUT a
 * compare-exchange (cooperative VM: no other context runs mid-function), so we
 * don't need cmpxchg. __shared_owners_ is the strong count minus one (== -1 when
 * expired); __shared_weak_owners_ the weak count minus one. */
std::__shared_count::~__shared_count() {}
std::__shared_weak_count::~__shared_weak_count() {}

void std::__shared_weak_count::__release_weak() noexcept {
    if (__shared_weak_owners_ == 0)
        __on_zero_shared_weak();
    else if (--__shared_weak_owners_ == -1)
        __on_zero_shared_weak();
}

std::__shared_weak_count* std::__shared_weak_count::lock() noexcept {
    if (__shared_owners_ == -1)
        return nullptr;          /* expired */
    ++__shared_owners_;          /* adopt a strong ref (cooperative: no race) */
    return this;
}

const void* std::__shared_weak_count::__get_deleter(const std::type_info&) const noexcept {
    return nullptr;
}

/* ---- std::string out-of-line members ------------------------------------ *
 * libc++ marks `basic_string<char>` as an extern template (its common members
 * — append, reserve, etc. — are instantiated in the libc++ dylib, not per-TU).
 * We have no dylib, so force the instantiation HERE; this emits every member
 * once into cvm_cxxstl.bc, satisfying the extern references from cart code. */
template class std::basic_string<char, std::char_traits<char>, std::allocator<char> >;

/* operator+(const char*, const string&) — a free function template instantiated
 * in the dylib; force it here (the iostream/locale library references it). */
template std::string std::operator+ <char, std::char_traits<char>, std::allocator<char> >(
    const char*, const std::string&);

/* ---- out-of-line libc++ runtime bits the iostream/locale library needs ---- *
 * These live in the libc++ dylib (and libc++abi) on a hosted system; with no
 * dylib we provide them here, against the toolchain headers so the mangling is
 * ABI-exact. cxxio.bc (the vendored stream/locale TUs) pulls them. */

/* libc++ internal throw helpers (versioned std::__1). They centralise the throw
 * so callers stay small; route to the matching exception type cvm_cxxstl already
 * defines above. */
_LIBCPP_BEGIN_NAMESPACE_STD
void __throw_runtime_error(const char* msg) { throw runtime_error(msg); }

/* std::call_once's slow path. The VM is cooperative (no preemption), so there is
 * no race: if the flag is not already the "complete" sentinel (~0), run the
 * initialiser and mark it complete. (The threaded libc++ src guards with a futex;
 * unnecessary here.) Signature matches once_flag::_State_type == unsigned long. */
void __call_once(volatile unsigned long& flag, void* arg, void (*func)(void*)) {
    if (flag == ~0ul) return;
    func(arg);
    flag = ~0ul;
}

/* std::bad_function_call (versioned std::__1): thrown when an empty std::function
 * is invoked. Exult installs its file-stream factories (U7set_*_factory) as
 * std::function, so the empty-call guard instantiates this type and references
 * its vtable/type_info — externally, since libc++ makes the dtor the key function
 * (it lives in the dylib we don't have). Defining the out-of-line members the
 * headers declare (under the same availability macros) emits the vtable + the
 * type_info locally. Inherits std::exception (vtable/type_info from above). */
#if _LIBCPP_AVAILABILITY_HAS_BAD_FUNCTION_CALL_KEY_FUNCTION
bad_function_call::~bad_function_call() noexcept {}
#endif
#if _LIBCPP_AVAILABILITY_HAS_BAD_FUNCTION_CALL_GOOD_WHAT_MESSAGE
const char* bad_function_call::what() const noexcept { return "std::bad_function_call"; }
#endif
_LIBCPP_END_NAMESPACE_STD

/* libc++abi-level symbols clang/libc++ reference in the NON-versioned std
 * namespace (ABI-fixed names, normally in libc++abi). We have only the minimal
 * cvm_cxxrt ABI, so define them here. */
namespace std {
/* uncaught_exceptions(): used by ostream/istream sentry to decide whether to
 * flush during stack unwinding. cvm_cxxrt tracks none across the cooperative
 * model; 0 ("not unwinding") is the safe answer for the stream sentries. */
int uncaught_exceptions() noexcept { return 0; }

/* __throw_bad_alloc(): allocation failure. On the VM an OOM is fatal, but throw
 * the proper bad_alloc (cvm_cxxstl defines it) so a cart's catch can run. */
void __throw_bad_alloc() { throw bad_alloc(); }

/* std::bad_cast: thrown by a failed dynamic_cast (use_facet's facet lookup uses
 * one). Defining the key function (dtor) emits its vtable + type_info; what()
 * gives the message. Inherits std::exception (vtable/type_info from cvm_cxxstl). */
bad_cast::bad_cast() noexcept {}
bad_cast::~bad_cast() noexcept {}
const char* bad_cast::what() const noexcept { return "std::bad_cast"; }
}
