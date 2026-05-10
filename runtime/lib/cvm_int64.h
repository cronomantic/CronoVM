/* CronoVM 64-bit integer support — user-side library, zero VM changes.
 *
 * The VM has no i64 register class. The translator rejects every
 * appearance of `i64` in LLVM IR, so user code that needs 64-bit
 * integers can't use C's `long long` / `int64_t`. Instead, this header
 * exposes a `cvm_i64` struct of two `uint32_t` halves and a complete
 * arithmetic surface implemented entirely in i32 land. Multiplication
 * uses the existing MULH/MULHU opcodes (via cvm_mulh / cvm_mulhu in
 * cvm_intrin.h) so 32×32→64 multiplies cost one extra opcode rather
 * than a full library call.
 *
 * Layout: `lo` is the low 32 bits, `hi` is the high 32 bits — same
 * arrangement as the host CPU's little-endian `uint64_t`. There's a
 * single struct type; signed vs unsigned interpretation is encoded in
 * the function name (e.g. `cvm_i64_lt` vs `cvm_u64_lt`), matching how
 * RISC-V and ARMv7 expose 64-bit operations on their 32-bit register
 * files.
 *
 * Inlining: the longer helpers carry `__attribute__((noinline))`
 * because clang -O1 would otherwise inline them into every call site
 * and explode past the translator's 254-register budget on a function
 * that does several 64-bit ops in a row. Same trade-off cvm_alloc.h
 * documents for its free-list walk. Cheap helpers (single-line bitwise
 * ops, etc.) stay `static inline`. */

#ifndef CVM_INT64_H
#define CVM_INT64_H

#include <stdint.h>
#include "cvm_intrin.h"

typedef struct cvm_i64 { uint32_t lo, hi; } cvm_i64;

/* --- Construction / truncation ---------------------------------------- */

static inline cvm_i64 cvm_i64_from_i32(int32_t x) {
    cvm_i64 r;
    r.lo = (uint32_t)x;
    r.hi = (uint32_t)(x >> 31);   /* sign-fill */
    return r;
}

static inline cvm_i64 cvm_i64_from_u32(uint32_t x) {
    cvm_i64 r;
    r.lo = x;
    r.hi = 0u;
    return r;
}

static inline cvm_i64 cvm_i64_from_parts(uint32_t lo, uint32_t hi) {
    cvm_i64 r; r.lo = lo; r.hi = hi; return r;
}

static inline int32_t  cvm_i64_to_i32(cvm_i64 x) { return (int32_t)x.lo; }
static inline uint32_t cvm_i64_to_u32(cvm_i64 x) { return x.lo; }

/* --- Negation --------------------------------------------------------- */

static inline cvm_i64 cvm_i64_neg(cvm_i64 x) {
    /* Two's complement: invert + 1, propagating carry. */
    cvm_i64 r;
    r.lo = ~x.lo + 1u;
    r.hi = ~x.hi + (r.lo == 0u ? 1u : 0u);
    return r;
}

/* --- Add / sub -------------------------------------------------------- */

static inline cvm_i64 cvm_i64_add(cvm_i64 a, cvm_i64 b) {
    cvm_i64 r;
    r.lo = a.lo + b.lo;
    /* Carry into hi when low half wraps (sum < either operand). */
    r.hi = a.hi + b.hi + (r.lo < a.lo ? 1u : 0u);
    return r;
}

static inline cvm_i64 cvm_i64_sub(cvm_i64 a, cvm_i64 b) {
    cvm_i64 r;
    r.lo = a.lo - b.lo;
    /* Borrow from hi when low half underflows. */
    r.hi = a.hi - b.hi - (a.lo < b.lo ? 1u : 0u);
    return r;
}

/* --- Multiplication --------------------------------------------------- */
/* (a.hi:a.lo) * (b.hi:b.lo)
 *   = a.lo*b.lo                          (low 64 bits of bottom*bottom)
 *   + (a.lo*b.hi + a.hi*b.lo) << 32       (mixed terms feed the top half)
 *   + (a.hi*b.hi)               << 64    (overflow — discarded)
 *
 * Low 64 bits of a*b (signed and unsigned identical by 2's-complement
 * identity) are: low = a.lo*b.lo, hi = mulhu(a.lo,b.lo) + a.lo*b.hi +
 * a.hi*b.lo. */
__attribute__((noinline))
static cvm_i64 cvm_u64_mul(cvm_i64 a, cvm_i64 b) {
    cvm_i64 r;
    r.lo = a.lo * b.lo;                              /* MUL  */
    uint32_t carry = cvm_mulhu(a.lo, b.lo);          /* MULHU */
    r.hi = carry + a.lo * b.hi + a.hi * b.lo;
    return r;
}

/* Signed multiply produces the same low 64 bits by 2's-complement
 * identity. Provided as a separate name for source-level clarity. */
