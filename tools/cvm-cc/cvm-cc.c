/* cvm-cc — single-command driver around `clang | cvm-translate`.
 *
 * Hides the two-step `.c → .bc → .bin` pipeline behind one invocation:
 *
 *     cvm-cc user.c -o game.bin --heap-reserve=4M --region=fb:64K:w
 *
 * Pipeline shape:
 *     1. If input is .c: invoke clang with `--target=i386-elf
 *        -emit-llvm -O<level>` plus the runtime include path,
 *        emitting an intermediate `<output>.tmp.bc`.
 *     2. Invoke cvm-translate on that .bc (or directly on the user's
 *        .bc if they passed one), with all pass-through flags.
 *     3. Delete the intermediate unless --keep-bc was given.
 *
 * Tool discovery:
 *     - clang: --clang= flag, then PATH.
 *     - cvm-translate: --translate= flag, then sibling of cvm-cc, then
 *       CVM_TRANSLATOR_DEFAULT (CMake-baked build-tree path), then PATH.
 *     - runtime headers: --runtime-dir= flag, then the installed-tree
 *       layout `<exedir>/../share/cronovm/runtime/lib`, then
 *       CVM_RUNTIME_DIR baked in by CMake (the build-tree path).
 *       Build-tree first via the bake means in-tree development
 *       continues to work; the install probe activates only when
 *       cvm-cc is run from `<prefix>/bin/`.
 */

/* strdup() is POSIX, not C standard. Without this, glibc's <string.h>
 * hides the declaration under -std=c11 and GCC implicitly types the
 * function as `int(...)` — on 64-bit Linux that truncates returned
 * heap pointers and the next read crashes. Set the smallest POSIX
 * level that exposes strdup (POSIX.1-2008). */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <io.h>
#  include <process.h>
#  define PATH_SEP '\\'
#else
#  include <sys/wait.h>
#  include <unistd.h>
#  define PATH_SEP '/'
#endif

#ifndef CVM_RUNTIME_DIR
/* Build system should always define this. The fallback only exists so a
 * stand-alone compile (without CMake) doesn't fail loudly. */
#  define CVM_RUNTIME_DIR "."
#endif

#ifndef CVM_TRANSLATOR_DEFAULT
/* Same idea: CMake bakes in the build-tree path so cvm-cc finds its
 * sibling tool without PATH plumbing. Override at run time with
 * --translate=. */
#  define CVM_TRANSLATOR_DEFAULT "cvm-translate"
#endif

#define MAX_REGIONS         64
#define MAX_INCLUDE_DIRS    32

struct cli {
    const char *input;          /* .c or .bc, required positional */
    const char *output;         /* -o, required */
    const char *opt_level;      /* -O<n>, default "1" */
    const char *clang_path;     /* --clang= */
    const char *translate_path; /* --translate= */
    const char *runtime_dir;    /* --runtime-dir= */

    const char *heap_reserve;   /* --heap-reserve=, raw string */
    const char *stack_reserve;  /* --stack-reserve=, raw string */
    const char *rom;            /* --rom=, raw string (pass-through) */
    const char *regions[MAX_REGIONS];
    int         region_count;
    const char *include_dirs[MAX_INCLUDE_DIRS];
    int         include_count;

    int keep_bc;
    int verbose;
};

static void usage(FILE *f) {
    fprintf(f,
        "Usage: cvm-cc <input.c|input.bc> -o <output.bin> [options]\n"
        "\n"
        "Driver around clang + cvm-translate. The .c case runs clang with\n"
        "--target=i386-elf -emit-llvm -O<level> and pipes the bitcode\n"
        "through cvm-translate. .bc input skips clang.\n"
        "\n"
        "Required:\n"
        "  -o <file>                  output .bin path\n"
        "\n"
        "Pass-through to cvm-translate:\n"
        "  --heap-reserve=N[K|M]      free heap region for the user allocator\n"
        "  --stack-reserve=N[K|M]     stack region for CALL/RET (default 16K\n"
        "                             when the binary uses any CALL)\n"
        "  --region=NAME:SIZE[:DIR]   host-shared region; DIR is r/w/rw\n"
        "  --rom=FILE                 bake FILE as read-only cartridge ROM\n"
        "                             (default rw); repeatable up to %d\n"
        "\n"
        "Pass-through to clang:\n"
        "  -I <dir>                   extra include dir (repeatable)\n"
        "  -O0|-O1|-O2|-O3|-Os        optimisation level (default -O1)\n"
        "\n"
        "Tool discovery overrides:\n"
        "  --clang=PATH               override clang binary\n"
        "  --translate=PATH           override cvm-translate binary\n"
        "  --runtime-dir=PATH         override the runtime/lib include dir\n"
        "                             (built-in default: %s)\n"
        "\n"
        "Misc:\n"
        "  --keep-bc                  don't delete the intermediate .bc\n"
        "  -v, --verbose              print every command before running\n"
        "  -h, --help                 this help\n",
        MAX_REGIONS, CVM_RUNTIME_DIR);
}

