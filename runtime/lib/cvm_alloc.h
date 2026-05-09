/* CronoVM reference allocator — header-only.
 *
 * Drop-in linear (bump) allocator that lives entirely in the user binary.
 * Calls `cvm_sys_heap_start()` / `cvm_sys_heap_size()` once on first use to
 * find the free region the binary reserved via `--heap-reserve=...`.
 *
 * `cvm_free` is a no-op — this is enough for many retro-game patterns
 * (allocate at startup, never free; or arena-reset between levels). When
 * CALL/RET lands and we ship a multi-function allocator, this header will
 * grow a real free list, but the API stays the same. */

#ifndef CVM_ALLOC_H
#define CVM_ALLOC_H

#include <stdint.h>

extern int cvm_sys_heap_start(void);
extern int cvm_sys_heap_size(void);

static char *_cvm_bump_ptr;
static char *_cvm_bump_end;
static int   _cvm_alloc_inited;

static __attribute__((always_inline)) inline
void cvm_alloc_init(void) {
    if (_cvm_alloc_inited) return;
    _cvm_bump_ptr     = (char *)(uintptr_t)cvm_sys_heap_start();
    _cvm_bump_end     = _cvm_bump_ptr + cvm_sys_heap_size();
    _cvm_alloc_inited = 1;
}

static __attribute__((always_inline)) inline
void *cvm_malloc(int bytes) {
    if (!_cvm_alloc_inited) cvm_alloc_init();
    int n = (bytes + 3) & ~3;
    if (_cvm_bump_ptr + n > _cvm_bump_end) return (void *)0;
    void *p = _cvm_bump_ptr;
    _cvm_bump_ptr += n;
    return p;
}

static __attribute__((always_inline)) inline
void cvm_free(void *p) {
    (void)p;
}

#endif /* CVM_ALLOC_H */
