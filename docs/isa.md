# Instruction set (v1.0)

CronoVM is **register-based**, with a flat 256-entry register file (`R0..R255`)
of 32-bit signed integers. Every instruction is **32 bits, little-endian**:

```text
 31           24 23           16 15            8 7             0
+---------------+---------------+---------------+---------------+
|       C       |       B       |       A       |    opcode     |
+---------------+---------------+---------------+---------------+
```

Instructions never span multiple words. The PC advances by one instruction
after fetch; branch and jump offsets are applied **after** the increment, so
an offset of `0` means "fall through to the next instruction".

## Opcode table

| ID | Mnemonic | Form | Effect |
| -: | -------- | ---- | ------ |
| 0x00 | `HALT` | `A` | Stop. Return value is `R[A]`. |
| 0x01 | `MOVI` | `A, imm16` | `R[A] = sext(imm16)` |
| 0x02 | `MOV` | `A, B` | `R[A] = R[B]` |
| 0x03 | `ADD` | `A, B, C` | `R[A] = R[B] + R[C]` |
| 0x04 | `SUB` | `A, B, C` | `R[A] = R[B] - R[C]` |
| 0x05 | `MUL` | `A, B, C` | `R[A] = R[B] * R[C]` |
| 0x06 | `LDW` | `A, B` | `R[A] = *(i32*)(heap + R[B])` |
| 0x07 | `STW` | `B, C` | `*(i32*)(heap + R[B]) = R[C]` |
| 0x08 | `JMP` | `imm24 (signed)` | `pc += imm24` |
| 0x09 | `BEQ` | `A, B, imm8` | if `R[A] == R[B]` then `pc += imm8` |
| 0x0A | `BNE` | `A, B, imm8` | if `R[A] != R[B]` then `pc += imm8` |
| 0x0B | `SYSCALL` | `imm16` | Invoke import #imm16. See [syscalls.md](syscalls.md). |
| 0x0C | `CMP_EQ` | `A, B, C` | `R[A] = (R[B] == R[C]) ? 1 : 0` |
| 0x0D | `CMP_NE` | `A, B, C` | `R[A] = (R[B] != R[C]) ? 1 : 0` |
| 0x0E | `CMP_LT` | `A, B, C` | `R[A] = (R[B] <  R[C]) ? 1 : 0` (signed) |
| 0x0F | `CMP_LE` | `A, B, C` | `R[A] = (R[B] <= R[C]) ? 1 : 0` (signed) |
| 0x10 | `CMP_LTU` | `A, B, C` | unsigned `<` |
| 0x11 | `CMP_LEU` | `A, B, C` | unsigned `<=` |
| 0x12 | `DIV` | `A, B, C` | signed division; trap on `R[C]==0` |
| 0x13 | `DIVU` | `A, B, C` | unsigned division; trap on `R[C]==0` |
| 0x14 | `MOD` | `A, B, C` | signed remainder; trap on `R[C]==0` |
| 0x15 | `MODU` | `A, B, C` | unsigned remainder; trap on `R[C]==0` |
| 0x16 | `SHL` | `A, B, C` | `R[A] = R[B] << (R[C] & 31)` |
| 0x17 | `SHR` | `A, B, C` | logical right shift, amount `& 31` |
| 0x18 | `SAR` | `A, B, C` | arithmetic right shift, amount `& 31` |
| 0x19 | `AND` | `A, B, C` | `R[A] = R[B] & R[C]` |
| 0x1A | `OR` | `A, B, C` | `R[A] = R[B] \| R[C]` |
| 0x1B | `XOR` | `A, B, C` | `R[A] = R[B] ^ R[C]` |
| 0x1C | `CALL` | `imm24 (unsigned)` | push pc; `pc = FUNCS[imm24]` |
| 0x1D | `RET` | — | `pc = pop()`; halt with `R[0]` if popped value is `0xFFFFFFFF` |
| 0x1E | `CALLR` | `A` | push pc; `pc = FUNCS[R[A]]` (indirect call via register) |
| 0x1F | `LDB` | `A, B` | `R[A] = (u32)*(u8*)(heap + R[B])` (zero-extend) |
| 0x20 | `STB` | `B, C` | `*(u8*)(heap + R[B]) = R[C] & 0xFF` |
| 0x21 | `LDH` | `A, B` | `R[A] = (u32)*(u16*)(heap + R[B])` (zero-extend) |
| 0x22 | `STH` | `B, C` | `*(u16*)(heap + R[B]) = R[C] & 0xFFFF` |
| 0x23 | `MOVHI` | `A, imm16` | `R[A] = ((u32)imm16 << 16) \| (R[A] & 0xFFFF)` |
| 0x24 | `MEMCPY` | `A, B, C` | `memcpy(heap+R[A], heap+R[B], R[C])` |
| 0x25 | `MEMSET` | `A, B, C` | `memset(heap+R[A], R[B] & 0xFF, R[C])` |
| 0x26 | `MEMMOVE` | `A, B, C` | `memmove(heap+R[A], heap+R[B], R[C])` (overlap-safe) |

