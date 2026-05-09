# Where I left off

> Short orientation note for the next coding session. Last touched
> **2026-05-09**, end of step 11 (`LLVMSwitch` lowering — chained
> linear search of CMP_EQ + BNE per case, with a JMP to the
> default at the end).

## Current state

41/41 ctest cases pass (34 prior + 7 new e2e cases for the
`switch_dispatch.c` fixture: 5 cases + default + an in-range gap
value, each path verified independently). Pipeline runs
end-to-end exactly as before:

```text
user.c → clang --target=i386-elf -O1 -emit-llvm → user.bc
       → cvm-translate [--heap-reserve=N] [--stack-reserve=N] -o
                                                            → game.bin
       → cvm_run / cvm_run_args                              → result
```

## Design constraints (locked in 2026-05-09)

The deployment target is **embedded systems** — Cortex-M class,
ESP32, RP2040, etc. This pins three architectural decisions that
were previously open:

1. **Strictly 32-bit ISA, forever.** No `i64` opcodes, no
   reg-pairs, no 64-bit accumulators in the register file. The
   register bank is 256 × `int32_t` (1 KiB total). All
   arithmetic, all addressing, all bounds checks are 32-bit.
2. **No `f64`. `f32` is optional at most.** Many embedded
   targets are integer-only or have only an FPv4-SP single-
   precision FPU. If float opcodes ever land, they must be
   feature-flagged so binaries that need them are rejected on
   integer-only hosts rather than failing silently.
3. **Single-file embeddable interpreter.** No external libs in
   the hot loop (just `memcpy`/`memset`). Hot loop must compile
   cleanly under freestanding builds (newlib/picolibc/none).

The renderer direction is **deliberately undecided** — both
software-rendered (DOOM/Chocolate Doom-style with the rasterizer
as bytecode + host primitives, or fully-bytecode for bragging
rights) and 3D-via-syscalls (PSX/N64-style fixed-function or
modern GPU passthrough) must remain viable until a concrete
target shapes the API.

## What works

| Layer | Coverage |
| ----- | -------- |
| Interpreter | 38 opcodes (HALT, MOVI, MOV, ADD/SUB/MUL, LDW/STW, JMP, BEQ, BNE, SYSCALL, 6×CMP, DIV/DIVU/MOD/MODU, SHL/SHR/SAR, AND/OR/XOR, CALL/RET/CALLR, LDB/STB/LDH/STH, MOVHI, MEMCPY/MEMSET/MEMMOVE) — see [isa.md](isa.md) |
| Loader | All 8 section types (CODE, DATA, BSS, IMPORTS, DEBUG, HEAP_RESERVE, STACK_RESERVE, FUNCS — slot 0 reserved for null-fn-ptr trap) — see [format.md](format.md) |
| Built-ins | `cvm_sys_heap_start` / `cvm_sys_heap_size` |
| Codegen | Scalar i32 + control flow (`br`, `switch`, `phi`, `ret`) + globals + memory + syscalls + intrinsics + multi-function CALL/RET, recursion, R0..R7 + stacked args, alloca, indirect calls (CALLR), function values in DATA initialisers, lifetime markers, i8/i16 loads/stores; SExt via SHL/SAR; arbitrary 32-bit immediates via MOVI+MOVHI; FUNCS[1..N] with NULL-fn-ptr trap; `llvm.mem*` lowered to MEMCPY/MEMSET/MEMMOVE; per-CALL spill narrowed by liveness; conditional brs >±127 reach get post-emission relaxation; `switch` lowered as chained `CMP_EQ + BNE` per case + `JMP default` |
| Allocator | Header-only bump in `runtime/lib/cvm_alloc.h` (free is no-op) |

Calling convention: `R255` = SP, `R254` = codegen scratch, `R0..R7`
mirror the syscall ABI for first-8 args, additional args pushed on
the stack by the caller. Return value in `R0`. See
[translator.md](translator.md) for the spill protocol, the
relaxation algorithm, and the switch lowering.

## Foundation roadmap (in order)

The goal of the next 5 steps is to land a base that's
**production-ready for either renderer direction** without
committing to one. Stop after step M' and the VM is "done" for
the foundation phase — anything further is renderer-specific or
embedded-target-specific work.

### C — Free-list allocator

`runtime/lib/cvm_alloc.h` is currently bump-only. With CALL/RET
landed it can grow into a real free-list with `cvm_free` and
basic coalescence. Purely user-side code in the runtime header;
no VM or translator changes needed. Good warm-up step.

### E — `gamecc` wrapper

Hide the `clang | cvm-translate` pipeline behind a single
command. Picks the right `--target=i386-elf -O1 -emit-llvm`
flags and pipes the bitcode through. Removes per-build friction
when engine code starts being non-trivial.

### K — Host memory regions (`CVM_SEC_HOST_REGION`)

Generalise the current `DATA / BSS / HEAP_RESERVE / STACK`
layout to allow named regions declared by the binary and
populated/observed by the host. Use cases that BOTH renderer
paths need:

