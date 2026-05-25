# Changelog

All notable changes to CronoVM are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project adheres to [Semantic Versioning](https://semver.org/) — once
1.0 is reached. Pre-1.0 releases may break compatibility on any minor
version bump; breaks are called out explicitly under **Breaking**.

## [Unreleased]

### Added

- **`setjmp`/`longjmp` (opcodes `SETJMP` 0x3A, `LONGJMP` 0x3B).** Real non-local
  jumps. A `jmp_buf` captures `{resume pc, SP, dest reg}` in the heap; `longjmp`
  restores SP+pc and writes the value to the saved dest register, resuming right
  after `setjmp` (0 on the direct call, the longjmp value — mapped to 1 if 0 —
  on return). Cheap because the register file/pc are interpreter locals and the
  call stack lives in the heap, so the convention's existing caller-saved spills
  cover the live state across the jump. The translator lowers `setjmp`/`longjmp`
  calls to the opcodes (no library body). New e2e fixture `setjmp.c`. (First
  exercised by a C program using `setjmp`/`longjmp` for error recovery.)

- **More intrinsic lowerings.** The translator now lowers `llvm.fabs.f32`
  (AND `0x7FFFFFFF`), `llvm.copysign.f32`, `llvm.bswap.i16` / `llvm.bswap.i32`
  (shift/mask/or), and `llvm.cttz.i32` (count-trailing-zeros loop, like the
  existing `ctlz`). These previously errored as "unsupported intrinsic"; clang
  emits them from `fabsf`/`copysignf`, byte-swap idioms, and power-of-two `log2`.

- **f64 / f32 global initialisers.** `serialize_constant` writes a `double`
  `ConstantFP` global as its native 8-byte IEEE-754 little-endian image (f32 as
  4 bytes), matching how the soft-float runtime reads doubles from memory.
  Previously only f32 constants were serialised, so any `double` global was
  rejected "unsupported initializer shape".

- **`cvm-cc -D<macro>[=val]`.** The driver now accepts and forwards `-D` to
  clang (was rejected as an unknown option), so a build can predefine macros —
  e.g. selecting an optional libc feature.

- **Integrity seal section (`CVM_SEC_SEAL`).** `cvm-translate --seal` appends a
  12-byte seal — magic `'C','R','M','1'`, version, and a CRC-32 of all preceding
  file bytes — as the last section. New public `cvm_crc32()` and
  `cvm_seal_check()` (1 = sealed & valid, 0 = unsealed, -1 = corrupt). The loader
  ignores the section; a host verifies it before running. Lets a launcher both
  identify valid cartridges (cheap magic via `cvm_peek_section`) and reject
  corrupt/tampered ones (CRC at load).

- **Source locations (`file:line`) in translator diagnostics.** `cvm-cc` now
  compiles each TU with `-gline-tables-only`, and the translator reads the
  current instruction's debug location, so every rejection prints
  `file:line: in 'func': message` instead of just the function name. No `.bin`
  impact (debug metadata is dropped when VM bytecode is emitted); minimal
  bitcode/compile overhead. Unsupported `llvm.*` intrinsics with no dedicated
  lowering now say "unsupported intrinsic 'X'" rather than the misleading
  "extern is not supported".

- **64-bit calling convention (phase 3 — completes i64/f64).** `i64`/`double`
  may now be function arguments and return values, across real calls. The
  convention is word-based and generalises the scalar one (no-64-bit
  signatures get byte-identical code): arguments fill word positions, a 64-bit
  value taking two consecutive words (lo, hi) — words 0–7 in `R0..R7`, the rest
  on the stack, straddling allowed; a 64-bit return comes back in `R0`(lo):`R1`
  (hi). The callee prologue reads each argument word into its param's home
  (register for a scalar, two frame slots for a 64-bit param); `ret` of a wide
  value loads its slots into R0:R1; a wide-returning call stores R0:R1 into the
  result's slots. Distinct from the soft-runtime helpers' `sret` ABI (those
  stay void + i32/pointer). With this the i64/f64 legaliser is complete — a
  stock `double`/`long long` C interface translates and runs. New e2e fixture
  `abi64` (i64/f64 params+returns, mixed scalar/wide args, five-i64 args that
  straddle to the stack, nested wide calls). Removed the obsolete
  `translator_reject_double` test (double is no longer rejected). Full suite
  120/120; DOOM rebuilds unchanged.
- **64-bit `phi`, `select`, variable shifts, and `sqrt` (completes the in-body
  64-bit surface).** Loop-carried `i64`/`double` `phi`s now work (the phi-move
  pass emits a lo and a hi word-move per wide phi, reusing the scalar parallel-
  copy / conflict-staging path). `select` of a wide value lowers to two
  parallel word-MOVs. Variable-amount `i64` `shl`/`lshr`/`ashr` lower to a soft
  runtime call (`__cvm_{shl,shr,sar}64`; constant amounts stay inline).
  `llvm.sqrt.f64` (and the plain `sqrt` libcall clang emits without
  `-fno-math-errno`) lowers to `__cvm_fsqrt` (Newton–Raphson, exponent-halving
  seed). With this, the i64 and f64 surfaces are complete inside a function
  body; the only remaining gap is the 64-bit calling convention. New e2e
  fixture `i64f64_gaps`.
- **`i64` multiply / divide / remainder (completes the i64 legaliser).**
  `i64` `mul` is lowered inline (`lo = al*bl`, `hi = mulhu(al,bl) + al*bh +
  ah*bl`, via `MUL`/`MULHU`). `udiv`/`sdiv`/`urem`/`srem` lower to a soft
  runtime CALL into the new `runtime/lib/cvm_int64_rt.c` (`__cvm_{u,s}div64` /
  `__cvm_{u,s}mod64`, a ~64-iteration restoring long division on the `cvm_i64`
  struct, sret return — same ABI path as the f64 helpers); cvm-cc auto-links it
  when a module uses i64 div/rem (the `--probe-runtime` exit code is now a
  bitmask: `10` f64, `20` i64, `30` both). Also: `freeze` is now accepted
  (lowered as identity — clang emits it to block poison propagation, e.g.
  around the div operands), and `llvm.experimental.noalias.scope.decl` is
  dropped as a no-op (clang emits it when inlining the restrict-qualified
  runtime helpers). New e2e fixture `i64_muldiv`. The remaining i64 gaps are
  `phi`/`select`, variable-amount shifts, and the 64-bit calling convention.
- **cvm-cc auto-links the soft-float runtime.** When a program uses `double`,
  cvm-cc now links `cvm_float64_rt.c` automatically — `double` "just works"
  without hand-listing the runtime TU. It runs the new `cvm-translate
  --probe-runtime` on each compiled module (which exits `10`/`CVM_PROBE_F64`
  when a `double` appears, `0` otherwise) and adds the runtime to the link set
  only when needed, so integer-only programs link no soft-float code. Skipped
  if the user already listed the runtime TU (no duplicate-symbol error).
- **`f64` (double) legalisation in the translator (phase 2).** Native `double`
  is no longer rejected. It reuses the i64 two-slot storage, but since f64 ops
  can't be open-coded, each is lowered to a soft-float runtime CALL into the new
  `runtime/lib/cvm_float64_rt.c` (`__cvm_f*` external wrappers over
  `cvm_float64.h`); cvm-cc links that TU into any module using `double`. Inline
  exceptions: `fneg`/`fabs` (sign-bit flip), constants, load/store. The calls
  reuse clang's existing `cvm_f64` ABI (two i32 words per arg, `sret` for a
  64-bit return) — which the translator already handled — so no new ABI work;
  the result slot's address is passed as the sret pointer. Each f64-runtime-call
  is registered as a caller-save spill point so the existing liveness-narrowed
  save/restore protects live registers across it. Lowered: fadd/fsub/fmul/fdiv,
  fcmp (all 16 predicates, one call + optional 0/1 negation), sitofp/uitofp/
  fptosi/fptoui/fpext/fptrunc, fneg, double const/load/store, and the
  `llvm.fmuladd.f64`/`llvm.fma.f64` (→ fmul+fadd, intermediate in the result
  slot) / `llvm.fabs.f64` / `llvm.copysign.f64` intrinsics — so default-O1
  `double` code (which clang contracts `a*b±c` into `fmuladd`) translates
  without `-ffp-contract=off`. New e2e fixtures `f64_slice` and `f64_fma`,
  linked via cvm-cc against the runtime. **Not yet** lowered (clear error):
  other `llvm.*.f64` math intrinsics (e.g. `llvm.sqrt.f64`), double
  `phi`/`select`, and double across a function boundary (64-bit calling
  convention, a later phase). See `docs/translator.md` → "f64 (double)
  legalisation".
