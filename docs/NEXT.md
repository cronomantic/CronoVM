# Where I left off

> Short orientation note for the next coding session. Last touched
> **2026-05-10**, end of step pair G + I from the deferred backlog:
> spill-area compaction (frames now hold a slot only for SSA regs
> live across some call) and dense-switch jump-table form (`JMPR`
> opcode + DATA-resident table; chosen automatically when
> `n_cases ≥ 4` AND density ≥ 0.5).

## Current state

56/56 ctest cases pass (47 prior + 9 new `e2e_switch_tab_*` covering
all 8 cases of the dense switch + default). G is observable as
slightly smaller binaries for call-heavy fixtures (`free_list`
went 371 → 367 instructions); I is observable as `switch_table.bin`
having a 32-byte DATA section holding the 8-entry jump table.
Pipeline runs end-to-end exactly as before:

```text
user.c → clang --target=i386-elf -O1 -emit-llvm → user.bc
       → cvm-translate [--heap-reserve=N] [--stack-reserve=N]
                       [--region=name:size[:dir]]... -o      → game.bin
       → cvm_run / cvm_run_args                              → result
```

## Design constraints (revised 2026-05-10 — overrides 2026-05-09)

The deployment target is **embedded systems** — Cortex-M class,
ESP32, RP2040, etc. — but the goal includes **simple 3D**, which
moved `f32` from "optional" to mandatory. Three architectural
decisions:

1. **32-bit register file.** The bank is 256 × `int32_t` (1 KiB
   total). Integer arithmetic, addressing, bounds checks all
   32-bit. `i64` doesn't exist as a register class — code that
   needs 64-bit integers uses two adjacent 32-bit values via
   `runtime/lib/cvm_int64.h` (deferred until a fixture asks).
   `MULH`/`MULHU` give 32×32→64 multiply without leaving the
   register file.
2. **`f32` first-class, `f64` rejected.** A register holds 32 bits
   and is reinterpreted as `float` inside any `F*` opcode (no
   separate float register file). On hosts without an FPU
   (Cortex-M0/M0+/M3, RP2040), the C compiler that builds `cvm.c`
   provides soft-float via `libgcc` — slower but functional, no
   feature flag needed. `double` is rejected by the type subset;
   code that needs it includes the future `runtime/lib/cvm_float64.h`
   software-emulated header.
3. **Single-file embeddable interpreter.** No external libs in the
   hot loop (just `memcpy`/`memset` plus the C compiler's float
   runtime where needed). Hot loop compiles cleanly under
   freestanding builds (newlib/picolibc/none).

The renderer direction is **deliberately undecided** — both
software-rendered (DOOM/Chocolate Doom-style with the rasterizer
as bytecode + host primitives, or fully-bytecode for bragging
rights) and 3D-via-syscalls (PSX/N64-style fixed-function or
modern GPU passthrough) must remain viable until a concrete
target shapes the API.

## What works