### Forms

- **3-register**: `A=rd`, `B=rs1`, `C=rs2`.
- **register + imm16**: `A=rd`, `BC` form a signed 16-bit immediate
  (`B` low byte, `C` high byte).
- **imm24**: `ABC` form a signed 24-bit immediate (`A` low byte,
  `C` high byte). Range: ±8,388,608 instructions.
- **branch (imm8)**: `A=rs1`, `B=rs2`, `C` is a signed 8-bit offset.
  Range: −128..+127 instructions.

### Arithmetic semantics

All arithmetic is two's-complement on 32 bits with **wrap-around** semantics.
Signed overflow is well-defined (it wraps), matching how most retro hardware
behaves and avoiding C's implementation-defined corners.

## Memory model

A single contiguous buffer of `data_size + bss_size + heap_reserve + stack_reserve`
bytes. Loads and stores are bounds-checked against `mem_size = heap_size +
stack_size`. The heap (data + bss + reserve) sits at the bottom; the stack
region sits at the top, starting at `mem_size` and growing downward via
`R255` (SP). See [format.md](format.md) for the section that controls
each region's size.

`LDB`/`STB` need 1 byte at `R[B]`, `LDH`/`STH` need 2, `LDW`/`STW` need 4.
Sub-word memory ops do not require natural alignment — bounds checking is
the only constraint.

Sub-word loads always **zero-extend** into the destination register. When
the IR asks for sign extension (e.g. `sext i8 to i32` after a signed-byte
load), the translator emits an explicit `MOVI scratch, (32-w); SHL; SAR`
sequence. Sub-word stores write only the low byte (`STB`) or low halfword
(`STH`); upper bits in the source register are ignored, so the codegen
doesn't have to mask before storing.

## Block memory ops

`MEMCPY`, `MEMSET`, and `MEMMOVE` take three register operands: destination
address, source address (or fill byte for `MEMSET`), and length in bytes
(read as `uint32`). Each region is bounds-checked once against `[0, mem_size)`
and then delegated to the host's `memcpy`/`memset`/`memmove` — typically a
SIMD-optimised libc routine — so a block move costs one dispatch and one
host call rather than one dispatch per byte/word in a hand-rolled loop.

A length of zero is a no-op even when the source/destination would otherwise
be out of range. `MEMSET`'s value register supplies a single byte (its low 8
bits); upper bits are ignored. `MEMCPY` is undefined when the regions
overlap; use `MEMMOVE` if the regions might overlap. These intrinsics are
the lowering target for `llvm.memcpy`/`llvm.memset`/`llvm.memmove` and for
the `__builtin_mem*` family Clang emits for struct copies and array
initialisation.

## Wide constants

`MOVI` carries a signed 16-bit immediate, which covers the common case of
small literals. For arbitrary 32-bit values the translator pairs `MOVI`
with `MOVHI`: `MOVI rd, lo16; MOVHI rd, hi16` lands `(hi16 << 16) | lo16`
in `R[rd]`. Two instructions for any constant, regardless of bit pattern.
This frees the codegen from any 16-bit cap on global addresses, GEP
strides/offsets, frame sizes, or in-source literals.

## Calls and the stack

