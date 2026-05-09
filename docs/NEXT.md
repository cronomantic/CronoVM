# Where I left off

> Short orientation note for the next coding session. Last touched
> **2026-05-09**, end of step 6b.

## Current state

19/19 tests pass. Pipeline runs end-to-end:

```text
user.c → clang --target=i386-elf -O1 -emit-llvm → user.bc
       → cvm-translate [--heap-reserve=N] -o    → game.bin
       → cvm_run / cvm_run_args                  → result
```

The first "real" C fixture (`tests/fixtures/alloc_sum.c`) compiles and
runs: it includes the bump allocator from `runtime/lib/cvm_alloc.h`,
asks for 4 KB of free heap, calls `cvm_malloc`, fills an array, sums it.

## What works

| Layer | Coverage |
| ----- | -------- |
| Interpreter | 27 opcodes (HALT, MOVI, MOV, ADD, SUB, MUL, LDW, STW, JMP, BEQ, BNE, SYSCALL, 6×CMP, DIV/DIVU/MOD/MODU, SHL/SHR/SAR, AND/OR/XOR) — see [isa.md](isa.md) |
| Loader | All 6 section types (CODE, DATA, BSS, IMPORTS, DEBUG, HEAP_RESERVE) — see [format.md](format.md) |
| Built-ins | `cvm_sys_heap_start` / `cvm_sys_heap_size` auto-bound on load |
| Codegen | Scalar i32 arithmetic, control flow (`br`/`phi`/`select`), `icmp`, memory (`load`/`store`/`getelementptr`), globals, syscall calls, intrinsic lowering for `llvm.abs`/`smax`/`smin`/`umax`/`umin` — see [translator.md](translator.md) |
| Allocator | Header-only bump in `runtime/lib/cvm_alloc.h` (free is no-op) |

## Natural next step: 6c — CALL/RET + stack + alloca

This is the biggest architectural piece left. Before coding, decide:

1. **Stack pointer**: dedicated register (e.g. R255) or implicit?
2. **Frame layout**: where do locals live? Where do return addresses?
3. **Caller- vs callee-saved**: simplest is "everything caller-saved"
   for v1.
4. **CALL encoding**: imm24 absolute target index? Two new opcodes
   (`CALL`, `RET`).
5. **Stack region**: top of heap (above HEAP_RESERVE), or a separate
   header-declared section like `STACK_RESERVE`?
6. **Argument passing**: mirror the syscall ABI (`R0..R7`), or
   different? Mirroring keeps things uniform.

Once CALL/RET works:
- The bump allocator can grow into a real free-list with `cvm_free`.
- Multi-function user programs (libraries, recursive functions) become
  possible.
- `alloca` lowers to SP arithmetic.

## Smaller alternatives if you don't want to dive into 6c

Any of these is a self-contained 1–2 hour session:

- **i8 / i16 memory ops** (`LDB`/`STB`/`LDH`/`STH`): unblocks `char` and
  `short` array access. Currently the codegen rejects loads/stores
  narrower than `i32`.
- **More intrinsics**: `llvm.memcpy`, `llvm.memset`, `llvm.memmove` —
  important for struct copies and string init.
- **Wide constants**: only when a fixture demands `>32 KB` of DATA.

## Files to read first when resuming

- `tools/translator/translator.c` — the `LLVMCall` handler at the
  bottom of the codegen switch contains the syscall lowering template;
  user-call lowering will look similar.
- `src/cvm.c` — interpreter is where new opcodes go (both threaded and
  switch paths).
- `docs/isa.md`, `docs/translator.md` — keep synced as you land
  changes.
