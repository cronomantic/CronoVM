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

/* ---- RTTI: __dynamic_cast + the Itanium type_info ABI ------------------- *
 * Enough of libc++abi for dynamic_cast over single + (non-)virtual multiple
 * inheritance. typeid is NOT supported (no engine in scope uses it). The three
 * abi type_info "vtables" below exist only so their ADDRESSES identify the
 * type_info kind — clang stores &vtable[2] as each type_info's first word, and
 * we compare against it; we never dispatch through them. */
namespace {

struct ti_base   { const void *vptr; const char *name; };
struct ti_si     { const void *vptr; const char *name; const ti_base *base; };
struct ti_bcinfo { const ti_base *base; long offset_flags; };
struct ti_vmi    { const void *vptr; const char *name;
                   unsigned flags; unsigned base_count; ti_bcinfo bases[1]; };
enum { VIRTUAL_MASK = 1, PUBLIC_MASK = 2, OFFSET_SHIFT = 8 };

} /* anon */

/* The abi type_info vtables. Only their addresses matter (kind discrimination);
 * clang's type_info objects point their first word at &<vt>[2]. */
const void *cvm_ti_class_vt[3] asm("_ZTVN10__cxxabiv117__class_type_infoE")    = {0,0,0};
const void *cvm_ti_si_vt[3]    asm("_ZTVN10__cxxabiv120__si_class_type_infoE") = {0,0,0};
const void *cvm_ti_vmi_vt[3]   asm("_ZTVN10__cxxabiv121__vmi_class_type_infoE")= {0,0,0};

namespace {

/* 0 = leaf class, 1 = single-base (si), 2 = multi/virtual (vmi), -1 = other. */
static int ti_kind(const ti_base *t) {
    const void *v = t->vptr;
    if (v == (const void *)&cvm_ti_class_vt[2]) return 0;
    if (v == (const void *)&cvm_ti_si_vt[2])    return 1;
    if (v == (const void *)&cvm_ti_vmi_vt[2])   return 2;
    return -1;
}

/* Search the static-type tree rooted at `t` (a subobject at byte offset `off`
 * within the most-derived object at `most`) for the public base `dst`. Returns
 * its offset within the most-derived object, or -1. First public match (correct
 * for non-ambiguous hierarchies — the common case incl. Exult's UI). */
static long ti_search(const ti_base *t, const ti_base *dst, long off,
                      const char *most) {
    if (t == dst) return off;
    switch (ti_kind(t)) {
    case 1: {  /* si_class: one public base at offset 0 */
        const ti_si *s = (const ti_si *)t;
        return ti_search(s->base, dst, off, most);
    }
    case 2: {  /* vmi_class: N bases with per-base offset + flags */
        const ti_vmi *v = (const ti_vmi *)t;
        for (unsigned i = 0; i < v->base_count; ++i) {
            const ti_bcinfo *b = &v->bases[i];
            if (!(b->offset_flags & PUBLIC_MASK)) continue;
            long boff;
            if (b->offset_flags & VIRTUAL_MASK) {
                /* virtual base: the vbase offset lives in the subobject's
                 * vtable at byte index (offset_flags>>OFFSET_SHIFT). */
                const char *sub = most + off;
                const char *vptr = *(const char *const *)sub;
                boff = *(const long *)(vptr + (b->offset_flags >> OFFSET_SHIFT));
            } else {
                boff = b->offset_flags >> OFFSET_SHIFT;
            }
            long r = ti_search(b->base, dst, off + boff, most);
            if (r >= 0) return r;
        }
        return -1;
    }
    default:
        return -1;  /* leaf, not dst */
    }
}

} /* anon */

extern "C" void *__dynamic_cast(const void *sub, const void * /*src_ti*/,
                                const void *dst_ti, long /*src2dst*/) {
    if (!sub) return 0;
    /* object's vtable: vptr[-1] = dynamic type_info, vptr[-2] = offset-to-top */
    const char *vptr = *(const char *const *)sub;
    long off_to_top = *(const long *)(vptr - 2 * (long)sizeof(void *));
    const ti_base *dyn =
        *(const ti_base *const *)(vptr - 1 * (long)sizeof(void *));
    const char *most = (const char *)sub + off_to_top;
    long r = ti_search(dyn, (const ti_base *)dst_ti, 0, most);
    return r < 0 ? 0 : (void *)(most + r);
}