- **`i64` legalisation in the translator (phase 1).** Native 64-bit integers
  are no longer rejected: code that uses `long long` inside a function body now
  translates without hand-writing `cvm_int64.h`. Each `i64` SSA value lives in
  **two consecutive value-spill slots** (lo/hi words) and every `i64` operation
  is lowered to explicit 32-bit word arithmetic — no register ever holds 64
  bits, so the ISA stays 32-bit (no new opcodes). Lowered: `sext`, `zext`,
  `load`, `store` (incl. constant `i64` stores, the old struct-zero-init
  special case), `add`/`sub` (carry/borrow via `CMP_LTU`), `and`/`or`/`xor`,
  constant-amount `shl`/`lshr`/`ashr`, `trunc`, and `icmp` (eq/ne + all ordered
  predicates). New e2e fixture `i64_slice` (sext + i64 const store + volatile
  i64 load + add-with-carry + lshr 32 + trunc). **Not yet** legalised (clear
  error): `i64` `mul`/`div`/`rem`, `phi`/`select`, variable-amount shifts, and
  `i64` across a function boundary (args/returns — the 64-bit calling
  convention is a later phase). `f64` is a follow-on that reuses the two-slot
  machinery but lowers to `cvm_float64.h` soft-float calls. See
  `docs/translator.md` → "64-bit integer legalisation".
