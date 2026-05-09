# Translator

The translator is the tool that turns LLVM bitcode (produced by Clang) into
CronoVM bytecode. It lives in `tools/translator/` and is built as
`cvm-translate`.

```text
user.c ──[ clang -emit-llvm ]──▶ user.bc ──[ cvm-translate ]──▶ game.bin
```

The translator is **not** part of the runtime. The VM binary you ship with
your game has zero LLVM dependency.

## Status (step 7b)

Codegen now covers **scalar i32 arithmetic, comparisons, branches,
multi-block control flow, `select`, calls between user functions
(direct, recursive, indirect, with > 8 args), entry-block allocas
with escaping pointers, i8/i16 memory ops, arbitrary 32-bit
constants, and a NULL-fn-pointer trap**. The biggest remaining
gaps are `llvm.memcpy`/`memset`/`memmove` (struct/string copies)
and 64-bit / floating-point types.

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

### Types accepted

- `void`
- `i1`, `i8`, `i16`, `i32`
- pointer (opaque, default address space only)
- arrays and structs whose elements are themselves accepted types
- function types

### Types rejected

| Type | Reason |
| ---- | ------ |
| `i64` and wider integers | deferred to int64 opcodes |
| `half`, `float`, `double`, etc. | deferred to float64 opcodes |
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
- control: `br` (both forms), `phi`, `ret`
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
  `llvm.umax.i32`, `llvm.umin.i32`; `llvm.lifetime.start/end` (no-ops)
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

`llvm.memcpy` / `llvm.memset` / `llvm.memmove` and `extern` data globals
still error out — they need per-intrinsic lowerings.

### Calling convention

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

Spill is currently *spill-everything*. A future pass will narrow it
to values genuinely live across the call.

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

The expected pipeline (until `gamecc` is built) is:

```sh
clang -emit-llvm -O1 -c user.c -o user.bc
cvm-translate user.bc -o game.bin
```

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
