# Translator

The translator is the tool that turns LLVM bitcode (produced by Clang) into
CronoVM bytecode. It lives in `tools/translator/` and is built as
`cvm-translate`.

```text
user.c ──[ clang -emit-llvm ]──▶ user.bc ──[ cvm-translate ]──▶ game.bin
```

The translator is **not** part of the runtime. The VM binary you ship with
your game has zero LLVM dependency.

## Status (step 5)

The parsing + subset validation half is complete, and codegen now exists
for a deliberately narrow case: **a single function with straight-line
scalar arithmetic, integer parameters, and a single `ret`**. Anything else
is rejected with a clear message. Multi-block control flow, allocas, and
calls between user functions are work for steps 6–7 (calling convention
and broader codegen coverage).

```text
$ cvm-translate build/add.bc                 # validate only
module: tests/fixtures/add.c
function add(i32, i32) -> i32 [1 block, 2 instructions]
translator: ok

$ cvm-translate build/add.bc -o build/add.bin
module: tests/fixtures/add.c
function add(i32, i32) -> i32 [1 block, 2 instructions]
translator: wrote build/add.bin (2 instructions)
```

The `add(int, int)` example produces exactly two CronoVM instructions:

```text
ADD  R2, R0, R1     ; R2 = R0 + R1     (params arrive in R0..R(N-1))
HALT R2             ; return R2
```

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

### Instructions accepted

- terminators: `ret`, `br`, `switch`, `unreachable`
- arithmetic: `add`, `sub`, `mul`, `udiv`, `sdiv`, `urem`, `srem`,
  `shl`, `lshr`, `ashr`, `and`, `or`, `xor`
- memory: `alloca`, `load`, `store`, `getelementptr`
- conversions: `trunc`, `zext`, `sext`, `ptrtoint`, `inttoptr`, `bitcast`
- comparison and select: `icmp`, `select`
- aggregates: `extractvalue`, `insertvalue`
- structural: `phi`, `call`

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
