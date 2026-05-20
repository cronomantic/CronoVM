/* Host harness for the fnptr_export fixture — see fixtures/fnptr_export.c.
 *
 * Verifies that a function whose address is taken (and handed to the host
 * via a syscall) but never internally called still lands in a FUNCS table,
 * so the host can invoke it with cvm_call. This is the exact usage pattern
 * a frame-callback console (Cronopio) relies on.
 *
 * Usage: test_fnptr_export <fnptr_export.bin> */

#include "cvm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int32_t g_registered = -1;

/* cvm_sys_register(fn): the cart passes callback's FUNCS index in R0. */
static int sys_register(struct cvm_image *img, int32_t *regs, void *ud) {
    (void)img; (void)ud;
    g_registered = regs[0];
    regs[0] = 0;
    return 0;
}

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *out_len = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <bin>\n", argv[0]); return 2; }

    size_t   len  = 0;
    uint8_t *blob = slurp(argv[1], &len);
    if (!blob) return 1;

    struct cvm_image img;
    int rc = cvm_load(blob, len, &img);
    if (rc != CVM_OK) { fprintf(stderr, "load: %s\n", cvm_strerror(rc)); return 1; }

    /* The FUNCS section must exist even though the cart never CALLs. */
    if (img.func_offsets == NULL || img.func_count == 0) {
        fprintf(stderr, "FAIL: no FUNCS section (func_count=%u)\n", img.func_count);
        return 1;
    }

    rc = cvm_link(&img, "cvm_sys_register", sys_register, NULL);
    if (rc != CVM_OK) { fprintf(stderr, "link: %s\n", cvm_strerror(rc)); return 1; }

    int32_t ret = 0;
    rc = cvm_run(&img, &ret);
    if (rc != CVM_OK) { fprintf(stderr, "run: %s\n", cvm_strerror(rc)); return 1; }

    if (g_registered <= 0) {
        fprintf(stderr, "FAIL: callback never registered (got %d)\n", g_registered);
        return 1;
    }

    /* Invoke the registered callback via cvm_call — the operation Cronopio
     * does every frame. It must resolve through FUNCS and return 42. */
    int32_t cb_ret = 0;
    rc = cvm_call(&img, (uint32_t)g_registered, NULL, 0, &cb_ret);
    if (rc != CVM_OK) {
        fprintf(stderr, "FAIL: cvm_call(%d): %s\n", g_registered, cvm_strerror(rc));
        return 1;
    }
    if (cb_ret != 42) {
        fprintf(stderr, "FAIL: callback returned %d, want 42\n", cb_ret);
        return 1;
    }

    cvm_image_free(&img);
    free(blob);
    fprintf(stderr, "test_fnptr_export: ok (callback at FUNCS[%d] returned 42)\n",
            g_registered);
    return 0;
}
