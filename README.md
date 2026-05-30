# CronoVM

Embeddable virtual machine with a Clang-driven C toolchain. Spiritual
successor to the Quake III VM — same single-file embeddable
interpreter shape, but redesigned for 2020s constraints (32-bit
register file, IEEE 754 single-precision floats, predictable frame
times, no JIT, no GC).

CronoVM is a generic VM toolkit. The application that consumes it
(game, scripting host, plugin runtime, …) lives in a separate
project; this repository ships the runtime, the toolchain, and a
small set of runtime headers.

## What's in the box

| Component | Path | Role |
| --- | --- | --- |
| Interpreter | `src/cvm.c` (linked as `cvm` — single static library) | Loads `.bin` images, runs them, exposes a thread-safe-ish `cvm_run` |
| Translator | `tools/translator/cvm-translate` | Reads LLVM bitcode, emits CronoVM `.bin` |
| Wrapper | `tools/cvm-cc/cvm-cc` | Single-command driver: `cvm-cc x.c -o x.bin` |
| Runtime headers | `runtime/lib/` | `cvm_alloc.h` (free-list malloc/free), `cvm_intrin.h` (MULH, F2I_SAT, FSQRT), `cvm_int64.h` (soft i64), `cvm_float64.h` (soft double) |
| Example consumer | `examples/embedder/` | Minimal downstream pattern via `add_subdirectory` |

## Quick example

```c
/* game.c — compiled to game.bin by cvm-cc */
int game(int n) { return n + 42; }
```

```sh
cvm-cc game.c -o game.bin
```

```c
/* host.c — links cvm, runs game.bin */
#include "cvm.h"
#include <stdio.h>

int main(void) {
    extern uint8_t *slurp(const char *, size_t *);   // load file → buffer
    size_t   n;
    uint8_t *blob = slurp("game.bin", &n);

    struct cvm_image img;
    cvm_load(blob, n, &img);

    int32_t result;
    cvm_run(&img, &result);                          // result == 42
    printf("game returned %d\n", result);

    cvm_image_free(&img);
    free(blob);
}
```

A complete, runnable version of this example lives in
[`examples/embedder/`](examples/embedder).

## Building

Requires CMake ≥ 3.20 and a C11 compiler (clang, gcc, MSVC). The
translator and `cvm-cc` additionally need an LLVM dev install with
`llvm-config` reachable on PATH.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The translator subdir is conditional on `llvm-config` being
discoverable; if it isn't, the library and basic tests still build —
only the round-trip pipeline through `cvm-cc` requires LLVM.

A parallel ASAN build tree under `build-asan/` is the sanctioned way
to run the suite under AddressSanitizer; instructions in
[docs/NEXT.md](docs/NEXT.md).

## Consuming CronoVM as a dependency

Two patterns are supported today:

### `add_subdirectory()` (development / monorepo)

```cmake
# in your CMakeLists.txt
add_subdirectory(deps/CronoVM)            # or wherever you placed it
target_link_libraries(my_host PRIVATE cvm)
```

Targets you'll see:
- `cvm` — static library; this is what host code links against.
- `cvm-translate`, `cvm-cc` — buildable executables. Use them with
  `$<TARGET_FILE:cvm-cc>` in `add_custom_command` to compile bytecode
  during your build.

The `examples/embedder/` directory under this repo is a
self-contained mini-project that demonstrates the pattern end-to-end
and runs as part of the test suite.

### `find_package` (release / install)

```sh
# Build + install CronoVM into a prefix you control.
cmake -S CronoVM -B build -G Ninja
cmake --build build
cmake --install build --prefix /opt/cronovm
```

```cmake
# in your CMakeLists.txt
list(APPEND CMAKE_PREFIX_PATH /opt/cronovm)
find_package(CronoVM 0.1 REQUIRED)

target_link_libraries(my_host PRIVATE cronovm::cvm)

# Drive bytecode compilation from CMake — same shape as the
# add_subdirectory pattern, just with the namespaced target.
add_custom_command(
    OUTPUT  ${CMAKE_BINARY_DIR}/game.bin
    COMMAND $<TARGET_FILE:cronovm::cvm-cc>
            ${CMAKE_CURRENT_SOURCE_DIR}/game.c
            -o ${CMAKE_BINARY_DIR}/game.bin
    DEPENDS cronovm::cvm-cc ${CMAKE_CURRENT_SOURCE_DIR}/game.c
    VERBATIM
)
```

Targets exported under the `cronovm::` namespace mirror the
`add_subdirectory` set: `cronovm::cvm`, `cronovm::cvm-cc`, and —
when CronoVM was built with `llvm-config` available —
`cronovm::cvm-translate`. The package also defines
`CRONOVM_RUNTIME_DIR` for consumers that want to pass it as an
extra `-I` to clang directly (cvm-cc finds it automatically).

