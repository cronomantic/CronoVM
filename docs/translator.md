# Translator

The translator is the tool that turns LLVM bitcode (produced by Clang) into
CronoVM bytecode. It lives in `tools/translator/` and is built as
`cvm-translate`.

```text
user.c ──[ clang -emit-llvm ]──▶ user.bc ──[ cvm-translate ]──▶ game.bin
```

The translator is **not** part of the runtime. The VM binary you ship with
your game has zero LLVM dependency.

## Status (step 6a)

Codegen now covers **scalar i32 arithmetic, comparisons, conditional and
unconditional branches, multi-block control flow with phi nodes, and
`select`** — enough to compile a recognisable function body with
branches and loops. Calls between user functions, allocas, and
non-i32 widths still error out with a precise message; those land
alongside the calling-convention work.

```text
$ cvm-translate build/fib.bc -o build/fib.bin
module: tests/fixtures/fib.c
function fib(i32) -> i32 [3 blocks, 11 instructions]
translator: wrote build/fib.bin (26 instructions)
```

Each translated function begins with a one-instruction prologue that
materialises a zero register; that register lets `BNE`/`BEQ` stand in for
"branch if non-zero" / "branch if zero" without dedicated opcodes. See
[isa.md](isa.md) for the full encoding rules.

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
- memory: `load` / `store` (i32, i1, ptr), `getelementptr`
- aggregate values via globals: arrays, structs, scalars
- value selection: `select`
- pointer reinterprets: `inttoptr`, `ptrtoint`, `bitcast`
- width casts: `trunc`, `zext`, `sext` (no-op MOV — i32-only register
  model means these don't change the live value at i386-elf -O1)
- intrinsic calls: `llvm.abs.i32`, `llvm.smax.i32`, `llvm.smin.i32`,
  `llvm.umax.i32`, `llvm.umin.i32`
- syscall calls: any `cvm_sys_*` function call lowers to `SYSCALL`

User-defined non-syscall calls and the rest of the LLVM intrinsics
(`llvm.memcpy`, `llvm.memset`, `llvm.ctlz`, …) still error out — they
need CALL/RET work or per-intrinsic lowerings. Likewise `alloca`,
i8/i16 loads/stores, and `extern` data globals are pending.

### Calling convention

Inside a translated function:

- `R0..R7` are reserved as **syscall argument-passing slots**. They
  never hold SSA values directly; they're scratch around `SYSCALL`
  instructions.
- `R8..` is where SSA values live. Function parameters arrive in
  `R0..R(N-1)` (host calls `cvm_run_args`) and the prologue copies
  them up to `R8..R(8+N-1)` immediately, so the body sees stable
  registers regardless of any later syscall clobbering R0..R7.
- Each function's `MOVI zero` lives at the next free register after
  the params.

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

The `DATA` section is currently capped at 32 KB because every global
address is materialised through `MOVI imm16`. When that limit binds, a
wide-constant lowering (or a constant pool) will lift it.

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
