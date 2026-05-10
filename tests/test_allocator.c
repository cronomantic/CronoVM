/* Drives `cvm_load_ex` with a custom allocator that:
 *   1. Counts every alloc/free.
 *   2. Verifies free is called with each pointer alloc returned.
 *   3. Tracks peak bytes outstanding.
 *
 * After the run, the bookkeeping must show alloc_count == free_count
 * (no leaks) and the same set of pointers on both sides. The expected
 * count depends on the binary — a vanilla `add.bin` allocates 2 blocks
 * (code, IMPORTS-free path) — but the test only checks consistency,
 * not absolute counts, so it's robust against future loader tweaks
 * that add or remove an allocation. */

#include "cvm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LIVE 32

struct tracking_alloc {
    size_t  alloc_calls;
    size_t  free_calls;
    size_t  peak_live;
    size_t  cur_live;
    void   *live_ptrs[MAX_LIVE];
    size_t  live_sizes[MAX_LIVE];
};

static void *track_alloc(size_t bytes, void *ud) {
    struct tracking_alloc *t = (struct tracking_alloc *)ud;
    void *p = malloc(bytes);
    if (!p) return NULL;
    t->alloc_calls++;
    if (t->cur_live >= MAX_LIVE) {
        fprintf(stderr, "track_alloc: too many live allocs (>%d)\n", MAX_LIVE);
        free(p); return NULL;
    }
    t->live_ptrs [t->cur_live] = p;
    t->live_sizes[t->cur_live] = bytes;
    t->cur_live++;
    if (t->cur_live > t->peak_live) t->peak_live = t->cur_live;
    return p;
}

static void track_free(void *p, void *ud) {
    struct tracking_alloc *t = (struct tracking_alloc *)ud;
    /* Find the pointer in our live set and remove it. Linear scan;
     * MAX_LIVE is small, this is a test, not the inner loop. */
    for (size_t i = 0; i < t->cur_live; i++) {
        if (t->live_ptrs[i] == p) {
            t->live_ptrs [i] = t->live_ptrs [t->cur_live - 1];
            t->live_sizes[i] = t->live_sizes[t->cur_live - 1];
            t->cur_live--;
            t->free_calls++;
            free(p);
            return;
        }
    }
    fprintf(stderr, "track_free: free of pointer %p not from track_alloc\n", p);
    free(p);
}

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *out_len = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <bin>\n", argv[0]);
        return 2;
    }

    size_t   blob_len = 0;
    uint8_t *blob     = slurp(argv[1], &blob_len);
    if (!blob) return 1;

    struct tracking_alloc tracker = {0};
    cvm_allocator_t       a       = { track_alloc, track_free, &tracker };

    struct cvm_image img;
    int rc = cvm_load_ex(blob, blob_len, &img, &a);
    if (rc != CVM_OK) {
        fprintf(stderr, "cvm_load_ex: %s\n", cvm_strerror(rc));
        free(blob);
        return 1;
    }

    /* Run the binary so the load path is exercised end-to-end (not
     * strictly needed to test the allocator, but rules out a
     * regression where load succeeded but produced an unrunnable
     * image). add.bin needs R0,R1 seeded; pass 7+5=12. */
    int32_t args[] = { 7, 5 };
    int32_t got = 0;
    rc = cvm_run_args(&img, args, 2, &got);
    if (rc != CVM_OK || got != 12) {
        fprintf(stderr, "cvm_run_args: rc=%d got=%d\n", rc, got);
        cvm_image_free(&img);
        free(blob);
        return 1;
    }

    size_t peak = tracker.peak_live;
    size_t allocs_before_free = tracker.alloc_calls;

    cvm_image_free(&img);
    free(blob);

    /* Verify no leaks: every alloc had a matching free, no live
     * pointers remaining. */
    if (tracker.cur_live != 0) {
        fprintf(stderr, "FAIL: %zu pointers still live after cvm_image_free\n",
                tracker.cur_live);
        return 1;
    }
    if (tracker.alloc_calls != tracker.free_calls) {
        fprintf(stderr, "FAIL: alloc/free count mismatch %zu vs %zu\n",
                tracker.alloc_calls, tracker.free_calls);
        return 1;
    }
    if (allocs_before_free == 0) {
        fprintf(stderr, "FAIL: load made no allocations through the hook\n");
        return 1;
    }

    printf("allocator ok: %zu allocs, peak %zu live, all freed\n",
           tracker.alloc_calls, peak);
    return 0;
}
