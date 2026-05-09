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

A single contiguous heap of `data_size + bss_size` bytes. Loads and stores
are bounds-checked: `addr + 4 <= heap_size`. There is currently no stack —
calls are not yet defined.

## Errors raised by the interpreter

| Code | When |
| ---- | ---- |
| `CVM_E_BAD_OPCODE` | Fetched word's low byte is not a defined opcode. |
| `CVM_E_BAD_PC` | `pc` advanced past the end of CODE. |
| `CVM_E_BAD_ADDR` | `LDW`/`STW` would read or write outside the heap. |
| `CVM_E_BAD_SYSCALL` | `SYSCALL imm16` references an out-of-range import. |
| `CVM_E_UNLINKED_SYSCALL` | Import has no host handler bound. |
| `CVM_E_SYSCALL_TRAP` | Host handler returned non-zero. |
| `CVM_E_DIV_BY_ZERO` | `DIV`/`DIVU`/`MOD`/`MODU` with `R[C]==0`. |

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

- `CALL` / `RET` / stack frames — pinned to the calling-convention work.
- 64-bit and float opcodes — landing alongside the LLVM IR types they map to.
- Indexed memory ops with displacement — the encoding has 8 bits left over;
  future revision may carve out room.
- Long branches — current ±127 reach for `BEQ`/`BNE` (signed `imm8`) is
  enough for current fixtures. When it isn't, the codegen will need to
  trampoline through `JMP` (which has 24-bit reach).
