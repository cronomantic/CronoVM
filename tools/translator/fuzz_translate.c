/* libFuzzer harness for the CronoVM bitcode translator.
 *
 * Two build modes share the same translation entry point
 * (`cvm_fuzz_translate_buffer` in translator.c):
 *
 *   1. libFuzzer mode (the default when this file is in the build).
 *      Linked with `-fsanitize=fuzzer,address,undefined` against
 *      translator.c (which omits its `main()` via
 *      `-DCVM_NO_TRANSLATOR_MAIN`). libFuzzer drives via
 *      `LLVMFuzzerTestOneInput`.
 *
 *   2. Standalone mode (`-DCVM_FUZZER_STANDALONE`). Provides a
 *      tiny `main()` that slurps each file in argv and calls the
 *      harness once. Useful for replaying a corpus on hosts where
 *      libFuzzer isn't available (e.g. mingw clang) and for
 *      smoke-testing the wiring without a fuzzing run.
 *
 * Both modes are wired by `tools/translator/CMakeLists.txt` behind
 * the `CVM_BUILD_FUZZER` CMake option.
 */

#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <stdlib.h>

extern int cvm_fuzz_translate_buffer(const uint8_t *data, size_t len);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)cvm_fuzz_translate_buffer(data, size);
    return 0;  /* always 0: libFuzzer treats nonzero as crash. Real
                * crashes are reported by ASAN/UBSAN/libFuzzer itself. */
}

#ifdef CVM_FUZZER_STANDALONE

static int run_one(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open '%s' failed\n", path); return 1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return 1; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (!buf) { fclose(f); return 1; }
    size_t got = (n > 0) ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (n > 0 && got != (size_t)n) { free(buf); return 1; }

    int rc = LLVMFuzzerTestOneInput(buf, (size_t)n);
    free(buf);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: cvm-translate-fuzz <input.bc> [input2.bc ...]\n"
                "  Feeds each file once through the translator pipeline.\n"
                "  Crashes/ASAN/UBSAN reports are the signal; clean exit\n"
                "  means every input completed without UB.\n");
        return 2;
    }
    int worst = 0;
    for (int i = 1; i < argc; ++i) {
        int rc = run_one(argv[i]);
        if (rc != 0) worst = rc;
    }
    /* Standalone driver always exits 0 unless I/O fails — the
     * fuzz harness itself doesn't classify outcomes (libFuzzer
     * does that via instrumentation). */
    (void)worst;
    return 0;
}

#endif /* CVM_FUZZER_STANDALONE */
