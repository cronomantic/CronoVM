# Where I left off

> Short orientation note for the next coding session. Last touched
> **2026-05-10**, end of a session that closed three interacting
> translator bugs and shipped the real `cvm_d_div`. 67/67 ctest
> still green; `e2e_f64_basic` now exercises 12 phases including
> the full division surface (specials, exact ratios, inexact 1/3).
>
> The "translator bug surfaced by cvm_d_div" follow-up from the
> previous round wasn't one bug — it was three, all latent for a
> while and only visible together. See "Closed this round" below
> for the post-mortem.

## Closed this round

1. **`zero_reg` clobbered across calls**. Each function used to
   allocate `zero_reg` via `cg_alloc_reg` right after its params,
   placing it inside the spillable SSA range. Liveness never
   tracked it (no LLVMValueRef), so it was never spilled. A
   callee's body would then write its own SSA values to that
   register number, and on return the caller's `zero_reg` held
   garbage — making subsequent `BEQ/BNE` against `zero_reg`
   silently take the wrong branch. **Fix**: `zero_reg` is now
   pinned at `R253` (`CG_REG_ZERO`), outside the spillable range
   (`CG_MAX_SSA_REG` lowered to 252). Every function's prologue
   still `MOVI R253, 0`, so on return the caller's R253 is still 0
   (the callee just re-set it on entry). Concrete failure that
   surfaced this: in `cvm_d_div`'s 54-iter loop, the back-edge
   `BEQ R93, R13, +1` exited after one iteration because R13
   read 15992 instead of 0 after `cvm_u64_shr` returned.

2. **`Trunc i8` + switch case-constant width mismatch**. `LLVMTrunc`
   was lowered as a plain MOV (no masking), so `trunc i32 0xFF to
   i8` left `cond_reg` holding the full 32-bit source. The switch
   chain form's case constants were loaded with
   `LLVMConstIntGetSExtValue`, which sign-extends `i8 -1` to
   `0xFFFFFFFF`. The `CMP_EQ` then compared `0xFF` vs `0xFFFFFFFF`,
   missed, and fell through to default. **Fix**: `LLVMTrunc` now
   AND-masks the result to the target width; the switch chain form
   uses `GetZExtValue` and masks to the cond operand's width.
   Concrete failure: `cvm_d_from_f32(+inf)` had its `switch i8`
   for `case i8 -1 → Inf branch` miss the case, fall to default
   (normal-float path), and produce `0x47F00000` instead of
   `0x7FF00000`. Latent for ages — only surfaced once `zero_reg`
   was reliable enough that wrong branches weren't masking it.

3. **`cvm_d_div` algorithm overflowed R**. The "54-iter restoring
   division starting with R = a_m" wasn't a correctly-bounded
   restoring divider — R doubles per iteration with at most one
   subtract, and blows past 64 bits around iter 13 when
   `a_m > b_m`. The `div(1, 3) → 0x3FD55555` standalone "verify"
   from the previous session was a coincidence: for that specific
   input R alternates between two small values and never grows.
   **Fix**: rewrote `cvm_d_div` to keep R bounded by `b_m`.
   Pre-shift `a_m` by 1 if `a_m < b_m` (so `a_m ∈ [b_m, 2*b_m)`),
   initialise `q = 1` and `R = a_m - b_m ∈ [0, b_m)`, then 52
   restoring-division iterations. Final q is a 53-bit mantissa
   with the leading 1 at bit 52. Truncate rounding (within 1 ULP
   of IEEE).

   Implementation note: kept the i64 halves as separate `uint32`
   locals (`a_m_lo` / `a_m_hi` / `b_m_*` / `r_*` / `q_*`) and
   inlined the comparisons / subtracts / shifts. Using
   `cvm_i64`-struct ops too liberally on the same struct values
   in a hot loop makes clang -O1 fold the merged-halves access
   into `load i64` + `lshr i64`, which the translator rejects.

## Next session — pinned items

The previously-pinned "1. translator bug" and "2. cvm_d_div" are
both DONE. Remaining backlog is small:

1. **L — Feature flags (`CVM_SEC_REQUIRES`)**. Still
   defer-until-needed. Surface when a host API needs versioning
   (e.g. `renderer.software@1` vs `renderer.gpu@1`).

2. **Embedded-target footprint audit**. Needs a cross-compile
   toolchain (Cortex-M GCC) that doesn't exist in this tree yet.
   Once it does: build `src/cvm.c` for thumbv7m-none-eabi, measure
   .text under `-Os`, decide whether to gate optional opcodes
   behind `#ifdef CVM_ENABLE_*`. Note: the `CVM_NO_STDLIB_FALLBACK`
   gate (below) is now in place, so a thumbv7m build won't pull in
   libc malloc/free unless the host explicitly wants it.