- **`QDIV6432` opcode (0x39) — general 64/32 unsigned divide.** Computes
  `R[A] = (u32)(((((u64)(u32)R[A]) << 32) | (u32)R[B]) / (u32)R[C])` in one
  host op, trapping on `R[C]==0` like `DIV`/`DIVU`. `A` is both the dividend's
  high word and the destination (a tied operand), which lets a 3-source/1-dest
  divide fit the 3-register encoding. Where `QDIV1616`'s numerator is fixed to
  `R[B]<<16`, this takes an arbitrary 64-bit dividend in two halves, so a
  software 64-bit long division collapses to a single instruction. Surfaced
  from `cvm_intrin_qdiv_64_32(hi, lo, divisor)` (`runtime/lib/cvm_intrin.h`);
  the translator stages it as `MOV tmp,hi; QDIV6432 tmp,lo,div; MOV dst,tmp`
  so the divide can't be corrupted by the destination aliasing an input. New
  e2e fixture `qdiv6432` (6 phases). Motivation: DOOM's `CVM_CrossDiv`
  (a 32×32 product difference / divisor) was a 64-iteration software loop at
  ~12.8% of in-level interpreter time; routing it through `QDIV6432` removed it
  from the hot path (−12.4% total in-level instructions on E1M1).

- **Optional interpreter self-time profiler (`-DCVM_PROFILE`).** Building
  `src/cvm.c` with `CVM_PROFILE` defined makes `cvm_exec_at` attribute every
  executed instruction to the currently-running FUNCS index, tracked with a
  shadow call stack kept in step with `CALL`/`CALLR`/`RET`. The result is
  per-function *self* instruction counts (exclusive of callees) — where the
  interpreter actually spends cycles. Surface: `cvm_prof_counts[]` /
  `cvm_prof_len` / `cvm_prof_total`, `cvm_profile_reset(func_count)`, plus
  caller attribution (`cvm_prof_watch = <fid>` → `cvm_prof_caller[caller]++`
  on each call into that fid). Zero overhead when `CVM_PROFILE` is undefined
  (the `PROF_*` hooks expand to no-ops), so the default build is unchanged.
  Pairs with the new `CVM_SYMS` sidecar to resolve indices to names; Cronopio's
  `headless_prof` is a ready-made driver.
- **`CVM_SYMS` symbol sidecar from the translator.** With the `CVM_SYMS`
  environment variable set, `cvm-translate` writes `<out>.bin.sym` —
  `fid<TAB>entry_offset<TAB>name` per user function, where `fid` is the runtime
  FUNCS index (the `+1` for the reserved `FUNCS[0]` null-fn slot is applied).
  Lets index-only tooling (e.g. the `CVM_PROFILE` profiler) map back to source
  names. Off by default; the loader never reads it. See [docs/translator.md].
- **`CVM_SEC_ROM` (section type 10) — read-only cartridge data.** A binary
  can bake an arbitrary byte blob (e.g. a game WAD) into the `.bin`; the
  loader copies it into the heap (layout `DATA | BSS | REGIONS | ROM |
  RESERVE`, before RESERVE so `cvm_sys_heap_start` is unchanged) and
  exposes its offset/size on `cvm_image` (`rom_offset`/`rom_size`) and to
  the program via two new auto-bound built-in syscalls `cvm_sys_rom_base`
  / `cvm_sys_rom_size`. Read-only by convention (the VM enforces only heap
  bounds). The translator/`cvm-cc` gain a `--rom=FILE` flag that appends
  the file's bytes as the section. Motivated by Cronopio needing to ship a
  multi-MB WAD without compiling a giant C array. New ctest `rom`
  (`tests/test_rom.c`) covers load, byte-exact heap placement, the built-in
  syscalls, and the no-ROM case.

