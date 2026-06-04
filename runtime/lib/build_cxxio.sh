#!/usr/bin/env bash
# build_cxxio.sh — compile the vendored libc++ iostream/locale src subset
# (runtime/lib/libcxx/src/) into one LLVM bitcode module, cxxio.bc.
#
# The CronoVM toolchain uses the libc++ HEADERS that ship with clang but does
# NOT build libc++; for the freestanding STL core (vector/string/exceptions) the
# out-of-line bits are hand-written in cvm_cxxstl.cpp. <iostream>/<locale>, by
# contrast, have a large library surface (the locale facets, the stream classes,
# the global cin/cout objects) that is impractical to hand-roll — so we VENDOR
# the matching LLVM-22.1.x libc++ src subset and compile it here, exactly the way
# build_picolibc.sh compiles a curated picolibc subset.
#
# cxxio.bc is a gitignored artifact (like picolibc.bc). cvm-cc auto-links it when
# a program references the iostream/locale ABI (the CVM_PROBE_IOSTREAM probe bit),
# so C carts and C++ carts that don't touch iostream never pay for it. Its
# undefined symbols are satisfied at link time by cvm_cxxrt (operator new / the
# __cxa_* EH+RTTI ABI), cvm_cxxstl (basic_string + the std exception hierarchy),
# and picolibc.bc (malloc/stdio/errno + the xlocale `*_l` functions — build that
# with --with-locale).
#
# Usage: build_cxxio.sh        (env: CLANG / CLANGXX / LLVM_LINK override PATH)
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Vendored libc++ (see libcxx/VENDOR.md): the pinned header tree + the src subset.
# We compile against THIS, not the host's libc++ — hermetic + reproducible across
# clang versions (any ~clang 21+).
V1="$HERE/libcxx/include/v1"
SRC="$HERE/libcxx/src"
PICO_INC="$HERE/../../external/picolibc/libc/include"
OUT="$HERE/cxxio.bc"
OPT="${CXXIO_OPT:--O1}"
VERBOSE="${VERBOSE:-0}"

CLANG="${CLANG:-clang}"
# Derive clang++ from CLANG unless given explicitly.
if [ -z "${CLANGXX:-}" ]; then
  d="$(dirname "$CLANG")"; b="$(basename "$CLANG")"
  case "$b" in
    clang)     CLANGXX="$d/clang++" ;;
    clang.exe) CLANGXX="$d/clang++.exe" ;;
    *)         CLANGXX="clang++" ;;
  esac
  [ -x "$CLANGXX" ] || command -v "$CLANGXX" >/dev/null || CLANGXX="clang++"
fi
LLVM_LINK="${LLVM_LINK:-llvm-link}"
command -v "$LLVM_LINK" >/dev/null || { echo "llvm-link not found" >&2; exit 1; }

[ -d "$V1" ] || { echo "build_cxxio.sh: vendored libc++ not found at $V1 (run libcxx/vendor_libcxx.sh)" >&2; exit 1; }

# Compile against the VENDORED libc++ ($V1) — independent of the host's libc++.
# Our freestanding <__config_site> (LOCALIZATION 1 + NEWLIB 1) via -I "$HERE" wins
# over the vendored tree's; the src's private headers via libcxx/src/include;
# picolibc as -isystem AFTER libc++ so libc++'s wrapper <cstdio>/<cstring>/... win
# and their #include_next reaches picolibc. picolibc MUST be -isystem (NOT
# -idirafter): -idirafter would place it BELOW the host C library, so glibc's
# /usr/include/string.h would be found first on Linux ("bits/libc-header-start.h").
# (Now that libc++ is vendored too, the version-skew that previously broke the
# Linux CI is gone — any clang 21+ compiles this. See ci-runs-failing.)
# RTTI ON: a real C++ program (e.g. the Exult engine) subclasses the iostream
# classes (custom streambuf for a ROM/SDL_IOStream source) and uses dynamic_cast,
# so the library must emit the iostream/locale class type_info — like a normal
# libc++ build. (-fno-rtti here left those externally-undefined: "typeinfo for
# std::basic_istream<char> has no initializer".)
CXXFLAGS=(--target=i386-elf -ffreestanding -std=c++23 -mlong-double-64
          -nostdinc++ -isystem "$V1"
          -I "$HERE" -I "$SRC/include" -isystem "$PICO_INC"
          -D_LIBCPP_BUILDING_LIBRARY -D_GNU_SOURCE -DNDEBUG
          -emit-llvm -gline-tables-only "$OPT")

# The vendored iostream/locale TUs -> cxxio.bc (auto-linked by cvm-cc on the
# iostream probe, so EVERY iostream cart links it). MUST stay free of any
# embedder/POSIX dependency.
SOURCES=(locale ios ios.instantiations iostream ostream fstream
         system_error error_category hash regex)

# The vendored std::filesystem library -> a SEPARATE cxxfs.bc. It calls a POSIX
# FS surface (open/stat/openat/lstat/readlink/...) that only an embedder (a
# cart's cron_sys.c + a small ENOSYS shim) can satisfy, so it must NOT live in
# cxxio.bc — otherwise every iostream consumer (incl. the conformance iostream
# fixture, which links no cron_sys.c) would fail translation on the undefined
# POSIX calls. cvm-cc does NOT auto-link cxxfs.bc; a cart that uses
# std::filesystem links it EXPLICITLY (Exult's build does) alongside its POSIX
# backend. See exult-gamewin-phase1 / fs_posix_stubs.c.
FS_SOURCES=(filesystem/operations filesystem/directory_iterator
            filesystem/directory_entry filesystem/path
            filesystem/filesystem_error filesystem/filesystem_clock)
FS_OUT="$HERE/cxxfs.bc"

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

# Compile a source group into bitcode, appending each .bc to the named array.
# Usage: compile_group <array_name> <sources...>
nfail=0
compile_group() {
  local _arr="$1"; shift
  local s src bc
  for s in "$@"; do
    src="$SRC/$s.cpp"
    if [ ! -f "$src" ]; then echo "  SKIP (no source): $s" >&2; continue; fi
    bc="$WORK/$(echo "$s" | tr ./ __).bc"
    [ "$VERBOSE" = 1 ] && echo "$CLANGXX ${CXXFLAGS[*]} -c $src -o $bc"
    if "$CLANGXX" "${CXXFLAGS[@]}" -c "$src" -o "$bc" 2>"$bc.err"; then
      eval "$_arr+=(\"\$bc\")"
    else
      echo "  FAIL: $s" >&2; grep -m3 'error:' "$bc.err" >&2 || true; nfail=$((nfail+1))
    fi
  done
}

BCS=();  compile_group BCS  "${SOURCES[@]}"
FSBCS=(); compile_group FSBCS "${FS_SOURCES[@]}"
[ "$nfail" -eq 0 ] || { echo "build_cxxio.sh: $nfail source(s) failed" >&2; exit 1; }

"$LLVM_LINK" "${BCS[@]}" -o "$OUT"
echo "build_cxxio.sh: wrote $OUT (${#BCS[@]} sources)"
"$LLVM_LINK" "${FSBCS[@]}" -o "$FS_OUT"
echo "build_cxxio.sh: wrote $FS_OUT (${#FSBCS[@]} sources, std::filesystem — link explicitly)"