`CALL imm24` pushes the address of the next instruction at `R[255] -= 4`
and jumps to `FUNCS[imm24]`. `RET` pops a 32-bit word from the stack and
uses it as the new `pc`; if that word equals the **completion sentinel**
`0xFFFFFFFF`, the run ends and `cvm_run` returns `R[0]`.

`FUNCS[0]` is reserved as the null-function-pointer slot: `CALL imm24=0`
and `CALLR Rd` with `R[d] == 0` trap with `CVM_E_NULL_FUNC_PTR` *before*
indexing the table. User functions live at `FUNCS[1..N]`. This makes the
natural C idiom `if (fp) fp(arg);` work — a zero-initialised function
pointer is the integer `0`, comparing equal to a null check, and calling
through it traps cleanly instead of dispatching into whatever happens to
be at function index 0.

On run start, the interpreter places `R[255] = mem_size`, then pushes the
sentinel as the outermost return PC. Programs that don't use `CALL` get a
zero-sized stack region and never touch SP.

The user-call ABI mirrors the syscall ABI for arguments 0..7 (`R0..R7`).
Any additional arguments (the 9th onward) are pushed by the caller on the
stack at increasing offsets — stacked arg 0 (the 9th overall) sits at
`SP+4` after `CALL`, stacked arg 1 at `SP+8`, and so on. The caller is
responsible for restoring SP after `RET`. The return value lands in `R0`.

## Errors raised by the interpreter

| Code | When |
| ---- | ---- |
| `CVM_E_BAD_OPCODE` | Fetched word's low byte is not a defined opcode. |
| `CVM_E_BAD_PC` | `pc` advanced past the end of CODE. |
| `CVM_E_BAD_ADDR` | `LDW`/`STW`/`RET` would touch memory outside `[0, mem_size)`. |
| `CVM_E_BAD_SYSCALL` | `SYSCALL imm16` references an out-of-range import. |
| `CVM_E_UNLINKED_SYSCALL` | Import has no host handler bound. |
| `CVM_E_SYSCALL_TRAP` | Host handler returned non-zero. |
| `CVM_E_DIV_BY_ZERO` | `DIV`/`DIVU`/`MOD`/`MODU` with `R[C]==0`. |
| `CVM_E_BAD_FUNC_INDEX` | `CALL imm24` references an out-of-range function index. |
| `CVM_E_NULL_FUNC_PTR` | `CALL imm24=0` or `CALLR` with `R[A]=0` — the reserved null-fn-ptr slot. |
| `CVM_E_STACK_OVERFLOW` | `CALL` would push past the bottom of the stack region. |

## Lowering of i1 conditions

LLVM's `icmp` produces an `i1` (boolean). It maps directly to the `CMP_*`
opcodes — they live in 32-bit registers but only ever hold 0 or 1. A
conditional `br i1 %c, %t, %f` lowers to:

```text
BNE  cond_reg, zero_reg, +offset_to_true
JMP  +offset_to_false
```

Every translated function therefore reserves one register at entry as the
zero register and emits `MOVI zero, 0` in the prologue. This is what
allows `BNE`/`BEQ` to act as "branch if non-zero" / "branch if zero" without
adding new opcodes.

## What's not here yet

- 64-bit and float opcodes — landing alongside the LLVM IR types they map to.
- Indexed memory ops with displacement — the encoding has 8 bits left over;
  future revision may carve out room.

## Branch reach and relaxation

`BEQ`/`BNE` carry a signed 8-bit immediate (±127 instructions). When the
translator's codegen produces a conditional branch whose target sits
outside that range, a post-emission **relaxation pass** rewrites the
branch into a 3-instruction trampoline that uses `JMP`'s 24-bit reach
(±8M instructions):

```text
relaxed:    BEQ cond, zero, +1     ; skip the next inst if condition false
            JMP true_target        ; imm24 (long forward/backward reach)
            JMP false_target       ; imm24 (unchanged from short form)
```

The opcode flips (`BNE` ↔ `BEQ`) and the imm8 is hard-coded to `+1`. No
new opcodes are introduced — relaxation only re-shapes emitted bytecode
using `BEQ`/`BNE` and `JMP`. The fast path (single `BEQ`/`BNE imm8` for
short branches) is preserved when the offset fits. See
[translator.md](translator.md) for the relaxation algorithm.