### Added

- **Constant `store i64` split into two word stores.** clang lowers
  struct/array zero-init and small copies with 8-byte `store i64` chunks
  even on a 32-bit target (e.g. zeroing a vec4). The VM has no 64-bit
  register, but a *constant* i64 store splits cleanly into `STW` at addr
  and addr+4; a dynamic i64 store (which the type subset can't produce) is
  rejected. New ctest `e2e_struct_init` (`tests/fixtures/struct_init.c`).

- **`float` loads/stores and `llvm.fmuladd.f32` lowering.** Storing or
  loading an `f32` to memory (a float in a global/struct/array) was rejected
  ("store: unsupported value type") even though f32 shares the 32-bit
  register file — it now lowers to `STW`/`LDW`. And `a*b + c` on floats,
  which clang folds to `llvm.fmuladd.f32` under the default fp-contract, now
  lowers to `FMUL` + `FADD`. Together these unblock float vector/matrix maths
  in memory — the basis of the Cronopio 3D pipeline header. New ctest
  `e2e_float_mem` (`tests/fixtures/float_mem.c`).

### Fixed

- **`i1` (boolean) constants now materialise zero-extended (0/1), not
  sign-extended.** `cg_reg_for` loaded constant operands with
  `LLVMConstIntGetSExtValue`, so `i1 true` became -1. clang lowers
  `cond ? 0 : 1` on a reused boolean to `zext(xor i1 %cond, true)`, and with
  `true` = -1 the xor became a full-width NOT (`!1` → -2, `!0` → -1) — a silent
  boolean miscompile (it dropped DOOM's keyboard events, whose event type is
  `down ? ev_keydown : ev_keyup`). i1 constants now use the 0/1 zero-extended
  value; wider integers keep sign-extension (normalised at the use). Regression
  test `tests/fixtures/xor_i1.c` → `e2e_xor_i1`.

- **Transient registers are now recycled per instruction — fixes spurious
  "ran out of registers" in constant-heavy functions.** `cg_reg_for`
  materialises constants, globals and constant-expressions into registers
  above `ssa_reg_high` and never caches them across instructions, but
  `next_reg` was only reset per function, so it grew monotonically through
  emission. A function issuing many calls with immediate arguments (e.g. a
  console frame that fires dozens of graphics syscalls) exhausted the 245-
  register file even though no two of those scratch values were ever live
  at once. Codegen now resets `next_reg` to `ssa_reg_high` at the start of
  each instruction; an instruction's SSA *result* always lands in a
  pre-allocated register (< `ssa_reg_high`), so the scratch above it is dead
  once the instruction finishes. New ctest `e2e_reg_pressure`
  (`tests/fixtures/reg_pressure.c`, 120 constant-addressed global stores).

- **`llvm.memset/​memcpy/​memmove.*.i64` accepted.** clang emits the i64-length
  variant (not just i32) depending on flags — e.g. zeroing a large static or
  filling a struct in hosted mode. The VM is 32-bit (a length over 4 GiB is
  impossible), so the translator now takes a constant i64 length that fits in
  32 bits; only a dynamic i64 length (which the type subset can't produce) is
  rejected. `cvm-cc` additionally now passes `-ffreestanding`, matching the
  fixture build and keeping clang on the i32-length intrinsics in the first
  place.

- **Constant-expression GEP/cast operands now lower correctly.** clang -O1
  emits a `ConstantExpr` getelementptr as the pointer operand whenever code
  touches a global at a fixed offset (`store v, ptr getelementptr(@g, k)`)
  — common for any function that writes more than one element of a global
  array, or reads/writes a struct field. `cg_reg_for` had no case for
  constant expressions and bailed with "operand has no register assigned",
  rejecting otherwise-valid carts (it surfaced in a Cronopio cart that
  initialised a colormap array). Now constant GEPs fold to base + static
  offset (via the new `cg_const_gep_offset`, mirroring the GEP-instruction
  index walk) and `bitcast`/`addrspacecast`/`int<->ptr` constant casts
  reinterpret the operand's register. New ctest `e2e_const_gep`
  (`tests/fixtures/const_gep.c`) stores to fixed indices of an external
  global and returns 2*n+3.

- **FUNCS section now emitted when a function's address is taken even if
  it is never internally called.** Previously the translator gated FUNCS
  emission on `has_calls` (did codegen emit a CALL/CALLR?). A binary that
  hands a function pointer to the host via a syscall — for the host to
  invoke later through `cvm_call` — got a pointer value (`fidx + 1`) that
  indexed an absent table, so `cvm_call` trapped with `CVM_E_BAD_FUNCS`.
  Codegen now sets a `funcs_referenced` flag wherever a function value is
  materialised as an operand, and FUNCS is emitted when
  `has_calls || funcs_referenced`. New regression test `e2e_fnptr_export`
  (`tests/fixtures/fnptr_export.c` + `tests/test_fnptr_export.c`) exercises
  exactly this: a callback registered through a syscall, never CALLed
  internally, then invoked by the host via `cvm_call`. This is the usage
  pattern the Cronopio console relies on for its per-frame callback.

### Added

- **`cvm_call` host API** — re-enter the VM at a function registered in
  the FUNCS table without bouncing through the image's entry point.
  Driven by Cronopio's frame-callback model (a fantasy console host that
  needs to invoke a cart-registered `frame()` every 1/60 s without
  reloading the image each tick). Signature:
  `int cvm_call(struct cvm_image *img, uint32_t fn_index,
                 const int32_t *args, uint32_t arg_count,
                 int32_t *return_value);`
  Internally factors the previous `cvm_run_args` body into a static
  `cvm_exec_at(img, start_pc, …)` helper; `cvm_run_args` now forwards
  to it with `img->entry` as the start PC and `cvm_call` does the same
  with `FUNCS[fn_index]`. Each call gets a fresh register file; only
  the heap persists between calls. Error surface piggy-backs on the
  existing codes — `CVM_E_BAD_FUNCS` (no FUNCS section),
  `CVM_E_BAD_FUNC_INDEX` (index out of range), `CVM_E_NULL_FUNC_PTR`
  (index 0). New ctest entry `cvm_call` (`tests/test_cvm_call.c`)
  exercises both the success and error surface end-to-end on hand-
  assembled blobs.

- **rv32imc cross-compile sanity build.** New
  `cmake/toolchains/riscv32-none-elf.cmake` targets 32-bit
  RISC-V with integer multiply + compressed instructions
  (rv32imc, soft-float ABI ilp32) — the embedded baseline that
  covers ESP32-C3, CH32V103/203, GD32VF103, BL602, etc. Uses
  `riscv64-unknown-elf-gcc` (the Ubuntu package targets all
  RISC-V variants from one binary) with
  `--specs=picolibc.specs` for headers, since the GCC
  package on Ubuntu doesn't bundle a libc (unlike
  `gcc-arm-none-eabi`).
  Footprint on rv32imc (`-Os`, `-DCVM_NO_STDLIB_FALLBACK`):
  **7005 bytes total** (`.text` 5198, `.rodata` 1180, strings
  615) — `.text` is +24 % vs Cortex-M3 because RV32 instructions
  are mostly 4 B (the C extension shortens common ops to 2 B but
  not all of them), while Thumb-2 is denser at 2 B for most
  arithmetic. 20 undefined symbols, all in the existing
  allowlist. The allowlist gained 16 compiler-rt soft-float
  helpers (`__addsf3`, `__subsf3`, `__mulsf3`, `__divsf3`,
  `__negsf2`, six SF compare helpers, four SF↔int conversions);
  ARM toolchains never saw these because GCC renames them to
  `__aeabi_f*` under the EABI wildcard. `linux-cortex-m-sanity`
  CI job now matrices over `{thumbv7m, thumbv6m, rv32imc}`.

