/* CronoVM intrinsics — direct opcode access for opcodes the C language
 * has no native operator for. Each `cvm_*` wrapper here is a thin shim
 * around an `extern int cvm_intrin_*` declaration; the translator
 * recognises the `cvm_intrin_` name and emits the corresponding opcode
 * inline instead of a CALL. The extern is never linked or defined
 * anywhere — the name exists only so clang has something to reference
 * in the IR.
 *
 * Surface:
 *   cvm_mulh(a, b)   signed   high 32 bits of (int64_t)(a * b)
 *   cvm_mulhu(a, b)  unsigned high 32 bits of (uint64_t)(a * b)
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

/* Q16.16 multiply: result = (a × b) >> 16 in fixed-point.
 * The (low, hi) full product is shifted right by 16 across the boundary,
 * which is exactly `(low >> 16) | (hi << 16)`. Single 32-bit value out,
 * no precision loss for in-range inputs. */
static inline int32_t cvm_qmul_16_16(int32_t a, int32_t b) {
    int32_t  lo = (int32_t)((uint32_t)a * (uint32_t)b);   /* MUL  */
    int32_t  hi = cvm_mulh(a, b);                         /* MULH */
    return (int32_t)((uint32_t)lo >> 16) | (hi << 16);
}

#endif /* CVM_INTRIN_H */
