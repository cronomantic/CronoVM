# Translator

The translator is the tool that turns LLVM bitcode (produced by Clang) into
CronoVM bytecode. It lives in `tools/translator/` and is built as
`cvm-translate`.

```text
user.c ──[ clang -emit-llvm ]──▶ user.bc ──[ cvm-translate ]──▶ game.bin
```

The translator is **not** part of the runtime. The VM binary you ship with
your game has zero LLVM dependency.

## Status (step 11)

Codegen now covers **scalar i32 arithmetic, comparisons, branches,
multi-block control flow, `select`, `switch`, calls between user
functions (direct, recursive, indirect, with > 8 args),
entry-block allocas with escaping pointers, i8/i16 memory ops,
arbitrary 32-bit constants, a NULL-fn-pointer trap, the
`llvm.memcpy`/`llvm.memset`/`llvm.memmove` block intrinsics,
liveness-based spill at every call site, and post-emission
branch relaxation**. `i64` and `double` are both legalised inside a
function body now — `i64` to inline 32-bit word pairs, `double` to
soft-float runtime calls (see "64-bit integer legalisation" and "f64
legalisation"), **including across function boundaries** — a 64-bit value
may be a function argument or return value (the 64-bit calling convention,
below). The i64/f64 surface is now complete: arithmetic incl.
`mul`/`div`/`rem`/`sqrt`, compares, conversions, `phi`, `select`, variable
shifts, params and returns.

`fib_recursive.bin` shrank from 135 to 51 instructions when
liveness landed (62% fewer); a deeply recursive function with
several SSA temps in flight is exactly the case the prior
spill-everything was worst at.

Branch relaxation only kicks in when needed (no overhead on
fixtures whose branches all fit in imm8); the new
`long_branch.c` fixture exercises it explicitly.

```text
$ cvm-translate build/fib_recursive.bc -o build/fib_recursive.bin
module: tests/fixtures/fib_recursive.c
function vm_main(i32) -> i32 [1 block, 2 instructions]
function fib(i32) -> i32 [4 blocks, 12 instructions]
translator: wrote build/fib_recursive.bin (135 instructions, 0 data
bytes, 0 imports, 2 funcs, 0 heap-reserve, 16384 stack-reserve)
```

Each translated function begins with a prologue that subtracts
`frame_size` from `R255` (SP), copies first-8 params from `R0..R7`
into their high SSA homes, materialises a zero register and any
alloca pointers, and then runs the function body. Every `ret`
emits `MOV R0, retval; ADD R255, frame_size; RET` — the last
`RET` of the entry function pops the run-completion sentinel that
`cvm_run` pre-pushed and the run halts with `R0` as the result.

Phi elimination is done by emitting register-to-register `MOV`s on each
predecessor's terminator. When the moves on a single edge form a cycle
(e.g. a loop back-edge that rotates state), the codegen detects the
conflict and round-trips through fresh temporary registers — wasteful but
always correct. A future pass can break cycles in-place without temps.

## Required clang flags

Bitcode for codegen must be compiled with **`--target=i386-elf` and `-O1`**
(or higher). Together they ensure:

- 32-bit pointers and 32-bit GEP indices, so the IR stays in `i32`-only
  territory (the default x86-64 target promotes loop counters to `i64`
  whenever pointer arithmetic is involved).
- `mem2reg` has run, so parameters live in registers rather than `alloca`
  stack slots.

Validation alone (no `-o`) accepts any optimisation level and any target.

## Supported IR subset (v1.0)

The subset is deliberately narrow. If your C code uses anything outside it,
the translator rejects with a clear message; that's a feature, not a bug.
Diagnostics carry the source `file:line` of the offending instruction (cvm-cc
builds with `-gline-tables-only`), e.g.
`translator: game.c:42: in 'frame': unsupported intrinsic 'llvm.fabs.f32'`.

### Types accepted

- `void`
- `i1`, `i8`, `i16`, `i32`
- pointer (opaque, default address space only)
- arrays and structs whose elements are themselves accepted types
- function types

### Types rejected

| Type | Reason |
| ---- | ------ |
| integers wider than `i64` | not in the subset |
| `half`, `fp128`, `x86_fp80`, etc. (`float` and `double` excepted) | not in the subset (`float` is first-class; `double` is legalised) |
| vectors (`<N x T>`) | not in the subset; games target scalars |
| address spaces other than 0 | not supported |
| `token`, `x86_amx`, target_ext | not in the subset |

### Instructions accepted by validation

The validator (no `-o`) accepts a wider set than codegen currently lowers;
the gap is what's still being implemented:

- terminators: `ret`, `br`, `switch`, `unreachable`
- arithmetic: `add`, `sub`, `mul`, `udiv`, `sdiv`, `urem`, `srem`,
  `shl`, `lshr`, `ashr`, `and`, `or`, `xor`
- memory: `alloca`, `load`, `store`, `getelementptr`
- conversions: `trunc`, `zext`, `sext`, `ptrtoint`, `inttoptr`, `bitcast`
- comparison and select: `icmp`, `select`
- aggregates: `extractvalue`, `insertvalue`
- structural: `phi`, `call`

### Instructions currently lowered by codegen

- arithmetic: `add`, `sub`, `mul`, `sdiv`, `udiv`, `srem`, `urem`,
  `shl`, `lshr`, `ashr`, `and`, `or`, `xor`
- comparisons: `icmp` (all 10 predicates)
- control: `br` (both forms), `switch`, `phi`, `ret`
- memory: `load` / `store` (i1/i8/i16/i32/ptr — narrow loads use `LDB`/`LDH`
  and zero-extend; narrow stores use `STB`/`STH` and write only the low byte
  or halfword), `getelementptr`
- aggregate values via globals: arrays, structs, scalars
- value selection: `select`
- pointer reinterprets: `inttoptr`, `ptrtoint`, `bitcast`
- width casts: `trunc`, `zext` are still no-op MOVs (LDB/LDH already
  zero-extend on load, and STB/STH only write the low byte/halfword on
  store, so upper-bit garbage in a "narrow" register never reaches
  memory). `sext` from a narrow source emits an explicit sign-extension
  via `MOVI scratch, (32-w); SHL dst, src, scratch; SAR dst, dst,
  scratch` — three instructions per narrow signed load
- intrinsic calls: `llvm.abs.i32`, `llvm.smax.i32`, `llvm.smin.i32`,
  `llvm.umax.i32`, `llvm.umin.i32`; `llvm.memcpy.*`, `llvm.memset.*`,
  `llvm.memmove.*` (lowered to single `MEMCPY`/`MEMSET`/`MEMMOVE`
  opcodes — the i1 isvolatile operand is metadata and ignored;
  length must be i32, since i64 size_t isn't in the subset);
  `llvm.lifetime.start/end` (no-ops)
- syscall calls: any `cvm_sys_*` function call lowers to `SYSCALL`
- user calls: direct calls to functions defined in the same module
  lower to `CALL imm24`, with caller-saved spill, R0..R7+stack arg
  passing, and R0 return value
- indirect calls: when the callee is an SSA value (loaded from a
  global, returned by another function, or selected between two
  candidates) the codegen emits `CALLR Rcallee`. Function values
  used as operands (`select i1 _, ptr @add, ptr @sub`) lower to
  `MOVI Rd, func_index`
- `alloca`: static-size, entry-block-only — the prologue materialises
  each alloca pointer as `SP + offset`

Function values stored in DATA-section globals (e.g. a `static const
fn_t ops[N] = { f0, f1, ... };` dispatch table) are also supported:
the constant-initialiser serialiser writes each function pointer as
its FUNCS-table index `(k + 1)` in little-endian u32 form, and runtime
code loads it with `LDW` and dispatches via `CALLR`. The `+1` shift
keeps `FUNCS[0]` reserved as the null-function-pointer trap slot, so
a zero-initialised function pointer (LLVM `null` → 0 bytes in DATA)
naturally traps with `CVM_E_NULL_FUNC_PTR` when called instead of
silently dispatching to the first user function.

`extern` data globals still error out (no host-side data linking yet).

### 64-bit integer legalisation

The register file is 32-bit, so a 64-bit integer can't live in a register.
Rather than reject `i64` outright, the translator **legalises** it: each `i64`
SSA value occupies **two consecutive value-spill slots** in the frame (lo word
at `i64_slot`, hi word at `i64_slot + 1`), and every `i64` operation is lowered
to explicit lo/hi 32-bit word arithmetic. An `i64` value never occupies a
register and is invisible to the linear-scan / caller-save spill machinery — it
already lives in memory. It crosses to the 32-bit world only at the boundaries
(`sext`/`zext` widen into it; `trunc`/`icmp`/`store` consume it).

Currently lowered `i64` operations:

- **`sext`** (`lo = src`, `hi = src >>(arith) 31`) and **`zext`**
  (`lo = src`, `hi = 0`)
- **`load`** / **`store`** — two 32-bit `LDW`/`STW` at `[ptr]` and `[ptr+4]`
  (a constant `i64` store, e.g. clang's struct zero-init, takes the same path)
- **`add`** / **`sub`** with carry / borrow propagated through `CMP_LTU`
- **`and`** / **`or`** / **`xor`** word-wise
- **`shl`** / **`lshr`** / **`ashr`** by a **constant** amount (the variable
  case is not legalised yet), correct across the `<32`, `==32` and `33..63`
  ranges
- **`trunc` i64→iN** — keeps the lo word (masked for `N < 32`)
- **`icmp`** — `eq`/`ne` combine the two word compares; the ordered predicates
  decompose to `(ah <hi bh) | ((ah == bh) & (al <lo bl))` with a signed or
  unsigned high-word compare and an unsigned low-word compare
- **`mul`** — inline `lo = al*bl`, `hi = mulhu(al,bl) + al*bh + ah*bl` (the
  `ah*bh` term overflows bit 63 and drops); signed/unsigned agree
- **`udiv`/`sdiv`/`urem`/`srem`** — a soft runtime CALL into `cvm_int64_rt.c`
  (`__cvm_{u,s}div64` / `__cvm_{u,s}mod64`, sret return, same ABI as the f64
  helpers), since a 64/64 long division is too large to open-code. cvm-cc
  auto-links that TU when a module uses i64 div/rem
- **`freeze`** — identity (copy the two words); clang emits it to block poison
  propagation around the volatile/div operands
- **variable-amount `shl`/`lshr`/`ashr`** — a soft runtime CALL into
  `cvm_int64_rt.c` (`__cvm_{shl,shr,sar}64`); a constant amount stays inline
- **`select`** — two parallel word-MOVs picked by the condition (the wide twin
  of the i32 select's branch)
- **`phi`** — its two slots are written by the incoming-edge moves
  (`cg_emit_phi_moves` pushes a lo and a hi word-move per wide phi, sharing the
  scalar parallel-copy / conflict-staging machinery)
- **params / returns / calls** — see "64-bit calling convention" below

The i64 surface is complete.

### f64 (double) legalisation

`double` reuses the same two-slot storage as `i64` (a 64-bit value in two
consecutive frame slots), but its operations can't be open-coded inline, so
each one is lowered to a **soft-float runtime call** into `cvm_float64_rt.c`
(the `__cvm_f*` helpers, thin external wrappers over `cvm_float64.h`). cvm-cc
**auto-links** that TU into any module that uses `double` — it runs
`cvm-translate --probe-runtime` on the compiled module (which exits
`CVM_PROBE_F64` when a `double` appears) and adds the runtime to the link set
only then, so integer-only programs carry no soft-float code. Only `fneg`/
`fabs` (a sign-bit flip), constants, and load/store are lowered inline.

The calls reuse clang's i386 ABI for `cvm_f64` — which the translator already
handles — so no new ABI work was needed: a 64-bit argument is passed as two i32
words in (lo, hi) order, and a 64-bit return uses an `sret` hidden pointer (the
result slot's address is handed in as that pointer, so the helper writes the
answer straight into the value's frame slots). Comparison/conversion helpers
that return `i32` use the normal R0 return.

Crucially, **each f64-runtime-call instruction is registered as a caller-save
spill point** (`cg_op_is_f64_runtime_call`, consumed by
`cg_compute_call_liveouts`), so the same liveness-narrowed save/restore that
protects a normal `call` protects these — live SSA registers survive the call.

Lowered `f64` operations:

- `fadd` / `fsub` / `fmul` / `fdiv` → `__cvm_fadd/fsub/fmul/fdiv` (sret)
- `fcmp` → one `__cvm_f{eq,ne,lt,le,gt,ge,ord,one}` call, plus a `1 - r`
  negation for the unordered-relational / `uno` / `ueq` predicates; `true`/
  `false` fold to a constant
- `fneg` → inline `xor` of the sign bit (no call)
- `sitofp` / `uitofp` / `fpext` (→ `double`) → `__cvm_f_from_{i32,u32,f32}` (sret)
- `fptosi` / `fptoui` / `fptrunc` (`double` →) → `__cvm_f_to_{i32,u32,f32}`
- `double` constant / `load` / `store` → materialised / two-word, like `i64`
- `llvm.fmuladd.f64` / `llvm.fma.f64` → two runtime calls (`fmul` then `fadd`),
  with the intermediate product written to the result's own slot. clang's
  default `-ffp-contract=on` synthesises `fmuladd` from `a*b±c`, so this is the
  common shape; the two-rounding lowering matches the soft runtime's existing
  truncating-mul accuracy and `fmuladd`'s relaxed contract.
- `llvm.fabs.f64` (inline sign-bit clear) / `llvm.copysign.f64` (inline)
- `llvm.sqrt.f64` (or a plain `sqrt` call — the errno-setting form clang emits
  without `-fno-math-errno`) → `__cvm_fsqrt` (Newton–Raphson, exponent-halving
  seed). `double` `phi` and `select` work like their i64 counterparts.

`double` `phi`, `select`, params, returns and calls work like their i64
counterparts (see "64-bit calling convention"). Not yet lowered (clear error):
the rarer `llvm.*.f64` math intrinsics (`sin`, `cos`, `exp`, …). The f64
surface is otherwise complete.

### 64-bit calling convention

i64/f64 cross function boundaries on a **word-based** convention that
generalises the scalar one (so a function with no 64-bit params/returns gets
byte-identical code):

- **Arguments** fill word positions 0,1,2,…; a scalar takes one word, a 64-bit
  value takes two (lo then hi). Words 0–7 arrive in `R0..R7`; words ≥ 8 (and
  every word of a variadic callee) sit on the stack. A 64-bit argument may
  straddle the register/stack boundary — each word is placed independently
  (`cg_place_arg_word`).
- **Return** — a scalar comes back in `R0`; a 64-bit value in `R0` (lo) and
  `R1` (hi). The callee's `ret` loads its two slots into R0:R1; the caller
  stores R0:R1 into the result value's two slots.
- **Callee prologue** reads each incoming argument word (from `R0..R7` or the
  stack) into its parameter's home — a register for a scalar, two frame slots
  for a 64-bit param.

This is distinct from the soft-runtime helpers' ABI: `cvm_float64_rt` /
`cvm_int64_rt` return a `cvm_f64`/`cvm_i64` *struct*, which clang lowers to an
`sret` hidden pointer (the legaliser hands the result slot's address as that
pointer) — those are void functions with i32/pointer params, not 64-bit
returns. Only native `i64`/`double` signatures use the R0:R1 convention.

### Calling convention (scalar)

Register reservations:

- `R0..R7` — argument slots for both syscalls and user calls. Return
  value of either lands in `R0`. They never hold SSA values directly;
  the prologue copies the first eight params into their high SSA
  homes immediately so subsequent calls can clobber them.
- `R8..R253` — SSA values, including the dedicated zero register and
  any alloca pointers materialised in the prologue.
- `R254` — codegen scratch (frame-size constants, spill addresses,
  stack-arg pointers). Never holds an SSA value, so it doesn't need
  to be saved across calls.
- `R255` — stack pointer (SP).

Frame layout (low → high addresses, from SP after prologue):

```text
[ alloca area | spill slots ]   ← cg->alloca_bytes + cg->spill_bytes
[ saved return PC ]              ← pushed by the caller's CALL
[ stacked args (9th onward) ]    ← pushed by the caller before CALL
```

The prologue does `SUB R255, R255, frame_size` once; every `ret`
reverses it with `ADD R255, R255, frame_size; RET`.

User-call lowering (caller side, around each `LLVMCall` to a
non-syscall non-intrinsic):

1. Spill `R8..R(ssa_reg_high-1)` to the spill area at
   `[SP+alloca_bytes ..]`. Constants and per-instruction temps that
   `cg_reg_for` materialised above `ssa_reg_high` are dead after
   their parent instruction and don't need to be saved.
2. If `narg > 8`, push the extras: `SUB SP, n*4; STW SP+i*4, arg_i`.
3. Move first-8 args into `R0..R7`.
4. `CALL imm24` (with the callee's index in the FUNCS table).
5. After return: `ADD SP, n*4` to drop stacked args (caller cleans).
6. Reload `R8..R(ssa_reg_high-1)` from the spill area.
7. `MOV dst, R0` for the call's return value (when not `void`).

Spill is **liveness-narrowed** as of step 9. The codegen runs a
standard fixed-point liveness analysis after pre-allocation:

- `cg_block_def_use` computes `def[bb]`, `phi_def[bb]`, and
  `use[bb]` (upward-exposed uses) for each block. Phi instructions
  are skipped during the upward-use walk because their source
  operands logically belong to the predecessor edges, not to the
  block containing the phi.
- The fixed-point computes `live_in[bb]` and `live_out[bb]` with
  `live_out[P] = ∪_{S} ((live_in[S] − phi_def[S]) ∪
  phi_use(P, S))`, where `phi_use(P, S)` is the set of phi-input
  source registers on edge P→S.
- `cg_compute_call_liveouts` then walks each block backward,
  snapshotting at every `LLVMCall` the set of spillable registers
  live at the program point immediately after the call.

The CALL handler consults that snapshot via
`cg_lookup_call_live` and only spills/restores registers in the
set, with the call's own destination register masked out (it
holds garbage from the caller's POV and is overwritten by the
post-call `MOV dst, R0`). The slot mapping in the spill area is
unchanged (slot k for register 8+k); only the count of emitted
STW/LDW pairs goes down.

A defensive fallback in the CALL handler reverts to spilling
everything if a call instruction has no recorded live set —
correctness never depends on the analysis being complete.

### Switch lowering

`LLVMSwitch` lowers to a chained linear search: for each case
`(case_const, case_bb)` in source order, emit
`CMP_EQ tmp, cond, case_const; BNE tmp, zero, case_bb`. After
all cases, fall through with `JMP default_bb`. The `tmp` and the
materialised `case_const` register are transient (allocated above
`ssa_reg_high`), so they don't need to be spilled across calls.

Phi moves for every successor (default + each case) are emitted
*before* the dispatch sequence. SSA's one-register-per-value
guarantee means each successor's phi-result registers are unique
to that successor, so the up-front moves don't interfere with
each other and the dispatch sequence (which only touches
transient temps) doesn't disturb them.

LLVM's C API doesn't expose case constants through the generic
operand list — only successor blocks are there. The case values
are read via `LLVMGetSwitchCaseValue(switch_inst, k)` where `k`
is the successor index (matching `LLVMGetSuccessor`'s indexing,
with 0 = default and 1..N = cases).

A dense jump-table form (one `LDW` from a precomputed table +
indirect jump) would be faster for switches with many contiguous
cases but needs a new opcode (`JMPR`). The chained form here is
opcode-neutral and benefits transparently from branch relaxation
when a case block sits more than ±127 instructions away.

### Branch relaxation

`cg_relax_branches` runs once per function between block emission
and `cg_resolve_fixups`. The codegen always emits conditional brs
in their compact 2-instruction form first (`BNE cond, zero, +K`
to true_bb, plus `JMP +M` to false_bb). Relaxation then walks the
fixup list and, for each imm8 fixup whose offset falls outside
[-128, 127], schedules a rewrite to the 3-instruction
trampoline:

```text
[BEQ cond, zero, +1]     ; skip the next inst if condition is false
[JMP true_bb_imm24]      ; reached when condition is true
[JMP false_bb_imm24]     ; reached when condition is false (via the +1 skip)
```

The pass iterates to a fixed point because inserting one
trampoline shifts all later code by +1 instruction, which can
push another previously-in-range branch over the edge. Each
iteration relaxes at least one fixup or terminates, so the loop
is bounded by the fixup count (typically 1–2 iterations in
practice). After convergence, the pass:

1. Builds a new `code` array with one extra placeholder `JMP 0`
   inserted after each relaxed BEQ/BNE.
2. Rewrites the BEQ/BNE in place: opcode flips
   (BNE ↔ BEQ) and imm8 is hard-coded to `+1`.
3. Updates the fixup table: each relaxed fixup migrates to the
   inserted JMP's position with `bits = 24`.
4. Updates `block_offsets[]` for all blocks.

`cg_resolve_fixups` then OR-patches every fixup as before. The
"out of range" branch error in `cg_resolve_fixups` is now
defensive — it should never fire after relaxation.

The fast path (single `BEQ`/`BNE imm8` for short branches) is
preserved verbatim: relaxation only rewrites branches that
actually need it. The included `tests/fixtures/long_branch.c`
fixture is a 168-instruction function whose entry block branches
forward over a long volatile-arithmetic body to an
`end_short:` block, exercising relaxation explicitly.

### Syscall lowering

A call to a function whose name begins with `cvm_sys_` is treated as a
syscall:

1. Each unique callee gets a stable `syscall_id` (its index in the
   IMPORTS section).
2. Args are read into temporaries via `cg_reg_for`, then `MOV`'d into
   `R0..R(narg-1)`.
3. `SYSCALL imm16=syscall_id` is emitted.
4. If the callee returns a value, the result (which arrives in `R0`)
   is `MOV`'d into the call's destination register.

### Globals layout

The translator walks every module global, lays them out one after
another with their natural alignment, serialises initialisers into a
single `DATA` section, and emits offsets that user code resolves via
`MOVI`. Zero-initialised globals get bytes of zero in `DATA` — a `BSS`
section is supported by the loader but not yet emitted by the
translator (cheap in binary size for now, swap-in is straightforward
when something pushes the size up).

Global addresses are materialised through `MOVI`/`MOVI+MOVHI` as needed,
so the `DATA` section has no codegen-imposed size cap (only the loader's
`heap_size + stack_size <= 4 GiB` invariant binds).

### Instructions rejected

| Family | Examples | Reason |
| ------ | -------- | ------ |
| floating-point | `fadd`, `fmul`, `fcmp`, `sitofp`, ... | deferred to float64 |
| exceptions | `invoke`, `landingpad`, `resume`, `cleanuppad` | not in subset |
| atomics/fences | `cmpxchg`, `atomicrmw`, `fence` | not in subset |
| vectors | `extractelement`, `shufflevector`, ... | not in subset |
| variadic | `va_arg` | not in subset |
| indirect calls | `indirectbr`, `callbr` | not in subset |
| `freeze`, `addrspacecast` | | not yet supported |

## Invoking the translator

```text
cvm-translate [-o <out.bin>] <input.bc>
```

Exit codes:

- `0` — success (validated, and emitted if `-o` was supplied)
- `1` — input has issues, or write failed; messages on stderr
- `2` — usage error

The two-step pipeline is normally driven by the `cvm-cc` wrapper
(`tools/cvm-cc/`), which runs clang with the right flags and pipes
the bitcode through:

```sh
cvm-cc user.c -o game.bin [--heap-reserve=N] [--region=name:size:dir]...
```

Manual invocation, when you want to inspect the intermediate `.bc` or
override clang flags directly:

```sh
clang --target=i386-elf -emit-llvm -O1 -I runtime/lib -c user.c -o user.bc
cvm-translate user.bc -o game.bin
```

## Symbol sidecar (`CVM_SYMS`)

Setting the `CVM_SYMS` environment variable makes the translator write a
`<out>.bin.sym` companion file next to the binary: one tab-separated line
per user function, `fid<TAB>entry_offset<TAB>name`.

```sh
CVM_SYMS=1 cvm-cc user.c -o game.bin   # also writes game.bin.sym
```

`fid` is the runtime **FUNCS index** — i.e. exactly the value a `CALL`/`CALLR`
targets, what `cvm_image.func_offsets` is indexed by, and what the optional
interpreter profiler reports. User function *k* lives at `FUNCS[k+1]` (slot 0
is the reserved null-function-pointer trap), so the sidecar already accounts
for the `+1`. The file is intended for tooling that has only function indices
(e.g. the `CVM_PROFILE` self-time profiler) and needs to map them back to
source names. It is never read by the loader.

## Adding new IR support

When the time comes to broaden the subset (e.g. enabling `i64`):

1. Update [isa.md](isa.md) with the new opcodes.
2. Add the opcodes to the interpreter in `src/cvm.c`.
3. Update `type_in_subset` and `opcode_in_subset` in
   `tools/translator/translator.c` to admit the construct.
4. Update the codegen table in `cg_function` to lower it.

Order matters: bytecode/interpreter first, translator validation second,
codegen last. The subset is the contract — it should always be the
*intersection* of what the interpreter understands and what the translator
promises to handle.
