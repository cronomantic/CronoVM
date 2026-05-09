# Binary format (v1.0)

CronoVM binaries (`.bin` by convention) are little-endian on disk. A binary
consists of a **fixed header**, a **section table**, and one or more
**section payloads**. Sections are referenced by file offset, so payloads can
appear in any order after the section table.

```text
+----------------+
|    Header      |  24 bytes
+----------------+
| Section table  |  16 bytes × section_count
+----------------+
|   Payloads     |  CODE / DATA / IMPORTS / DEBUG
|      ...       |
+----------------+
```

## Header

| Offset | Size | Field | Meaning |
| -----: | ---: | ----- | ------- |
| 0 | 4 | `magic` | The four bytes `'C','V','M','1'`. |
| 4 | 4 | `version` | `0x00010000` for v1.0. |
| 8 | 4 | `flags` | Reserved, must be `0`. |
| 12 | 4 | `section_count` | Number of entries in the section table. |
| 16 | 4 | `section_table_off` | Absolute file offset to the section table. |
| 20 | 4 | `entry` | Instruction index into the CODE section where execution starts. |

## Section table entry (16 bytes)

| Offset | Size | Field | Meaning |
| -----: | ---: | ----- | ------- |
| 0 | 4 | `type` | Section type (see below). |
| 4 | 4 | `file_off` | Absolute file offset to payload. `0` for BSS. |
| 8 | 4 | `size` | Payload size in bytes (BSS: virtual size). |
| 12 | 4 | `flags` | Reserved, must be `0`. |

### Section types

| ID | Name | Required? | Notes |
| -: | ---- | --------- | ----- |
| 1 | `CODE` | yes | Array of 32-bit instructions. Size must be a multiple of 4. |
| 2 | `DATA` | no | Initial bytes copied to the start of the heap. |
| 3 | `BSS` | no | Zero-filled bytes appended after DATA on the heap. `file_off` must be 0. |
| 4 | `IMPORTS` | no | Symbol table of host syscalls; see [syscalls.md](syscalls.md). |
| 5 | `DEBUG` | no | Opaque blob ignored by the loader. |
| 6 | `HEAP_RESERVE` | no | Zero-filled free region appended after BSS for the user-side allocator; see [memory.md](memory.md). `file_off` must be 0. |
| 7 | `STACK_RESERVE` | no | Zero-filled stack region above the heap for `CALL`/`RET`. `file_off` must be 0; `size` must be a multiple of 4 and at least 4 (room for the run-completion sentinel). |
| 8 | `FUNCS` | no | `u32[N]` array of function entry points (instruction indices into CODE), indexed by `CALL imm24`. Required if any `CALL` instruction is emitted. `size` must be a multiple of 4. **Slot 0 is reserved as the null-function-pointer trap target** — the interpreter rejects `CALL imm24=0` / `CALLR R[A]=0` with `CVM_E_NULL_FUNC_PTR` before any indexing, so the value at offset 0 is unused (the translator writes 0). User function `k` (in source order) lives at `FUNCS[k+1]`. |

Each type may appear at most once except `DEBUG`. CODE is required.

## Memory layout

After loading, the VM's address space is a single contiguous allocation of
`data_size + bss_size + heap_reserve + stack_reserve` bytes:

```text
mem  +-----+-----+--------+--------+
     |DATA |BSS  |RESERVE |STACK   |
     +-----+-----+--------+--------+
0       data    data+bss heap_size mem_size
```

The **heap** (DATA + BSS + RESERVE) holds initialised globals, zero
globals, and the user-side allocator's free region. The **stack region**
(STACK_RESERVE) sits immediately above and is used by `CALL`/`RET`; SP
(`R255`) starts at `mem_size` and grows downward. Binaries that don't
include `HEAP_RESERVE` have no allocator budget; binaries that don't
include `STACK_RESERVE` cannot use `CALL`/`RET`. See [memory.md](memory.md).

VM-side addresses are 32-bit byte offsets into this region. Loads and
stores are bounds-checked against `mem_size`, so the same opcodes can
access either the heap or the stack region.

## What's outside the format (intentional)

- No relocations. The translator emits absolute heap offsets for data
  references, since the VM owns the address space.
- No ELF-style program headers, RPATH, or dynamic loading. There's exactly
  one binary per game.
- No symbol table for code addresses. `CALL imm24` indexes a `FUNCS` table
  whose entries are absolute instruction indices into CODE; the loader
  populates the table once at load time.
