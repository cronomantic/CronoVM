# Translator

The translator is the tool that turns LLVM bitcode (produced by Clang) into
CronoVM bytecode. It lives in `tools/translator/` and is built as
`cvm-translate`.

```
user.c ──[ clang -emit-llvm ]──▶ user.bc ──[ cvm-translate ]──▶ game.bin
```

The translator is **not** part of the runtime. The VM binary you ship with
your game has zero LLVM dependency.

## Status (step 4)

This step builds only the **parsing + subset validation** half of the
translator. It reads a `.bc` file, walks its module, and reports any IR
construct outside the supported subset. It does not yet emit bytecode —
that's step 5.

```
$ cvm-translate build/add.bc
module: tests/fixtures/add.c
function add(i32, i32) -> i32 [1 block, 8 instructions]
function sub_mul(i32, i32, i32) -> i32 [1 block, 12 instructions]
translator: ok
```

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
|------|--------|
| `i64` and wider integers       | deferred to int64 opcodes |
| `half`, `float`, `double`, etc. | deferred to float64 opcodes |
| vectors (`<N x T>`)            | not in the subset; games target scalars |
| address spaces other than 0    | not supported |
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
|--------|----------|--------|
| floating-point | `fadd`, `fmul`, `fcmp`, `sitofp`, ... | deferred to float64 |
| exceptions     | `invoke`, `landingpad`, `resume`, `cleanuppad` | not in subset |
| atomics/fences | `cmpxchg`, `atomicrmw`, `fence` | not in subset |
| vectors        | `extractelement`, `shufflevector`, ... | not in subset |
| variadic       | `va_arg` | not in subset |
| indirect calls | `indirectbr`, `callbr` | not in subset |
| `freeze`, `addrspacecast` | | not yet supported |

## Invoking the translator

```
cvm-translate <input.bc>
```

Exit codes:
- `0` — input is in the supported subset
- `1` — input has issues; messages on stderr
- `2` — usage error

The expected pipeline (until `gamecc` is built) is:

```sh
clang -emit-llvm -O0 -c user.c -o user.bc
cvm-translate user.bc
```

`-O0` is fine for now — the IR Clang emits at any optimisation level is
within the subset as long as the source code is. Higher `-O` produces
smaller IR with more aggressive transforms; the translator handles either.

## Adding new IR support

When the time comes to broaden the subset (e.g. enabling `i64`):

1. Update [isa.md](isa.md) with the new opcodes.
2. Add the opcodes to the interpreter in `src/cvm.c`.
3. Update `type_in_subset` and `opcode_in_subset` in
   `tools/translator/translator.c` to admit the construct.
4. Once codegen lands (step 5+), update the codegen table.

Order matters: bytecode/interpreter first, translator validation second.
The subset is the contract — it should always be the *intersection* of what
the interpreter understands and what the translator promises to handle.