| Layer | Coverage |
| ----- | -------- |
| Interpreter | 54 opcodes (HALT, MOVI, MOV, ADD/SUB/MUL, LDW/STW, JMP, BEQ, BNE, SYSCALL, 6×CMP, DIV/DIVU/MOD/MODU, SHL/SHR/SAR, AND/OR/XOR, CALL/RET/CALLR, LDB/STB/LDH/STH, MOVHI, MEMCPY/MEMSET/MEMMOVE, MULH/MULHU, FADD/FSUB/FMUL/FDIV/FNEG, 4×FCMP, F2I_S/F2I_U, I2F_S/I2F_U, JMPR) — see [isa.md](isa.md) |
| Loader | All 9 section types (CODE, DATA, BSS, IMPORTS, DEBUG, HEAP_RESERVE, STACK_RESERVE, FUNCS — slot 0 reserved for null-fn-ptr trap, HOST_REGION) — see [format.md](format.md) |
| Built-ins | `cvm_sys_heap_start` / `cvm_sys_heap_size` / `cvm_sys_get_region` |
| Codegen | Scalar i32 + control flow (`br`, `switch`, `phi`, `ret`) + globals + memory + syscalls + intrinsics + multi-function CALL/RET, recursion, R0..R7 + stacked args, alloca, indirect calls (CALLR), function values in DATA initialisers, lifetime markers, i8/i16 loads/stores; SExt via SHL/SAR; arbitrary 32-bit immediates via MOVI+MOVHI; FUNCS[1..N] with NULL-fn-ptr trap; `llvm.mem*` lowered to MEMCPY/MEMSET/MEMMOVE; per-CALL spill narrowed by liveness AND compacted (slot only for regs ever live across some call); conditional brs >±127 reach get post-emission relaxation; `switch` lowered as chained `CMP_EQ + BNE` per case (sparse) or DATA jump-table + `LDW + JMPR` (dense, n_cases ≥ 4 AND density ≥ 0.5) |
| Allocator | Header-only first-fit free list in `runtime/lib/cvm_alloc.h` (split on alloc, forward coalesce on free) |

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

### ~~C — Free-list allocator~~  *(done 2026-05-09)*

Shipped as `runtime/lib/cvm_alloc.h`: one-word block header (size
plus a low-bit free flag, no boundary tags), first-fit walk on
`cvm_malloc`, single forward-coalesce pass on `cvm_free`. The
helpers carry `__attribute__((noinline))` because clang -O1 would
otherwise inline the loop into every call site and blow past the
translator's 254-register budget. Test fixture: `free_list.c`
against a 512-byte heap, three phases (reuse, split+coalesce, OOM)
with a `if (!ptr) return N` after every alloc — the null-check
pattern is what made the translator bug below reproducible.

Two translator fixes piggybacked on the same step:

1. **`LLVMIsUndef` handler in `cg_reg_for`.** clang -O1 emits
   `phi ptr [undef, %prev_block]` whenever a value is dynamically
   dead on some incoming edge; the previous code treated that as
   an unsupported value kind. The handler hands back the dedicated
   zero register — defined enough for the phi MOV to land, and the
   dynamic path that would read it is dead by construction.

2. **`cg_relax_branches` stale-`cap` fix** (translator.c:1019).
   After relaxation reallocates `cg->code` with `malloc(new_size)`,
   `cg->count` was updated but `cg->cap` still held the *old*
   buffer's larger capacity. The next function's `cg_emit` then
   compared `count == cap`, decided no growth was needed, and wrote
   past the end of the freshly-malloc'd buffer. ASAN flagged it as
   a 1-byte heap-buffer-overflow inside `cg_emit` against the
   buffer allocated in `cg_relax_branches`. The fix is one line
   (`cg->cap = new_size;` after the buffer swap). The bug was
   latent for fixtures where (a) the function before relaxation
   was the *last* function emitted (`long_branch.c`) or (b) no
   branch ever reached far enough to trigger relaxation in the
   first place — i.e. nothing else in the suite stressed it.

### ~~E — `cvm-cc` wrapper~~  *(done 2026-05-10)*

Shipped. `tools/cvm-cc/cvm-cc.c` is a single-command driver:

```sh
cvm-cc user.c -o game.bin --heap-reserve=4M --region=fb:64K:w
```

Pipeline: `.c → <output>.tmp.bc → .bin → rm <tmp>` (unless
`--keep-bc`). `.bc` input skips clang. Pass-through flags:
`--heap-reserve`, `--stack-reserve`, `--region` go to
cvm-translate; `-I <dir>` and `-O<n>` go to clang (default `-O1`,
matches what mem2reg promotion needs). Tool discovery: `--clang=` /
`--translate=` overrides, then sibling-of-cvm-cc lookup, then
CMake-baked default (the build-tree path), then PATH. Runtime
header dir is also baked at build time so `#include "cvm_intrin.h"`
works without `-I`.

