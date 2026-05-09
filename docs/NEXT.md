# Where I left off

> Short orientation note for the next coding session. Last touched
> **2026-05-09**, end of step 7b (wide-constant lowering via MOVHI
> and reserved `FUNCS[0]` slot for null-function-pointer trap).

## Current state

31/31 tests pass (test_vm now covers MOVHI + null-fn-ptr trap; e2e
narrow_ops runs without `volatile` since the wide constant `0xFFFF`
mask now lowers cleanly). Pipeline runs end-to-end, including
multi-function modules, recursion, > 8 args via the stack, entry-block
allocas with escaping pointers, indirect calls through function
pointers, static dispatch tables, full sub-word memory access (signed
and unsigned `char` / `short` arrays), arbitrary 32-bit constants,
and a clean trap on calls through a NULL function pointer:

```text
user.c → clang --target=i386-elf -O1 -emit-llvm → user.bc
       → cvm-translate [--heap-reserve=N] [--stack-reserve=N] -o
                                                            → game.bin
       → cvm_run / cvm_run_args                              → result
```

## What works

| Layer | Coverage |
| ----- | -------- |
| Interpreter | 35 opcodes (HALT, MOVI, MOV, ADD/SUB/MUL, LDW/STW, JMP, BEQ, BNE, SYSCALL, 6×CMP, DIV/DIVU/MOD/MODU, SHL/SHR/SAR, AND/OR/XOR, CALL/RET/CALLR, LDB/STB/LDH/STH, **MOVHI**) — see [isa.md](isa.md) |
| Loader | All 8 section types (CODE, DATA, BSS, IMPORTS, DEBUG, HEAP_RESERVE, STACK_RESERVE, FUNCS — slot 0 reserved for null-fn-ptr trap) — see [format.md](format.md) |
| Built-ins | `cvm_sys_heap_start` / `cvm_sys_heap_size` |
| Codegen | Scalar i32 + control flow + globals + memory + syscalls + intrinsics + multi-function CALL/RET, recursion, hybrid R0..R7 + stacked args, alloca, indirect calls (CALLR), function values in DATA initialisers (static dispatch tables), lifetime markers, i8/i16 loads (zero-extend) and stores; SExt via SHL/SAR shift-pair for narrow signed loads; **arbitrary 32-bit immediates via MOVI+MOVHI; user functions live at FUNCS[1..N] so a NULL function pointer cleanly traps** |
| Allocator | Header-only bump in `runtime/lib/cvm_alloc.h` (free is no-op) |

Calling convention: `R255` = SP, `R254` = codegen scratch, `R0..R7`
mirror the syscall ABI for first-8 args, additional args pushed on
the stack by the caller. Return value in `R0`. See
[translator.md](translator.md) for the spill protocol.

## Natural next steps (any is shippable in a session)

### B — `llvm.memcpy` / `llvm.memset` / `llvm.memmove`

Important for struct copies (e.g. returning a struct from a syscall)
and string init. Two paths:

- **Lower to a loop in IR.** Cleanest: emit a small bytecode loop
  per call site. Doesn't need new opcodes.
- **Add MEMCPY/MEMSET opcodes.** Faster but more invasive.

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

## Smaller alternative paths

- **i64 / float64**: bigger lift; new opcodes plus reg-pair handling.
- **`gamecc` wrapper**: hides the `clang | cvm-translate` pipeline
  behind a single command.

## Files to read first when resuming

- `tools/translator/translator.c` — the `LLVMCall` handler is the
  template for any new lowering pattern; `cg_collect_allocas` and
  the prologue/epilogue helpers show how SP is manipulated.
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
