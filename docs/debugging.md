# VM diagnostics

A compile-gated, env-var-driven set of instruments built into the interpreter
(`src/cvm.c`) for chasing **memory corruption** and **codegen miscompiles** in
carts running on the VM. It is the tool of last resort: when a cart faults deep
inside a giant optimized function and the source-level cause is invisible, these
probes pin the faulting store, name the function that wrote a bad pointer, and
reconstruct a loop's full write history — all from the running image.

```text
cart.crom ──[ cronovm-headless built -DCVM_DIAG ]──▶ stderr probe output
                         ▲
                 CVM_REDZONE / CVM_PCDUMP / CVM_WADDR / … env vars
```

## Building it in

The whole subsystem is behind `#if defined(CVM_DIAG)` and is **off by default**,
so production carts and hosts carry none of its overhead. Enable it when
configuring the VM (or any host that statically links `libcvm.a`):

```sh
cmake -B build -DCVM_DIAG=ON
cmake --build build --target cronovm-headless   # or your host target
```

`CVM_DIAG` is a `PRIVATE` definition on the `cvm` target — the probes live
entirely inside `cvm.c` and touch neither the public ABI nor `cvm.h`, so the
rest of a consumer project never sees the flag.

Every probe is then selected at **run time** by an environment variable, so one
debug build serves every investigation. All output goes to **stderr**, each line
prefixed by the probe name (`[RZ-TRAP]`, `[PCDUMP]`, `[WADDR]`, …).

## Probes

### `CVM_REDZONE` — bounds-checking allocator shadow

The headline tool. It hooks the cart's allocator and traps the **first** store
that lands outside any live allocation — i.e. the original out-of-bounds write,
caught deterministically regardless of where the corruption later surfaces.

```sh
CVM_REDZONE=1 \
CVM_RZ_MALLOC=<slot> CVM_RZ_FREE=<slot> \
CVM_RZ_REALLOC=<slot> CVM_RZ_CALLOC=<slot> \
  cronovm-headless cart.crom
```

The `CVM_RZ_*` values are **FUNCS slots** of the cart's allocator functions
(a slot is `(symbol index) << 1`; map a name to its slot with the `.sym` file:
`slot = idx * 2`). The probe keeps a per-heap-byte shadow (`1` = live user
allocation, `0` = redzone/freed/metadata), skips checks while executing inside
the allocator itself, and on the first store to a `0` byte prints:

- `[RZ-TRAP]` — the faulting address, size, `pc`, enclosing `func`.
- `[RZ-BT]` — a heuristic backtrace (stack words that look like return PCs).
- `[RZ-HOLDERS] / [RZ-HOLDERS2]` — every memory word equal to the OOB pointer
  (and to `OOB-4`), each annotated with the `pc`/`func` that **last wrote** it.
- `[RZ-REGS]` — the full register file at the trap.
- `[RZ-FRAME]` — a window of the stack from SP upward, annotating return PCs,
  in-heap (mis)aligned pointers, and each slot's last writer.
- `[RING-A] / [RING-B]` and `[RZ-TRIP]` — see below, dumped here if armed.

A per-word **last-writer-PC map** over `[heap_start, mem_size)` backs the holder
scan and the trip report; it is maintained whenever `CVM_REDZONE` is set.

### `CVM_RING_A`, `CVM_RING_B` — slot write-history ring buffers

Watch up to two memory addresses. Each records the last `RING_N` (256) writes as
`(pc, old → new)` and dumps them at the redzone trap. Indispensable for a hot
slot (e.g. a loop cursor reused across millions of writes) where a plain
`CVM_WADDR` would drown you: the ring shows just the recent history, so you can
see a cursor advance by its stride and spot the exact write that goes wrong.

```sh
CVM_REDZONE=1 … CVM_RING_A=0x05906398 CVM_RING_B=0x0590639c  cronovm-headless cart.crom
```