Naming: `cvm-cc` follows the `cvm-translate` pattern. `cc` is the
universal "C compiler driver" suffix (gcc, clang, tcc). The earlier
`gamecc` working name was rejected because it baked "game" into the
toolchain when the VM is deliberately generic.

Implementation notes worth keeping in head:

- **Process spawn, not `system()`.** Windows' cmd.exe strips
  outer quotes from quoted command lines, breaking
  `"clang" "--target=…"`. Wrapping in extra quotes works for some
  cases but fails when both the executable and an argument are
  quoted (`""cvm-translate""` becomes empty + name + empty).
  `_spawnvp` on Windows / `fork+execvp` on POSIX bypass shell
  parsing entirely; the CRT handles per-arg quoting.
- **Bake the translator path** via CMake (`CVM_TRANSLATOR_DEFAULT`
  macro). The build-tree layout puts cvm-cc in `build/tools/cvm-cc/`
  and cvm-translate in `build/tools/translator/` — they're not
  siblings — so the obvious "look next to me" lookup misses.
  Override at run time with `--translate=`.
- **Test** `e2e_cvm_cc_add` drives the full wrapper on `add.c`
  and runs the resulting binary under `test_e2e`. Catches argv
  plumbing, tool discovery, intermediate-file handling.

### ~~K — Host memory regions (`CVM_SEC_HOST_REGION`)~~  *(done 2026-05-10)*

Shipped. New section type `CVM_SEC_HOST_REGION = 9` with 28-byte
entries (`name[16] + size:u32 + direction:u32 + flags:u32`); the
binary declares only `{name, size, direction}`, the loader assigns
offsets between BSS and HEAP_RESERVE in declaration order (each
size rounded up to a 4-byte multiple). Direction (`r`/`w`/`rw`) is
informational — the VM enforces only heap bounds.

- Translator flag: `--region=name:size[:rw|r|w]` (default `rw`),
  repeatable, max 64 per binary, max 15-char visible name.
- Built-in syscall: `cvm_sys_get_region(name_addr) → offset` or
  `-1` if the name isn't declared. The argument is a heap address
  pointing at a NUL-terminated string ≤ 16 bytes including the NUL;
  the loader bounds-checks the read so a malicious VM can't escape
  the heap.
- Host API: `cvm_image_get_region(img, name, &off, &size) → CVM_OK
  / CVM_E_NO_SUCH_REGION`. The host accesses bytes at
  `img->heap + off`.
- Test harness: dedicated `tests/test_regions.c` that pre-fills an
  `input` region with an i32, runs the fixture (which reads input
  and writes a derived pattern into a `fb` region), then verifies
  `fb` byte-for-byte from the host side.

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

### ~~M' — `MULH` / `MULHU` opcodes (fixed-point completion)~~  *(done 2026-05-10)*

Shipped. Two opcodes (`MULH = 0x27`, `MULHU = 0x28`) that return
the upper 32 bits of a signed / unsigned 32×32→64 product. With
`MUL` + `MULH` a Q16.16 multiply lands in four instructions
without any 64-bit value ever entering the register file.

Surface choice: the user calls `cvm_mulh` / `cvm_mulhu` /
`cvm_qmul_16_16` from `runtime/lib/cvm_intrin.h`. Those wrappers
forward to extern decls `cvm_intrin_mulh` / `cvm_intrin_mulhu`;
the translator pattern-matches the `cvm_intrin_` name and emits
the opcode inline (no CALL, no FUNCS slot, no syscall import).
Pattern lives next to `llvm.abs.i32` / `llvm.smax.i32` in the
LLVMCall handler, so future "I want a single opcode for this"
intrinsics can land on the same hook.

The translator deliberately does NOT pattern-match
`(int64_t)a * b >> 32` IR — clang -O1 emits real `i64 mul` for
that, which our type-subset rejects. Going through the explicit
wrapper keeps the boundary clean and avoids fragile multi-instr
IR matching.

