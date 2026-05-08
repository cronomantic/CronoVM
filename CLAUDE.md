# CLAUDE.md — Draft for the new VM project

> This file is a **draft** to copy into the root of the new repo as `CLAUDE.md`.
> It captures the architectural decisions made during the design conversation in
> the q3vm repo, so future Claude Code sessions in the new repo start with the
> right context.

## Project goal

Build a small embeddable virtual machine for **retro-style videogames**, with
a **modern C toolchain** (Clang as the frontend), as the spiritual successor to
the Quake III VM (q3vm) — but redesigned for today's constraints, not 1999's.

## Architecture in one diagram

```
user.c ──[clang -emit-llvm-bc]──▶ user.bc ──[translator]──▶ game.bin ──[VM]──▶ run
            (upstream, untouched)              (we own)        (we own)
```

User-facing command is a single step:

```
gamecc user.c -o game.bin
```

The wrapper hides the Clang invocation. Users never see `.ll`, `.bc`, `.asm`,
`.wat` or any other intermediate.

## What we own (and only this)

1. **The VM interpreter** — single-file C, embeddable, computed-goto dispatch.
2. **The translator** — reads LLVM bitcode, emits VM bytecode directly.
3. **The `gamecc` wrapper** — small driver that invokes Clang with the right
   flags and pipes the output through the translator.

We do **not** maintain a C compiler. Clang is upstream. When Clang adds C23
features, we get them for free.

## VM design decisions

- **Register-based**, not stack-based. Infinite virtual registers in the IR,
  resolved during codegen. Rationale: faster interpretation (fewer ops per
  source operation), trivial mapping from SSA, more compact bytecode, easier
  future JIT. (Q3VM's stack/OPSTACK design is inheritance to **avoid**, not
  preserve.)
- **Fixed-width 32-bit instructions**, ~80–120 opcodes target.
- **32-bit base type system**, with `int64`/`float64` opcodes added (Q3VM
  lacked `double`, we don't).
- **Single contiguous heap** with bounds checking — same sandbox model as
  q3vm, that part was good.
- **No GC**, no heap allocator built into the VM. Host-provided memory only.
- **Predictable performance** is a hard requirement (it's a game VM).
- **Threaded dispatch** with computed gotos under GCC/Clang; switch fallback
  for MSVC.

## Toolchain decisions

- **Frontend**: Clang. We never fork it, never patch it, never ship it.
- **IR**: LLVM bitcode (binary `.bc`), not textual `.ll`. The translator
  parses bitcode directly. No intermediate text format exposed.
- **Subset of LLVM IR accepted**: scalar ops, control flow, calls, GEP,
  basic intrinsics (`memcpy`, `memset`, `memmove`, math). Vectors, atomics,
  exceptions, threads → **error out** in the translator with a clear message.
- **Binary file format**: header + sections (code, data, bss, syscall imports,
  optional debug). Not q3asm-style text.
- **Calling convention**: cleaner than q3vm's `OP_ENTER N` frame setup. To
  be designed, but the syscall ABI must be friendlier than q3vm's `VMA` macro.

## What this project explicitly is NOT

- Not a WebAssembly runtime. WASM was considered and rejected as too generic /
  too far from the retro-game character we want.
- Not Q3VM-compatible. We can break `.qvm` compat freely.
- Not a general-purpose runtime. Game-shaped tradeoffs win over generality.
- Not a compiler project. If a question is "how should the compiler do X",
  the answer is "Clang already does X, our job ends at consuming its IR".

## Key constraints to remember when advising

- Game VMs care about **predictable frame-time cost**. Avoid features that
  introduce variance (GC, JIT warmup, allocations on hot paths).
- The interpreter must stay **single-file embeddable**. If a change requires
  splitting it across many files or adding dependencies, push back.
- LLVM IR was chosen as the input boundary because **the translator's job is
  bounded**. If a proposal makes the translator unbounded ("we'll handle
  arbitrary LLVM IR"), push back — the subset is the feature.
- The user values **character** in the VM. Generic-looking design choices
  ("just like everyone else does it") need stronger justification than
  retro-flavored ones.

## Decisions deferred to the new repo

These were not pinned down in the design conversation and should be
discussed before implementation starts:

- Exact opcode set and instruction encoding.
- Memory model details: address space size, stack vs heap split, alignment.
- Calling convention specifics (register vs frame slot for args, struct return).
- Syscall ABI (improvement over q3vm's `VMA` translation macro).
- Binary file format (header layout, section table, debug info shape).
- Exact LLVM IR subset boundaries and how rejections are reported.
- Build system (CMake? plain Make? something else?).
- Repo layout.

## Implementation order suggestion (when ready)

1. Define the binary format and write a loader stub.
2. Implement a minimal interpreter for ~10 opcodes; verify it runs hand-written
   bytecode.
3. Write a hand-coded "hello world" bytecode file end-to-end.
4. Build the LLVM bitcode parser side of the translator.
5. Map a single C function (e.g. `int add(int a, int b)`) end-to-end.
6. Expand opcode set and translator coverage in lockstep, driven by progressively
   more complex C programs.
7. `gamecc` wrapper last — only once the pipeline works manually.

The goal of this order is to always have **something runnable** rather than
spending months on infrastructure before the first program executes.

## Origin

This project is the successor to the q3vm fork at
[github.com/Cronomantic/q3vm](https://github.com/Cronomantic/q3vm)
(or wherever the fork lives). The decision history that led here is in that
repo's design conversation, not reproduced in full here.