### `CVM_TRIP_PC` (+ `CVM_TRIP_SP`) — capture a register at a PC

At `CVM_TRIP_PC`, snapshot `R[b]` of that instruction (e.g. the `this` pointer in
a `LDW Rd,[Rthis]`). `CVM_TRIP_SP=<sp>` restricts the capture to a single stack
frame (so a recursive function's deeper frames don't overwrite it). At the
redzone trap the captured pointer is reported with its first three heap words
(`__begin_`/`__end_`/`__cap_` of a `std::vector`, say) and the **last writer** of
its first word — pinning who wrote a corrupt pointer into the object.

### `CVM_PCDUMP` (+ `CVM_PCDUMP_RB` / `_R1` / `_SP`) — instruction dump

Dump `R[a]/R[b]/R[c]`, `SP`, the call args `R0/R1`, the return PC at `[SP]`,
`R8..R40`, and 16 bytes at `R[b]`, when execution reaches `CVM_PCDUMP=<pc>`.
Filter to a single occurrence with `CVM_PCDUMP_RB=<v>` (only when `R[b]==v`),
`CVM_PCDUMP_R1=<v>`, or `CVM_PCDUMP_SP=<v>` (only that stack frame).

### `CVM_WADDR`, `CVM_WVAL` — write watchpoints

- `CVM_WADDR=<addr>` — log every store (scalar or `memcpy`) overlapping `addr`,
  with size, value, `pc`, `func`.
- `CVM_WVAL=<val>` — log every 4-byte store of `val`, with the destination
  address, `pc`, `func`. (Useful to find where a known-garbage pointer is born.)

### `CVM_MISP` — misaligned-pointer-write detector

Flag a store or `memcpy` that writes a **misaligned** (`&3 != 0`) in-heap pointer
into a 4-aligned heap slot — the signature of a corrupted `__begin_`-style field.

- `CVM_MISP=1` — only when the slot previously held an aligned in-heap pointer.
- `CVM_MISP=2` — any such write.
- `CVM_MISP_TARGET=<val>` — watch one exact value over **both** stores and
  `memcpy` (the coverage `CVM_WVAL` lacks — it sees scalar stores only).
- `CVM_MISP_FUNC=<idx>`, `CVM_MISP_AND3=<n>` — additional filters.

## Worked example — the Exult spilled-alloca corruption

This toolkit pinned a multi-session bug in the Exult port. A cart faulted deep
in `Usecode_internal::run()`. The redzone trapped an out-of-bounds `memcpy` in a
`std::vector<Usecode_value>` relocate:

1. `CVM_REDZONE` → `[RZ-TRAP]` in `copy_internal`, plus `[RZ-HOLDERS2]` naming
   the slot holding the wild source pointer and **who last wrote it**.
2. `CVM_RING_A/B` on the two cursor slots → both advanced by a clean stride of
   20, proving the relocate codegen was *correct* — it was over-walking a
   garbage count.
3. `CVM_TRIP_PC` at the cursor-init load (SP-filtered to the faulting frame) →
   the vector `this` was `0x05`, `__begin_=0x23`, `__end_=0x01ae5c20` ⇒ a count
   of ~1.4 million → the relocate walked ~22 MB off the heap.
4. A second `CVM_TRIP_PC` at the caller's `this` load + the last-writer map →
   the garbage `this` came from `run()`'s prologue, where a spilled alloca
   pointer was written to the lost `CG_REG_SPILLED` sentinel register instead of
   its frame slot. `cvm-dis` of that prologue confirmed the broken
   `ADD R0, SP, off` sequence.

The conclusion (a stale pre-fix `cvm-translate` had produced the image) was only
reachable because each probe converted an invisible heap corruption into an
exact `pc`/`func`/register fact. See also [`docs/translator.md`](translator.md)
and the `cvm-dis` disassembler (`tools/cvm-dis/`), the natural companion tool.
