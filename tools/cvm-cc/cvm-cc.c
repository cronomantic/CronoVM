/* cvm-cc — single-command driver around `clang | llvm-link | cvm-translate`.
 *
 * Hides the `.c → .bc → .bin` pipeline behind one invocation, for one or
 * many inputs (no linker on the VM side, so multi-file ports link here):
 *
 *     cvm-cc user.c -o game.bin --heap-reserve=4M --region=fb:64K:w
 *     cvm-cc r_draw.c r_main.c i_video.c ... -o doom.bin
 *
 * Pipeline shape:
 *     1. Compile each .c with clang `--target=i386-elf -ffreestanding
 *        -emit-llvm -O<level>` plus the runtime include path, each to an
 *        intermediate `<output>.<i>.tmp.bc`. (.bc inputs skip this.)
 *     2. If there is more than one bitcode module, llvm-link them into one
 *        `<output>.linked.bc`. A single module skips the link step. This is
 *        real C linkage: file-local statics are uniqued per module, only
 *        true extern duplicates error.
 *     3. With --lto, run `opt 'default<O2>'` on the (linked) module so the
 *        inliner runs across all files (cross-TU inlining). Vectorisation is
 *        forced off — the VM/translator has no vector types.
 *     4. Invoke cvm-translate on the resulting module with all pass-through
 *        flags.
 *     5. Delete the intermediates unless --keep-bc was given.
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

/* Bitmask exit codes from `cvm-translate --probe-runtime`: which soft runtimes
 * the module needs linked. Must match the CVM_PROBE_* values in
 * tools/translator/translator.c. The two bits are disjoint, so they OR/AND
 * cleanly (both => 30). */
#define CVM_PROBE_F64    10   /* uses double -> link the f64 runtime  */
#define CVM_PROBE_I64    20   /* uses i64 div/rem -> link the i64 runtime */
#define CVM_PROBE_CXXSTL 64   /* references the std exception ABI -> link
                               * cvm_cxxstl (bit-disjoint from F64|I64) */
#define CVM_PROBE_IOSTREAM 128 /* references the libc++ iostream/locale ABI ->
                               * link the prebuilt cxxio.bc (implies cxxstl) */

/* Basenames of the soft runtime TUs, auto-linked on demand. They live in the
 * runtime dir and are never hand-listed by the user. */
#define CVM_F64_RUNTIME_TU "cvm_float64_rt.c"
#define CVM_I64_RUNTIME_TU "cvm_int64_rt.c"
/* C++ ABI runtime TUs auto-linked when any input is C++. cvm_cxxrt: operator
 * new/delete, __cxa_*, the EH unwinder, RTTI. cvm_cxxstl: the out-of-line std
 * exception hierarchy (std::exception/logic_error/length_error/bad_alloc/...)
 * the STL headers reference. Both are .cpp, compiled against libc++. */
#define CVM_CXX_RUNTIME_TUS { "cvm_cxxrt.cpp", "cvm_cxxstl.cpp" }
#define CVM_CXX_RUNTIME_TU_COUNT 2

#define MAX_REGIONS         64
#define MAX_INCLUDE_DIRS    32
#define MAX_DEFINES         32
#define MAX_PASSTHRU        64   /* verbatim clang args: -isystem/-idirafter/-std */
#define INPUTS_INIT_CAP    256   /* initial capacity; grows dynamically (a whole-
                                  * engine build — e.g. UQM — is ~270 TUs) */

struct cli {
    const char **inputs;        /* .c and/or .bc, >=1 positional (grown) */
    int         input_count;
    int         input_cap;
    const char *output;         /* -o, required */
    const char *opt_level;      /* -O<n>, default "1" */
    const char *clang_path;     /* --clang= */
    const char *llvm_link_path; /* --llvm-link= */
    const char *opt_path;       /* --opt= */
    const char *translate_path; /* --translate= */
    const char *runtime_dir;    /* --runtime-dir= */
    const char *libcxx_dir;     /* --libcxx-dir=: explicit libc++ v1 header dir
                                 * (default: the toolchain clang's own, via
                                 * -stdlib=libc++ — always version-matched) */