3. **Round-to-nearest in `cvm_d_div`** (optional). Currently
   truncates (round-toward-zero). Within 1 ULP of IEEE, sufficient
   for game math, but worth revisiting if a fixture exposes the
   gap.

## Recently closed

- ~~**`CVM_NO_STDLIB_FALLBACK` guard** in cvm.c~~ — landed.
  Defining `-DCVM_NO_STDLIB_FALLBACK` at build time skips the
  `<stdlib.h>` include and turns a missing allocator hook into
  hard-fail (alloc → NULL, free → no-op). Default builds
  unchanged.
- ~~**Drop the `volatile uint32_t inv` workaround**~~ — landed,
  along with a fix to a typo in the translator's fshr/fshl
  discriminator (`name[6]` → `name[8]`) that the workaround had
  been hiding.

## Current state

67/67 ctest cases pass (64 prior + `e2e_fsqrt` + `e2e_i64_basic` +
`e2e_f64_basic`). Release build clean.
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
| Interpreter | 55 opcodes (HALT, MOVI, MOV, ADD/SUB/MUL, LDW/STW, JMP, BEQ, BNE, SYSCALL, 6×CMP, DIV/DIVU/MOD/MODU, SHL/SHR/SAR, AND/OR/XOR, CALL/RET/CALLR, LDB/STB/LDH/STH, MOVHI, MEMCPY/MEMSET/MEMMOVE, MULH/MULHU, FADD/FSUB/FMUL/FDIV/FNEG, 4×FCMP, F2I_S/F2I_U, I2F_S/I2F_U, JMPR, FSQRT) — see [isa.md](isa.md) |
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

### ~~Runtime libs (`cvm_int64.h`, `cvm_float64.h`) + FSQRT~~  *(done 2026-05-10)*

The "wait-for-a-downstream-fixture" rule was lifted for this drop;
all three ship now.

- **FSQRT opcode** (`0x37`): single-precision square root via the
  host's `sqrtf()`. Mirrors FNEG's encoding shape (single-source,
  single-dest). On hosts without an FPU, `sqrtf` resolves through
  libm/libgcc — same trade-off the FADD/FMUL family already
  documents. Surfaced from `cvm_intrin_fsqrt` (translator inline
  intrinsic, no CALL); users call `cvm_fsqrt(x)`. Required adding
  `<math.h>` to `src/cvm.c` — the only addition to the cvm.c
  include set. Test fixture: `tests/fixtures/fsqrt.c` — sqrt of
  perfect squares + ±Inf + NaN + sqrt(-1) → NaN.

- **`runtime/lib/cvm_int64.h`**: complete u64/i64 surface as a
  `struct {uint32_t lo, hi}` — construction, neg, add/sub
  (carry-propagating), mul (via MUL + MULHU on the cross-product),
  shl/shr/sar (with the funnel-shift workaround below), comparisons
  signed AND unsigned, bitwise. Multi-precision helpers carry
  `__attribute__((noinline))` for the same reason `cvm_alloc.h`
  helpers do — keeps the user's per-function register footprint
  inside the translator's 254-register budget. Test fixture:
  `tests/fixtures/i64_basic.c` — eight phases covering construction,
  carry-across-32-bit-boundary on add/sub, full-precision mul,
  every shift slice, signed vs unsigned comparison, bitwise.

- **`runtime/lib/cvm_float64.h`**: software-emulated IEEE 754
  binary64 with explicit, documented trade-offs:
  - **Truncate rounding** (round-toward-zero), not RNE — results
    within 1 ULP of IEEE-correct.
  - **Flush-to-zero subnormals** on input AND output.
  - **NaN propagation** to canonical quiet NaN (`CVM_D_NAN`); NaN
    payloads are not preserved.
  - **Division stubbed** to return NaN — long-division on 53-bit
    mantissas is the one remaining piece of work in this header.
  Mantissa arithmetic uses `cvm_int64.h` primitives; mul does the
  full 53×53→106-bit schoolbook product via four MULHU partial
  products. Test fixture `tests/fixtures/f64_basic.c` is split
  into 10 phase functions (each `__attribute__((noinline))`) so
  the per-phase SSA register count stays within budget — split
  came from the fixture initially busting 254 regs as a single
  function.

Two translator additions piggybacked on this step:

