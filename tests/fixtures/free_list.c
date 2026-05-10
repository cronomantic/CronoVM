#include "cvm_alloc.h"

/* Exercises the free-list allocator's split + coalesce paths. Returns 0
 * on success, otherwise a small non-zero code identifying which step
 * regressed. The CMake rule reserves a 512-byte heap so phase 2 can
 * fill the heap tightly enough that coalescing is the only way the
 * final allocation can succeed.
 *
 * Per-call `if (!ptr) return N` is deliberate: that pattern (many calls
 * each followed by a null-check branch) is what triggered the
 * cg_relax_branches stale-cap bug fixed in this same step. Keep it. */
int free_list_main(int n) {
    (void)n;

    /* Phase 1 — reuse: alloc, free, alloc same size returns the same
     * address (coalesce restored the heap to one big free block). */
    void *a = cvm_malloc(64);
    if (!a) return 1;
    cvm_free(a);
    void *b = cvm_malloc(64);
    if (!b) return 2;
    if (a != b) return 3;
    cvm_free(b);

    /* Phase 2 — split + coalesce: the third allocation pins the tail of
     * the 512-byte heap, leaving a too-small remainder. After freeing
     * the first two, only a merged neighbour pair is large enough for
     * `mid`. Without coalescing this returns NULL. */
    char *A = (char *)cvm_malloc(100);
    char *B = (char *)cvm_malloc(100);
    char *C = (char *)cvm_malloc(280);
    if (!A || !B || !C) return 4;
    cvm_free(A);
    cvm_free(B);
    void *mid = cvm_malloc(200);
    if (!mid) return 5;
    cvm_free(mid);
    cvm_free(C);

    /* Phase 3 — OOM: the heap can't possibly fit this. */
    if (cvm_malloc(10000)) return 6;

    return 0;
}