- **thumbv6m-none-eabi cross-compile sanity build.** New
  `cmake/toolchains/thumbv6m-none-eabi.cmake` targets Cortex-M0 /
  M0+ / RP2040 (`-mcpu=cortex-m0 -mthumb -mfloat-abi=soft`).
  Same allowlist as thumbv7m; M0 additionally pulls in
  `__aeabi_uidiv*`, `__aeabi_lmul`, and `__gnu_thumb1_case_uhi`
  (libgcc helper for Thumb-1 switch-jump-table dispatch — M0 has
  no efficient table-dispatch instruction). Added a
  `__gnu_thumb1_*` wildcard to the allowlist to cover the latter
  family. Footprint on Cortex-M0 (`-Os`,
  `-DCVM_NO_STDLIB_FALLBACK`): **6125 bytes** (`.text` 4404, plus
  the same rodata/strings as M3) — +224 B vs M3, +3.8 %. CI job
  `linux-cortex-m-sanity` now matrices over `{thumbv7m,
  thumbv6m}`.

### Changed

- **`cvm_d_div` rounds to nearest-even.** Previously truncated
  (round-toward-zero, within 1 ULP of IEEE). Now bit-exact against
  hardware IEEE 754 binary64 for ratios that fit in 53 bits.
  Implementation: the restoring-division loop runs one extra
  iteration to produce a guard bit; the residual remainder
  supplies the sticky bit; round-up fires when `G && (S || LSB)`
  with carry-out into the exponent on a 0x1FFFFF…FF + 1 boundary.
  `cvm_d_add` / `cvm_d_sub` / `cvm_d_mul` still truncate — div is
  the operation where bit-exactness mattered most for the fixtures
  we care about, and the others would each need their own
  guard/sticky plumbing for full RNE. Documented in the header's
  trade-offs section.

