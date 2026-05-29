/* cvm_cxxrt.cpp — C++ ABI runtime for CronoVM, auto-linked by cvm-cc whenever a
 * translation unit is C++ (.cpp/.cc/.cxx). It resolves the symbols clang emits
 * for ordinary freestanding C++ (-fno-exceptions -fno-rtti):
 *
 *   - operator new / new[] / delete / delete[] (+ sized delete). Every class
 *     with a virtual destructor references operator delete through its
 *     deleting-destructor vtable slot, so these are needed even by code that
 *     never explicitly new/deletes. They forward to the C allocator —
 *     malloc/free, which the cart's libc (the Cronopio SDK) provides.
 *   - __cxa_pure_virtual: a pure-virtual slot was called (a bug); trap.
 *   - __cxa_guard_acquire/release/abort: function-local static initialisation
 *     guards. CronoVM carts are cooperatively scheduled (no preemption), so a
 *     single "initialised" byte in the guard word is sufficient.
 *   - __cxa_atexit / __dso_handle: static-destructor registration. Carts run to
 *     exit and never tear down, so dropping the registration is correct.
 *
 * NOT covered (by design, matching cvm-cc's -fno-exceptions -fno-rtti): the
 * exception ABI (__cxa_throw/_Unwind_*) and RTTI (type_info). The C++ standard
 * library is a separate concern (an external freestanding libc++/libstdc++
 * subset), not part of this minimal ABI shim.
 *
 * Global constructors are handled by the translator (it runs llvm.global_ctors
 * before main), not here. */
#include <stddef.h>

extern "C" void *malloc(size_t);
extern "C" void  free(void *);

void *operator new(size_t n)         { return malloc(n ? n : 1); }
void *operator new[](size_t n)       { return malloc(n ? n : 1); }
void  operator delete(void *p) noexcept           { free(p); }
void  operator delete[](void *p) noexcept         { free(p); }
void  operator delete(void *p, size_t) noexcept   { free(p); }
void  operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" {

void __cxa_pure_virtual(void) { for (;;) {} }

int  __cxa_guard_acquire(void *g) { return *(volatile char *)g == 0; }
void __cxa_guard_release(void *g) { *(volatile char *)g = 1; }
void __cxa_guard_abort(void *g)   { (void)g; }

int  __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
void *__dso_handle = 0;

} /* extern "C" */
