/* CronoVM intrinsics — direct opcode access for opcodes the C language
 * has no native operator for. Each `cvm_*` wrapper here is a thin shim
 * around an `extern int cvm_intrin_*` declaration; the translator
 * recognises the `cvm_intrin_` name and emits the corresponding opcode
 * inline instead of a CALL. The extern is never linked or defined
 * anywhere — the name exists only so clang has something to reference
 * in the IR.
 *
 * Surface:
 *   cvm_mulh(a, b)         signed   high 32 bits of (int64_t)(a * b)
 *   cvm_mulhu(a, b)        unsigned high 32 bits of (uint64_t)(a * b)
 *   cvm_fsqrt(f)           single-precision square root (FSQRT opcode)
 *   cvm_f2i_sat_s(f)       float → int32  with saturating semantics
 *   cvm_f2i_sat_u(f)       float → uint32 with saturating semantics
 *   cvm_qmul_16_16(a, b)   Q16.16 fixed-point multiply (composes MUL + MULH)
 *
 * With (low = a*b, hi = cvm_mulh(a, b)) you get the full 64-bit product
 * of two 32-bit operands without any 64-bit value ever appearing in a
 * register. That's the canonical embedded-ISA primitive (ARM SMULL,
 * MIPS mult/mfhi, RISC-V MULH); CronoVM exposes the same shape because
 * Q16.16 fixed-point multiply, BSP-traversal cross products and PSX-
 * style perspective math all need it. */

#ifndef CVM_INTRIN_H
#define CVM_INTRIN_H

#include <stdint.h>

extern int32_t  cvm_intrin_mulh (int32_t  a, int32_t  b);
extern uint32_t cvm_intrin_mulhu(uint32_t a, uint32_t b);

static inline int32_t  cvm_mulh (int32_t  a, int32_t  b) { return cvm_intrin_mulh (a, b); }
static inline uint32_t cvm_mulhu(uint32_t a, uint32_t b) { return cvm_intrin_mulhu(a, b); }

/* Float → int with the VM's pinned saturating semantics:
 *   NaN       → 0
 *   ≥ INT_MAX → INT_MAX  (or UINT_MAX for the unsigned variant)
 *   ≤ INT_MIN → INT_MIN  (negatives saturate to 0 for the unsigned variant)
 * Plain `(int32_t)f` in C is UB for out-of-range / NaN inputs, and
 * clang's optimiser is allowed to fold those to any value (often 0).
 * The intrinsic call is opaque to the optimiser, so the VM's F2I_S /
 * F2I_U opcodes always reach the interpreter at runtime — guaranteeing
 * the same value on every host. Use these whenever the input might be
 * NaN, ±Inf, or beyond the int range; use plain `(int32_t)f` when you
 * already know the value is in range and want max codegen freedom. */
extern int32_t  cvm_intrin_f2i_sat_s(float f);
extern uint32_t cvm_intrin_f2i_sat_u(float f);

static inline int32_t  cvm_f2i_sat_s(float f) { return cvm_intrin_f2i_sat_s(f); }
static inline uint32_t cvm_f2i_sat_u(float f) { return cvm_intrin_f2i_sat_u(f); }

/* Single-precision square root via the FSQRT opcode. The translator
 * pattern-matches the `cvm_intrin_fsqrt` name and emits FSQRT inline
 * (no CALL). Routed through the intrinsic shim rather than declaring
 * `extern float sqrtf(float)` directly because the latter would (a)
 * potentially conflict with libm declarations on the host build of the
 * .c source if any test code is compiled outside the VM, and (b) make
 * cvm-compiled .bc files with `sqrtf` calls accidentally portable to
 * non-CronoVM hosts that link against libm. The wrapper makes the
 * dependency on the VM explicit. */
extern float cvm_intrin_fsqrt(float f);

static inline float cvm_fsqrt(float f) { return cvm_intrin_fsqrt(f); }

/* Q16.16 multiply: result = (a × b) >> 16 in fixed-point.
 * The (low, hi) full product is shifted right by 16 across the boundary,
 * which is exactly `(low >> 16) | (hi << 16)`. Single 32-bit value out,
 * no precision loss for in-range inputs. */
static inline int32_t cvm_qmul_16_16(int32_t a, int32_t b) {
    int32_t  lo = (int32_t)((uint32_t)a * (uint32_t)b);   /* MUL  */
    int32_t  hi = cvm_mulh(a, b);                         /* MULH */
    return (int32_t)((uint32_t)lo >> 16) | (hi << 16);
}

/* Q16.16 divide (unsigned): result = ((uint64_t)a << 16) / b, computed by
 * the QDIV1616 opcode as one host 64/32 division. The divide sibling of
 * cvm_qmul_16_16 — the canonical fixed-point quotient an embedded ISA would
 * provide so a `(a << 16) / b` doesn't fall back to a software 48-bit long
 * division. `b == 0` traps (CVM_E_DIV_BY_ZERO), same as DIV/DIVU. Operands
 * are unsigned magnitudes; signed callers (e.g. DOOM's FixedDiv) apply the
 * sign and the overflow guard around it. */
extern uint32_t cvm_intrin_qdiv_16_16(uint32_t a, uint32_t b);

static inline uint32_t cvm_qdiv_16_16(uint32_t a, uint32_t b) {
    return cvm_intrin_qdiv_16_16(a, b);
}

/* General 64/32 unsigned divide: result = (((u64)hi << 32) | lo) / divisor,
 * computed by the QDIV6432 opcode as one host 64/32 division. Where
 * cvm_qdiv_16_16's numerator is fixed to a<<16, this takes the full 64-bit
 * dividend in two halves — so an arbitrary 64-bit numerator (e.g. a 32×32
 * product difference) divides in one op instead of a software 64-bit long
 * division. `divisor == 0` traps (CVM_E_DIV_BY_ZERO). Operands are unsigned
 * magnitudes; signed callers (e.g. DOOM's CVM_CrossDiv) apply the sign and
 * any overflow guard around it. */
extern uint32_t cvm_intrin_qdiv_64_32(uint32_t hi, uint32_t lo, uint32_t divisor);

static inline uint32_t cvm_qdiv_64_32(uint32_t hi, uint32_t lo, uint32_t divisor) {
    return cvm_intrin_qdiv_64_32(hi, lo, divisor);
}

#endif /* CVM_INTRIN_H */