### ~~F — IEEE 754 single-precision floats~~  *(done 2026-05-10)*

Shipped. 13 opcodes (`FADD/FSUB/FMUL/FDIV` 0x29–0x2C, `FNEG` 0x2D,
`FCMP_EQ/NE/LT/LE` 0x2E–0x31, `F2I_S/F2I_U` 0x32–0x33,
`I2F_S/I2F_U` 0x34–0x35) over the same 32-bit register file —
`bitcast f32 ↔ i32` is a no-op MOV. `double` is rejected by the
type subset (clear error message); `float` is accepted by every
LLVM IR opcode the translator now handles (`LLVMFAdd`, `LLVMFSub`,
`LLVMFMul`, `LLVMFDiv`, `LLVMFNeg`, `LLVMFCmp`, `LLVMSIToFP`,
`LLVMUIToFP`, `LLVMFPToSI`, `LLVMFPToUI`).

`F2I_S/F2I_U` semantics are **pinned saturating**: NaN → 0, ±Inf
or out-of-range → INT32_MAX / INT32_MIN / UINT32_MAX. Matches
ARM/RISC-V/Wasm-saturating consensus; explicitly does NOT inherit
x86's "indefinite integer" 0x80000000 behaviour, otherwise the
same `.bin` would yield different results on x86 vs ARM hosts.

Subtlety the source language imposes: in C, `(int32_t)f` for
NaN / out-of-range `f` is **undefined behaviour**, and clang -O1
folds those casts to any value (often 0) before they reach the
F2I opcode. To preserve runtime saturating from C, route through
`cvm_f2i_sat_s` / `cvm_f2i_sat_u` in `runtime/lib/cvm_intrin.h` —
extern intrinsic calls are opaque to the optimiser. Same opcode
either way; the intrinsic just guarantees the opcode actually
runs. The float fixture (`tests/fixtures/float_basic.c`)
discovered this by failing on `(int32_t)+Inf != INT32_MAX` — clang
had folded the comparison to "always true" via UB-exploit.

`FCmp` predicates: only the six that natural C emits (`OEQ`, `UNE`,
`OLT`, `OLE`, `OGT`, `OGE`) are supported; the other eight are
rejected with a clear error. Clean enough for any code that uses
straight `==/!=/<` etc. Reordering to swap operands handles `>` and
`>=` against the ordered `LT`/`LE` opcodes.

Soft-float story: the VM does not implement soft-float internally.
The interpreter does C float arithmetic (`(float)R[b] * (float)R[c]`);
whether that resolves to hardware FPU or libgcc soft-float helpers
is decided by the compiler that builds `cvm.c`. Cortex-M0/M0+/M3
and RP2040 take the soft-float path (~50–100× slower than hardware)
without any feature flag. Cortex-M4F+ and ESP32 LX6/LX7 use hardware
transparently.

### ~~G — Spill-area compaction~~  *(done 2026-05-10)*

Frame size shrinks. Previously every SSA register R8..R(ssa_reg_high-1)
got a 4-byte spill slot whether or not it crossed a call. Now slots
are allocated only for regs that appear in the OR of every call's
live-after set (`cg->ever_spilled`). The mapping lives in
`cg->slot_of[bit] → compact slot or 0xFF`; the LLVMCall handler's
spill/restore loops dereference it before computing the SP offset and
skip regs with no slot reserved. `spill_bytes = spill_slot_count * 4`,
`frame_bytes = alloca_bytes + spill_bytes`. Observable effect on
call-heavy fixtures (`free_list.bin` 371 → 367 instructions). Pure
optimisation; the 47 prior tests pass identically.

### ~~I — Switch jump-table form~~  *(done 2026-05-10)*

