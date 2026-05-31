#!/usr/bin/env bash
# build_cxxio.sh — compile the vendored libc++ iostream/locale subset
# (runtime/lib/libcxx-src/) into one LLVM bitcode module, cxxio.bc.
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
SRC="$HERE/libcxx-src"
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

# Locate the toolchain's libc++ v1 header dir (the <isystem> for -nostdinc++).
# `clang++ -print-resource-dir` gives .../lib/clang/NN; the c++/v1 tree is a few
# levels up under include/. Probe the common layouts.
V1=""
for cand in \
    "$(dirname "$CLANGXX")/../include/c++/v1" \
    "$("$CLANGXX" -print-resource-dir 2>/dev/null)/../../../include/c++/v1"; do
  [ -d "$cand" ] && { V1="$cand"; break; }
done
[ -n "$V1" ] || { echo "build_cxxio.sh: cannot locate libc++ <v1> headers" >&2; exit 1; }

# The same recipe the C++ conformance fixtures use (run_conformance.sh): our
# freestanding <__config_site> (LOCALIZATION 1 + NEWLIB 1) via -I "$HERE"; the
# vendored src's private headers via libcxx-src/include; libc++ and picolibc as
# explicit -isystem dirs, libc++ FIRST so its wrapper <cstdio>/<cstring>/... win
# and their #include_next reaches picolibc. picolibc MUST be -isystem (NOT
# -idirafter): -idirafter would place it BELOW the host C library, so on the
# linux-clang/linux-gcc CI runners glibc's /usr/include/string.h is found first
# and fails ("bits/libc-header-start.h"/"bits/wordsize.h" — no i386-elf multiarch).
# A bare-metal host (no /usr/include) doesn't expose the bug, which is why it
# only fires on the Linux CI. See ci-runs-failing.
CXXFLAGS=(--target=i386-elf -ffreestanding -std=c++23 -fno-rtti -mlong-double-64
          -nostdinc++ -isystem "$V1"
          -I "$HERE" -I "$SRC/include" -isystem "$PICO_INC"
          -D_LIBCPP_BUILDING_LIBRARY -D_GNU_SOURCE -DNDEBUG
          -emit-llvm -gline-tables-only "$OPT")

# The vendored TUs (must match runtime/lib/libcxx-src/*.cpp).
SOURCES=(locale ios ios.instantiations iostream ostream fstream
         system_error error_category)

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
BCS=(); nfail=0
for s in "${SOURCES[@]}"; do
  src="$SRC/$s.cpp"
  if [ ! -f "$src" ]; then echo "  SKIP (no source): $s" >&2; continue; fi
  bc="$WORK/$(echo "$s" | tr ./ __).bc"
  [ "$VERBOSE" = 1 ] && echo "$CLANGXX ${CXXFLAGS[*]} -c $src -o $bc"
  if "$CLANGXX" "${CXXFLAGS[@]}" -c "$src" -o "$bc" 2>"$bc.err"; then
    BCS+=("$bc")
  else
    echo "  FAIL: $s" >&2; grep -m3 'error:' "$bc.err" >&2 || true; nfail=$((nfail+1))
  fi
done
[ "$nfail" -eq 0 ] || { echo "build_cxxio.sh: $nfail source(s) failed" >&2; exit 1; }

"$LLVM_LINK" "${BCS[@]}" -o "$OUT"
echo "build_cxxio.sh: wrote $OUT (${#BCS[@]} sources)"
