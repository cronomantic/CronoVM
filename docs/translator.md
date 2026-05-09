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

## Required input: -O1 (or higher)

Until alloca lowering lands, **bitcode for codegen must be compiled with
`-O1` or higher** so Clang's `mem2reg` pass has run and the IR is in clean
SSA form. With `-O0`, parameters live in `alloca` stack slots and
`load`/`store` shuffles them around — codegen will reject this.

Validation alone (no `-o`) accepts any optimisation level.

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
- value selection: `select`
- intrinsic calls: `llvm.abs.i32`, `llvm.smax.i32`, `llvm.smin.i32`,
  `llvm.umax.i32`, `llvm.umin.i32`

User-defined function calls and the rest of the LLVM intrinsics
(`llvm.memcpy`, `llvm.memset`, `llvm.ctlz`, …) still error out — they
need calling-convention work or per-intrinsic lowerings.

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