### Notes

- **Embedded footprint measured.** Cross-built `libcvm.a` for
  `thumbv7m-none-eabi` (Cortex-M3-class, soft-float) with
  `-Os` + `-DCVM_NO_STDLIB_FALLBACK`: **5901 bytes total** —
  4180 B `.text`, 1024 B dispatch jump table, 577 B error
  strings, 120 B switch tables, 0 `.data`, 0 `.bss`.
  ~9 % of a 64 KiB STM32F103 flash.
  Compile-time opcode gating (`#ifdef CVM_ENABLE_*`) was on
  the roadmap but the measurement retired it — removing an
  opcode saves ~34 B in the handler body but cannot shrink
  the dispatch table, which indexes the full 0–255 opcode
  space regardless. The full ISA stays in every build.

## [0.1.0] — 2026-05-11

First public release. Closes the foundation phase: the VM runs C
programs compiled by clang via the bitcode translator end-to-end,
with calling convention, memory model, allocator hooks, host memory
regions, full `f32` and (software-emulated) `i64`/`f64` arithmetic,
and a cross-compile sanity build for Cortex-M targets.

### Added

#### Toolchain

- `cvm-cc` — single-command driver wrapping clang + cvm-translate.
  Compiles `.c → .bin` with pass-through flags for `--heap-reserve`,
  `--stack-reserve`, `--region`, `-I`, `-O<n>`. Default `-O1` (matches
  what mem2reg promotion needs). `.bc` input bypasses clang.
- `cvm-translate` — bitcode translator. Reads LLVM bitcode (binary
  `.bc`), validates against the CronoVM IR subset, emits a CronoVM
  `.bin`. Rejects vectors, atomics, exceptions, threads, and types
  outside i1/i8/i16/i32/f32 + pointer with a clear error message.

#### Interpreter (55 opcodes)

- Integer arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `DIVU`, `MOD`,
  `MODU`, `AND`, `OR`, `XOR`, `SHL`, `SHR`, `SAR`, `MULH`, `MULHU`.
- Float arithmetic: `FADD`, `FSUB`, `FMUL`, `FDIV`, `FNEG`, `FSQRT`,
  four `FCMP_*` predicates, `F2I_S`/`F2I_U` (saturating), `I2F_S`/
  `I2F_U`.
- Control flow: `JMP`, `BEQ`, `BNE`, six `CMP_*`, `CALL`, `RET`,
  `CALLR`, `JMPR`, `SYSCALL`, `HALT`.
- Memory: `LDW`/`STW`, `LDB`/`STB`, `LDH`/`STH`,
  `MEMCPY`/`MEMSET`/`MEMMOVE`.
- Constants: `MOVI`, `MOV`, `MOVHI` (arbitrary 32-bit immediates via
  `MOVI + MOVHI`).

The interpreter is a single C source (`src/cvm.c`) with
computed-goto dispatch under GCC/Clang and a switch fallback for
MSVC. `f32` shares the integer register file (bitcast is a no-op
`MOV`). `i64` and `f64` are rejected at the ISA level — code that
needs them uses the runtime headers below.

#### Binary format

- Header + 9 section types (`CODE`, `DATA`, `BSS`, `IMPORTS`,
  `DEBUG`, `HEAP_RESERVE`, `STACK_RESERVE`, `FUNCS`, `HOST_REGION`).
- Fixed-width 32-bit instructions, little-endian.
- `FUNCS[0]` reserved as a NULL-fn-ptr trap.
- Format documented in [docs/format.md](docs/format.md).

#### Runtime headers (drop into user code)

- `cvm_alloc.h` — header-only first-fit free-list allocator;
  one-word block header, splits on alloc, forward-coalesces on free.
- `cvm_intrin.h` — wrappers for `MULH`/`MULHU`/`FSQRT`,
  fixed-point Q16.16 multiply, and saturating `F2I_S`/`F2I_U`
  (`cvm_f2i_sat_s`/`cvm_f2i_sat_u` — guarantees the opcode actually
  runs even when clang -O1 would fold the C cast as UB).
