/* cvm-fuzz — a seed-reproducible, UB-free differential test generator for the
 * CronoVM translator/VM.
 *
 * WHY: most CronoVM translator/VM bugs (spill / alloca-spill / invoke-spill /
 * narrow-int / global-align / zext-unsigned / sitofp-narrow / dup-operand-regfree
 * / phi-parallel-copy, ...) were found LATE, via a game OOB hours into a port.
 * The hand-written conformance corpus (tests/conformance/conf_*.c) is too narrow
 * to flush them out proactively. This tool emits random C translation units that
 * plug straight into the EXISTING differential harness (driver.c / vm_entry.c /
 * test_e2e): each emitted program exports `int conf_main(void)` returning a single
 * int32 FNV-1a checksum. The runner compiles it both natively (the oracle) and on
 * the VM (cvm-cc) and compares the checksums — a mismatch is a translator bug.
 *
 * HARD INVARIANT — the generated program MUST be free of undefined behaviour, or
 * native and VM could legitimately diverge (a false positive). We guarantee that
 * by construction:
 *   - ALL arithmetic is done on UNSIGNED fixed-width types (wraparound is defined).
 *   - shift counts are constants in [0, width-1].
 *   - every divisor/modulus is forced nonzero with `| 1`.
 *   - every array index is masked into range with a power-of-two length.
 *   - every variable is initialised at its declaration; no uninitialised reads.
 *   - NO `long`/`size_t`/pointers-to-heap in any value folded into the checksum,
 *     so the int32 result is identical on ILP32 (VM) and LP64 (host) — no -m32.
 *   - NO floating point (host hw vs VM soft-float rounding could diverge); integer
 *     codegen is where our bugs live. (float is a deliberate future extension.)
 * Signed values appear ONLY as reinterpretations for signed comparisons; the
 * unsigned->signed conversion is implementation-defined (not UB) and BOTH sides
 * are the same clang front-end, so it is consistent — and it is exactly what the
 * narrow-icmp / sign-extend lowering must preserve.
 *
 * The bug classes above are targeted on purpose: high simultaneous live-variable
 * pressure (forces spills), i8/i16 narrow intermediates and narrow signed compares,
 * i64 arithmetic (soft runtime), branch/loop/switch merges (phi + parallel copies),
 * noinline calls (caller-save spills across calls), and by-value struct copies +
 * local-array (alloca) indexing.
 *
 * Usage:
 *   cvm-fuzz --seed N [-o FILE]      emit one program for seed N (default stdout)
 *   cvm-fuzz --seed N --stats        also print a trailing comment of the knobs used
 *
 * Pure C, no dependencies. Determinism: same seed -> byte-identical program.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------- RNG ---- */
