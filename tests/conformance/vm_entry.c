/* vm_entry.c — VM-side support for the conformance corpus.
 *
 * (1) A real `main` tail-calling conf_main, to pin the VM entry
 *     deterministically: fixtures export `conf_main` (not `main`), and the
 *     translator's "named main, else first defined function" heuristic could
 *     otherwise pick the wrong function in a multi-function module (e.g. a C++
 *     global-ctor). Real carts already name their entry `main`.
 *
 * (2) A tiny bump allocator providing malloc/free. The C++ fixtures pull in
 *     cvm-cc's auto-linked C++ ABI runtime (runtime/lib/cvm_cxxrt.cpp), whose
 *     operator new/delete forward to malloc/free. Real carts get those from the
 *     Cronopio SDK machine port; the standalone conformance build has no libc, so
 *     this supplies just enough. (The fixtures use stack objects, so this is
 *     rarely exercised at runtime — it mainly needs to exist for linking.)
 *
 *     These are WEAK: the conf_pico* fixtures link picolibc.bc, which provides
 *     the CANONICAL (strong) malloc/free; llvm-link drops these weak ones in
 *     favour of picolibc's. Non-picolibc fixtures keep using this bump pair. */
#include <stddef.h>

extern int conf_main(void);
int main(void) { return conf_main(); }

static unsigned char g_heap[1u << 16];
static size_t g_hp = 0;

__attribute__((weak)) void *malloc(size_t n) {
    size_t a = (n + 7u) & ~(size_t)7u;
    if (g_hp + a > sizeof g_heap) return 0;
    void *p = &g_heap[g_hp];
    g_hp += a;
    return p;
}
__attribute__((weak)) void free(void *p) { (void)p; }
