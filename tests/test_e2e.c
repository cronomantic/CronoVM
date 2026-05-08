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

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_e2e <path-to-bin>\n");
        return 2;
    }

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

    int32_t args[]   = { 7, 5 };
    int32_t expected = 12;
    int32_t got      = 0;

    r = cvm_run_args(&img, args, 2, &got);
    cvm_image_free(&img);
    free(blob);

    if (r != CVM_OK) {
        fprintf(stderr, "run failed: %s\n", cvm_strerror(r));
        return 1;
    }
    if (got != expected) {
        fprintf(stderr, "FAIL: add(7,5) returned %d, expected %d\n",
                got, expected);
        return 1;
    }
    printf("e2e ok: add(7,5) = %d\n", got);
    return 0;
}
