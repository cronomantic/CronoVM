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

/* ---- stdio machine port (when picolibc.bc is built --with-stdio) -------- *
 * tinystdio's FILE layer sits on POSIX open/read/write/lseek/close; its float
 * formatter calls __isnand. A real cart routes these to cron syscalls + the
 * RAM-FS/ROM (cron_sys.c). For the standalone test: console (fd 1/2) is
 * discarded, and fd>=3 is ONE in-memory file (enough for an fopen round-trip).*/
int __isnand(double x) { return x != x; }

/* strerror: picolibc omits it (it would take the _user_strerror hook address);
 * the embedder supplies it (cron_sys.c in a cart). picolibc's perror() calls it. */
char *strerror(int n) { (void)n; return (char *)"error"; }

#define PICO_FILE_BYTES (1u << 16)
static unsigned char g_file[PICO_FILE_BYTES];
static size_t g_file_len = 0;     /* logical EOF */
static size_t g_file_pos = 0;     /* read/write cursor (shared, single fd) */

/* O_TRUNC=0x200 (picolibc fcntl.h); good enough for the fixture's "w"/"r". */
int open(const char *path, int flags, ...) {
    (void)path;
    if (flags & 0x200) { g_file_len = 0; }   /* O_TRUNC */
    g_file_pos = 0;
    return 3;
}
long write(int fd, const void *buf, unsigned long n) {
    if (fd == 1 || fd == 2) return (long)n;   /* console: discard */
    const unsigned char *p = (const unsigned char *)buf;
    for (unsigned long i = 0; i < n; ++i) {
        if (g_file_pos >= PICO_FILE_BYTES) break;
        g_file[g_file_pos++] = p[i];
        if (g_file_pos > g_file_len) g_file_len = g_file_pos;
    }
    return (long)n;
}
long read(int fd, void *buf, unsigned long n) {
    if (fd < 3) return 0;
    unsigned char *p = (unsigned char *)buf;
    unsigned long i = 0;
    for (; i < n && g_file_pos < g_file_len; ++i) p[i] = g_file[g_file_pos++];
    return (long)i;
}
long lseek(int fd, long off, int whence) {
    if (fd < 3) return -1;
    long base = (whence == 1) ? (long)g_file_pos : (whence == 2) ? (long)g_file_len : 0;
    long np = base + off;
    if (np < 0) { errno = 22; return -1; }   /* EINVAL */
    g_file_pos = (size_t)np;
    return np;
}
int close(int fd) { (void)fd; return 0; }
