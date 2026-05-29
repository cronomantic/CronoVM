#!/usr/bin/env bash
# build_picolibc.sh — compile a curated subset of picolibc to ONE i386-elf
# LLVM bitcode module (picolibc.bc) for the CronoVM toolchain.
#
# CronoVM does not drive picolibc's meson/cmake build. It compiles picolibc
# sources straight to bitcode with the SAME clang flags cvm-cc uses
# (--target=i386-elf -ffreestanding -emit-llvm), the hand-written picolibc.h in
# this directory pinning every feature knob, then llvm-links the objects into
# picolibc.bc — exactly mirroring how cvm_libc.c is compiled today, but as one
# reusable artifact.
#
# Output: runtime/lib/picolibc.bc  (a build artifact; gitignored)
#
# The OS layer (errno storage, malloc/free, and any write/read/sbrk/exit hooks)
# is NOT here — it is the embedder's machine port (Cronopio's SDK libc over cron
# syscalls). This module is the pure, embedder-independent C standard surface.
#
# Usage: bash build_picolibc.sh [-O<n>] [--keep-ll] [-v]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PICO="$HERE/../../external/picolibc"
OUT="$HERE/picolibc.bc"
OPT="-O1"
KEEP_LL=0
VERBOSE=0
for a in "$@"; do
  case "$a" in
    -O*) OPT="$a" ;;
    --keep-ll) KEEP_LL=1 ;;
    -v|--verbose) VERBOSE=1 ;;
    *) echo "build_picolibc.sh: unknown arg '$a'" >&2; exit 2 ;;
  esac
done

CLANG="${CLANG:-clang}"
LLVM_LINK="${LLVM_LINK:-llvm-link}"
command -v "$CLANG" >/dev/null     || { echo "clang not found" >&2; exit 1; }
command -v "$LLVM_LINK" >/dev/null || { echo "llvm-link not found" >&2; exit 1; }
[ -d "$PICO/libc/include" ] || { echo "picolibc submodule not checked out at $PICO" >&2; exit 1; }

# picolibc.h (this dir) must be found as <picolibc.h>; then the public headers,
# then the per-module private headers picolibc sources include by relative path.
L="$PICO/libc"
INC=(-I"$HERE" -I"$L/include"
     -I"$L/locale" -I"$L/ctype" -I"$L/string" -I"$L/stdlib"
     -I"$L/stdio" -I"$L/time" -I"$L/search")

CFLAGS=(--target=i386-elf -ffreestanding -emit-llvm -gline-tables-only "$OPT"
        -D_LIBC -U_FORTIFY_SOURCE "${INC[@]}")

# --- Source manifest: "<subdir>/<file>" relative to libc/, no .c suffix. ----
# Curated to the standard C surface the ports need; extend as engines demand.
# (The machine port — errno/malloc/free/OS hooks — is intentionally excluded.)
SOURCES=(
  # string.h
  string/strlen string/strnlen string/strcmp string/strncmp
  string/strcpy string/strncpy string/strcat string/strncat
  string/strchr string/strrchr string/strstr string/strdup
  string/memchr string/memcmp string/memcpy string/memmove string/memset
  string/strtok string/strtok_r string/strpbrk string/strspn string/strcspn
  # NOTE: string/strerror is omitted — it takes the address of the user-override
  # hook _user_strerror (undefined here), which the translator rejects (extern
  # address-of). Re-add with its default _user_strerror source when needed.
  # stdlib.h (integer/util; the allocator is the embedder's). The 64-bit
  # variants (llabs/lldiv/imaxabs/atoll) work now that the translator lowers
  # llvm.abs.i64 + the i64 mul/div legalizer is complete.
  stdlib/abs stdlib/labs stdlib/llabs stdlib/imaxabs
  stdlib/div stdlib/ldiv stdlib/lldiv
  stdlib/atoi stdlib/atol stdlib/atoll
  search/bsearch search/qsort
  # numeric parsing. strtoul/strtoull emit llvm.{uadd,umul}.with.overflow
  # (the {iN,i1} aggregate) — lowered now (i32 + i64).
  stdio/strtol stdio/strtoul stdio/strtoll stdio/strtoull
  # ctype: the classification table (_ctype_b) + table builders. Most is*()
  # are macros over the table; include a few real classifier fns defensively.
  ctype/ctype_ ctype/ctype_table ctype/ctype_class
  ctype/isalnum ctype/isalpha ctype/isdigit ctype/isspace
  ctype/isupper ctype/islower ctype/isxdigit ctype/toupper ctype/tolower
)

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
BCS=()
nfail=0
for s in "${SOURCES[@]}"; do
  src="$L/$s.c"
  if [ ! -f "$src" ]; then echo "  SKIP (no source): $s" >&2; continue; fi
  bc="$WORK/$(echo "$s" | tr / _).bc"
  [ "$VERBOSE" = 1 ] && echo "$CLANG ${CFLAGS[*]} -c $src -o $bc"
  if "$CLANG" "${CFLAGS[@]}" -c "$src" -o "$bc" 2>"$bc.err"; then
    BCS+=("$bc")
  else
    echo "  FAIL: $s" >&2; grep -m2 'error:' "$bc.err" >&2 || true; nfail=$((nfail+1))
  fi
done
[ "$nfail" -eq 0 ] || { echo "build_picolibc.sh: $nfail source(s) failed" >&2; exit 1; }

"$LLVM_LINK" "${BCS[@]}" -o "$OUT"
echo "build_picolibc.sh: wrote $OUT (${#BCS[@]} sources)"

# Report the still-undefined symbols = the contract the embedder's machine port
# must satisfy (errno, malloc/free, ...). Informational, not an error.
if command -v llvm-nm >/dev/null; then
  echo "machine-port surface (undefined symbols):"
  llvm-nm "$OUT" | grep ' U ' | grep -v _GLOBAL_OFFSET_TABLE_ | sort -u | sed 's/^/  /' || true
fi
