/* CronoVM reference allocator — header-only.
 *
 * First-fit free-list allocator that lives entirely in the user binary.
 * The heap is the contiguous region the binary reserved via
 * `--heap-reserve=...`; `cvm_sys_heap_start` / `cvm_sys_heap_size` locate
 * it on first use.
 *
 * Block layout (one word per header, no boundary tags):
 *
 *     +--------+----------- payload -------------+
 *     | header |                                 |
 *     +--------+---------------------------------+
 *      ^4 byte  ^pointer returned to the caller
 *
 * The header is `int32_t size_flag`, where the low bit is the FREE flag
 * and the upper bits are the *whole-block* size (header + payload),
 * always a multiple of 4 — so the low bit is always available. Min
 * block size is 8 (header + 4-byte payload), which also bounds the
 * smallest split remainder we'll keep as its own free block.
 *
 * `cvm_malloc`: first-fit walk from heap start; split if remainder ≥ 8.
 * `cvm_free`: mark free, then a single forward pass merging every
 * adjacent free pair. Coalescing here, rather than on alloc, keeps the
 * fast path simple — games typically free much less often than they
 * allocate. */

#ifndef CVM_ALLOC_H
#define CVM_ALLOC_H

#include <stdint.h>

extern int cvm_sys_heap_start(void);
extern int cvm_sys_heap_size(void);

typedef struct { int32_t size_flag; } _cvm_blk_t;

#define _CVM_BLK_HDR    ((int32_t)4)
#define _CVM_BLK_MIN    ((int32_t)8)
#define _CVM_BLK_SIZE(b) ((int32_t)((b)->size_flag & ~1))
#define _CVM_BLK_FREE(b) ((int32_t)((b)->size_flag & 1))

static char *_cvm_heap_start;
static char *_cvm_heap_end;
static int   _cvm_alloc_inited;

static __attribute__((noinline))
void cvm_alloc_init(void) {
    if (_cvm_alloc_inited) return;
    _cvm_heap_start = (char *)(uintptr_t)cvm_sys_heap_start();
    _cvm_heap_end   = _cvm_heap_start + cvm_sys_heap_size();
    int32_t total = (int32_t)(_cvm_heap_end - _cvm_heap_start) & ~3;
    if (total >= _CVM_BLK_MIN) {
        _cvm_blk_t *b = (_cvm_blk_t *)_cvm_heap_start;
        b->size_flag = total | 1;
        _cvm_heap_end = _cvm_heap_start + total;
    } else {
        _cvm_heap_end = _cvm_heap_start;
    }
    _cvm_alloc_inited = 1;
}

/* `noinline` is deliberate: a free-list walk inlined at every call site
 * blows past the translator's 254-register budget when a function makes
 * more than a couple of allocations. */
static __attribute__((noinline))
void *cvm_malloc(int bytes) {
    if (!_cvm_alloc_inited) cvm_alloc_init();
    if (bytes <= 0) return (void *)0;
    int32_t need = (bytes + 3) & ~3;
    int32_t want = need + _CVM_BLK_HDR;

    char *p = _cvm_heap_start;
    while (p < _cvm_heap_end) {
        _cvm_blk_t *b = (_cvm_blk_t *)p;
        int32_t sz = _CVM_BLK_SIZE(b);
        if (sz < _CVM_BLK_MIN) return (void *)0;
        if (_CVM_BLK_FREE(b) && sz >= want) {
            int32_t leftover = sz - want;
            if (leftover >= _CVM_BLK_MIN) {
                b->size_flag = want;
                _cvm_blk_t *r = (_cvm_blk_t *)(p + want);
                r->size_flag = leftover | 1;
            } else {
                b->size_flag = sz;
            }
            return (void *)(p + _CVM_BLK_HDR);
        }
        p += sz;
    }
    return (void *)0;
}

static __attribute__((noinline))
void cvm_free(void *p) {
    if (!p) return;
    _cvm_blk_t *b = (_cvm_blk_t *)((char *)p - _CVM_BLK_HDR);
    b->size_flag = _CVM_BLK_SIZE(b) | 1;

    char *q = _cvm_heap_start;
    while (q < _cvm_heap_end) {
        _cvm_blk_t *cur = (_cvm_blk_t *)q;
        int32_t sz = _CVM_BLK_SIZE(cur);
        if (sz < _CVM_BLK_MIN) break;
        char *nq = q + sz;
        if (_CVM_BLK_FREE(cur) && nq < _cvm_heap_end) {
            _cvm_blk_t *nxt = (_cvm_blk_t *)nq;
            if (_CVM_BLK_FREE(nxt)) {
                cur->size_flag = (sz + _CVM_BLK_SIZE(nxt)) | 1;
                continue;
            }
        }
        q = nq;
    }
}

#endif /* CVM_ALLOC_H */