static inline cvm_i64 cvm_i64_mul(cvm_i64 a, cvm_i64 b) {
    return cvm_u64_mul(a, b);
}

/* --- Shifts (amount masked to 6 bits, range 0..63) ------------------- */

/* The split-shift expressions like `(x.hi << n) | (x.lo >> (32-n))` are
 * canonically a funnel shift, and clang folds them to `llvm.fshl.i32` /
 * `llvm.fshr.i32` whenever it recognises the pattern. The translator
 * doesn't lower those intrinsics (they're outside the supported subset),
 * so we route the inverse shift amount through a `volatile` stack slot to
 * break the SSA chain clang uses for pattern matching. The cost is one
 * extra load/store per call — invisible compared to the shift itself, and
 * paid only when the shift amount is in (0, 32). */
__attribute__((noinline))
static cvm_i64 cvm_u64_shl(cvm_i64 x, uint32_t n_in) {
    uint32_t n = n_in & 63u;
    cvm_i64 r;
    if (n == 0u) { r = x; return r; }
    if (n >= 32u) { r.lo = 0u; r.hi = x.lo << (n - 32u); return r; }
    volatile uint32_t inv = 32u - n;
    r.lo = x.lo << n;
    r.hi = (x.hi << n) | (x.lo >> inv);
    return r;
}

__attribute__((noinline))
static cvm_i64 cvm_u64_shr(cvm_i64 x, uint32_t n_in) {
    uint32_t n = n_in & 63u;
    cvm_i64 r;
    if (n == 0u) { r = x; return r; }
    if (n >= 32u) { r.hi = 0u; r.lo = x.hi >> (n - 32u); return r; }
    volatile uint32_t inv = 32u - n;
    r.hi = x.hi >> n;
    r.lo = (x.lo >> n) | (x.hi << inv);
    return r;
}

__attribute__((noinline))
static cvm_i64 cvm_i64_sar(cvm_i64 x, uint32_t n_in) {
    uint32_t n = n_in & 63u;
    int32_t hi_signed = (int32_t)x.hi;
    cvm_i64 r;
    if (n == 0u) { r = x; return r; }
    if (n >= 32u) {
        r.lo = (uint32_t)(hi_signed >> (int32_t)(n - 32u));
        r.hi = (uint32_t)(hi_signed >> 31);                   /* sign-fill */
        return r;
    }
    volatile uint32_t inv = 32u - n;
    r.hi = (uint32_t)(hi_signed >> (int32_t)n);
    r.lo = (x.lo >> n) | (x.hi << inv);
    return r;
}

/* --- Comparisons (return 1 / 0) -------------------------------------- */

static inline int cvm_i64_eq(cvm_i64 a, cvm_i64 b) { return (a.lo == b.lo) & (a.hi == b.hi); }
static inline int cvm_i64_ne(cvm_i64 a, cvm_i64 b) { return (a.lo != b.lo) | (a.hi != b.hi); }

static inline int cvm_u64_lt(cvm_i64 a, cvm_i64 b) {
    if (a.hi != b.hi) return a.hi < b.hi;
    return a.lo < b.lo;
}
static inline int cvm_u64_le(cvm_i64 a, cvm_i64 b) {
    if (a.hi != b.hi) return a.hi < b.hi;
    return a.lo <= b.lo;
}
static inline int cvm_i64_lt(cvm_i64 a, cvm_i64 b) {
    if (a.hi != b.hi) return (int32_t)a.hi < (int32_t)b.hi;
    return a.lo < b.lo;
}
static inline int cvm_i64_le(cvm_i64 a, cvm_i64 b) {
    if (a.hi != b.hi) return (int32_t)a.hi < (int32_t)b.hi;
    return a.lo <= b.lo;
}

/* --- Bitwise --------------------------------------------------------- */

static inline cvm_i64 cvm_i64_and(cvm_i64 a, cvm_i64 b) {
    cvm_i64 r; r.lo = a.lo & b.lo; r.hi = a.hi & b.hi; return r;
}
static inline cvm_i64 cvm_i64_or (cvm_i64 a, cvm_i64 b) {
    cvm_i64 r; r.lo = a.lo | b.lo; r.hi = a.hi | b.hi; return r;
}
static inline cvm_i64 cvm_i64_xor(cvm_i64 a, cvm_i64 b) {
    cvm_i64 r; r.lo = a.lo ^ b.lo; r.hi = a.hi ^ b.hi; return r;
}
static inline cvm_i64 cvm_i64_not(cvm_i64 x) {
    cvm_i64 r; r.lo = ~x.lo; r.hi = ~x.hi; return r;
}

/* --- Constants ------------------------------------------------------- */

#define CVM_I64_ZERO  ((cvm_i64){0u, 0u})
#define CVM_I64_ONE   ((cvm_i64){1u, 0u})

#endif /* CVM_INT64_H */
