# Memory & dynamic allocation

CronoVM does **not** ship a heap allocator. The VM gives the program a single
contiguous block of memory; whatever `malloc`/`free` semantics a game wants
are provided by **its own C code**, compiled into the binary.

This document explains how that works, what the VM exposes, and what the
reference allocator does for games that don't want to roll their own.

## The big picture

```text
heap +------+-------+----------+--------------+
     | DATA |  BSS  | REGIONS  | free region  |
     +------+-------+----------+--------------+
0       data    data+bss    heap_start       heap_size
                            (cvm_sys_heap_start)
```

- `DATA` and `BSS` come from the user's compiled C code, just like in a
  hosted environment. Globals and statics live there.
- `REGIONS` are the host-shared slices declared by the binary via
  `--region=name:size:dir`. The loader assigns each one a heap-relative
  offset; the binary discovers it at runtime with `cvm_sys_get_region`,
  the host gets the same offset via `cvm_image_get_region`. See
  [regions](#host-shared-regions) below.
- The **free region** beyond regions is what `malloc` / `free` operate on.
  Its size is declared by the binary itself (see "HEAP_RESERVE section"
  below). If the binary doesn't ask for any, there is no free region.
- Bounds checking applies across the entire heap; the user-side allocator
  is sandboxed automatically.

## The principle: no allocator inside the VM

CLAUDE.md is explicit:

> "No GC, no heap allocator built into the VM. Host-provided memory only."

So `malloc` / `free` *must* live somewhere outside the VM core. The two
candidates are:

| Approach | Where the allocator runs | Trade-off |
| -------- | ------------------------ | --------- |
| User-side (chosen) | Compiled into the user's `.bin` | Predictable; no host-call cost; allocator metadata is sandboxed in the heap |
| Host-side via syscall | In the host C program | Each `malloc` is a host call; allocator policy is fixed by the host, not the game |

CronoVM takes the user-side path because it stays true to "predictable
performance" — the cost of `malloc(n)` is exactly the cost of whichever
allocator the game compiled in, with no boundary crossings.

## What the VM exposes

To make the user-side allocator work, the VM exposes two pieces of
information at run time, via syscalls:

```c
extern int32_t cvm_sys_heap_start(void);  // first byte after DATA+BSS
extern int32_t cvm_sys_heap_size (void);  // size of the free region in bytes
```

Both are *built-in* syscalls — the runtime registers default
implementations on every loaded image; the host doesn't need to bind them.
A game's startup code can call them once to initialise its allocator and
then never touch them again.

Why two syscalls and not a translator-resolved constant? Because the same
`.bin` may be loaded with different free-region sizes (a host can grant
more or less memory) — querying at run time is the only thing that always
gives the right answer.

## The HEAP_RESERVE section (binary format addition)

A binary asks for free heap by emitting a `HEAP_RESERVE` section:

| Field | Value |
| ----- | ----- |
| `type` | `6` (CVM_SEC_HEAP_RESERVE) |
| `file_off` | `0` (no on-disk payload) |
| `size` | bytes of free heap requested |
| `flags` | `0` |

The loader allocates `data_size + bss_size + heap_reserve` bytes total
and zeros the entire reserve region. Binaries that don't include this
section get a free region of size 0 and must use static allocation only.

This is an *additive* change to the v1.0 format: pre-existing v1.0 loaders
that don't know section type 6 simply ignore it. The version stays at 1.0.

The translator opts a binary into a reserve via:

```sh
cvm-translate user.bc -o game.bin --heap-reserve=4M
```

(The flag is sketched here; the syntax may evolve.)

## Host-shared regions

A binary can declare named slices of its heap that are visible to the
host program. Use cases the engine tier needs regardless of renderer
direction:

| Pattern | Direction | Example use |
| ------- | --------- | ----------- |
| Framebuffer | `w` (VM writes, host reads) | Software renderer pixel buffer the host blits to a window. |
| Command buffer | `w` (VM writes, host reads) | Display-list / draw calls the host consumes at `present()`. |
| Input state | `r` (host writes, VM reads) | Per-frame controller / keyboard snapshot. |
| Audio buffer | `w` (VM writes, host reads) | PCM samples the host mixes into the output stream. |
| Shared comms | `rw` (both) | Anything where both sides update in place. |

The translator declares them with `--region=name:size[:dir]` (default
direction is `rw`). The loader carves them out of the heap between BSS
and HEAP_RESERVE, in declaration order, with each size rounded up to a
4-byte multiple. Names are at most 15 visible chars (16 bytes including
the trailing NUL).

The `direction` field is **informational** — the VM only enforces heap
bounds. A future host wrapper can use it to flag misuse (a `w` region
read by the binary, etc.); the runtime itself does not.

The binary discovers a region's offset at runtime:

```c
extern int cvm_sys_get_region(const char *name);

int main(void) {
    int fb_off = cvm_sys_get_region("fb");   /* -1 if not declared */
    unsigned char *fb = (unsigned char *)fb_off;
    fb[0] = 0xFF;
    return 0;
}
```

The host discovers the same offset via `cvm_image_get_region`:

```c
uint32_t fb_off, fb_size;
if (cvm_image_get_region(&img, "fb", &fb_off, &fb_size) == CVM_OK) {
    /* img.heap + fb_off is the region's first byte */
}
```

`cvm_sys_get_region` returns `-1` for an unknown name (offset 0 cannot
name a region — regions always sit after DATA+BSS, which together
occupy at least one byte for any non-trivial binary, and the loader
rejects empty region names). `cvm_image_get_region` returns
`CVM_E_NO_SUCH_REGION` in the same case.

## Reference allocator (`runtime/lib/cvm_alloc.h`)

Games that don't want to bring their own allocator can drop in the
reference one. It's a header-only library — `#include "cvm_alloc.h"`
and the helpers compile into the user binary:

```sh
clang -emit-llvm -O1 -I runtime/lib -c game.c -o game.bc
cvm-translate game.bc -o game.bin --heap-reserve=4M
```

Surface:

```c
void  cvm_alloc_init(void);                /* call once at startup */
void *cvm_malloc(uint32_t bytes);          /* returns NULL if no space */
void  cvm_free(void *p);                   /* p must come from cvm_malloc */
```

The reference implementation is a small free-list / first-fit allocator —
no fragmentation guarantees beyond what first-fit gives you, no thread
safety (single-threaded by design). For a game that needs better
behaviour (real-time predictability, fixed-size pools, frame allocators),
**rolling your own is the expected path** — the reference is a starting
point, not a moral default.

A game can also just `#define malloc cvm_malloc` in a wrapper header so
that ported C code that uses `malloc`/`free` works unchanged.

## Patterns the project endorses

For game code, prefer in this order:

1. **Static allocation.** Declare exactly what you need at file scope.
   Best predictability; most retro.
2. **Arena/bump.** Allocate a big block, sub-allocate, reset between
   levels or frames. Cheap, easy, fragmentation-free.
3. **Reference allocator.** When you genuinely need free-anything-anytime
   semantics.
4. **Custom allocator.** When the reference one isn't right for your
   shape of allocations.

Avoid frame-by-frame `malloc` from the general-purpose allocator. The cost
is small but it's not zero, and predictable frame time matters more than
convenience here.

## What's not provided

- No `realloc` in the v1 surface (callers can `cvm_malloc` + `memcpy` +
  `cvm_free`).
- No `calloc` (callers zero memory themselves; for fixed-size init,
  static arrays are usually right).
- No allocation tracking, no leak detection. A debug build of the
  reference allocator may add this later; the runtime won't.
- No NULL-on-OOM panic in the runtime. `cvm_malloc` returns NULL and the
  game decides what to do.

## Status

Implemented. The loader recognises `HEAP_RESERVE`, allocates the extra
memory, and zero-fills it. `cvm_sys_heap_start` and `cvm_sys_heap_size`
are auto-bound built-in syscalls registered on every load. The
translator accepts `--heap-reserve=N[K|M]` and emits the section.

`runtime/lib/cvm_alloc.h` ships a header-only **first-fit free-list
allocator**: `cvm_alloc_init` / `cvm_malloc` / `cvm_free`. Each block
carries a one-word header (size + free flag in the low bit, no boundary
tags); `cvm_malloc` walks the heap and splits if the remainder is at
least one minimum block; `cvm_free` marks the block free and runs a
single forward coalesce pass merging every adjacent free pair. The
helpers are `__attribute__((noinline))` so the loop doesn't get inlined
into every call site — that would otherwise blow past the translator's
register budget the moment a function makes more than a couple of
allocations.