- `cvm_int64.h` — complete `u64`/`i64` surface as `struct {uint32_t
  lo, hi}`. Carry-propagating add/sub, full-precision mul (via
  `MUL` + `MULHU` cross-products), shifts (lowered through
  `llvm.fshl`/`fshr`), signed and unsigned comparisons.
- `cvm_float64.h` — software-emulated IEEE 754 binary64.
  Truncate rounding (within 1 ULP of IEEE), flush-to-zero
  subnormals, NaN propagation to canonical quiet NaN (payloads
  not preserved). `cvm_d_div` is a 52-iter restoring divider.

#### Host API

- `cvm_load(bytes, len, &img)` — original entry, malloc/free
  fallback for image-internal allocations.
- `cvm_load_ex(bytes, len, &img, &allocator)` — pluggable
  allocator: `cvm_allocator_t {alloc_fn, free_fn, user_data}`.
  Either function pointer NULL → fall through to stdlib; whole
  struct NULL → equivalent to `cvm_load`.
- `cvm_run` / `cvm_run_args(&img, args, n_args, &ret)`.
- `cvm_image_free`, `cvm_strerror`, `cvm_image_get_region`.
- Built-in syscalls: `cvm_sys_heap_start`, `cvm_sys_heap_size`,
  `cvm_sys_get_region`.
- `CVM_NO_STDLIB_FALLBACK` build-time gate: skip the `<stdlib.h>`
  include and turn a missing allocator hook into hard-fail
  (`alloc` returns NULL, `free` is a no-op).
- Host memory regions declared via `--region=NAME:SIZE[:DIR]` at
  translate time (DIR ∈ `r`/`w`/`rw`, default `rw`); discovered
  at run time via `cvm_sys_get_region(name_ptr)`. Up to 64 regions
  per binary, name ≤ 15 chars.
- Public version macros (`CVM_VERSION_MAJOR/MINOR/PATCH/STRING`)
  and matching runtime `cvm_version_string()` /
  `cvm_version_number()` accessors. `CVM_VERSION_STRING` and
  `CVM_VERSION_1_0` (binary-format magic) are deliberately
  distinct.

#### Translator codegen

- Calling convention: `R255` = SP, `R254` = scratch, `R0..R7`
  mirror the syscall ABI for first-8 args, additional args
  stacked. Return value in `R0`. FUNCS table indexed by stable
  per-function id (slot 0 = NULL-fn-ptr trap).
- Block-local SSA register reuse: per-block free pool;
  cross-block values, phi results, and phi inputs get
  permanent regs.
- Liveness-based spill at every `CALL` site, narrowed by an
  `ever_spilled` bitmap and packed via `slot_of[]` so the
  frame holds only regs ever live across some call.
- Post-emission branch relaxation: `BEQ`/`BNE` whose offsets
  exceed imm8 ±127 are rewritten to `BEQ +1; JMP imm24`.
- Switch lowering: chained `CMP_EQ + BNE` (sparse) or a DATA
  jump table read via `LDW + JMPR` (dense, `n_cases ≥ 4` AND
  density ≥ 0.5). Negative `iN` case constants handled
  correctly in both forms.
- `llvm.fshl.i32` / `llvm.fshr.i32` lowered to 9 opcodes with
  a `c == 0` fixup (the VM's `SHL`/`SHR` mask shift amounts to
  the low 5 bits, so naive lowering would corrupt `b >> 32`).
- Recognised intrinsics: `llvm.abs.i32`, `llvm.smax.i32`,
  `llvm.mem*`, plus `cvm_intrin_*` extern names which inline
  directly to opcodes (`MULH`, `MULHU`, `FSQRT`, `F2I_SAT_*`).
- `Trunc iN` masks to the target width; `SExt` lowered via
  `SHL + SAR`.

#### Distribution

- CMake `add_subdirectory(deps/CronoVM)` consumer pattern,
  exercised end-to-end by `examples/embedder/` (configure +
  build + run asserted by ctest).
- `cmake --install` populates a standard GNUInstallDirs
  layout; consumers `find_package(CronoVM 0.1 REQUIRED)` and
  link against `cronovm::cvm`. `cronovm::cvm-cc` and
  `cronovm::cvm-translate` (if available) are exported targets.
  Exercised end-to-end by `examples/installed_consumer/`.
- Runtime header dir is resolved by `cvm-cc` at runtime: probes
  `<exedir>/../share/cronovm/runtime/lib/`, then falls back to
  the CMake-baked build-tree path. `--runtime-dir=` overrides.

#### Tests

