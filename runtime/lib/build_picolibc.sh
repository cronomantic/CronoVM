#!/usr/bin/env bash
# build_picolibc.sh — compile a curated subset of picolibc to ONE i386-elf
# LLVM bitcode module (picolibc.bc) for the CronoVM toolchain.
#
# CronoVM does not drive picolibc's meson/cmake build. It compiles picolibc
# sources straight to bitcode with the SAME clang flags cvm-cc uses
# (--target=i386-elf -ffreestanding -emit-llvm), the hand-written picolibc.h in
# this directory pinning every feature knob, then llvm-links the objects into
# picolibc.bc — exactly mirroring how cron_sys.c is compiled, but as one
# reusable artifact.
#
# Output: runtime/lib/picolibc.bc  (a build artifact; gitignored)
#
# picolibc owns the CANONICAL allocator (malloc/free/calloc/realloc); its only OS
# hook is sbrk(), which the embedder's machine port backs over the cron heap
# (errno storage is the other machine-port symbol). write/read/exit hooks (stdio)
# are still NOT here. Everything else is the pure, embedder-independent C surface.
#
# --no-malloc omits picolibc's malloc family (malloc/free/calloc/realloc), so a
# cart can supply the CANONICAL malloc itself (e.g. the Cronopio tuned allocator
# via -DCRON_LIBC_TUNED_MALLOC). The machine-port surface then shrinks back to
# just `errno` (no sbrk). See sdk/lib/cron_sys.c + libc-libcxx-decision.
#
# Usage: bash build_picolibc.sh [-O<n>] [--no-malloc] [--keep-ll] [-v]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PICO="$HERE/../../external/picolibc"
OUT="$HERE/picolibc.bc"
OPT="-O1"
KEEP_LL=0
VERBOSE=0
WITH_MALLOC=1
WITH_STDIO=0
for a in "$@"; do
  case "$a" in
    -O*) OPT="$a" ;;
    --no-malloc) WITH_MALLOC=0 ;;
    --with-stdio) WITH_STDIO=1 ;;
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
     -I"$L/stdio" -I"$L/time" -I"$L/search" -I"$L/../libm/common")

CFLAGS=(--target=i386-elf -ffreestanding -emit-llvm -gline-tables-only "$OPT"
        -D_LIBC -DNDEBUG -U_FORTIFY_SOURCE "${INC[@]}")

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
  # libm (math.h, double precision) — added on demand. UQM's planet-surface
  # generator (plangen/pl_stuff) needs exp() + acos(); acos pulls sqrt, both pull
  # the error helpers + exp's data table. f64 lowers to the soft-float runtime.
  # (Paths are ../libm relative to libc/; the loop normalises them.) __isnand
  # (referenced transitively) is the embedder's, in cron_sys.c.
  ../libm/math/s_exp ../libm/math/s_acos ../libm/math/s_sqrt ../libm/math/s_log
  ../libm/common/exp_data ../libm/common/log_data
  ../libm/common/math_err_oflow ../libm/common/math_err_uflow
  ../libm/common/math_err_invalid ../libm/common/math_err_divzero
)

# The malloc family is OPTIONAL (see --no-malloc). When included, picolibc owns
# the canonical malloc/free/calloc/realloc; its only OS hook is sbrk() (the
# embedder backs it over the cron heap). MALLOC_LOCK is a no-op under
# __SINGLE_THREAD; -DNDEBUG drops realloc's assert (no __assert_func).
if [ "$WITH_MALLOC" = 1 ]; then
  SOURCES+=( stdlib/malloc stdlib/free stdlib/calloc stdlib/realloc )
fi

# tinystdio — the full stdio (printf/scanf/FILE/fopen). OPTIONAL (--with-stdio)
# because it leaves a POSIX backend (open/read/write/lseek/close) + __isnand
# undefined for the embedder; a fixture/cart that doesn't supply those must not
# pull it in. The variant wrappers (vfprintf.c etc.) self-#include their cores;
# %f uses the CLASSIC dtoa_engine (f64 soft-float), not ryu. See cron_sys.c.
if [ "$WITH_STDIO" = 1 ]; then
  SOURCES+=(
    stdio/vfprintf stdio/vfscanf
    stdio/printf stdio/fprintf stdio/vprintf
    stdio/snprintf stdio/vsnprintf stdio/sprintf stdio/vsprintf
    stdio/sscanf stdio/scanf stdio/fscanf
    stdio/bufio stdio/bufio_close stdio/bufio_close_nf
    stdio/filestrget stdio/filestrput stdio/fseeko
    stdio/dtoa_engine stdio/dtox_engine stdio/ftoa_engine
    stdio/atof_engine stdio/atod_engine
    stdio/puts stdio/putchar stdio/fputc stdio/fputs
    stdio/fgetc stdio/fgets stdio/getchar stdio/ungetc
    stdio/fopen stdio/fdopen stdio/fclose stdio/fread stdio/fwrite
    stdio/fseek stdio/ftell stdio/fflush
    stdio/clearerr stdio/feof stdio/ferror stdio/fileno stdio/perror
    stdio/posixiob_stdin stdio/posixiob_stdout stdio/posixiob_stderr
    stdio/sflags stdio/fdevopen
  )
fi

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
