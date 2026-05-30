/* pico_machine.c — the minimal CronoVM machine port for the picolibc
 * conformance fixture (conf_pico.c).
 *
 * picolibc.bc is the pure, embedder-independent C surface; it leaves a tiny set
 * of symbols for the embedder to supply (see build_picolibc.sh's reported
 * "machine-port surface"). picolibc now owns the CANONICAL allocator
 * (malloc/free/calloc/realloc), whose only OS hook is sbrk(); the other symbol
 * its string/stdlib routines reference is the global errno.
 *
 * A real cart gets all of this from the Cronopio SDK machine port (sbrk over the
 * cron heap reserved via --heap-reserve, errno as a global). This file is just
 * enough for the standalone VM test: errno + a self-contained static-arena sbrk
 * (so the fixture needs no --heap-reserve). vm_entry.c's bump malloc/free are
 * WEAK, so picolibc's strong definitions win once picolibc.bc is linked. */
#include <stddef.h>

/* __GLOBAL_ERRNO: a single global int, defined here, declared `extern int errno`
 * by picolibc's <errno.h>. */
int errno;

/* sbrk: hand out a contiguous static arena linearly. picolibc's malloc grows the
 * heap by calling this; returning (void *)-1 + errno=ENOMEM signals exhaustion,
 * exactly as a real machine port over the cron heap would. */
#define PICO_HEAP_BYTES (1u << 18)        /* 256 KiB — ample for the fixture */
#define ENOMEM 12
static char  g_heap[PICO_HEAP_BYTES] __attribute__((aligned(16)));
static size_t g_brk = 0;

void *sbrk(ptrdiff_t incr) {
    if (incr < 0) {
        size_t dec = (size_t)(-incr);
        if (dec > g_brk) { errno = ENOMEM; return (void *)-1; }
        g_brk -= dec;
        return &g_heap[g_brk];
    }
    if ((size_t)incr > PICO_HEAP_BYTES - g_brk) { errno = ENOMEM; return (void *)-1; }
    void *p = &g_heap[g_brk];
    g_brk += (size_t)incr;
    return p;
}