- 78 ctest cases covering: arithmetic, control flow, calls,
  recursion, alloca, indirect calls (function pointers + dispatch
  tables), narrow loads/stores, memory intrinsics, branch
  relaxation, switch (chain + table, negative case constants),
  multi-precision integer + float, fixed-point Q16.16,
  allocator hook end-to-end, host regions, end-to-end via
  `cvm-cc`, `find_package` round trip.
- Adversarial fixtures: `long_branch` (relaxation),
  `switch_neg_table` (negative-iN case constants),
  `spill_loop` (call-heavy loop simultaneously stressing
  spill compaction + branch relaxation).

#### Cross-compile sanity

- Toolchain file `cmake/toolchains/thumbv7m-none-eabi.cmake`
  drives a Cortex-M3-class cross build via `arm-none-eabi-gcc`.
- POST_BUILD hook runs `arm-none-eabi-nm --undefined-only`
  against `libcvm.a` and fails the build if any undefined
  symbol falls outside `cmake/cortex_m_allowed_symbols.txt`.
- The allowlist permits: the freestanding `<string.h>` subset
  (`memcpy`/`memset`/`memmove`/`memcmp`/`memchr`/`strcmp`/
  `strncmp`), `sqrtf`, `__aeabi_*`, and a handful of
  compiler-rt builtin names. Malloc/free/printf/exit are
  pointedly absent.

#### Fuzzing

- libFuzzer harness wrapping `cvm_fuzz_translate_buffer` (the
  parse + codegen pipeline on an in-memory bitcode blob).
  Gated behind `-DCVM_BUILD_FUZZER=ON`; auto-detects
  `-fsanitize=fuzzer` support via configure-time probe.
  A `cvm-translate-fuzz-replay` standalone driver works on
  any compiler/OS for corpus replay without sanitizers.
- 31-file seed corpus auto-staged from the existing fixture
  `.bc` files into `build/tools/translator/fuzz_corpus/`.
- First exploratory run (Linux clang-21, 5 minutes, 2 fork
  workers): 970 000 executions; coverage grew 1854 → 8025
  features; corpus grew 31 → 281 inputs; zero crashes rooted
  in the translator (all 1252 crash artifacts live inside
  libLLVM's bitcode parser).

#### CI

- Five GitHub Actions jobs on push-to-main / pull_request:
  `linux × {clang, gcc}` (full ctest); `linux-no-stdlib-fallback`
  (embedded gate); `linux-cortex-m-sanity` (thumbv7m cross-build
  with nm allowlist); `windows-ucrt64-clang` (msys2 ucrt64);
  `macos-clang` (brew LLVM).

#### Documentation

- [docs/isa.md](docs/isa.md) — full opcode table.
- [docs/format.md](docs/format.md) — binary format.
- [docs/translator.md](docs/translator.md) — codegen and spill
  protocol, switch lowering, branch relaxation.
- [docs/NEXT.md](docs/NEXT.md) — orientation notes for the next
  session (kept up to date as decisions land).
- [CLAUDE.md](CLAUDE.md) — design constraints and project
  character for AI coding sessions.

### Known limitations

- `cvm_d_div` is a 52-iter restoring divider with truncate
  rounding — within 1 ULP of IEEE, not bit-exact. Round-to-
  nearest is a deferred follow-up.
- f64 NaN payloads are not preserved; all NaNs collapse to
  `CVM_D_NAN`.
- `F2I_S` / `F2I_U` saturate (NaN → 0, overflow →
  `INT_MAX`/`MIN`/`UINT_MAX`). In C, casting a float to an
  integer when out of range is UB, and clang -O1 can fold
  the cast to any value (often 0) before reaching the opcode.
  Route through `cvm_f2i_sat_s` / `cvm_f2i_sat_u` from
  `cvm_intrin.h` to guarantee runtime saturating behaviour.
- Soft-float on hosts without an FPU (Cortex-M0/M0+/M3,
  RP2040) is provided by the C compiler that builds `cvm.c`
  (libgcc / compiler-rt) — ~50–100× slower than hardware,
  no feature flag.
- Little-endian only.
- Bitcode parser (`libLLVM`) is known to have crash-prone
  edge cases on adversarial input — the fuzz run above
  exclusively surfaced libLLVM crashes, not translator
  crashes. Run `cvm-translate` on untrusted `.bc` in a
  sandboxed subprocess.
- Translator's IR subset is intentionally narrow; anything
  outside scalar i1/i8/i16/i32/f32 + control flow + calls +
  GEP + the supported intrinsic set is rejected with a clear
  error.

[0.1.0]: https://github.com/cronomantic/CronoVM/releases/tag/v0.1.0
