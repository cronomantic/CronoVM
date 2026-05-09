# Where I left off

> Short orientation note for the next coding session. Last touched
> **2026-05-09**, end of step 8 (block memory ops: `MEMCPY`,
> `MEMSET`, `MEMMOVE` opcodes plus the `llvm.mem*` intrinsic
> lowering).

## Current state

37/37 tests pass (test_vm gained six unit tests covering basic
copy/fill, forward-overlap memmove, zero-length corner case, and
out-of-bounds traps for both `MEMCPY` and `MEMSET`; e2e gained
`mem_intrin` exercising struct copy + array memset + overlap-safe
memmove all from a single C source). Pipeline runs end-to-end,
including multi-function modules, recursion, > 8 args via the stack,
entry-block allocas with escaping pointers, indirect calls through
function pointers, static dispatch tables, full sub-word memory
access, arbitrary 32-bit constants, a clean trap on calls through
a NULL function pointer, and now native-speed block memory ops
delegated to the host's `memcpy`/`memset`/`memmove`:

```text
user.c → clang --target=i386-elf -O1 -emit-llvm → user.bc
       → cvm-translate [--heap-reserve=N] [--stack-reserve=N] -o
                                                            → game.bin
       → cvm_run / cvm_run_args                              → result
```

## What works

| Layer | Coverage |
| ----- | -------- |
| Interpreter | 38 opcodes (HALT, MOVI, MOV, ADD/SUB/MUL, LDW/STW, JMP, BEQ, BNE, SYSCALL, 6×CMP, DIV/DIVU/MOD/MODU, SHL/SHR/SAR, AND/OR/XOR, CALL/RET/CALLR, LDB/STB/LDH/STH, MOVHI, **MEMCPY/MEMSET/MEMMOVE**) — see [isa.md](isa.md) |
| Loader | All 8 section types (CODE, DATA, BSS, IMPORTS, DEBUG, HEAP_RESERVE, STACK_RESERVE, FUNCS — slot 0 reserved for null-fn-ptr trap) — see [format.md](format.md) |
| Built-ins | `cvm_sys_heap_start` / `cvm_sys_heap_size` |
| Codegen | Scalar i32 + control flow + globals + memory + syscalls + intrinsics + multi-function CALL/RET, recursion, hybrid R0..R7 + stacked args, alloca, indirect calls (CALLR), function values in DATA initialisers (static dispatch tables), lifetime markers, i8/i16 loads (zero-extend) and stores; SExt via SHL/SAR shift-pair for narrow signed loads; arbitrary 32-bit immediates via MOVI+MOVHI; user functions live at FUNCS[1..N] so a NULL function pointer cleanly traps; **`llvm.memcpy`/`llvm.memset`/`llvm.memmove` lower to single MEMCPY/MEMSET/MEMMOVE opcodes** |
| Allocator | Header-only bump in `runtime/lib/cvm_alloc.h` (free is no-op) |

Calling convention: `R255` = SP, `R254` = codegen scratch, `R0..R7`
mirror the syscall ABI for first-8 args, additional args pushed on
the stack by the caller. Return value in `R0`. See
[translator.md](translator.md) for the spill protocol.

Block ops have a single bounds check per region, then delegate to
the host's libc — typically a SIMD-optimised routine. A struct copy
or `memset` is one dispatch and one host call, not one dispatch per
byte/word in a hand-rolled loop.

## Natural next steps (any is shippable in a session)

### C — Free-list allocator

`runtime/lib/cvm_alloc.h` is currently a bump-only allocator. With
CALL/RET landed it can grow into a real free-list with `cvm_free`.
This is purely user-side code in the runtime header; no VM or
translator changes needed.

### D — Liveness-based spill

Today's `LLVMCall` lowering spills *every* SSA register R8.. across
every call. For programs with many SSA values this is slow. A simple
liveness pass (per basic block, walking uses) would let us spill
only values actually live across each call site.

### E — `gamecc` wrapper

Hide the `clang | cvm-translate` pipeline behind a single command.
Small driver script or C tool that picks the right `--target` and
optimisation flags and pipes the bitcode through.

## Smaller alternative paths

- **i64 / float64**: bigger lift; new opcodes plus reg-pair handling.
- **Long-branch trampolining**: today `BEQ`/`BNE` (signed `imm8`)
  caps reach at ±127 instructions. Generated code can outgrow this
  for large basic blocks; a trampoline through `JMP` (24-bit reach)
  fixes it.

## Files to read first when resuming

- `tools/translator/translator.c` — the `LLVMCall` handler is the
  template for any new lowering pattern; `cg_collect_allocas` and
  the prologue/epilogue helpers show how SP is manipulated. The
  `llvm.mem*` block right above the lifetime-marker drop is the
  most recent example of intrinsic lowering.
- `src/cvm.c` — interpreter is where new opcodes go (both threaded
  and switch paths).
- `docs/isa.md`, `docs/format.md`, `docs/translator.md` — keep
  synced as you land changes.

## Test fixtures of note

- `tests/fixtures/two_funcs.c` — vm_main → add (single user CALL).
- `tests/fixtures/fib_recursive.c` — recursive `fib` (deep stack).
- `tests/fixtures/many_args.c` — sum10 with 10 args (stacked-arg path).
- `tests/fixtures/alloca_swap.c` — `alloca` with escaping pointers.
- `tests/fixtures/fnptr.c` — `select` between functions + indirect call (CALLR).
- `tests/fixtures/dispatch_table.c` — `static const op_t ops[3] = { ... };` table in DATA, indexed at runtime via LDW + CALLR.
- `tests/fixtures/alloc_sum.c` — heap allocator via syscalls.
- `tests/fixtures/narrow_ops.c` — unsigned/signed `char`/`short` arrays with STB/STH round-trip; exercises LDB/LDH/STB/STH and shift-based SExt.
- `tests/fixtures/mem_intrin.c` — `__builtin_memset` on a 16-byte
  array, struct copy via `__builtin_memcpy`, and a forward-overlap
  `__builtin_memmove`. Exercises all three new opcodes from a
  single C function.