static int has_suffix(const char *s, const char *suf) {
    size_t sl = strlen(s), tl = strlen(suf);
    return sl >= tl && memcmp(s + sl - tl, suf, tl) == 0;
}

/* Run `argv` (NULL-terminated) via exec/spawn. Returns the child's
 * exit status, or -1 on infrastructure failure. We deliberately
 * bypass system()/cmd.exe so argument quoting isn't an issue:
 * Windows' _spawnvp uses CreateProcess (no shell), POSIX uses
 * fork+execvp. */
static void verbose_dump(char **argv) {
    fprintf(stderr, "cvm-cc:");
    for (int i = 0; argv[i]; ++i) fprintf(stderr, " %s", argv[i]);
    fprintf(stderr, "\n");
}

static int run_cmd(const struct cli *cli, char **argv) {
    if (cli->verbose) verbose_dump(argv);

#if defined(_WIN32)
    /* Microsoft's CRT joins argv into a command line for CreateProcess
     * and re-quotes elements containing spaces; we don't have to. */
    intptr_t r = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
    if (r == -1) {
        fprintf(stderr, "cvm-cc: failed to spawn '%s': %s\n",
                argv[0], strerror(errno));
        return -1;
    }
    return (int)r;
#else
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "cvm-cc: failed to exec '%s': %s\n",
                argv[0], strerror(errno));
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { perror("waitpid"); return -1; }
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

