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

Each type may appear at most once except `DEBUG`. CODE is required.

## Heap layout

After loading, the heap is a single contiguous allocation of
`data_size + bss_size + heap_reserve` bytes:

```text
heap +----+----+----+----+----+----+----+----+----+----+
     | DATA bytes  | zeroed BSS  | free region for      |
     |             |             | the user allocator   |
     +----+----+----+----+----+----+----+----+----+----+
0          data_size      data_size+bss_size       heap_size
```

`heap_reserve` is the size of the free region; it comes from the optional
`HEAP_RESERVE` section. Binaries that don't include the section get a
heap of just `data_size + bss_size` bytes and have no run-time allocation
budget. See [memory.md](memory.md).

VM-side addresses are 32-bit byte offsets into this region. Loads and stores
are bounds-checked.

## What's outside the format (intentional)

- No relocations. The translator emits absolute heap offsets for data
  references, since the VM owns the address space.
- No ELF-style program headers, RPATH, or dynamic loading. There's exactly
  one binary per game.
- No symbol table for code addresses. CALL/RET (when added) will use absolute
  instruction indices encoded in the bytecode.
