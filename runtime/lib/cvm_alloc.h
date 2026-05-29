/* CronoVM reference allocator — header-only.
 *
 * Explicit free-list allocator with boundary tags, living entirely in the
 * user binary. The heap is the contiguous region reserved via
 * `--heap-reserve=...`; `cvm_sys_heap_start` / `cvm_sys_heap_size` locate it.
 *
 * Block layout (4-byte header + 4-byte footer, both carry the whole-block
 * size which is always a multiple of 4):
 *
 *     +--------+------------- payload -------------+--------+
 *     | header |                                   | footer |
 *     +--------+-----------------------------------+--------+
 *      ^4 byte  ^pointer returned to the caller      ^4 byte
 *
 * The header is `int32_t size_flag`: low bit = FREE flag, upper bits = whole
 * block size (header + payload + footer). The footer is the plain whole size
 * (no flag) and lets `cvm_free` find the PREVIOUS physical block in O(1) for
 * backward coalescing. FREE blocks additionally hold a doubly-linked
 * free-list {next, prev} in the first 8 payload bytes — so `cvm_malloc` walks
 * only FREE blocks (not every block) and `cvm_free` splices in O(1).
 *
 * WHY (history): the previous version was a first-fit walk over ALL blocks
 * from the heap start on every malloc, plus a full-heap coalescing scan on
 * every free — both O(total blocks), i.e. O(n^2) for allocation-heavy
 * workloads. Mounting a ~10k-entry archive (UQM's libs/uio over a .uqm ZIP)
 * spent minutes there. The explicit free list + boundary tags make malloc
 * proportional to the free-list length and free O(1) amortised.
 *
 * Min block is 16 (header + 8-byte payload for the free links + footer),
 * which also bounds the smallest split remainder kept as its own free block.
 * Allocations smaller than 8 bytes are rounded up so a freed block can always
 * hold its {next, prev} links. */

#ifndef CVM_ALLOC_H
#define CVM_ALLOC_H

#include <stdint.h>

extern int cvm_sys_heap_start(void);
extern int cvm_sys_heap_size(void);

typedef struct { int32_t size_flag; } _cvm_blk_t;

#define _CVM_HDR    ((int32_t)4)
#define _CVM_FTR    ((int32_t)4)
#define _CVM_MIN    ((int32_t)16)
#define _CVM_SIZE(b) ((int32_t)(((_cvm_blk_t *)(b))->size_flag & ~1))
#define _CVM_FREE(b) ((int32_t)(((_cvm_blk_t *)(b))->size_flag & 1))

static char *_cvm_heap_start;
static char *_cvm_heap_end;
static char *_cvm_free_head;        /* doubly-linked free list (NULL = empty) */
static int   _cvm_alloc_inited;

/* Free-block link accessors (the links live in the payload, at +4 and +8). */
static char *_cvm_fl_next(char *b)              { return *(char **)(b + 4); }
static char *_cvm_fl_prev(char *b)              { return *(char **)(b + 8); }
static void  _cvm_fl_set_next(char *b, char *v) { *(char **)(b + 4) = v; }
static void  _cvm_fl_set_prev(char *b, char *v) { *(char **)(b + 8) = v; }

/* Stamp a block's header + footer with `size` and the free flag. */
static void _cvm_stamp(char *b, int32_t size, int free) {
    ((_cvm_blk_t *)b)->size_flag = size | (free ? 1 : 0);
    *(int32_t *)(b + size - _CVM_FTR) = size;
}

/* Doubly-linked free-list insert (LIFO) / remove. */
static void _cvm_fl_push(char *b) {
    _cvm_fl_set_prev(b, (char *)0);
    _cvm_fl_set_next(b, _cvm_free_head);
    if (_cvm_free_head)
        _cvm_fl_set_prev(_cvm_free_head, b);
    _cvm_free_head = b;
}
static void _cvm_fl_remove(char *b) {
    char *pv = _cvm_fl_prev(b);
    char *nx = _cvm_fl_next(b);
    if (pv) _cvm_fl_set_next(pv, nx); else _cvm_free_head = nx;
    if (nx) _cvm_fl_set_prev(nx, pv);
}

static __attribute__((noinline))
void cvm_alloc_init(void) {
    if (_cvm_alloc_inited) return;
    _cvm_heap_start = (char *)(uintptr_t)cvm_sys_heap_start();
    _cvm_heap_end   = _cvm_heap_start + cvm_sys_heap_size();
    _cvm_free_head  = (char *)0;
    int32_t total = (int32_t)(_cvm_heap_end - _cvm_heap_start) & ~3;
    if (total >= _CVM_MIN) {
        _cvm_heap_end = _cvm_heap_start + total;
        _cvm_stamp(_cvm_heap_start, total, 1);
        _cvm_fl_push(_cvm_heap_start);
    } else {
        _cvm_heap_end = _cvm_heap_start;
    }
    _cvm_alloc_inited = 1;
}

/* `noinline` is deliberate: a free-list walk inlined at every call site blows
 * past the translator's 254-register budget when a function makes more than a
 * couple of allocations. */
static __attribute__((noinline))
void *cvm_malloc(int bytes) {
    if (!_cvm_alloc_inited) cvm_alloc_init();
    if (bytes <= 0) return (void *)0;
    int32_t need = (bytes + 3) & ~3;
    if (need < 8) need = 8;                 /* room for the free links later */
    int32_t want = need + _CVM_HDR + _CVM_FTR;

    char *b = _cvm_free_head;
    while (b) {
        int32_t sz = _CVM_SIZE(b);
        if (sz >= want) {
            _cvm_fl_remove(b);
            int32_t leftover = sz - want;
            if (leftover >= _CVM_MIN) {
                _cvm_stamp(b, want, 0);
                char *r = b + want;
                _cvm_stamp(r, leftover, 1);
                _cvm_fl_push(r);
            } else {
                _cvm_stamp(b, sz, 0);
            }
            return (void *)(b + _CVM_HDR);
        }
        b = _cvm_fl_next(b);
    }
    return (void *)0;
}

static __attribute__((noinline))
void cvm_free(void *p) {
    if (!p) return;
    char *b = (char *)p - _CVM_HDR;
    int32_t sz = _CVM_SIZE(b);

    /* Coalesce with the next physical block if it is free (O(1)). */
    char *nx = b + sz;
    if (nx < _cvm_heap_end && _CVM_FREE(nx)) {
        _cvm_fl_remove(nx);
        sz += _CVM_SIZE(nx);
    }
    /* Coalesce with the previous physical block if it is free, found via its
     * footer (the 4 bytes immediately before our header). */
    if (b > _cvm_heap_start) {
        int32_t psz = *(int32_t *)(b - _CVM_FTR);
        char *pb = b - psz;
        if (pb >= _cvm_heap_start && _CVM_FREE(pb)) {
            _cvm_fl_remove(pb);
            sz += psz;
            b = pb;
        }
    }
    _cvm_stamp(b, sz, 1);
    _cvm_fl_push(b);
}

#endif /* CVM_ALLOC_H */