    const char *heap_reserve;   /* --heap-reserve=, raw string */
    const char *stack_reserve;  /* --stack-reserve=, raw string */
    const char *rom;            /* --rom=, raw string (pass-through) */
    const char *meta;           /* --meta=, raw string (pass-through) */
    int         seal;           /* --seal: append an integrity seal */
    const char *regions[MAX_REGIONS];
    int         region_count;
    const char *include_dirs[MAX_INCLUDE_DIRS];
    int         include_count;
    const char *defines[MAX_DEFINES];   /* -D<macro>[=val], passed to clang */
    int         define_count;
    /* Verbatim clang args forwarded in order: -isystem D / -idirafter D / -std=.
     * -idirafter lets a build place the C library (picolibc/SDK) headers BELOW
     * libc++ so libc++'s own wrapper <math.h>/<cstring>/... win and #include_next
     * through to the C ones (required when compiling C++ with the STL). */
    const char *passthru[MAX_PASSTHRU];
    int         passthru_count;
    int         has_std;        /* user passed -std=: don't inject the default */

    int lto;        /* --lto: run opt default<O2> on the (linked) module */
    int keep_bc;
    int verbose;
};

static void usage(FILE *f) {
    fprintf(f,
        "Usage: cvm-cc <input.c|input.cpp|input.bc>... -o <output.bin> [opts]\n"
        "\n"
        "Driver around clang + llvm-link + cvm-translate. Each .c is compiled\n"
        "with clang --target=i386-elf -emit-llvm -O<level> to bitcode; .cpp/.cc/\n"
        ".cxx are compiled as C++ (-x c++) and the C++\n"
        "ABI runtime (cvm_cxxrt) is auto-linked; .bc inputs skip clang. Multiple\n"
        "inputs are llvm-link'd into one module (true multi-file linking —\n"
        "file-local statics don't collide), then translated. (C++ note: the\n"
        "translator runs global constructors before main; operator new/delete\n"
        "forward to the cart's malloc/free.)\n"
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
        "  --meta=FILE                append FILE as a host-only metadata blob\n"
        "  --seal                     append an integrity seal (magic + crc32)\n"
        "                             (default rw); repeatable up to %d\n"
        "\n"
        "Pass-through to clang:\n"
        "  -I <dir>                   extra include dir (repeatable)\n"
        "  -isystem <dir>             extra system include dir (repeatable)\n"
        "  -idirafter <dir>           include dir searched LAST (repeatable).\n"
        "                             Use for the C library (picolibc/SDK)\n"
        "                             headers when compiling C++ so libc++'s\n"
        "                             wrapper headers win and #include_next.\n"
        "  -D<macro>[=val]            predefine a macro (repeatable)\n"
        "  -std=<std>                 language standard (C++ defaults to c++20)\n"
        "  -O0|-O1|-O2|-O3|-Os        optimisation level (default -O1)\n"
        "\n"
        "C++ (.cpp/.cc/.cxx):\n"
        "  libc++ from the toolchain clang (-stdlib=libc++) by default; the\n"
        "  freestanding <__config_site>/<__external_threading> in the runtime\n"
        "  dir override it. --libcxx-dir=PATH pins an explicit v1 header tree.\n"
        "  --libcxx-dir=PATH          explicit libc++ v1 header dir (else the\n"
        "                             toolchain clang's own, version-matched)\n"
        "\n"
        "Link-time optimisation:\n"
        "  --lto                      run opt 'default<O2>' on the linked module\n"
        "                             before translating, enabling cross-file\n"
        "                             inlining. Vectorisation is forced off (the\n"
        "                             VM has no vector types). Off by default.\n"
        "\n"
        "Tool discovery overrides:\n"
        "  --clang=PATH               override clang binary\n"
        "  --llvm-link=PATH           override llvm-link binary\n"
        "  --opt=PATH                 override opt binary (for --lto)\n"
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

/* A C++ source input (compiled with clang -x c++).
 * .C is intentionally omitted — on case-insensitive filesystems it aliases the
 * C extension .c. */
static int is_cpp_src(const char *s) {
    return has_suffix(s, ".cpp") || has_suffix(s, ".cc") ||
           has_suffix(s, ".cxx");
}

/* The last path component of `s` (after the final '/' or '\'). */
static const char *basename_of(const char *s) {
    const char *b = s;
    for (const char *p = s; *p; ++p)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
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

/* Locate llvm-link. It ships with the same LLVM as clang, so by default we
 * resolve it from PATH ("llvm-link") just like clang ("clang") — keep the two
 * from the same toolchain or the bitcode versions won't match. --llvm-link=
 * overrides. Caller owns the returned string. */
static char *find_llvm_link(const struct cli *cli) {
    if (cli->llvm_link_path) return strdup(cli->llvm_link_path);
#if defined(_WIN32)
    return strdup("llvm-link.exe");
#else
    return strdup("llvm-link");
#endif
}

/* Locate opt (the LLVM optimiser, used for --lto). Same toolchain as clang;
 * default to PATH, --opt= overrides. Caller owns the returned string. */
static char *find_opt(const struct cli *cli) {
    if (cli->opt_path) return strdup(cli->opt_path);
#if defined(_WIN32)
    return strdup("opt.exe");
#else
    return strdup("opt");
#endif
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
        } else if (strncmp(a, "-D", 2) == 0 && a[2] != '\0') {
            /* -D<macro>[=val] (glued), forwarded verbatim to clang. Lets a build
             * select a libc feature (e.g. CVM_LIBC_ENABLE_F64 -> the f64 atof). */
            if (cli->define_count >= MAX_DEFINES) {
                fprintf(stderr, "cvm-cc: too many -D (max %d)\n", MAX_DEFINES);
                return 2;
            }
            cli->defines[cli->define_count++] = a;
        } else if (strcmp(a, "-isystem") == 0 || strcmp(a, "-idirafter") == 0) {
            /* Two-token clang include flags, forwarded verbatim (in order).
             * -idirafter is how a C++ build demotes the C library headers
             * below libc++ (see struct cli). */
            if (i + 1 >= argc) { usage(stderr); return 2; }
            if (cli->passthru_count + 2 > MAX_PASSTHRU) {
                fprintf(stderr, "cvm-cc: too many passthrough args (max %d)\n",
                        MAX_PASSTHRU);
                return 2;
            }
            cli->passthru[cli->passthru_count++] = a;
            cli->passthru[cli->passthru_count++] = argv[++i];
        } else if (strncmp(a, "-std=", 5) == 0) {
            if (cli->passthru_count + 1 > MAX_PASSTHRU) {
                fprintf(stderr, "cvm-cc: too many passthrough args (max %d)\n",
                        MAX_PASSTHRU);
                return 2;
            }
            cli->passthru[cli->passthru_count++] = a;
            cli->has_std = 1;
        } else if (strncmp(a, "--libcxx-dir=", 13) == 0) {
            cli->libcxx_dir = a + 13;
        } else if (strncmp(a, "-O", 2) == 0 && a[2] != '\0') {
            cli->opt_level = a + 2;
        } else if (strncmp(a, "--heap-reserve=", 15) == 0) {
            cli->heap_reserve = a;
        } else if (strncmp(a, "--stack-reserve=", 16) == 0) {
            cli->stack_reserve = a;
        } else if (strncmp(a, "--rom=", 6) == 0) {
            cli->rom = a;
        } else if (strncmp(a, "--meta=", 7) == 0) {
            cli->meta = a;
        } else if (strcmp(a, "--seal") == 0) {
            cli->seal = 1;
        } else if (strncmp(a, "--region=", 9) == 0) {
            if (cli->region_count >= MAX_REGIONS) {
                fprintf(stderr, "cvm-cc: too many --region (max %d)\n",
                        MAX_REGIONS);
                return 2;
            }
            cli->regions[cli->region_count++] = a;
        } else if (strncmp(a, "--clang=", 8) == 0) {
            cli->clang_path = a + 8;
        } else if (strncmp(a, "--llvm-link=", 12) == 0) {
            cli->llvm_link_path = a + 12;
        } else if (strncmp(a, "--opt=", 6) == 0) {
            cli->opt_path = a + 6;
        } else if (strncmp(a, "--translate=", 12) == 0) {
            cli->translate_path = a + 12;
        } else if (strncmp(a, "--runtime-dir=", 14) == 0) {
            cli->runtime_dir = a + 14;
        } else if (strcmp(a, "--lto") == 0) {
            cli->lto = 1;
        } else if (strcmp(a, "--keep-bc") == 0) {
            cli->keep_bc = 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            cli->verbose = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "cvm-cc: unknown option '%s'\n", a);
            usage(stderr);
            return 2;
        } else {
            if (cli->input_count >= cli->input_cap) {
                int newcap = cli->input_cap ? cli->input_cap * 2 : INPUTS_INIT_CAP;
                const char **grown = (const char **)realloc(
                    (void *)cli->inputs, (size_t)newcap * sizeof *grown);
                if (!grown) { perror("realloc"); return 1; }
                cli->inputs = grown;
                cli->input_cap = newcap;
            }
            cli->inputs[cli->input_count++] = a;
        }
    }
    if (cli->input_count == 0) { fprintf(stderr, "cvm-cc: missing input file\n"); usage(stderr); return 2; }
    if (!cli->output) { fprintf(stderr, "cvm-cc: missing -o <output>\n"); usage(stderr); return 2; }
    return 0;
}