/* Build "<dirname(argv0)><sep><name>" if argv0 has a directory part. */
static char *sibling_of(const char *argv0, const char *name) {
    const char *slash = strrchr(argv0, '/');
#if defined(_WIN32)
    const char *back = strrchr(argv0, '\\');
    if (back && (!slash || back > slash)) slash = back;
#endif
    if (!slash) return NULL;
    size_t dirlen = (size_t)(slash - argv0);
    size_t namlen = strlen(name);
    char  *p = (char *)malloc(dirlen + 1 + namlen + 1);
    if (!p) { perror("malloc"); return NULL; }
    memcpy(p, argv0, dirlen);
    p[dirlen] = PATH_SEP;
    memcpy(p + dirlen + 1, name, namlen + 1);
    return p;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* If cvm-cc lives in `<prefix>/bin/`, the install layout puts the
 * runtime headers at `<prefix>/share/cronovm/runtime/lib/`. Probe by
 * looking for `cvm_intrin.h` there; on hit, return the directory.
 * Caller owns the returned string. */
static char *find_install_runtime_dir(const char *argv0) {
    const char *slash = strrchr(argv0, '/');
#if defined(_WIN32)
    const char *back = strrchr(argv0, '\\');
    if (back && (!slash || back > slash)) slash = back;
#endif
    if (!slash) return NULL;
    size_t dirlen = (size_t)(slash - argv0);

    char probe[1024];
    int n = snprintf(probe, sizeof probe,
                     "%.*s%c..%cshare%ccronovm%cruntime%clib%ccvm_intrin.h",
                     (int)dirlen, argv0,
                     PATH_SEP, PATH_SEP, PATH_SEP,
                     PATH_SEP, PATH_SEP, PATH_SEP);
    if (n < 0 || n >= (int)sizeof probe) return NULL;
    if (!file_exists(probe)) return NULL;

    char *result = (char *)malloc((size_t)n + 1);  /* same envelope */
    if (!result) return NULL;
    snprintf(result, (size_t)n + 1,
             "%.*s%c..%cshare%ccronovm%cruntime%clib",
             (int)dirlen, argv0,
             PATH_SEP, PATH_SEP, PATH_SEP,
             PATH_SEP, PATH_SEP);
    return result;
}

/* Locate cvm-translate. Order:
 *   1. --translate=PATH (explicit override)
 *   2. sibling of cvm-cc (the install layout)
 *   3. CVM_TRANSLATOR_DEFAULT baked in by CMake (the build-tree
 *      layout: tools/translator/cvm-translate next to tools/cvm-cc)
 *   4. "cvm-translate" resolved by the OS via PATH
 * Returned pointer is owned by the caller (free). */
static char *find_translator(const struct cli *cli, const char *argv0) {
    if (cli->translate_path) return strdup(cli->translate_path);

#if defined(_WIN32)
    const char *exename = "cvm-translate.exe";
#else
    const char *exename = "cvm-translate";
#endif
    char *sib = sibling_of(argv0, exename);
    if (sib && file_exists(sib)) return sib;
    free(sib);

    if (file_exists(CVM_TRANSLATOR_DEFAULT))
        return strdup(CVM_TRANSLATOR_DEFAULT);

    return strdup(exename);
}

static int parse_argv(int argc, char **argv, struct cli *cli) {
    cli->opt_level   = "1";
    /* runtime_dir resolved in main() after parse_argv: either user's
     * --runtime-dir, or the install-layout probe, or the build-tree bake. */

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) { usage(stderr); return 2; }
            cli->output = argv[++i];
        } else if (strcmp(a, "-I") == 0) {
            if (i + 1 >= argc) { usage(stderr); return 2; }
            if (cli->include_count >= MAX_INCLUDE_DIRS) {
                fprintf(stderr, "cvm-cc: too many -I (max %d)\n",
                        MAX_INCLUDE_DIRS);
                return 2;
            }
            cli->include_dirs[cli->include_count++] = argv[++i];
        } else if (strncmp(a, "-O", 2) == 0 && a[2] != '\0') {
            cli->opt_level = a + 2;
        } else if (strncmp(a, "--heap-reserve=", 15) == 0) {
            cli->heap_reserve = a;
        } else if (strncmp(a, "--stack-reserve=", 16) == 0) {
            cli->stack_reserve = a;
        } else if (strncmp(a, "--rom=", 6) == 0) {
            cli->rom = a;
        } else if (strncmp(a, "--region=", 9) == 0) {
            if (cli->region_count >= MAX_REGIONS) {
                fprintf(stderr, "cvm-cc: too many --region (max %d)\n",
                        MAX_REGIONS);
                return 2;
            }
            cli->regions[cli->region_count++] = a;
        } else if (strncmp(a, "--clang=", 8) == 0) {
            cli->clang_path = a + 8;
        } else if (strncmp(a, "--translate=", 12) == 0) {
            cli->translate_path = a + 12;
        } else if (strncmp(a, "--runtime-dir=", 14) == 0) {
            cli->runtime_dir = a + 14;
        } else if (strcmp(a, "--keep-bc") == 0) {
            cli->keep_bc = 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            cli->verbose = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "cvm-cc: unknown option '%s'\n", a);
            usage(stderr);
            return 2;
        } else if (cli->input) {
            fprintf(stderr, "cvm-cc: only one input file is supported "
                            "(got '%s' and '%s')\n", cli->input, a);
            return 2;
        } else {
            cli->input = a;
        }
    }
    if (!cli->input)  { fprintf(stderr, "cvm-cc: missing input file\n"); usage(stderr); return 2; }
    if (!cli->output) { fprintf(stderr, "cvm-cc: missing -o <output>\n"); usage(stderr); return 2; }
    return 0;
}