1. **`fcmp ord` / `fcmp uno` lowering** in `LLVMFCmp`. clang -O1
   InstCombine folds `x == x` and isnan-style bit-pattern checks
   to `fcmp ord` / `fcmp uno`, which the translator previously
   rejected with "predicate 7 not in supported subset". Lowered as
   three opcodes each:
   - `ord(a, b)` → `FCMP_EQ t1, a, a; FCMP_EQ t2, b, b; AND dst, t1, t2`
   - `uno(a, b)` → `FCMP_NE t1, a, a; FCMP_NE t2, b, b; OR  dst, t1, t2`
   IEEE NaN-not-equals-itself makes the lowering correct without
   needing a dedicated opcode. The fix unblocked f64_basic.c's
   NaN checks too.

2. **Funnel-shift workaround in cvm_int64.h**, NOT translator
   support. clang -O1 folds the canonical multi-precision shift
   pattern `(x.hi << n) | (x.lo >> (32-n))` to
   `llvm.fshl.i32` / `llvm.fshr.i32`, which the translator
   doesn't handle. Worked around in the header by routing the
   inverse shift amount through a `volatile uint32_t inv` local —
   the resulting load/store breaks clang's SSA chain and prevents
   the fold. One extra mem op per shift call; invisible vs. the
   shift cost itself. Translator-side fshl/fshr support is a
   reasonable follow-up if any other code triggers the same
   pattern, but for now the workaround is contained to
   cvm_int64.h.

### ~~`llvm.fshl.i32` / `llvm.fshr.i32` translator lowering~~  *(done 2026-05-10)*

Shipped in the same round that prototyped `cvm_d_div`. clang -O1
folds any `(a << n) | (b >> (32 - n))` pattern to these intrinsics,
so without lowering the translator rejected every multi-precision
shift in cvm_d_div's body.

Lowered to 9 opcodes including a c==0 fixup:

```text
SHL/SHR a_sh, a, c        ; forward shift on the leading half
MOVI    scratch, 32
SUB     inv, scratch, c   ; inv = 32 - c
SHR/SHL b_sh, b, inv      ; inverse shift on the other half
OR      dst, a_sh, b_sh   ; combine
MOVI    scratch, 31
AND     c2, c, scratch    ; c2 = c & 31
BNE     c2, zero, +1      ; skip fixup when c2 != 0
MOV     dst, a (or b)     ; identity when c2 == 0
```

The c==0 fixup is necessary because the VM's SHR/SHL mask the
shift amount to its low 5 bits — `b >> (32 - 0) = b >> 32` becomes
`b >> 0 = b` rather than 0, contaminating the OR. Cost: 9 opcodes
per fshl/fshr call; cheap because most use sites are outside hot
loops.

The cvm_int64.h shifts no longer need the `volatile uint32_t inv`
workaround now that the translator lowers fshl/fshr correctly. The
workaround is still in place — harmless, but redundant; drop it
when next touching that file.

Lives in `tools/translator/translator.c` LLVMCall handler, right
after the `llvm.abs.i32` block, before the `cvm_intrin_*` block.

### Other deferred items

- **Saturating arithmetic** (`ADDS`/`SUBS`): useful for audio
  mixing and blends. Niche; add when a fixture asks.

## Distribution / consumer surface  *(done 2026-05-10)*

The VM is now consumable as a CMake dependency. `add_subdirectory()`
wires up the `cvm` library plus the `cvm-cc` and `cvm-translate`
executable targets; `examples/embedder/` is a standalone mini-project
that exercises the full pattern (configure + build + run) and is
asserted by ctest. CronoVM's own CMakeLists captures
`CVM_SOURCE_ROOT` so subdir paths don't leak through `CMAKE_SOURCE_DIR`
when the parent project takes over the top-level role.

`project(cronovm VERSION 0.1.0)` exposes the library version both via
the runtime API (`cvm_version_string()`, `cvm_version_number()`) and
via compile-time macros (`CVM_VERSION_MAJOR/MINOR/PATCH`,
`CVM_VERSION_STRING`). Distinct from `CVM_VERSION_1_0` which names the
binary-format magic.

### ~~`cmake --install` + `find_package(CronoVM)`~~  *(done 2026-05-10)*

Both halves of the distribution story now ship. `cmake --install`
populates a standard GNUInstallDirs layout:

- `<prefix>/bin/`               cvm-cc, cvm-translate
- `<prefix>/include/cvm.h`      public header
- `<prefix>/lib/libcvm.a`       static library
- `<prefix>/lib/cmake/CronoVM/` `CronoVMConfig.cmake`,
                                `CronoVMTargets.cmake`,
                                `CronoVMConfigVersion.cmake`
