/* Host shell that consumes CronoVM as a library: load a .bin from
 * disk, run it, print the result. Mirrors what a downstream
 * application (game engine, scripting host, plugin runtime) would do.
 *
 * Build via CMake: `cmake --build` produces `embedder_host` linked
 * against `cvm`, plus `game.bin` produced by cvm-cc. Run:
 *     ./embedder_host game.bin
 * Expect "game returned 42" and exit 0. */

#include "cvm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        fprintf(stderr, "usage: %s <game.bin>\n", argv[0]);
        return 2;
    }

    size_t   blob_len = 0;
    uint8_t *blob     = slurp(argv[1], &blob_len);
    if (!blob) return 1;

    printf("CronoVM %s loading %s (%zu bytes)\n",
           cvm_version_string(), argv[1], blob_len);

    struct cvm_image img;
    int rc = cvm_load(blob, blob_len, &img);
    if (rc != CVM_OK) {
        fprintf(stderr, "cvm_load: %s\n", cvm_strerror(rc));
        free(blob);
        return 1;
    }

    int32_t result = 0;
    rc = cvm_run(&img, &result);
    cvm_image_free(&img);
    free(blob);

    if (rc != CVM_OK) {
        fprintf(stderr, "cvm_run: %s\n", cvm_strerror(rc));
        return 1;
    }

    printf("game returned %d\n", result);
    return result == 42 ? 0 : 1;
}
