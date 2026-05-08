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

/* Usage: test_e2e <bin> <expected> [args...]
 *   <expected>   integer the program must return via HALT
 *   [args...]    seed for R0..R(N-1) before run
 */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: test_e2e <bin> <expected> [args...]\n");
        return 2;
    }

    size_t len = 0;
    uint8_t *blob = slurp(argv[1], &len);
    if (!blob) return 1;

    int32_t expected = (int32_t)strtol(argv[2], NULL, 0);
    int32_t args[8] = {0};
    int     n_args  = argc - 3;
    if (n_args > 8) n_args = 8;
    for (int i = 0; i < n_args; ++i)
        args[i] = (int32_t)strtol(argv[3 + i], NULL, 0);

    struct cvm_image img;
    int r = cvm_load(blob, len, &img);
    if (r != CVM_OK) {
        fprintf(stderr, "load failed: %s\n", cvm_strerror(r));
        free(blob);
        return 1;
    }

    int32_t got = 0;
    r = cvm_run_args(&img, args, (uint32_t)n_args, &got);
    cvm_image_free(&img);
    free(blob);

    if (r != CVM_OK) {
        fprintf(stderr, "run failed: %s\n", cvm_strerror(r));
        return 1;
    }
    if (got != expected) {
        fprintf(stderr, "FAIL: returned %d, expected %d\n", got, expected);
        return 1;
    }
    printf("e2e ok: returned %d\n", got);
    return 0;
}
