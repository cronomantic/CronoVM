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

/* ---- Exceptions: a setjmp/longjmp unwinder ----------------------------- *
 * The translator lowers each `invoke` to: alloca a cvm_eh_frame, set its
 * descriptor (the target landingpad's catch clauses), __cvm_eh_push it,
 * SETJMP(frame.jb), then branch — setjmp==0 runs the call + __cvm_eh_pop +
 * normal edge; setjmp!=0 means we were unwound here, so it reads the in-flight
 * exception (__cvm_eh_exc / __cvm_eh_sel) and runs the landingpad. `resume`
 * lowers to __cvm_eh_resume (continue unwinding). __cxa_throw drives the walk.
 *
 * Layout below MUST match what the translator emits (jb first so &frame ==
 * &frame.jb). No DWARF/LSDA — our own per-landingpad descriptors. */

extern "C" void longjmp(int *env, int val) __attribute__((__noreturn__));
extern "C" void __cvm_eh_terminate(void);

namespace {

struct eh_clause { const void *ti; };          /* ti == 0 => catch(...) */
struct eh_desc   { int n_clauses; int has_cleanup; const eh_clause *clauses; };
struct eh_frame  { int jb[4]; const eh_desc *desc; eh_frame *next; };

eh_frame  *g_eh_top = 0;       /* top of the active-landingpad chain */
void      *g_eh_exc = 0;       /* in-flight exception object */
const ti_base *g_eh_ti = 0;    /* its type_info */
int        g_eh_sel = 0;       /* selector handed to the landingpad */

/* Exception header sits just before the thrown object. handler_count tracks how
 * many `catch` handlers are live on this exception (Itanium __cxa semantics, in
 * the header so a rethrow that transits an intervening cleanup's end_catch is
 * not freed prematurely): __cxa_rethrow negates it, __cxa_begin_catch undoes the
 * negation + increments, __cxa_end_catch frees only when it returns to 0 on the
 * non-rethrown path. */
struct exc_header { const void *ti; void (*dtor)(void *); int handler_count; };
inline exc_header *hdr_of(void *obj) { return (exc_header *)obj - 1; }

/* Does a thrown object of type `thrown` match a `catch (Caught)` clause? True
 * if thrown == caught or thrown publicly derives from caught (catch-by-base). */
bool eh_type_matches(const ti_base *thrown, const ti_base *caught) {
    char dummy = 0;
    return ti_search(thrown, caught, 0, &dummy) >= 0;
}

/* Walk the chain from the top, popping each frame. Stop at the first frame that
 * catches the in-flight exception (a matching typed clause, a catch-all, or a
 * cleanup) by longjmp-ing into it; that frame is already popped. If none, the
 * program cannot handle the exception -> terminate. */
[[noreturn]] void eh_unwind() {
    while (g_eh_top) {
        eh_frame *f = g_eh_top;
        g_eh_top = f->next;                 /* pop before transferring */
        const eh_desc *d = f->desc;
        if (d) {
            for (int i = 0; i < d->n_clauses; ++i) {
                const void *cti = d->clauses[i].ti;
                if (cti == 0) {             /* catch(...) */
                    g_eh_sel = 0;
                    longjmp(f->jb, 1);
                }
                if (eh_type_matches(g_eh_ti, (const ti_base *)cti)) {
                    g_eh_sel = (int)(long)cti;   /* == llvm.eh.typeid.for(cti) */
                    longjmp(f->jb, 1);
                }
            }
            if (d->has_cleanup) {           /* run dtors, then it will resume */
                g_eh_sel = 0;
                longjmp(f->jb, 1);
            }
        }
    }
    /* nobody caught it */
    __cvm_eh_terminate();
    for (;;) {}
}

} /* anon */

extern "C" {

/* translator-emitted helpers */
void  __cvm_eh_push(eh_frame *f) { f->next = g_eh_top; g_eh_top = f; }
void  __cvm_eh_pop(void)         { if (g_eh_top) g_eh_top = g_eh_top->next; }
void *__cvm_eh_exc(void)         { return g_eh_exc; }     /* landingpad value[0] */
int   __cvm_eh_sel(void)         { return g_eh_sel; }     /* landingpad value[1] */
int   __cvm_eh_typeid(const void *ti) { return (int)(long)ti; }  /* eh.typeid.for */
[[noreturn]] void __cvm_eh_resume(void) { eh_unwind(); }  /* `resume` */

/* Itanium __cxa_* surface */
void *__cxa_allocate_exception(unsigned long size) {
    char *p = (char *)malloc(sizeof(exc_header) + (size ? size : 1));
    if (!p) { for (;;) {} }
    exc_header *h = (exc_header *)p;
    h->ti = 0; h->dtor = 0; h->handler_count = 0;
    return p + sizeof(exc_header);
}
void __cxa_free_exception(void *obj) { if (obj) free(hdr_of(obj)); }

[[noreturn]] void __cxa_throw(void *obj, void *ti, void (*dtor)(void *)) {
    exc_header *h = hdr_of(obj);
    h->ti = ti; h->dtor = dtor;
    g_eh_exc = obj;
    g_eh_ti  = (const ti_base *)ti;
    eh_unwind();
}

void *__cxa_begin_catch(void *exc) {
    (void)exc;
    if (g_eh_exc) {
        exc_header *h = hdr_of(g_eh_exc);
        /* A negative count marks an in-flight rethrow (see __cxa_rethrow): undo
         * the negation and count this handler. Otherwise just count it. */
        h->handler_count = (h->handler_count < 0)
            ? -h->handler_count + 1 : h->handler_count + 1;
    }
    return g_eh_exc;          /* the (possibly base-adjusted) object */
}
void __cxa_end_catch(void) {
    if (!g_eh_exc) return;
    exc_header *h = hdr_of(g_eh_exc);
    if (h->handler_count < 0) {
        /* Rethrown and now transiting an intervening cleanup's end_catch — the
         * exception is still propagating, so step the count back toward zero but
         * NEVER free here. */
        ++h->handler_count;
    } else if (--h->handler_count == 0) {
        if (h->dtor) h->dtor(g_eh_exc);
        free(h);
        g_eh_exc = 0; g_eh_ti = 0;
    }
}
/* `throw;` — re-raise the exception currently being handled. Negate its handler
 * count so the cleanup end_catch this unwind transits does not free it; the next
 * __cxa_begin_catch undoes the negation. */
[[noreturn]] void __cxa_rethrow(void) {
    if (g_eh_exc) {
        exc_header *h = hdr_of(g_eh_exc);
        h->handler_count = -h->handler_count;
    }
    eh_unwind();
}

/* Last-resort terminate. The cart has no std::terminate handler chain; trap. */
void __cvm_eh_terminate(void) { for (;;) {} }

} /* extern "C" */

/* std::terminate — clang routes a noexcept violation and an unhandled exception
 * through `__clang_call_terminate` (which it emits, defined, in any module with
 * a terminate landingpad) -> `std::terminate`. clang references it MANGLED and
 * we have no <exception> header here, so define it by its mangled asm name. The
 * cart has no terminate-handler chain; trap (same as __cvm_eh_terminate). */
extern "C" void cvm_std_terminate(void) asm("_ZSt9terminatev");
extern "C" void cvm_std_terminate(void) { __cvm_eh_terminate(); for (;;) {} }