- **Framebuffer** (software path): host reads, VM writes.
- **Command buffer** (3D path): VM writes, host consumes at `present()`.
- **Input state** (both): host writes once per frame, VM reads.
- **Audio buffer** (both): VM writes samples, host mixes.
- **Texture / asset memory** (3D path): host loads, VM references by offset.

Format: a new section type with `{name[16], offset, size,
direction (R/W/RW), flags}` entries. Loader reserves the space
inside `mem_size` and exposes offsets. Add:

- `cvm_image_get_region(img, name) → offset` (host-side public API)
- `cvm_sys_get_region(name) → offset` (built-in syscall the
  game uses to discover its regions at startup)

This is the most conceptual change of the foundation phase — it
defines the layered contract between VM and host *without*
constraining what's in each region.

### L — Feature flags (`CVM_SEC_REQUIRES`)

Once binaries depend on specific host APIs, a `.bin` requesting
`renderer.software@1` must NOT load on a host that only provides
`renderer.gpu@1`. Today the only signal is import names — works
but fragile.

Format: a new section with NUL-separated feature strings
(`"renderer.software@1\0audio.pcm@1\0fixedpoint.q16_16@1\0"`).
Loader validates against host-advertised features; mismatch
returns a new `CVM_E_MISSING_FEATURE` error code. Forces API
versioning from day one — exactly what we want.

### M' — `MULH` / `MULHU` opcodes (fixed-point completion)

Substitute for the dropped i64 opcodes. Returns the upper 32
bits of a signed/unsigned 32×32 → 64 multiply, the canonical
embedded-ISA primitive (ARM `SMULL`/`UMULL`, MIPS `mult`/`mfhi`,
RISC-V `MULH`). With `MUL` + `MULH` you compose Q16.16
fixed-point multiplication at full precision in 4 instructions
(MUL, MULH, two shifts) without ever leaving 32-bit registers.

This is what unlocks DOOM's hot loop (texture mapping, BSP
traversal, perspective) and PSX-style coordinate math without a
single 64-bit value in the architecture. Two new opcodes; no
reg-pairs; no ABI changes.

## Smaller / deferred work

- **Optional `f32` opcodes**: only if a real game pushes for
  them. Would need feature flag (`fp.f32@1`) so integer-only
  hosts reject the binary cleanly.
- **Saturating arithmetic** (`ADDS`/`SUBS`): useful for audio
  mixing and blends. Niche; add when a fixture asks.
- **Switch jump-table form** (the previously-numbered "I"):
  needs new `JMPR Rd` opcode for indirect dispatch. Worth it
  once a real game uses a state-machine switch in a hot loop.
- **Spill-area compaction** (the previously-numbered "G"):
  `spill_bytes` still reserves a slot per pre-allocated SSA
  register; only used slots could be allocated. Pure
  optimization, no correctness impact.

## Embedded-specific concerns to address before "embed-ready"

### Pluggable allocator at load time

`cvm_load` calls `malloc` directly. Embedded systems often have
no malloc, custom allocators (FreeRTOS heap_4, Zephyr k_malloc),
or fixed memory pools. Two options that complement each other:

- Add `cvm_allocator_t` (`malloc_fn`, `free_fn`) parameter to a
  new `cvm_load_ex` overload. Existing `cvm_load` keeps working
  with the system malloc.
- Add `cvm_load_with_workspace(bytes, len, void *ws, size_t
  ws_size, ...)` that takes a pre-allocated buffer for *all*
  load-time allocations. Lets bare-metal targets without dynamic
  malloc run binaries.

### Interpreter footprint audit

`src/cvm.c` should compile under 16 KiB on Cortex-M with `-Os`.
Measure when there's a real target; if it grows too much,
guard optional opcodes behind compile-time `#ifdef CVM_ENABLE_*`
flags so a minimal build can drop them.

### libc-freestanding audit

Hot loop uses `memcpy`/`memset` only — both available in
freestanding builds. Load-time path uses `malloc`/`free`/
`strcmp`/`strlen` — also freestanding. Confirm no `printf`,
`assert`, or `errno` slip in (one quick `grep` when
embed-readiness becomes a milestone).

### Default sizes

`CVM_DEFAULT_STACK_RESERVE = 16 KiB` is fine for desktop, often
excessive on embedded. Keep the default but document in
[translator.md] that embedded targets should set
`--stack-reserve` based on worst-case CALL depth.

### Endianness

Format is explicitly little-endian; interpreter uses `memcpy`
for word access (assumes host LE for bit-exact copies). All
realistic embedded targets (Cortex-M, ESP32, RISC-V) are LE by
default. Big-endian would need byte-swap on load — not a
priority.

## Files to read first when resuming

- `tools/translator/translator.c` — `LLVMSwitch` handling sits
  in `cg_function`'s opcode switch right after `LLVMBr`. Note
  the use of `LLVMGetSwitchCaseValue(i, k)` to read case
  constants (not `LLVMGetOperand` — the C API doesn't expose
  case values through the generic operand list).
- `src/cvm.c` — interpreter (threaded + switch paths must stay
  in sync); no changes since step 11.
- `include/cvm.h` — opcodes, error codes, section types,
  `cvm_image` struct.
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
