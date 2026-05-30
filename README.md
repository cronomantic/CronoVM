# CronoVM

**A small, deterministic bytecode virtual machine with a real C/C++ toolchain.**

Write a program in C or C++, compile it with Clang, and CronoVM turns the LLVM
bitcode into a compact `.bin` image that runs identically on any host —
desktop, server, or microcontroller. The runtime is a single static library
with no dependencies, no JIT, and no garbage collector, so execution timing
stays predictable and the whole VM embeds in a few hundred kilobytes.

It is a **general-purpose VM toolkit**, deliberately decoupled from any one
application. Typical uses: sandbox untrusted or hot-swappable logic, ship
portable plugins, embed a deterministic scripting or simulation layer, or run
one compiled module unchanged across very different hosts. Whatever embeds it —
an application, a scripting host, a plugin runtime — lives in its own project;
this repository ships the runtime, the toolchain, and a small set of runtime
support libraries.

---

## Highlights

- **Real languages, real standard library.** Compile C (C99/C11) and C++20.
  The C standard library is [picolibc](https://github.com/picolibc/picolibc);
  the C++ standard library is LLVM's **libc++** — `std::vector`, `std::string`,
  `std::map`, `<memory>` smart pointers and `std::atomic` all work, along with
  exceptions and RTTI. See [Language support](#language-support).
- **Deterministic by design.** A bytecode interpreter with no JIT and no GC:
  no compilation pauses, no collection pauses, reproducible behaviour run to
  run and host to host — ideal for deterministic, reproducible workloads.
- **Genuinely embeddable.** The runtime (`cvm`) is one static library in C11
  with zero third-party dependencies. Bring your own allocator (or none) via a
  hook, so it drops into FreeRTOS, Zephyr, or a bare-metal pool allocator.
- **Portable images.** A `.bin` compiled once runs unchanged on every host —
  same results on x86-64, ARM, or RISC-V, big- or little-endian host, because
  the VM defines its own 32-bit little-endian execution model.
- **A compact, well-specified ISA.** A register machine (256 registers,
  IEEE-754 binary32 floats native, 64-bit integers and doubles via small
  software runtimes) with a fully documented opcode set — see
  [docs/isa.md](docs/isa.md).
- **Cooperative concurrency.** A single coroutine-swap opcode lets a program
  build fibers, generators, or a cooperative scheduler on top — the basis for
  `std::thread`-style APIs without preemption.
- **Sandboxed.** Every memory access is bounds-checked against the image's
  regions; the guest can only reach the host through explicitly linked syscalls.

## Quick example

```c
/* program.c — compiled to program.bin by cvm-cc */
int main(int n) { return n + 42; }
```

```sh
cvm-cc program.c -o program.bin
```

```c
/* host.c — links the cvm static library, runs program.bin */
#include "cvm.h"
#include <stdio.h>

int main(void) {
    extern uint8_t *slurp(const char *, size_t *);   /* read file -> buffer */
    size_t   n;
    uint8_t *blob = slurp("program.bin", &n);

    struct cvm_image img;
    cvm_load(blob, n, &img);

    int32_t result;
    cvm_run(&img, &result);                           /* result == 42 */
    printf("program returned %d\n", result);

    cvm_image_free(&img);
    free(blob);
}
```

A complete, runnable version lives in [`examples/embedder/`](examples/embedder).

## How it works

```text
  program.c / program.cpp
        │   clang -emit-llvm
        ▼
  LLVM bitcode (.bc)
        │   cvm-translate          (build-time only; no LLVM in the shipped runtime)
        ▼
  program.bin  ──cvm_load / cvm_run──▶  cvm  (static interpreter, linked into the host)
```

`cvm-cc` is a one-shot driver that runs the whole left side for you
(`clang | llvm-link | cvm-translate`), including multi-file builds. **The
translator and Clang are build-time tools only** — the `cvm` library you ship
has no LLVM dependency.

## Building

Requires CMake ≥ 3.20 and a C11 compiler (Clang, GCC, or MSVC). The translator
and `cvm-cc` additionally need an LLVM development install with `llvm-config`
on `PATH`.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

If `llvm-config` isn't found, the runtime library and its tests still build;
only the bytecode-compilation pipeline (`cvm-translate` / `cvm-cc`) is skipped.

CI builds and tests on Linux (Clang + GCC), Windows (MSYS2 UCRT Clang) and
macOS, plus bare-metal sanity builds for `thumbv6m`, `thumbv7m` and `rv32imc`.

## What's in the box

| Component | Path | Role |
| --- | --- | --- |
| Interpreter | `src/cvm.c` → `cvm` static lib | Loads and runs `.bin` images; the only thing a host links against |
| Translator | `tools/translator/` → `cvm-translate` | LLVM bitcode → CronoVM bytecode (build-time only) |
| Compiler driver | `tools/cvm-cc/` → `cvm-cc` | One-command `clang \| llvm-link \| cvm-translate` for C/C++ |
| C library | `external/picolibc` + `runtime/lib/build_picolibc.sh` | picolibc compiled to a reusable bitcode module |
| C++ ABI + STL glue | `runtime/lib/cvm_cxxrt.cpp`, `cvm_cxxstl.cpp` | `operator new`/`__cxa_*`, exceptions, RTTI; the out-of-line std exception + shared-ptr surface |
| Soft runtimes | `runtime/lib/cvm_{int64,float64}_rt.c`, `*.h` | 64-bit integer and `double` arithmetic; intrinsics (MULH, FSQRT, …) |
| Coroutines | `runtime/lib/`/SDK `coro.h` | Cooperative context-swap primitives |

## Consuming CronoVM as a dependency

### `add_subdirectory()` — development / monorepo

```cmake
add_subdirectory(deps/CronoVM)
target_link_libraries(my_host PRIVATE cvm)
```

Targets: `cvm` (the static library host code links), and the build-time tools
`cvm-translate` / `cvm-cc` (drive them from `add_custom_command` via
`$<TARGET_FILE:cvm-cc>`). [`examples/embedder/`](examples/embedder) shows the
full pattern and runs as part of the test suite.

### `find_package` — release / install

```sh
cmake -S CronoVM -B build -G Ninja
cmake --build build
cmake --install build --prefix /opt/cronovm
```

```cmake
list(APPEND CMAKE_PREFIX_PATH /opt/cronovm)
find_package(CronoVM 0.4 REQUIRED)
target_link_libraries(my_host PRIVATE cronovm::cvm)

add_custom_command(
    OUTPUT  ${CMAKE_BINARY_DIR}/program.bin
    COMMAND $<TARGET_FILE:cronovm::cvm-cc>
            ${CMAKE_CURRENT_SOURCE_DIR}/program.c
            -o ${CMAKE_BINARY_DIR}/program.bin
    DEPENDS cronovm::cvm-cc ${CMAKE_CURRENT_SOURCE_DIR}/program.c
    VERBATIM)
```

Exported under the `cronovm::` namespace: `cronovm::cvm`, `cronovm::cvm-cc`,
and — when built with `llvm-config` available — `cronovm::cvm-translate`. The
package also defines `CRONOVM_RUNTIME_DIR`. A worked example lives in
[`examples/installed_consumer/`](examples/installed_consumer), exercised by the
`installed_*` tests.

## Language support

### C

`cvm-cc file.c -o file.bin`. The C standard library is picolibc, compiled to a
bitcode module and linked automatically — the full C99/C11 surface (string,
`stdlib`, `ctype`, numerics, `stdio` via tinystdio, 64-bit integer routines).
The host supplies a tiny machine port (errno, allocator, the `open`/`read`/
`write`/`lseek`/`close` backend, `exit`); everything else is portable.

### C++

`cvm-cc file.cpp -o file.bin` compiles as C++20 and auto-links the C++ ABI
runtime (`cvm_cxxrt`: `operator new`/`delete`, `__cxa_*`, exceptions lowered
onto setjmp/longjmp, RTTI / `dynamic_cast`). The standard library is the
toolchain Clang's own **libc++** (`-stdlib=libc++`, always version-matched to
the compiler). Working today: `std::vector`, `std::string`, `std::map`,
`<memory>` (`unique_ptr`/`shared_ptr`/`weak_ptr`), and the full integer/pointer
`std::atomic` surface (load/store/exchange/RMW/`compare_exchange`), plus
`throw`/`catch` across the std exception hierarchy.

CronoVM overrides just two small headers (in `runtime/lib`, found ahead of the
toolchain copies): a freestanding `__config_site` (no locale/wide/filesystem;
hardening off; cooperative threads) and `__external_threading` (the libc++
thread API mapped onto CronoVM coroutines — mutexes are no-ops with no
preemption; `std::thread`/`std::condition_variable` are not yet wired).

Because libc++ ships wrapper headers (`<math.h>`, `<cstring>`, …) that must
shadow the C library's and `#include_next` through to them, pass the C headers
**below** libc++ — either with `-idirafter`, or, where a host C library
(e.g. glibc) is also on the search path, point both at explicit `-isystem`
dirs with libc++ first:

```sh
cvm-cc program.cpp -idirafter path/to/picolibc/libc/include -o program.bin
```

Use `--libcxx-dir=PATH` to pin an explicit libc++ `v1` header tree (e.g. for a
hermetic or CI build) instead of the toolchain's.

## Runtime API

```c
const char *cvm_version_string(void);   /* "0.4.0" */
uint32_t    cvm_version_number(void);   /* 0x00040000 for 0.4.0 */

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

int  cvm_heap_read (struct cvm_image *img, uint32_t addr, void *out, size_t n);
int  cvm_heap_write(struct cvm_image *img, uint32_t addr, const void *in, size_t n);
```

`cvm_link` binds a host C function to a name the guest program imports — its
only channel to the outside world. `cvm_load_ex` takes a `cvm_allocator_t
{alloc_fn, free_fn, user_data}` so targets without `malloc` can plug in their
own (FreeRTOS heap_4, Zephyr `k_malloc`, a fixed pool, …).

## Documentation

- [docs/isa.md](docs/isa.md) — instruction set: encoding, the full opcode table, floats, calls, coroutines.
- [docs/format.md](docs/format.md) — the `.bin` image format (header, section table, payloads).
- [docs/memory.md](docs/memory.md) — address space, heap, stack, host-shared regions, the allocator hook.
- [docs/syscalls.md](docs/syscalls.md) — the host syscall ABI and how to link host functions.
- [docs/translator.md](docs/translator.md) — the LLVM IR subset accepted and how it is lowered.
- [CHANGELOG.md](CHANGELOG.md) — release history.
- [docs/NEXT.md](docs/NEXT.md) — internal design log: rationale, roadmap, and deferred work (references the project that drove CronoVM's development).

## Project status

**Beta (0.4.x).** The runtime, toolchain and language support are stable enough
for real projects, and the test suite is comprehensive: 137 CTest cases cover
the interpreter, the translator's accepted IR subset, the soft i64/double
runtimes, the host APIs and both consumer integration patterns, alongside a
**28-fixture differential conformance corpus** that compiles each fixture for
the VM *and* natively and compares results bit-for-bit — so toolchain gaps and
miscompiles are caught proactively. CI is green on Linux, Windows and macOS,
with bare-metal sanity builds for Cortex-M and RISC-V.

Being pre-1.0, the binary format and ABI may still change on a minor-version
bump; such breaks are called out under **Breaking** in the changelog.

## License

MIT — see [LICENSE](LICENSE). CronoVM can be vendored into commercial or
closed-source projects; the only requirement is to preserve the copyright
notice in your distribution.

## Origin

Successor to a fork of the Quake III VM
([github.com/Cronomantic/q3vm](https://github.com/Cronomantic/q3vm)). See
[CLAUDE.md](CLAUDE.md) for the project's guiding principles.
