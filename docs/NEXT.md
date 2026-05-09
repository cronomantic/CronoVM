# Where I left off

> Short orientation note for the next coding session. Last touched
> **2026-05-09**, end of step 10 (post-emission branch relaxation
> for conditional brs whose imm8 offset doesn't reach).

## Current state

34/34 ctest cases pass (32 prior + 2 new e2e cases for the
`long_branch.c` fixture exercising the relaxation path). Pipeline
runs end-to-end exactly as before, with the translator no longer
rejecting programs whose conditional branches happen to span more
than ±127 instructions:

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
| Codegen | Scalar i32 + control flow + globals + memory + syscalls + intrinsics + multi-function CALL/RET, recursion, hybrid R0..R7 + stacked args, alloca, indirect calls (CALLR), function values in DATA initialisers, lifetime markers, i8/i16 loads (zero-extend) and stores; SExt via SHL/SAR; arbitrary 32-bit immediates via MOVI+MOVHI; FUNCS[1..N] for user fns with NULL-fn-ptr trap; `llvm.mem*` lowered to single MEMCPY/MEMSET/MEMMOVE opcodes; per-CALL spill narrowed by liveness; **conditional brs whose imm8 offset exceeds ±127 are post-emission relaxed to `BEQ +1; JMP imm24; JMP imm24`** |
| Allocator | Header-only bump in `runtime/lib/cvm_alloc.h` (free is no-op) |

Calling convention: `R255` = SP, `R254` = codegen scratch, `R0..R7`
mirror the syscall ABI for first-8 args, additional args pushed on
the stack by the caller. Return value in `R0`. See
[translator.md](translator.md) for the spill protocol and the
relaxation algorithm.

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

### H — Switch lowering

`LLVMSwitch` currently errors out. Lowering it to a chain of
`CMP_EQ + BNE` (or a jump table when cases are dense) would
unblock C `switch` statements. Dense jump-table form would benefit
from a new opcode (`JMPR Rd` — pc = pc + R[d]) but the chained
compare form needs nothing new.

## Smaller alternative paths

- **i64 / float64**: bigger lift; new opcodes plus reg-pair handling.

## Files to read first when resuming

- `tools/translator/translator.c` — `cg_relax_branches` sits right
  before `cg_resolve_fixups` (search for "Branch relaxation"). It
  iterates to a fixed point, then rebuilds the code array with
  inserted JMPs and updates fixup metadata. The CALL spill loop
  in `cg_function`'s LLVMCall handler still drives liveness from
  `cg_compute_call_liveouts`.
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
- `tests/fixtures/mem_intrin.c` — `__builtin_memset` on a 16-byte
  array, struct copy via `__builtin_memcpy`, and a forward-overlap
  `__builtin_memmove`. Exercises all three block-memory opcodes.
- `tests/fixtures/long_branch.c` — forward goto over a huge
  volatile-arithmetic body to an `end_short:` return block placed
  at the end of the function. The conditional `br` in entry has
  to reach a target ~145 instructions ahead; without
  `cg_relax_branches` the translator errors with
  "branch offset 145 out of range". Confirms relaxation runs and
  produces correct code.
