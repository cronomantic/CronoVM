#include "cvm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open '%s' failed\n", path); return NULL; }
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

/* Drives the regions.c fixture:
 *  1. Resolves the "input" and "fb" regions via cvm_image_get_region.
 *  2. Pre-fills "input" with the i32 value INPUT_VAL.
 *  3. Runs the binary; expects return 0.
 *  4. Reads "fb" back and checks it holds the pattern
 *     fb[i] == (uint8_t)(i * INPUT_VAL) for i in [0, 16). */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_regions <bin>\n");
        return 2;
    }

    const int32_t INPUT_VAL = 7;

    size_t len = 0;
    uint8_t *blob = slurp(argv[1], &len);
    if (!blob) return 1;

    struct cvm_image img;
    int r = cvm_load(blob, len, &img);
    if (r != CVM_OK) {
        fprintf(stderr, "load failed: %s\n", cvm_strerror(r));
        free(blob);
        return 1;
    }

    uint32_t in_off = 0, in_size = 0, fb_off = 0, fb_size = 0;
    if (cvm_image_get_region(&img, "input", &in_off, &in_size) != CVM_OK) {
        fprintf(stderr, "region 'input' not found\n");
        goto fail;
    }
    if (cvm_image_get_region(&img, "fb", &fb_off, &fb_size) != CVM_OK) {
        fprintf(stderr, "region 'fb' not found\n");
        goto fail;
    }
    if (in_size < sizeof(int32_t)) {
        fprintf(stderr, "input region size %u < %zu\n",
                in_size, sizeof(int32_t));
        goto fail;
    }
    if (fb_size < 16) {
        fprintf(stderr, "fb region size %u < 16\n", fb_size);
        goto fail;
    }

    if (cvm_heap_write(&img, in_off, &INPUT_VAL, sizeof INPUT_VAL) != CVM_OK) {
        fprintf(stderr, "host write to 'input' failed\n");
        goto fail;
    }

    int32_t got = 0;
    r = cvm_run(&img, &got);
    if (r != CVM_OK) {
        fprintf(stderr, "run failed: %s\n", cvm_strerror(r));
        goto fail;
    }
    if (got != 0) {
        fprintf(stderr, "fixture returned %d (expected 0)\n", got);
        goto fail;
    }

    for (int i = 0; i < 16; ++i) {
        uint8_t want = (uint8_t)(i * INPUT_VAL);
        uint8_t have = img.heap[fb_off + (uint32_t)i];
        if (have != want) {
            fprintf(stderr, "fb[%d] = %u, expected %u\n", i, have, want);
            goto fail;
        }
    }

    /* Lookup of an unknown name must report missing, not crash. */
    if (cvm_image_get_region(&img, "nope", NULL, NULL) != CVM_E_NO_SUCH_REGION) {
        fprintf(stderr, "lookup of unknown region didn't return NO_SUCH_REGION\n");
        goto fail;
    }

    printf("regions ok: in_off=%u fb_off=%u\n", in_off, fb_off);
    cvm_image_free(&img);
    free(blob);
    return 0;

fail:
    cvm_image_free(&img);
    free(blob);
    return 1;
}
