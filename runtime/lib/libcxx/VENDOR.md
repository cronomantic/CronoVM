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
  as headers: the `<iostream>`/`<locale>` library (`locale.cpp`, `ios*.cpp`,
  `iostream.cpp`, `ostream.cpp`, `fstream.cpp`, `system_error.cpp`,
  `error_category.cpp`) + their private `include/`. Built into `cxxio.bc` by
  `build_cxxio.sh`. (The STL exception/string out-of-line bits are hand-written in
  `cvm_cxxstl.cpp`, not vendored.)

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