/* SplitMix64 — tiny, deterministic, good enough for structure selection. */
static uint64_t g_rng;
static uint64_t rng_next(void) {
    uint64_t z = (g_rng += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
/* uniform in [0, n)  (n must be > 0) */
static uint32_t rnd(uint32_t n) { return (uint32_t)(rng_next() % n); }
static int chance(uint32_t pct) { return rnd(100) < pct; }

/* --------------------------------------------------------------- types ---- */
enum { T_U8, T_U16, T_U32, T_U64, NTYPES };
static const char *TYNAME[NTYPES] = { "uint8_t", "uint16_t", "uint32_t", "uint64_t" };
static const char *TYPFX[NTYPES]  = { "b", "s", "a", "w" };  /* var-name prefix */
static int TYBITS[NTYPES]         = { 8, 16, 32, 64 };

/* variable pool: counts per type, names are <prefix><index> */
static int g_count[NTYPES];

/* When 0, suppress 64-bit rotates: clang canonicalises `(a<<r)|(a>>(64-r))` to
 * llvm.fshl.i64, which the translator does not yet lower (a known gap, tied to
 * the i64 legalizer work). `--no-rot64` lets the runner produce a GAP-free
 * baseline; rot64 is ON by default so the corpus keeps surfacing that real gap. */
static int g_allow_rot64 = 1;

static FILE *out;

/* print a random literal of the given type (well within range) */
static void emit_lit(int t) {
    switch (t) {
        case T_U8:  fprintf(out, "UINT8_C(%u)",  (unsigned)(rnd(256))); break;
        case T_U16: fprintf(out, "UINT16_C(%u)", (unsigned)(rnd(65536))); break;
        case T_U32: fprintf(out, "UINT32_C(%u)", (unsigned)rng_next()); break;
        default: {
            uint64_t v = rng_next();
            fprintf(out, "UINT64_C(%llu)", (unsigned long long)v);
            break;
        }
    }
}

/* name of a random existing variable of type t (caller guarantees count>0) */
static void emit_var(int t) { fprintf(out, "%s%u", TYPFX[t], rnd((uint32_t)g_count[t])); }

/* a leaf of type t: a variable (if any) or a literal */
static void emit_leaf(int t) {
    if (g_count[t] > 0 && chance(70)) emit_var(t);
    else                              emit_lit(t);
}

/* The unsigned "compute type" used to evaluate an op for result-type t, chosen
 * wide enough that the operation can NEVER trigger signed-integer-promotion UB:
 * narrow types (u8/u16) would otherwise promote to signed `int` and a product or
 * a shifted-then-added value could overflow it. We compute in uint32_t (or
 * uint64_t for T_U64), which has well-defined wraparound, then truncate back to
 * t on store. This still exercises the narrow LOAD (zext/sext of the operand
 * vars), the narrow STORE (the `(t)` truncation), and the narrow SIGNED COMPARE
 * (the select form) — i.e. the real narrow-int bug classes. */
static const char *ctype(int t) { return t == T_U64 ? "uint64_t" : "uint32_t"; }

/* Emit an rvalue expression of static type t, recursion-depth limited. UB-free by
 * construction: every arithmetic/shift/rotate/div is evaluated in `ctype(t)` (so
 * no signed-int promotion can overflow), shift counts are constants < width, and
 * every divisor is forced nonzero with `| 1`. */
static void emit_expr(int t, int depth) {
    if (depth <= 0 || chance(35)) { emit_leaf(t); return; }
    const char *ct = ctype(t);
    int form = rnd(7);
    if (form == 3 && t == T_U64 && !g_allow_rot64) form = 0;  /* avoid fshl.i64 gap */
    switch (form) {
        case 0: case 1: { /* binary arithmetic / bitwise (in the wide compute type) */
            static const char *OPS[] = { "+", "-", "*", "&", "|", "^" };
            const char *op = OPS[rnd(6)];
            fprintf(out, "(%s)((%s)(", TYNAME[t], ct); emit_expr(t, depth - 1);
            fprintf(out, ") %s (%s)(", op, ct);
            emit_expr(t, depth - 1); fprintf(out, "))");
            break;
        }
        case 2: { /* shift by a constant < width, evaluated in the wide type */
            int sh = (int)rnd((uint32_t)TYBITS[t]);
            const char *dir = chance(50) ? "<<" : ">>";
            fprintf(out, "(%s)((%s)(", TYNAME[t], ct);
            emit_expr(t, depth - 1);
            fprintf(out, ") %s %d)", dir, sh);
            break;
        }
        case 3: { /* rotate within the type's width (masked, well-defined) */
            int w = TYBITS[t];
            int r = 1 + (int)rnd((uint32_t)(w - 1));
            fprintf(out, "(%s)(((%s)(", TYNAME[t], ct);
            emit_expr(t, depth - 1);
            fprintf(out, ") << %d) | ((%s)(", r, ct);
            emit_expr(t, depth - 1);
            fprintf(out, ") >> %d))", w - r);
            break;
        }
        case 4: { /* unsigned division / modulus by a guaranteed-nonzero divisor */
            const char *op = chance(50) ? "/" : "%";
            fprintf(out, "(%s)((%s)(", TYNAME[t], ct); emit_expr(t, depth - 1);
            fprintf(out, ") %s ((%s)(", op, ct);
            emit_expr(t, depth - 1);
            fprintf(out, ") | 1u))");
            break;
        }
        case 5: { /* cast in from another type (narrow/widen conversions) */
            int s = rnd(NTYPES);
            fprintf(out, "(%s)(", TYNAME[t]);
            emit_expr(s, depth - 1);
            fputc(')', out);
            break;
        }
        default: { /* select via a narrow SIGNED comparison (narrow-icmp path) */
            int cmpt = chance(60) ? (chance(50) ? T_U8 : T_U16) : T_U32;
            const char *sgn = cmpt == T_U8 ? "int8_t" : cmpt == T_U16 ? "int16_t" : "int32_t";
            static const char *CMP[] = { "<", ">", "<=", ">=", "==", "!=" };
            fprintf(out, "(%s)((%s)(", TYNAME[t], sgn); emit_expr(cmpt, depth - 1);
            fprintf(out, ") %s (%s)(", CMP[rnd(6)], sgn); emit_expr(cmpt, depth - 1);
            fprintf(out, ") ? ("); emit_expr(t, depth - 1);
            fprintf(out, ") : ("); emit_expr(t, depth - 1);
            fprintf(out, "))");
            break;
        }
    }
}

/* assign a fresh expression into a random variable of a random non-empty type */
static void emit_assign(int indent) {
    int t, tries = 0;
    do { t = rnd(NTYPES); } while (g_count[t] == 0 && ++tries < 8);
    if (g_count[t] == 0) return;
    for (int i = 0; i < indent; i++) fputc(' ', out);
    emit_var(t); fprintf(out, " = "); emit_expr(t, 2 + (int)rnd(2)); fprintf(out, ";\n");
}

/* a boolean condition (well-defined): a narrow signed or plain unsigned compare */
static void emit_cond(void) {
    int t = rnd(NTYPES);
    if (chance(50)) { /* narrow signed */
        const char *sgn = t == T_U8 ? "int8_t" : t == T_U16 ? "int16_t"
                        : t == T_U32 ? "int32_t" : "int64_t";
        static const char *CMP[] = { "<", ">", "<=", ">=" };
        fprintf(out, "(%s)(", sgn); emit_expr(t, 2);
        fprintf(out, ") %s (%s)(", CMP[rnd(4)], sgn); emit_expr(t, 2); fputc(')', out);
    } else {            /* unsigned */
        static const char *CMP[] = { "<", ">", "<=", ">=", "==", "!=" };
        fputc('(', out); emit_expr(t, 2);
        fprintf(out, ") %s (", CMP[rnd(6)]); emit_expr(t, 2); fputc(')', out);
    }
}

/* a structured statement: if/else, bounded loop, switch, a call, or array I/O */
static void emit_stmt(int indent) {
    int kind = rnd(100);
    char pad[64]; int n = indent < 60 ? indent : 60;
    memset(pad, ' ', (size_t)n); pad[n] = 0;

    if (kind < 45) {                 /* plain assignment(s) */
        int k = 1 + (int)rnd(3);
        while (k--) emit_assign(indent);
    } else if (kind < 60) {          /* if / else — merge => phi nodes */
        fprintf(out, "%sif (", pad); emit_cond(); fprintf(out, ") {\n");
        int k = 1 + (int)rnd(3); while (k--) emit_assign(indent + 4);
        fprintf(out, "%s} else {\n", pad);
        k = 1 + (int)rnd(3); while (k--) emit_assign(indent + 4);
        fprintf(out, "%s}\n", pad);
    } else if (kind < 72) {          /* bounded for-loop */
        int trips = 1 + (int)rnd(8);
        fprintf(out, "%sfor (uint32_t i = 0; i < %u; i++) {\n", pad, (unsigned)trips);
        if (g_count[T_U32] > 0) {    /* feed the IV in so the loop isn't dead */
            fprintf(out, "%s    a%u = a%u + i;\n", pad,
                    rnd((uint32_t)g_count[T_U32]), rnd((uint32_t)g_count[T_U32]));
        }
        int k = 1 + (int)rnd(2); while (k--) emit_assign(indent + 4);
        fprintf(out, "%s}\n", pad);
    } else if (kind < 84) {          /* switch — many edges into one merge */
        if (g_count[T_U32] == 0) { emit_assign(indent); return; }
        int m = 2 + (int)rnd(6);
        fprintf(out, "%sswitch (a%u & %u) {\n", pad, rnd((uint32_t)g_count[T_U32]),
                (unsigned)(m - 1));
        for (int c = 0; c < m; c++) {
            fprintf(out, "%s    case %d: ", pad, c);
            emit_var(T_U32); fprintf(out, " = "); emit_expr(T_U32, 2);
            fprintf(out, "; break;\n");
        }
        fprintf(out, "%s    default: break;\n%s}\n", pad, pad);
    } else if (kind < 94) {          /* call a noinline helper (caller-save spill) */
        if (g_count[T_U32] >= 1) {
            if (chance(50)) {
                fprintf(out, "%s", pad); emit_var(T_U32);
                fprintf(out, " = mix32("); emit_leaf(T_U32); fprintf(out, ", ");
                emit_leaf(T_U32); fprintf(out, ", "); emit_leaf(T_U32);
                fprintf(out, ", "); emit_leaf(T_U32); fprintf(out, ");\n");
            } else if (g_count[T_U64] >= 1) {
                fprintf(out, "%s", pad); emit_var(T_U64);
                fprintf(out, " = mix64("); emit_leaf(T_U64); fprintf(out, ", ");
                emit_leaf(T_U64); fprintf(out, ", "); emit_leaf(T_U64);
                fprintf(out, ");\n");
            } else {
                emit_assign(indent);
            }
        } else emit_assign(indent);
    } else {                          /* by-value struct copy through a helper */
        if (g_count[T_U32] >= 1 && g_count[T_U64] >= 1 &&
            g_count[T_U16] >= 1 && g_count[T_U8] >= 1) {
            /* own brace-scope so repeated struct snippets don't redefine _s/_s2 */
            fprintf(out, "%s{ struct S _s = { ", pad);
            emit_leaf(T_U32); fprintf(out, ", "); emit_leaf(T_U16); fprintf(out, ", ");
            emit_leaf(T_U8); fprintf(out, ", "); emit_leaf(T_U64); fprintf(out, " };\n");
            fprintf(out, "%s  struct S _s2 = _s;\n", pad);
            fprintf(out, "%s  ", pad); emit_var(T_U32);
            fprintf(out, " = use_s(_s2); }\n");
        } else emit_assign(indent);
    }
}

/* ------------------------------------------------------ emit a program ---- */
static void emit_helpers(void) {
    fprintf(out,
        "/* fixed noinline helpers: call boundaries (caller-save spills) + i64 + struct copy */\n"
        "struct S { uint32_t x; uint16_t y; uint8_t z; uint64_t w; };\n"
        "__attribute__((noinline)) static uint32_t mix32(uint32_t a, uint32_t b,\n"
        "                                                 uint32_t c, uint32_t d) {\n"
        "    a += b; a ^= a >> 13; a *= UINT32_C(0x9e3779b1); a -= c;\n"
        "    a ^= a << 7; a += d; a ^= a >> 17; return a;\n"
        "}\n"
        "__attribute__((noinline)) static uint64_t mix64(uint64_t a, uint64_t b,\n"
        "                                                 uint64_t c) {\n"
        "    a += b; a ^= a >> 29; a *= UINT64_C(0xff51afd7ed558ccd); a -= c;\n"
        "    a ^= a << 17; return a;\n"
        "}\n"
        "__attribute__((noinline)) static uint32_t use_s(struct S s) {\n"
        "    return s.x ^ (uint32_t)s.y ^ (uint32_t)s.z\n"
        "         ^ (uint32_t)s.w ^ (uint32_t)(s.w >> 32);\n"
        "}\n\n");
}

/* emit a generated noinline helper of moderate internal pressure that conf_main
 * calls — mirrors real code (a big function calling smaller hot ones). */
static void emit_gen_helper(int id) {
    int saved[NTYPES]; memcpy(saved, g_count, sizeof saved);
    /* the helper has its OWN small variable pool */
    g_count[T_U8] = 2 + (int)rnd(3);
    g_count[T_U16] = 2 + (int)rnd(3);
    g_count[T_U32] = 3 + (int)rnd(5);
    g_count[T_U64] = 1 + (int)rnd(3);
    fprintf(out, "__attribute__((noinline)) static uint32_t gen_h%d(uint32_t p0, uint32_t p1) {\n", id);
    for (int t = 0; t < NTYPES; t++)
        for (int i = 0; i < g_count[t]; i++) {
            fprintf(out, "    %s %s%d = ", TYNAME[t], TYPFX[t], i);
            emit_lit(t); fprintf(out, ";\n");
        }
    if (g_count[T_U32] >= 2) fprintf(out, "    a0 = p0; a1 = p1;\n");
    int stmts = 8 + (int)rnd(12);
    while (stmts--) emit_stmt(4);
    /* fold the helper pool into one u32 */
    fprintf(out, "    uint32_t _r = UINT32_C(2166136261);\n");
    for (int t = 0; t < NTYPES; t++)
        for (int i = 0; i < g_count[t]; i++) {
            if (t == T_U64) {
                fprintf(out, "    _r = (_r ^ (uint32_t)%s%d) * UINT32_C(16777619);\n", TYPFX[t], i);
                fprintf(out, "    _r = (_r ^ (uint32_t)(%s%d >> 32)) * UINT32_C(16777619);\n", TYPFX[t], i);
            } else {
                fprintf(out, "    _r = (_r ^ (uint32_t)%s%d) * UINT32_C(16777619);\n", TYPFX[t], i);
            }
        }
    fprintf(out, "    return _r;\n}\n\n");
    memcpy(g_count, saved, sizeof saved);
}

static void emit_program(uint64_t seed, int with_stats) {
    g_rng = seed ^ 0xD1B54A32D192ED03ULL;

    /* per-program knobs (vary register pressure + which helpers exist) */
    int n8  = 2 + (int)rnd(6);
    int n16 = 2 + (int)rnd(6);
    int n32 = 6 + (int)rnd(40);     /* the main pressure knob: up to ~45 live u32 */
    int n64 = 2 + (int)rnd(8);
    int nhelpers = (int)rnd(3);     /* 0..2 generated helpers */
    int nstmts = 30 + (int)rnd(90);

    fprintf(out, "/* generated by cvm-fuzz seed=%llu -- DO NOT EDIT.\n", (unsigned long long)seed);
    fprintf(out, " * UB-free differential fixture: conf_main() returns an int32 FNV checksum. */\n");
    fprintf(out, "#include <stdint.h>\n\n");
    emit_helpers();
    for (int i = 0; i < nhelpers; i++) emit_gen_helper(i);

    fprintf(out, "int conf_main(void) {\n");
    /* a volatile seed array defeats constant folding — forces real codegen */
    fprintf(out, "    static const uint32_t SEED[8] = {\n        ");
    for (int i = 0; i < 8; i++) fprintf(out, "UINT32_C(%u),%s", (unsigned)rng_next(),
                                        i == 3 ? "\n        " : " ");
    fprintf(out, "\n    };\n");
    fprintf(out, "    volatile uint32_t vseed = SEED[(uint32_t)%u & 7];\n", (unsigned)rnd(8));

    /* declare + initialise the variable pool from the volatile seed */
    g_count[T_U8] = n8; g_count[T_U16] = n16; g_count[T_U32] = n32; g_count[T_U64] = n64;
    for (int t = 0; t < NTYPES; t++)
        for (int i = 0; i < g_count[t]; i++) {
            fprintf(out, "    %s %s%d = (%s)(vseed ^ ", TYNAME[t], TYPFX[t], i, TYNAME[t]);
            emit_lit(t); fprintf(out, ");\n");
        }
    /* a local array (alloca) we index with masked indices */
    fprintf(out, "    uint32_t arr[16];\n");
    fprintf(out, "    for (uint32_t i = 0; i < 16; i++) arr[i] = vseed * (i + 1u);\n");

    /* the body */
    for (int s = 0; s < nstmts; s++) {
        if (chance(12) && g_count[T_U32] > 0) {  /* array read/write through indices */
            fprintf(out, "    arr[("); emit_leaf(T_U32); fprintf(out, ") & 15] = ");
            emit_expr(T_U32, 2); fprintf(out, ";\n");
            fprintf(out, "    a%u = arr[(", rnd((uint32_t)g_count[T_U32]));
            emit_leaf(T_U32); fprintf(out, ") & 15];\n");
        } else if (nhelpers > 0 && chance(10) && g_count[T_U32] > 0) {
            fprintf(out, "    a%u = gen_h%d(", rnd((uint32_t)g_count[T_U32]), (int)rnd((uint32_t)nhelpers));
            emit_leaf(T_U32); fprintf(out, ", "); emit_leaf(T_U32); fprintf(out, ");\n");
        } else {
            emit_stmt(4);
        }
    }

    /* fold the WHOLE live pool + the array into the FNV checksum */
    fprintf(out, "    uint32_t h = UINT32_C(2166136261);\n");
    fprintf(out, "    #define MIX(v) do { h = (h ^ (uint32_t)(v)) * UINT32_C(16777619); } while (0)\n");
    for (int t = 0; t < NTYPES; t++)
        for (int i = 0; i < g_count[t]; i++) {
            if (t == T_U64) {
                fprintf(out, "    MIX((uint32_t)%s%d); MIX((uint32_t)(%s%d >> 32));\n",
                        TYPFX[t], i, TYPFX[t], i);
            } else {
                fprintf(out, "    MIX((uint32_t)%s%d);\n", TYPFX[t], i);
            }
        }
    fprintf(out, "    for (uint32_t i = 0; i < 16; i++) MIX(arr[i]);\n");
    fprintf(out, "    #undef MIX\n");
    fprintf(out, "    return (int)h;\n}\n");

    if (with_stats)
        fprintf(out, "/* knobs: u8=%d u16=%d u32=%d u64=%d helpers=%d stmts=%d */\n",
                n8, n16, n32, n64, nhelpers, nstmts);

    /* reset pool so a subsequent emit in the same process starts clean */
    memset(g_count, 0, sizeof g_count);
}

/* ------------------------------------------------------------------ CLI --- */
int main(int argc, char **argv) {
    uint64_t seed = 1;
    const char *outpath = NULL;
    int with_stats = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strncmp(argv[i], "--seed=", 7))        seed = strtoull(argv[i] + 7, NULL, 0);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outpath = argv[++i];
        else if (!strcmp(argv[i], "--stats"))            with_stats = 1;
        else if (!strcmp(argv[i], "--no-rot64"))         g_allow_rot64 = 0;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr,
                "cvm-fuzz — UB-free differential fixture generator for CronoVM\n"
                "Usage: cvm-fuzz --seed N [-o FILE] [--stats] [--no-rot64]\n"
                "  emits a self-contained C program exporting `int conf_main(void)`\n"
                "  that returns an int32 FNV checksum; feed it to run_corpus.sh.\n"
                "  --no-rot64  suppress 64-bit rotates (avoids the known llvm.fshl.i64\n"
                "              translator gap) for a GAP-free baseline run.\n");
            return 0;
        } else {
            fprintf(stderr, "cvm-fuzz: unknown arg '%s' (try --help)\n", argv[i]);
            return 2;
        }
    }
    out = stdout;
    if (outpath) {
        out = fopen(outpath, "wb");
        if (!out) { fprintf(stderr, "cvm-fuzz: cannot open '%s'\n", outpath); return 1; }
    }
    emit_program(seed, with_stats);
    if (out != stdout) fclose(out);
    return 0;
}