/* Compile one .c to bitcode at `bc_out` via clang. Returns the child exit
 * status (0 on success). The clang flags match the test fixtures: i386-elf
 * freestanding (no hosted libc, and clang emits the i32-length mem intrinsics
 * the VM expects, not the i64 ones it picks in hosted mode) + -emit-llvm. */
static int compile_c_to_bc(const struct cli *cli, const char *clang,
                           const char *input, const char *bc_out) {
    char optflag[8];
    snprintf(optflag, sizeof optflag, "-O%s", cli->opt_level);
    char rtinc[1024];
    snprintf(rtinc, sizeof rtinc, "-I%s", cli->runtime_dir);
    char libcxxinc[1024];

    char *cargv[32 + 2 * MAX_INCLUDE_DIRS + MAX_DEFINES + MAX_PASSTHRU];
    int   n = 0;
    cargv[n++] = (char *)clang;
    cargv[n++] = (char *)"--target=i386-elf";
    cargv[n++] = (char *)"-ffreestanding";
    /* C++ inputs: force the C++ frontend. Exceptions AND RTTI are both
     * supported now — cvm_cxxrt provides the C++ exception runtime (the
     * setjmp/longjmp unwinder + __cxa_*) and __dynamic_cast + the type_info abi
     * vtables; the translator lowers invoke/landingpad/resume and serialises
     * the type_info objects + per-landingpad catch descriptors. typeid is
     * untested. Global ctors + operator new/delete + __cxa_* are all handled. */
    if (is_cpp_src(input)) {
        cargv[n++] = (char *)"-x";
        cargv[n++] = (char *)"c++";
        /* libc++ headers. By default use the toolchain clang's own libc++
         * (-stdlib=libc++ self-locates them — always version-matched, the most
         * portable choice since libc++ is co-versioned with clang). The
         * runtime dir (added via rtinc as -I, highest priority) supplies our
         * freestanding <__config_site> + <__external_threading> overrides, so
         * they win over the toolchain's. --libcxx-dir pins an explicit v1 tree
         * instead (for a vendored/hermetic build): point clang at it with
         * -nostdinc++ -isystem and drop -stdlib. The C library headers
         * (picolibc/SDK) must be passed by the caller via -idirafter so
         * libc++'s wrapper headers shadow them and #include_next through. */
        if (cli->libcxx_dir) {
            cargv[n++] = (char *)"-nostdinc++";
            cargv[n++] = (char *)"-isystem";
            snprintf(libcxxinc, sizeof libcxxinc, "%s", cli->libcxx_dir);
            cargv[n++] = libcxxinc;
        } else {
            cargv[n++] = (char *)"-stdlib=libc++";
        }
        /* Default to C++20 (modern STL; <compare> etc.) unless the user set
         * -std explicitly (forwarded via passthru). */
        if (!cli->has_std)
            cargv[n++] = (char *)"-std=c++20";
        /* libc++ with localization (for <iostream>/<locale>) dispatches its
         * locale facets to picolibc's POSIX xlocale `*_l` functions (newlocale,
         * strtod_l, toupper_l, …). picolibc gates those behind __GNU_VISIBLE,
         * which -std=c++NN turns OFF (it sets __STRICT_ANSI__); _GNU_SOURCE
         * re-exposes them. Define it for ALL C++ (harmless when iostream is
         * unused — it only widens the visible C surface). NOT defined for C
         * carts, which opt in deliberately. -mlong-double-64 makes `long double`
         * == double so libc++/picolibc never form an x86_fp80 the VM rejects
         * (and strtold_l degenerates to strtod_l). */
        cargv[n++] = (char *)"-D_GNU_SOURCE";
        cargv[n++] = (char *)"-mlong-double-64";
    }
    cargv[n++] = (char *)"-emit-llvm";
    /* Line tables only: enough for the translator to report file:line on
     * rejected constructs; no .bin impact (debug metadata is dropped when the
     * translator emits VM bytecode), minimal bitcode/compile overhead. */
    cargv[n++] = (char *)"-gline-tables-only";
    cargv[n++] = optflag;
    cargv[n++] = rtinc;
    for (int k = 0; k < cli->include_count; ++k) {
        cargv[n++] = (char *)"-I";
        cargv[n++] = (char *)cli->include_dirs[k];
    }
    for (int k = 0; k < cli->define_count; ++k)
        cargv[n++] = (char *)cli->defines[k];
    /* Verbatim passthrough (-isystem / -idirafter / -std=), in the order given.
     * After rtinc (-I, highest) and -D, but the -idirafter entries still land
     * at the very end of clang's search chain by definition — that's the point
     * (C library below libc++). */
    for (int k = 0; k < cli->passthru_count; ++k)
        cargv[n++] = (char *)cli->passthru[k];
    cargv[n++] = (char *)"-c";
    cargv[n++] = (char *)input;
    cargv[n++] = (char *)"-o";
    cargv[n++] = (char *)bc_out;
    cargv[n] = NULL;

    int crc = run_cmd(cli, cargv);
    if (crc != 0)
        fprintf(stderr, "cvm-cc: clang failed on %s (exit %d)\n", input, crc);
    return crc;
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

    int any_cpp = 0;
    for (int i = 0; i < cli.input_count; ++i) {
        if (!has_suffix(cli.inputs[i], ".c") && !has_suffix(cli.inputs[i], ".bc") &&
            !is_cpp_src(cli.inputs[i])) {
            fprintf(stderr, "cvm-cc: input must end in .c, .cpp/.cc/.cxx or .bc "
                    "(got '%s')\n", cli.inputs[i]);
            return 2;
        }
        if (is_cpp_src(cli.inputs[i])) any_cpp = 1;
    }

    /* Per-input bitcode. A .c is compiled to an owned temp .bc next to the
     * output (predictable name keeps temp-dir logic out of this tool); a .bc
     * input is used in place (not owned). --keep-bc surfaces the temps. */
    /* One .bc per input + up to 4 auto-linked runtime TUs (f64/i64 + 2 C++). */
    int    bc_cap = cli.input_count + 8;
    char **bc_paths = (char **)malloc((size_t)bc_cap * sizeof *bc_paths);
    int   *bc_owned = (int  *)malloc((size_t)bc_cap * sizeof *bc_owned);
    int    bc_count = 0;
    int    fail = (!bc_paths || !bc_owned);
    if (fail) perror("malloc");
    const char *clang = cli.clang_path ? cli.clang_path : "clang";
    size_t outlen = strlen(cli.output);

    for (int i = 0; i < cli.input_count && !fail; ++i) {
        const char *in = cli.inputs[i];
        if (has_suffix(in, ".bc")) {
            bc_paths[bc_count] = (char *)in;
            bc_owned[bc_count] = 0;
            ++bc_count;
            continue;
        }
        char *bc = (char *)malloc(outlen + 24);
        if (!bc) { perror("malloc"); fail = 1; break; }
        snprintf(bc, outlen + 24, "%s.%d.tmp.bc", cli.output, i);
        if (compile_c_to_bc(&cli, clang, in, bc) != 0) { free(bc); fail = 1; break; }
        bc_paths[bc_count] = bc;
        bc_owned[bc_count] = 1;
        ++bc_count;
    }

    /* Locate cvm-translate once: needed both for the soft-float probe below
     * and the final translate. */
    char *translator = NULL;
    if (!fail) {
        translator = find_translator(&cli, argv[0]);
        if (!translator) fail = 1;
    }

    /* Auto-link the soft runtimes a program needs. Probe each compiled module
     * (cvm-translate --probe-runtime returns a CVM_PROBE_* bitmask: f64 for
     * `double`, i64 for i64 div/rem, cxxstl for the std exception ABI); then
     * compile and add each needed runtime TU so the translator's legaliser finds
     * the helpers. Integer-only programs link nothing extra. A runtime the user
     * already listed is left alone (no duplicate-symbol llvm-link error). */
    int need_cxxstl = 0;   /* module references the std exception ABI */
    int need_iostream = 0; /* module references the libc++ iostream/locale ABI */
    if (!fail) {
        struct { int bit; const char *tu; int listed; } rts[2] = {
            { CVM_PROBE_F64, CVM_F64_RUNTIME_TU, 0 },
            { CVM_PROBE_I64, CVM_I64_RUNTIME_TU, 0 },
        };
        for (int i = 0; i < cli.input_count; ++i) {
            const char *b = basename_of(cli.inputs[i]);
            for (int j = 0; j < 2; ++j)
                if (strcmp(b, rts[j].tu) == 0) rts[j].listed = 1;
        }

        int need = 0;   /* accumulated CVM_PROBE_* bitmask over all modules */
        for (int i = 0; i < bc_count && !fail; ++i) {
            char *pargv[4];
            int   pn = 0;
            pargv[pn++] = translator;
            pargv[pn++] = (char *)"--probe-runtime";
            pargv[pn++] = bc_paths[i];
            pargv[pn]   = NULL;
            int prc = run_cmd(&cli, pargv);
            if (prc & ~(CVM_PROBE_F64 | CVM_PROBE_I64 | CVM_PROBE_CXXSTL
                        | CVM_PROBE_IOSTREAM)) {
                fprintf(stderr, "cvm-cc: runtime probe failed on %s "
                        "(exit %d)\n", bc_paths[i], prc);
                fail = 1;
                break;
            }
            need |= prc;
            /* No early break: must probe every module to catch a late cxxstl
             * reference (the f64/i64 scan is cheap). */
        }
        need_cxxstl = (need & CVM_PROBE_CXXSTL) != 0;
        need_iostream = (need & CVM_PROBE_IOSTREAM) != 0;

        for (int j = 0; j < 2 && !fail; ++j) {
            if (!(need & rts[j].bit) || rts[j].listed) continue;
            if (bc_count >= bc_cap) {
                fprintf(stderr, "cvm-cc: too many inputs to auto-link %s "
                        "(max %d)\n", rts[j].tu, bc_cap);
                fail = 1;
                break;
            }
            char rt_src[1100];
            snprintf(rt_src, sizeof rt_src, "%s/%s",
                     cli.runtime_dir, rts[j].tu);
            char *bc = (char *)malloc(outlen + 24);
            if (!bc) { perror("malloc"); fail = 1; break; }
            snprintf(bc, outlen + 24, "%s.rt%d.tmp.bc", cli.output, j);
            if (compile_c_to_bc(&cli, clang, rt_src, bc) != 0) {
                free(bc);
                fail = 1;
                break;
            }
            bc_paths[bc_count] = bc;
            bc_owned[bc_count] = 1;
            ++bc_count;
        }
    }

    /* Auto-link the C++ ABI runtime TUs when any input is C++:
     *  - cvm_cxxrt.cpp: operator new/delete (referenced by every class with a
     *    virtual destructor via the deleting-dtor vtable slot), __cxa_*, the
     *    local-static guards, the EH unwinder, RTTI.
     *  - cvm_cxxstl.cpp: the out-of-line std exception hierarchy
     *    (std::exception/logic_error/length_error/bad_alloc/...) the STL headers
     *    reference. Harmless for non-STL C++ (its defs go unused).
     * operator new/delete + malloc/free come from the cart's C library. A TU the
     * user already listed is skipped (no duplicate-symbol llvm-link error). */
    if (!fail && any_cpp) {
        const char *cxx_tus[] = CVM_CXX_RUNTIME_TUS;
        for (int j = 0; j < CVM_CXX_RUNTIME_TU_COUNT && !fail; ++j) {
            /* cvm_cxxrt (j==0) is freestanding and always linked. cvm_cxxstl
             * (j>=1) pulls libc++'s <stdexcept>/<new> (which need the cart's C
             * headers) so it is linked ONLY when the module actually references
             * the std exception ABI — otherwise it can't even compile and a
             * non-STL C++ cart shouldn't pay for it. */
            if (j >= 1 && !need_cxxstl) continue;
            int listed = 0;
            for (int i = 0; i < cli.input_count; ++i)
                if (strcmp(basename_of(cli.inputs[i]), cxx_tus[j]) == 0)
                    listed = 1;
            if (listed) continue;
            if (bc_count >= bc_cap) {
                fprintf(stderr, "cvm-cc: too many inputs to auto-link %s "
                        "(max %d)\n", cxx_tus[j], bc_cap);
                fail = 1;
                break;
            }
            char rt_src[1100];
            snprintf(rt_src, sizeof rt_src, "%s/%s", cli.runtime_dir, cxx_tus[j]);
            char *bc = (char *)malloc(outlen + 24);
            if (!bc) { perror("malloc"); fail = 1; break; }
            snprintf(bc, outlen + 24, "%s.rtcxx%d.tmp.bc", cli.output, j);
            if (compile_c_to_bc(&cli, clang, rt_src, bc) != 0) {
                free(bc); fail = 1; break;
            }
            bc_paths[bc_count] = bc;
            bc_owned[bc_count] = 1;
            ++bc_count;
        }
    }

    /* Auto-link the PREBUILT iostream/locale library cxxio.bc when the program
     * references the libc++ stream/locale ABI (CVM_PROBE_IOSTREAM). Unlike the
     * other runtimes, cxxio is NOT compiled on the fly — it is the vendored
     * libc++ src subset, built once into runtime_dir/cxxio.bc by build_cxxio.sh
     * (like picolibc.bc). It depends on cvm_cxxstl (basic_string + std
     * exceptions, linked above since the probe implies cxxstl) and on the cart's
     * picolibc.bc being built with the locale `*_l` functions (--with-locale). */
    if (!fail && need_iostream) {
        int listed = 0;
        for (int i = 0; i < cli.input_count; ++i)
            if (strcmp(basename_of(cli.inputs[i]), "cxxio.bc") == 0) listed = 1;
        if (!listed) {
            static char cxxio[1100];
            snprintf(cxxio, sizeof cxxio, "%s/cxxio.bc", cli.runtime_dir);
            if (!file_exists(cxxio)) {
                fprintf(stderr, "cvm-cc: the program uses <iostream>/<locale> but "
                        "%s is missing — build it with runtime/lib/build_cxxio.sh\n",
                        cxxio);
                fail = 1;
            } else if (bc_count >= bc_cap) {
                fprintf(stderr, "cvm-cc: too many inputs to auto-link cxxio.bc "
                        "(max %d)\n", bc_cap);
                fail = 1;
            } else {
                bc_paths[bc_count] = cxxio;   /* prebuilt; not owned */
                bc_owned[bc_count] = 0;
                ++bc_count;
            }
        }
    }

    /* The module to translate: a single input is used as-is; multiple inputs
     * are llvm-link'd into one module first. llvm-link gives real multi-file
     * linkage — file-local statics are uniqued, only true extern duplicates
     * (ODR violations) error, exactly like a C linker. */
    char       *linked = NULL;   /* owned llvm-link output, when we link */
    const char *module = NULL;
    if (!fail) {
        if (bc_count == 1) {
            module = bc_paths[0];
        } else {
            linked = (char *)malloc(outlen + 16);
            if (!linked) { perror("malloc"); fail = 1; }
            else {
                snprintf(linked, outlen + 16, "%s.linked.bc", cli.output);
                char *ll = find_llvm_link(&cli);
                char **largv = (char **)malloc((size_t)(bc_count + 8) * sizeof *largv);
                if (!largv) { perror("malloc"); free(ll); fail = 1; }
                else {
                    int   n = 0;
                    largv[n++] = ll;
                    for (int i = 0; i < bc_count; ++i) largv[n++] = bc_paths[i];
                    largv[n++] = (char *)"-o";
                    largv[n++] = linked;
                    largv[n] = NULL;
                    int lrc = run_cmd(&cli, largv);
                    free(ll);
                    free(largv);
                    if (lrc != 0) {
                        fprintf(stderr, "cvm-cc: llvm-link failed (exit %d)\n", lrc);
                        fail = 1;
                    } else {
                        module = linked;
                    }
                }
            }
        }
    }

    /* Optional cross-file optimisation: run opt 'default<O2>' on the module.
     * After llvm-link the inliner sees every TU, so small cross-file helpers
     * (e.g. fixed-point and renderer accessors) inline at their call sites.
     * Vectorisation is forced off because the translator/VM has no vector
     * types (an O2 pipeline would otherwise emit <N x iM> and be rejected). */
    char *opt_out = NULL;
    if (!fail && cli.lto) {
        opt_out = (char *)malloc(outlen + 16);
        if (!opt_out) { perror("malloc"); fail = 1; }
        else {
            snprintf(opt_out, outlen + 16, "%s.opt.bc", cli.output);
            char *opt = find_opt(&cli);
            char *oargv[10];
            int   n = 0;
            oargv[n++] = opt;
            oargv[n++] = (char *)"--passes=default<O2>";
            oargv[n++] = (char *)"-vectorize-loops=false";
            oargv[n++] = (char *)"-vectorize-slp=false";
            oargv[n++] = (char *)module;
            oargv[n++] = (char *)"-o";
            oargv[n++] = opt_out;
            oargv[n] = NULL;
            int orc = run_cmd(&cli, oargv);
            free(opt);
            if (orc != 0) {
                fprintf(stderr, "cvm-cc: opt failed (exit %d)\n", orc);
                fail = 1;
            } else {
                module = opt_out;
            }
        }
    }

    /* cvm-translate the (possibly linked, possibly optimised) module. The
     * translator was located earlier (for the soft-float probe). */
    int trc = 0;
    if (!fail) {
        char *targv[16 + MAX_REGIONS];
        int   n = 0;
        targv[n++] = translator;
        targv[n++] = (char *)module;
        targv[n++] = (char *)"-o";
        targv[n++] = (char *)cli.output;
        if (cli.heap_reserve)  targv[n++] = (char *)cli.heap_reserve;
        if (cli.stack_reserve) targv[n++] = (char *)cli.stack_reserve;
        if (cli.rom)           targv[n++] = (char *)cli.rom;
        if (cli.meta)          targv[n++] = (char *)cli.meta;
        if (cli.seal)          targv[n++] = (char *)"--seal";
        for (int k = 0; k < cli.region_count; ++k)
            targv[n++] = (char *)cli.regions[k];
        targv[n] = NULL;

        trc = run_cmd(&cli, targv);
        if (trc != 0)
            fprintf(stderr, "cvm-cc: cvm-translate failed (exit %d)\n", trc);
    }
    free(translator);

    /* Clean up intermediates unless the user asked to keep them. */
    if (!cli.keep_bc) {
        for (int i = 0; i < bc_count; ++i)
            if (bc_owned[i]) remove(bc_paths[i]);
        if (linked)  remove(linked);
        if (opt_out) remove(opt_out);
    }
    for (int i = 0; i < bc_count; ++i)
        if (bc_owned[i]) free(bc_paths[i]);
    free(bc_paths);
    free(bc_owned);
    free((void *)cli.inputs);
    free(linked);
    free(opt_out);

    if (fail) return 1;
    return trc < 0 ? 1 : trc;
}