`LLVMSwitch` now picks chain or table per switch. Heuristic: table
when `n_cases ≥ 4` AND `range ≤ 2 × n_cases` (density ≥ 0.5). New
opcode `JMPR A=rs1` (`0x36`): `pc = (uint32_t)R[A]` after a
code_count bounds check. New fixup type
(`struct cg_table_fixup {data_off, target}`) — the table lives in
DATA as `u32[n_range]` of absolute instruction indices, patched
after branch relaxation freezes block layout.

Dispatch sequence:
`SUB off=cond-lo; CMP_LTU inrange,off,n; BEQ inrange,zero,+M; …;
LDW target,addr; JMPR target; default JMP`.
Negative values fall through to default automatically thanks to
`CMP_LTU` (the `cond - lo` underflow wraps to a huge unsigned).

Test fixture `tests/fixtures/switch_table.c` covers all 8 contiguous
cases plus default. The existing `switch_dispatch.c` (5 cases over
range 11, density ≈ 0.45) still hits the chain path, so both
lowerings are exercised by the suite.

## Smaller / deferred work

- **`runtime/lib/cvm_int64.h`**: 64-bit integer struct + helpers
  (add/sub/mul via MULH, shift, compare). Defer until a fixture
  needs it.
- **`runtime/lib/cvm_float64.h`**: software `double` emulation.
  Heavyweight (multi-KB code, slow); defer until a fixture
  genuinely needs `double` precision. Berkeley SoftFloat would
  be a starting point.
- **FSQRT opcode**: useful for vector length / distance. Not in
  v1; can be added later or implemented via Newton-Raphson on i32.
- **Saturating arithmetic** (`ADDS`/`SUBS`): useful for audio
  mixing and blends. Niche; add when a fixture asks.

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
  case values through the generic operand list). f32 codegen
  (FADD/FSUB/FMUL/FDIV/FNEG/FCMP/SIToFP/etc.) sits next to
  LLVMICmp; `cvm_intrin_*` recognition (MULH/MULHU/F2I_SAT_*)
  in the LLVMCall handler.
- `src/cvm.c` — interpreter (threaded + switch paths must stay
  in sync); host-region parsing/carving + `cvm_sys_get_region`
  +`cvm_image_get_region` landed in step K.
- `include/cvm.h` — opcodes, error codes, section types,
  `cvm_image` struct, `cvm_region` struct.
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
  with 5 cases doing different binary ops, plus a default. Range
  11, density ≈ 0.45 — sub-threshold, so this fixture exercises
  the chain lowering. Each arm depends on runtime inputs so
  Clang doesn't fold the switch into a lookup table.
- `tests/fixtures/switch_table.c` — 8 contiguous cases (range 8,
  density 1.0), exercises the jump-table lowering. Same trick of
  runtime-input-dependent arms to keep the IR `switch` intact.
- `tests/fixtures/free_list.c` — strict free-list exercise (per-call
  null checks); stresses both the allocator and the
  `cg_relax_branches` path that needed the stale-cap fix.
- `tests/fixtures/regions.c` — discovers `input` and `fb` regions
  via `cvm_sys_get_region`, reads from one, writes to the other.
  Driven by `tests/test_regions.c`, the only fixture so far that
  needs a custom host harness instead of `test_e2e`.
- `tests/fixtures/mulh.c` — signed/unsigned high-half multiply +
  Q16.16; cases include `100000 × 100000` (high=2, low=0x540BE400)
  and `-1 × -1` to differentiate `MULH` from `MULHU`.
- `tests/fixtures/float_basic.c` — full f32 tour: arithmetic,
  every C comparison operator, int↔float conversions in range,
  saturating F2I on ±Inf / NaN / overflow via `cvm_f2i_sat_*`
  intrinsics, NaN ordered/unordered distinction (`OEQ` vs `UNE`),
  exact `1.5^8` chain, FDIV-by-zero producing ±Inf bit patterns.
  Inputs are threaded through `n` (always 0) to defeat clang -O1
  constant folding.
