/* cvm_cxxrt.cpp — minimal freestanding C++ ABI runtime for the CronoVM
 * conformance corpus. Resolves the symbols clang emits for any C++ program:
 * operator new/delete (referenced by every class with a virtual destructor,
 * via the deleting-destructor vtable slot) and __cxa_pure_virtual.
 *
 * Self-contained (a tiny bump allocator over a static buffer) so the C++
 * fixtures link standalone, without the SDK libc. NOTE: real carts would back
 * operator new/delete with the SDK's cron malloc/free instead — this version
 * exists only so the differential corpus can run a C++ fixture in isolation. */
#include <stddef.h>

static unsigned char g_heap[1u << 16];
static size_t g_hp = 0;

static void *bump(size_t n) {
    size_t a = (n + 7u) & ~(size_t)7u;
    if (g_hp + a > sizeof g_heap) return 0;
    void *p = &g_heap[g_hp];
    g_hp += a;
    return p;
}

void *operator new(size_t n)        { return bump(n ? n : 1); }
void *operator new[](size_t n)      { return bump(n ? n : 1); }
void  operator delete(void *) noexcept            {}
void  operator delete[](void *) noexcept          {}
void  operator delete(void *, size_t) noexcept    {}
void  operator delete[](void *, size_t) noexcept  {}

extern "C" void __cxa_pure_virtual() { for (;;) {} }

/* Function-local static initialisation guards (Itanium C++ ABI). The VM is
 * cooperatively scheduled (no preemption), so a single "initialised" byte in
 * the guard word suffices: acquire returns 1 the first time (caller runs the
 * initialiser, then calls release), 0 thereafter. */
extern "C" int  __cxa_guard_acquire(void *g) { return *(volatile char *)g == 0; }
extern "C" void __cxa_guard_release(void *g) { *(volatile char *)g = 1; }
extern "C" void __cxa_guard_abort(void *g)   { (void)g; }

/* Static-destructor registration: carts don't tear down (run-to-exit), so
 * dropping the dtor registration is correct. */
extern "C" int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
void *__dso_handle = 0;