- `<prefix>/share/cronovm/runtime/lib/` `cvm_intrin.h`, `cvm_alloc.h`

Consumers do `find_package(CronoVM 0.1 REQUIRED)`, then link against
`cronovm::cvm` and use `$<TARGET_FILE:cronovm::cvm-cc>` to drive
.c → .bin compilation. `cronovm::cvm-translate` is exported only
when the build host had llvm-config; consumers that need it should
guard with `if(TARGET cronovm::cvm-translate)`.

Three notable choices:

1. **Install gated on `CRONOVM_INSTALL`**, defaulted ON only when
   CronoVM is the top-level project (`CMAKE_SOURCE_DIR STREQUAL
   CVM_SOURCE_ROOT`). Parents that pull CronoVM in via
   `add_subdirectory()` shouldn't silently inherit our install rules
   (they'd clash with the parent's own packaging).
2. **`target_include_directories` uses BUILD/INSTALL_INTERFACE**.
   `$<BUILD_INTERFACE:${CVM_SOURCE_ROOT}/include>` for in-tree
   consumers, `$<INSTALL_INTERFACE:include>` for the exported
   target. Without the split, `install(EXPORT)` refuses to write
   the absolute build-tree path into the exported targets file.
3. **cvm-cc resolves runtime headers at runtime**, not just at
   build time. The CMake-baked `CVM_RUNTIME_DIR` points at
   `${CVM_SOURCE_ROOT}/runtime/lib` (build-tree); installed copies
   wouldn't otherwise find their headers. New
   `find_install_runtime_dir(argv0)` probes
   `<exedir>/../share/cronovm/runtime/lib/cvm_intrin.h`; on hit, it
   wins over the bake. Order: `--runtime-dir` > install probe >
   bake. Same precedence shape as cvm-translate discovery.

Test fixtures: `examples/installed_consumer/` is a stand-alone
CMake project that does `find_package(CronoVM)` and reuses the
embedder's `host.c` + `game.c` verbatim. CronoVM's own ctest drives
the full chain via four sequential tests (`installed_install` →
`installed_configure` → `installed_build` → `installed_run`,
chained via `FIXTURES_SETUP` / `FIXTURES_REQUIRED`). The install
target dir is `${CMAKE_BINARY_DIR}/install_prefix/`, populated by
`cmake --install ${CMAKE_BINARY_DIR} --prefix ...`.

## Embedded-specific concerns

### ~~Pluggable allocator at load time~~  *(done 2026-05-10)*

`cvm_load_ex(bytes, len, out, &allocator)` accepts a `cvm_allocator_t
{alloc_fn, free_fn, user_data}`. Either function pointer NULL means
"fall through to stdlib"; passing `NULL` for the whole struct is
identical to calling the original `cvm_load`. The image stashes the
allocator so `cvm_image_free` releases blocks via the matching free.
Test `e2e_allocator` (`tests/test_allocator.c`) drives a counting
allocator and asserts every block reaches a paired free.

The `cvm_load_with_workspace` variant (fixed buffer for all
load-time allocations) wasn't shipped and isn't planned — you can
write a 50-line bump-from-buffer allocator on top of the existing
hooks. Document the recipe when an embedded fixture needs it.

### ~~libc-freestanding audit~~  *(done 2026-05-10)*

Confirmed by grep on `src/cvm.c`. The only stdlib symbols pulled in
are:

- `<stdlib.h>`: `malloc` / `free` — both routed through
  `cvm_int_alloc` / `cvm_int_free`, which fall through only when the
  allocator hook is NULL. An embedded host that always passes a
  custom allocator never invokes the stdlib path.
- `<string.h>`: `memcpy`, `memset`, `memcmp`, `memchr`, `memmove`,
  `strcmp`, `strncmp`. All in the C-standard freestanding subset.

Hot loop uses only `memcpy` / `memset` / `memmove` (the block memory
opcodes plus the f32 bitcast helpers). No `printf`, `errno`,
`assert`, `exit`, file I/O, or env access anywhere.

If a future build wants to avoid the stdlib `malloc`/`free` fallback
entirely, that's a one-line `#ifdef CVM_NO_STDLIB_FALLBACK` guard
around the fallback bodies of `cvm_int_alloc` / `cvm_int_free`.
Defer until a target asks.

### Interpreter footprint audit

`src/cvm.c` should compile under 16 KiB on Cortex-M with `-Os`.
Measure when there's a real target; if it grows too much, guard
optional opcodes behind compile-time `#ifdef CVM_ENABLE_*` flags so
a minimal build can drop them. Not done yet — needs a cross-compile
toolchain that doesn't yet exist in this tree.

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