int main(int argc, char **argv) {
    struct cli cli;
    memset(&cli, 0, sizeof cli);
    int rc = parse_argv(argc, argv, &cli);
    if (rc) return rc;

    /* Resolve runtime_dir: explicit > install-layout probe > build-tree bake.
     * `installed` is leaked deliberately — its lifetime is the process. */
    if (!cli.runtime_dir) {
        char *installed = find_install_runtime_dir(argv[0]);
        cli.runtime_dir = installed ? installed : CVM_RUNTIME_DIR;
    }

    int   input_is_c = has_suffix(cli.input, ".c");
    int   input_is_bc = has_suffix(cli.input, ".bc");
    if (!input_is_c && !input_is_bc) {
        fprintf(stderr, "cvm-cc: input must end in .c or .bc (got '%s')\n",
                cli.input);
        return 2;
    }

    /* Intermediate .bc path. Predictable name next to the output keeps
     * temp-dir logic out of this tool; --keep-bc surfaces it for
     * inspection during debugging. */
    char *bc_path = NULL;
    int   bc_owned = 0;
    if (input_is_c) {
        size_t outlen = strlen(cli.output);
        bc_path = (char *)malloc(outlen + 8);
        if (!bc_path) { perror("malloc"); return 1; }
        memcpy(bc_path, cli.output, outlen);
        memcpy(bc_path + outlen, ".tmp.bc", 8);
        bc_owned = 1;
    } else {
        bc_path = (char *)cli.input;
    }

    /* Step 1: clang (only for .c input). */
    if (input_is_c) {
        const char *clang = cli.clang_path ? cli.clang_path : "clang";
        char optflag[8];
        snprintf(optflag, sizeof optflag, "-O%s", cli.opt_level);
        char rtinc[1024];
        snprintf(rtinc, sizeof rtinc, "-I%s", cli.runtime_dir);

        char *cargv[64];
        int   n = 0;
        cargv[n++] = (char *)clang;
        cargv[n++] = (char *)"--target=i386-elf";
        /* Freestanding: carts run on the VM, not a hosted OS. Besides the
         * correct semantics (no __STDC_HOSTED__, clang's own <stdint.h> etc.),
         * it keeps clang emitting the i32-length mem intrinsics the VM expects
         * rather than the i64 variants it picks in hosted mode. Matches how
         * the test fixtures are compiled. */
        cargv[n++] = (char *)"-ffreestanding";
        cargv[n++] = (char *)"-emit-llvm";
        cargv[n++] = optflag;
        cargv[n++] = rtinc;
        for (int k = 0; k < cli.include_count; ++k) {
            cargv[n++] = (char *)"-I";
            cargv[n++] = (char *)cli.include_dirs[k];
        }
        cargv[n++] = (char *)"-c";
        cargv[n++] = (char *)cli.input;
        cargv[n++] = (char *)"-o";
        cargv[n++] = bc_path;
        cargv[n] = NULL;

        int crc = run_cmd(&cli, cargv);
        if (crc != 0) {
            fprintf(stderr, "cvm-cc: clang failed (exit %d)\n", crc);
            if (bc_owned && !cli.keep_bc) remove(bc_path);
            if (bc_owned) free(bc_path);
            return crc < 0 ? 1 : crc;
        }
    }

    /* Step 2: cvm-translate. */
    char *translator = find_translator(&cli, argv[0]);
    if (!translator) {
        if (bc_owned && !cli.keep_bc) remove(bc_path);
        if (bc_owned) free(bc_path);
        return 1;
    }

    char *targv[16 + MAX_REGIONS];
    int   n = 0;
    targv[n++] = translator;
    targv[n++] = bc_path;
    targv[n++] = (char *)"-o";
    targv[n++] = (char *)cli.output;
    if (cli.heap_reserve)  targv[n++] = (char *)cli.heap_reserve;
    if (cli.stack_reserve) targv[n++] = (char *)cli.stack_reserve;
    if (cli.rom)           targv[n++] = (char *)cli.rom;
    for (int k = 0; k < cli.region_count; ++k)
        targv[n++] = (char *)cli.regions[k];
    targv[n] = NULL;

    int trc = run_cmd(&cli, targv);
    free(translator);

    if (bc_owned && !cli.keep_bc) remove(bc_path);
    if (bc_owned) free(bc_path);

    if (trc != 0) {
        fprintf(stderr, "cvm-cc: cvm-translate failed (exit %d)\n", trc);
        return trc < 0 ? 1 : trc;
    }
    return 0;
}