A complete worked example lives under
[`examples/installed_consumer/`](examples/installed_consumer) and
is exercised end-to-end by the `installed_*` ctest entries.

## C++ and the STL

`cvm-cc` compiles `.cpp/.cc/.cxx` as C++ and auto-links the C++ ABI
runtime (`runtime/lib/cvm_cxxrt.cpp`: `operator new/delete`, `__cxa_*`,
exceptions on setjmp/longjmp, RTTI). The standard library is the
toolchain clang's own **libc++** (`-stdlib=libc++`, so it is always
version-matched to the compiler — the most portable choice, since libc++
is co-versioned with clang). CronoVM overrides only two small headers in
`runtime/lib`, found ahead of the toolchain's copies:

- `__config_site` — freestanding configuration (no locale / wide chars /
  unicode / filesystem; hardening off; cooperative **external** thread
  API; the C library underneath is picolibc).
- `__external_threading` — the libc++ thread API mapped onto CronoVM's
  cooperative coroutines (mutexes are no-ops under no-preemption;
  `std::thread`/`std::condition_variable` are deferred — vector/string/
  map/memory/atomic do not use them).

C++ defaults to `-std=c++20`. Because libc++ ships its own wrapper
`<math.h>`/`<cstring>`/… that must shadow the C library's and
`#include_next` through, pass the C headers (picolibc/SDK) with
`-idirafter` (not `-I`) so they sit *below* libc++:

```sh
cvm-cc game.cpp -idirafter path/to/picolibc/libc/include -o game.bin
```

Use `--libcxx-dir=PATH` to pin an explicit libc++ `v1` header tree
instead of the toolchain's (e.g. for a hermetic/CI build).

## Runtime APIs

```c
const char *cvm_version_string(void);   /* "0.1.0" */
uint32_t    cvm_version_number(void);   /* 0x00010000 for 0.1.0 */

int  cvm_load(const void *bytes, size_t len, struct cvm_image *out);
int  cvm_load_ex(const void *bytes, size_t len, struct cvm_image *out,
                 const cvm_allocator_t *allocator);
void cvm_image_free(struct cvm_image *img);

int  cvm_link(struct cvm_image *img, const char *name,
              cvm_syscall_fn fn, void *user_data);

int  cvm_run     (struct cvm_image *img, int32_t *return_value);
int  cvm_run_args(struct cvm_image *img,
                  const int32_t *args, uint32_t arg_count,
                  int32_t *return_value);

int  cvm_image_get_region(struct cvm_image *img, const char *name,
                          uint32_t *out_offset, uint32_t *out_size);

int  cvm_heap_read (struct cvm_image *img, uint32_t addr,
                    void *out, size_t n);
int  cvm_heap_write(struct cvm_image *img, uint32_t addr,
                    const void *in, size_t n);
```

`cvm_load_ex` accepts a `cvm_allocator_t {alloc_fn, free_fn,
user_data}` so embedded targets without `malloc` can plug in
FreeRTOS heap_4, Zephyr `k_malloc`, a fixed-pool allocator, etc.

## Documentation

- [docs/isa.md](docs/isa.md) — full opcode reference (54 opcodes).
- [docs/format.md](docs/format.md) — `.bin` file format (header, section table, payload kinds).
- [docs/syscalls.md](docs/syscalls.md) — host-side syscall ABI and built-ins.
- [docs/memory.md](docs/memory.md) — heap layout, host-shared regions, allocator story.
- [docs/translator.md](docs/translator.md) — LLVM IR subset accepted, lowering rules, fixup pipeline.
- [docs/NEXT.md](docs/NEXT.md) — design constraints, roadmap, and what's deferred-with-reason.

## Project status

**Pre-1.0.** The binary format and ABI are not yet stabilised — minor
versions may break compatibility. 67 ctest cases cover the
opcode set (55 opcodes incl. FSQRT), codegen, translator subset,
host APIs, the soft-i64 / soft-double runtime headers, and both
consumer patterns (`add_subdirectory` and `find_package` against
a freshly-installed prefix); release build clean.

## License

MIT — see [LICENSE](LICENSE). You can vendor CronoVM into commercial or
closed-source projects; the only requirement is to preserve the copyright
notice in your distribution.

## Origin

Successor to a fork of the Quake III VM
([github.com/Cronomantic/q3vm](https://github.com/Cronomantic/q3vm)).
The decision history that led to this repo's architecture lives in
that fork's design conversation, not reproduced here. See [CLAUDE.md](CLAUDE.md)
for the project's guiding principles.
