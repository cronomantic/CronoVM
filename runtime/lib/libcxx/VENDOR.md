# Vendored libc++ (CronoVM toolchain)

CronoVM is a **hermetic, reproducible** toolchain: just as it vendors **picolibc**
(the C library, a pinned submodule), it vendors **libc++** here so the C++ surface
does NOT depend on whatever libc++ the host clang happens to ship. `cvm-cc`
compiles C++ against *this* tree (`-nostdinc++ -isystem include/v1`), not the
system libc++, so any reasonably recent clang (≈ clang 21+) produces identical
results — independent of the distro's libc++ version. (Before this, depending on
the system libc++ broke the linux-clang/linux-gcc CI when it ran clang-21 against
22.x-pinned src — see ci-runs-failing.)

## What's here

- `include/v1/` — the full libc++ header tree (`__config_site` excepted: CronoVM's
  freestanding override lives in `runtime/lib/__config_site` and wins via `-I`).
- `src/` — the out-of-line library TUs CronoVM needs that the toolchain ships only
  as headers. Built by `build_cxxio.sh` into TWO modules:
  - **`cxxio.bc`** (auto-linked by `cvm-cc` on the iostream/locale probe): the
    `<iostream>`/`<locale>` library (`locale.cpp`, `ios*.cpp`, `iostream.cpp`,
    `ostream.cpp`, `fstream.cpp`, `system_error.cpp`, `error_category.cpp`),
    `hash.cpp` (`__next_prime` + `__hash_memory`, the `std::unordered_map` deps),
    and `regex.cpp` (`std::regex`) + their private `include/`.
  - **`cxxfs.bc`** (NOT auto-linked — a cart that uses `std::filesystem` links it
    EXPLICITLY, plus a POSIX backend): the `filesystem/` subset (`operations.cpp`,
    `directory_iterator.cpp`, `directory_entry.cpp`, `path.cpp`,
    `filesystem_error.cpp`, `filesystem_clock.cpp` + the private `filesystem/*.h`).
    Kept out of `cxxio.bc` because its POSIX FS calls (`open`/`stat`/`openat`/…)
    can only be satisfied by an embedder, so folding it in would break every
    iostream consumer (e.g. the `conf_pico_cpp_iostream` fixture).
  (The STL exception/string out-of-line bits are hand-written in `cvm_cxxstl.cpp`,
  not vendored.)

- **Local `[CRONOPIO]` edits to the vendored tree** (grep `[CRONOPIO]`; re-apply on
  every bump):
  - `include/v1/__algorithm/simd_utils.h` — `_LIBCPP_HAS_ALGORITHM_VECTOR_UTILS`
    forced to 0 (the VM has no vector types; libc++'s SIMD `find`/`count`/… would
    emit `<N x iM>` ops it can't lower).
  - `src/regex.cpp` — ClassNames ctype-mask table narrowed through `unsigned char`
    (picolibc's `ctype_base::mask` is signed; `blank`=0x80 would sign-extend).

## Version

**libc++ `llvmorg-22.1.6`** (`_LIBCPP_VERSION 220106`). The headers and the `src/`
subset MUST be the same release (the src uses private symbols — e.g.
`__atoms_offset` — that only exist in the matching headers).

## Regenerating / bumping

Run `vendor_libcxx.sh` (next to this file). It copies a v1 header tree from an
installed libc++ of the desired version and fetches the matching `src/` subset
from the corresponding `llvmorg-*` tag. To bump libc++, install that clang/libc++,
re-run the script pointing at its `c++/v1`, and rebuild + run the conformance
suite.
