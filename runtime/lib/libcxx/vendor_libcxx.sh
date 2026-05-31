#!/usr/bin/env bash
# Regenerate the vendored libc++ (headers + the src subset CronoVM builds into
# cxxio.bc). See VENDOR.md. The headers come from an INSTALLED libc++ of the
# desired version (the full v1 tree is impractical to fetch file-by-file); the
# src subset is fetched from the matching llvmorg-* tag on github.
#
# Usage:
#   vendor_libcxx.sh --v1 <path/to/c++/v1> [--tag llvmorg-X.Y.Z]
# If --tag is omitted it is derived from the v1's _LIBCPP_VERSION.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

V1="" ; TAG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --v1)  V1="$2"; shift 2 ;;
    --tag) TAG="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[ -n "$V1" ] && [ -d "$V1" ] || { echo "usage: vendor_libcxx.sh --v1 <c++/v1 dir> [--tag llvmorg-X.Y.Z]" >&2; exit 2; }

# Derive the tag from _LIBCPP_VERSION (e.g. 220106 -> llvmorg-22.1.6) if not given.
if [ -z "$TAG" ]; then
  ver="$(grep -hoE '_LIBCPP_VERSION[[:space:]]+[0-9]+' "$V1/__config" | grep -oE '[0-9]+$' | head -1)"
  TAG="llvmorg-$((10#${ver:0:2})).$((10#${ver:2:2})).$((10#${ver:4:2}))"
fi
echo "vendor_libcxx: headers from $V1 ; src from $TAG"

# --- headers: copy the whole v1 tree (CronoVM's __config_site override stays in
# runtime/lib and wins via -I, so the copied one is harmless). -------------------
rm -rf "$HERE/include/v1"
mkdir -p "$HERE/include"
cp -r "$V1" "$HERE/include/v1"

# --- src subset: fetch from the matching tag ----------------------------------
BASE="https://raw.githubusercontent.com/llvm/llvm-project/$TAG/libcxx/src"
SRCS=(locale ios ios.instantiations iostream ostream fstream system_error error_category)
INCS=(refstring.h config_elast.h atomic_support.h sso_allocator.h iostream_init.h std_stream.h)
rm -rf "$HERE/src"; mkdir -p "$HERE/src/include"
for s in "${SRCS[@]}"; do curl -fsSL "$BASE/$s.cpp" -o "$HERE/src/$s.cpp"; done
for h in "${INCS[@]}"; do curl -fsSL "$BASE/include/$h" -o "$HERE/src/include/$h" 2>/dev/null || true; done
echo "vendor_libcxx: wrote $(find "$HERE/include/v1" -type f | wc -l) headers + ${#SRCS[@]} src TUs"
