# Where I left off

> Short orientation note for the next coding session. Last touched
> **2026-05-09**, end of step 11 (`LLVMSwitch` lowering — chained
> linear search of CMP_EQ + BNE per case, with a JMP to the
> default at the end).

## Current state

41/41 ctest cases pass (34 prior + 7 new e2e cases for the
`switch_dispatch.c` fixture: 5 cases + default + an in-range gap
value, each path verified independently). Pipeline runs
end-to-end exactly as before; C `switch` statements no longer
get rejected by the translator:

```text
user.c → clang --target=i386-elf -O1 -emit-llvm → user.bc
       → cvm-translate [--heap-reserve=N] [--stack-reserve=N] -o
                                                            → game.bin
       → cvm_run / cvm_run_args                              → result
```

## What works

| Layer | Coverage |
| ----- | -------- |
| Interpreter | 38 opcodes (HALT, MOVI, MOV, ADD/SUB/MUL, LDW/STW, JMP, BEQ, BNE, SYSCALL, 6×CMP, DIV/DIVU/MOD/MODU, SHL/SHR/SAR, AND/OR/XOR, CALL/RET/CALLR, LDB/STB/LDH/STH, MOVHI, MEMCPY/MEMSET/MEMMOVE) — see [isa.md](isa.md) |
| Loader | All 8 section types (CODE, DATA, BSS, IMPORTS, DEBUG, HEAP_RESERVE, STACK_RESERVE, FUNCS — slot 0 reserved for null-fn-ptr trap) — see [format.md](format.md) |
| Built-ins | `cvm_sys_heap_start` / `cvm_sys_heap_size` |
| Codegen | Scalar i32 + control flow (`br`, `switch`, `phi`, `ret`) + globals + memory + syscalls + intrinsics + multi-function CALL/RET, recursion, R0..R7 + stacked args, alloca, indirect calls (CALLR), function values in DATA initialisers, lifetime markers, i8/i16 loads/stores; SExt via SHL/SAR; arbitrary 32-bit immediates via MOVI+MOVHI; FUNCS[1..N] with NULL-fn-ptr trap; `llvm.mem*` lowered to MEMCPY/MEMSET/MEMMOVE; per-CALL spill narrowed by liveness; conditional brs >±127 reach get post-emission relaxation; **`switch` lowered as chained `CMP_EQ + BNE` per case + `JMP default` fall-through** |
| Allocator | Header-only bump in `runtime/lib/cvm_alloc.h` (free is no-op) |

Calling convention: `R255` = SP, `R254` = codegen scratch, `R0..R7`
mirror the syscall ABI for first-8 args, additional args pushed on
the stack by the caller. Return value in `R0`. See
[translator.md](translator.md) for the spill protocol, the
relaxation algorithm, and the switch lowering.

## Natural next steps (any is shippable in a session)

### C — Free-list allocator

`runtime/lib/cvm_alloc.h` is currently a bump-only allocator. With
CALL/RET landed it can grow into a real free-list with `cvm_free`.
This is purely user-side code in the runtime header; no VM or
translator changes needed.

### E — `gamecc` wrapper

Hide the `clang | cvm-translate` pipeline behind a single command.
Small driver script or C tool that picks the right `--target` and
optimisation flags and pipes the bitcode through.

### G — Spill-area compaction

Liveness narrows *which* registers we spill, but the spill area
still reserves a slot per pre-allocated SSA register
(`spill_bytes = (ssa_reg_high - 8) * 4`), wasting frame space for
registers that turn out never to need spilling. A second pass over
`call_lives` could collect the union of all per-call spill sets
and assign compact slot numbers, shrinking `frame_bytes`.

### I — Switch jump-table form (and JMPR opcode)

The chained `CMP_EQ + BNE` form from step 11 is O(N) for N cases.
For dense switches (many contiguous case constants), a jump-table
form is O(1): one `LDW` from a precomputed table indexed by the
input value, followed by an indirect jump to the loaded target.
Needs a new `JMPR Rd` opcode (pc = R[d]) plus extra DATA-section
emission for the jump table. Worth it once a real game uses a
state-machine `switch` in a hot loop.

## Smaller alternative paths

- **i64 / float64**: bigger lift; new opcodes plus reg-pair handling.

## Files to read first when resuming

- `tools/translator/translator.c` — `LLVMSwitch` handling sits
  in `cg_function`'s opcode switch right after `LLVMBr`. Note
  the use of `LLVMGetSwitchCaseValue(i, k)` to read case
  constants (not `LLVMGetOperand` — the C API doesn't expose
  case values through the generic operand list).
- `src/cvm.c` — interpreter (threaded + switch paths must stay
  in sync); no changes this step.
- `docs/isa.md`, `docs/format.md`, `docs/translator.md` — keep
  synced as you land changes.

## Test fixtures of note

- `tests/fixtures/two_funcs.c` — vm_main → add (single user CALL).
- `tests/fixtures/fib_recursive.c` — recursive `fib`; the showcase
  for liveness-based spill (135 → 51 instructions in step 9).
- `tests/fixtures/many_args.c` — sum10 with 10 args (stacked-arg path).
- `tests/fixtures/alloca_swap.c` — `alloca` with escaping pointers.
- `tests/fixtures/fnptr.c` — `select` between functions + indirect call (CALLR).
- `tests/fixtures/dispatch_table.c` — `static const op_t ops[3] = { ... };` table in DATA, indexed at runtime via LDW + CALLR.
- `tests/fixtures/alloc_sum.c` — heap allocator via syscalls.
- `tests/fixtures/narrow_ops.c` — unsigned/signed `char`/`short` arrays with STB/STH round-trip; exercises LDB/LDH/STB/STH and shift-based SExt.
- `tests/fixtures/mem_intrin.c` — exercises MEMCPY/MEMSET/MEMMOVE.
- `tests/fixtures/long_branch.c` — forward goto over a huge body to
  an `end_short:` block. Confirms `cg_relax_branches` works.
- `tests/fixtures/switch_dispatch.c` — `switch` over (op, x, y)
  with 5 cases doing different binary ops, plus a default. Each
  arm depends on runtime inputs so Clang doesn't fold the switch
  into a lookup table, keeping the IR's `switch` instruction
  intact for the codegen to lower.
