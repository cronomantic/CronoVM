# Where I left off

> Short orientation note for the next coding session. Last touched
> **2026-05-09**, end of step 9 (liveness-based spill at every call
> site).

## Current state

32/32 ctest cases pass (no new tests this step — all 6 e2e fixtures
that exercise CALL/RET, recursion, indirect calls, and
multi-function modules continue to return identical results, with
substantially smaller emitted bytecode):

| Fixture | Before | After |
| ------- | -----: | ----: |
| `fib_recursive.bin` | 135 | 51 (-62%) |
| `fnptr.bin` | — | 40 |
| `dispatch_table.bin` | — | 49 |
| `mem_intrin.bin` | — | 67 |
| `many_args.bin` | — | 25 |
| `two_funcs.bin` | — | 25 |

(Only `fib_recursive` had a recorded baseline before this step.)
Pipeline runs end-to-end exactly as before:

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
| Codegen | Scalar i32 + control flow + globals + memory + syscalls + intrinsics + multi-function CALL/RET, recursion, hybrid R0..R7 + stacked args, alloca, indirect calls (CALLR), function values in DATA initialisers (static dispatch tables), lifetime markers, i8/i16 loads (zero-extend) and stores; SExt via SHL/SAR shift-pair for narrow signed loads; arbitrary 32-bit immediates via MOVI+MOVHI; user functions live at FUNCS[1..N] so a NULL function pointer cleanly traps; `llvm.memcpy`/`llvm.memset`/`llvm.memmove` lower to single MEMCPY/MEMSET/MEMMOVE opcodes; **per-CALL spill narrowed by fixed-point liveness analysis** |
| Allocator | Header-only bump in `runtime/lib/cvm_alloc.h` (free is no-op) |

Calling convention: `R255` = SP, `R254` = codegen scratch, `R0..R7`
mirror the syscall ABI for first-8 args, additional args pushed on
the stack by the caller. Return value in `R0`. See
[translator.md](translator.md) for the spill protocol — the
liveness-narrowed version is the new normal.

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

### F — Long-branch trampolining

Today `BEQ`/`BNE` (signed `imm8`) caps reach at ±127 instructions.
Generated code can outgrow this for large basic blocks; a
trampoline through `JMP` (24-bit reach) fixes it without touching
the ISA.

### G — Spill-area compaction

Liveness narrows *which* registers we spill, but the spill area
still reserves a slot per pre-allocated SSA register
(`spill_bytes = (ssa_reg_high - 8) * 4`), wasting frame space for
registers that turn out never to need spilling. A second pass over
`call_lives` could collect the union of all per-call spill sets
and assign compact slot numbers, shrinking `frame_bytes`.

## Smaller alternative paths

- **i64 / float64**: bigger lift; new opcodes plus reg-pair handling.

## Files to read first when resuming

- `tools/translator/translator.c` — the new liveness section
  (`cg_bits` helpers + `cg_block_def_use`, `cg_compute_liveness`,
  `cg_compute_call_liveouts`, `cg_lookup_call_live`) sits right
  before `cg_function`. The CALL handler reads the precomputed
  `live_after` set and only spills bits it finds set.
- `src/cvm.c` — interpreter (threaded + switch paths must stay
  in sync); no changes this step.
- `docs/isa.md`, `docs/format.md`, `docs/translator.md` — keep
  synced as you land changes.

## Test fixtures of note

- `tests/fixtures/two_funcs.c` — vm_main → add (single user CALL).
- `tests/fixtures/fib_recursive.c` — recursive `fib` (deep stack
  with several values live across each recursive call —
  best showcase for the liveness benefit).
- `tests/fixtures/many_args.c` — sum10 with 10 args (stacked-arg path).
- `tests/fixtures/alloca_swap.c` — `alloca` with escaping pointers.
- `tests/fixtures/fnptr.c` — `select` between functions + indirect call (CALLR).
- `tests/fixtures/dispatch_table.c` — `static const op_t ops[3] = { ... };` table in DATA, indexed at runtime via LDW + CALLR.
- `tests/fixtures/alloc_sum.c` — heap allocator via syscalls.
- `tests/fixtures/narrow_ops.c` — unsigned/signed `char`/`short` arrays with STB/STH round-trip; exercises LDB/LDH/STB/STH and shift-based SExt.
- `tests/fixtures/mem_intrin.c` — `__builtin_memset` on a 16-byte
  array, struct copy via `__builtin_memcpy`, and a forward-overlap
  `__builtin_memmove`. Exercises all three block-memory opcodes
  from a single C function.
