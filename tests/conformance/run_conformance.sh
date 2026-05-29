#!/usr/bin/env bash
# Differential conformance runner for the CronoVM translator.
#
# For each fixture tests/conformance/conf_*.{c,cpp}:
#   1. Build it on the VM    (cvm-cc — it compiles C++ natively and auto-links
#                             the C++ ABI runtime runtime/lib/cvm_cxxrt.cpp)
#   2. Build it natively     (clang / clang++ -> the oracle)
#   3. Run native -> the expected int32 checksum
#   4. Run the VM bin via test_e2e and compare to the native checksum
#
# Every fixture exports `conf_main`; vm_entry.c (a real `main` tail-calling it)
# pins the VM entry deterministically. Fixtures use only fixed-width types + an
# int32 checksum, so host-64 and VM-32 agree bit-for-bit.
#
# Outcomes: PASS / GAP (cvm-cc failed to translate) / MISCOMPILE (wrong value).
#
# Usage: run_conformance.sh <cvm-cc> <test_e2e> <clang> [fixture ...]
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CVMCC="${1:?usage: run_conformance.sh <cvm-cc> <test_e2e> <clang> [fixtures...]}"
E2E="${2:?missing test_e2e path}"
CLANG="${3:?missing clang path}"
shift 3

# Derive clang++ (C++ driver: links the host C++ runtime for the native oracle).
CLANGXX="$(dirname "$CLANG")/clang++"
[[ -x "$CLANGXX" || -x "$CLANGXX.exe" ]] || CLANGXX="clang++"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fixtures=( "$@" )
if [[ ${#fixtures[@]} -eq 0 ]]; then
  fixtures=( "$HERE"/conf_*.c "$HERE"/conf_*.cpp )
fi

pass=0 gap=0 mis=0
for src in "${fixtures[@]}"; do
  [[ -e "$src" ]] || continue
  name="$(basename "$src")"; name="${name%.*}"
  bin="$TMP/$name.bin"
  nat="$TMP/$name.exe"

  if [[ "$src" == *.cpp ]]; then
    # VM: cvm-cc compiles the .cpp and auto-links runtime/lib/cvm_cxxrt.cpp.
    vmlog="$("$CVMCC" "$src" "$HERE/vm_entry.c" -o "$bin" 2>&1)"; vmrc=$?
    natcmd=("$CLANGXX" -O1 -x c++ "$src" -x c "$HERE/driver.c" -o "$nat")
  else
    vmlog="$("$CVMCC" "$src" "$HERE/vm_entry.c" -o "$bin" 2>&1)"; vmrc=$?
    natcmd=("$CLANG" -O1 "$src" "$HERE/driver.c" -o "$nat")
  fi

  if [[ $vmrc -ne 0 ]]; then
    msg="$(printf '%s\n' "$vmlog" | grep -iE "not yet lowered|outside the supported|unsupported|callee has no" | head -1)"
    printf 'GAP        %-22s %s\n' "$name" "${msg:-(cvm-cc exit $vmrc)}"; gap=$((gap+1)); continue
  fi
  if ! "${natcmd[@]}" 2>"$TMP/nat.err"; then
    printf 'GAP        %-22s (native oracle build failed)\n' "$name"
    grep -iv deprecated "$TMP/nat.err" | head -3; gap=$((gap+1)); continue
  fi

  expected="$("$nat")"
  if "$E2E" "$bin" "$expected" >/dev/null 2>&1; then
    printf 'PASS       %-22s %s\n' "$name" "$expected"; pass=$((pass+1))
  else
    got="$("$E2E" "$bin" "$expected" 2>&1 | grep -oE "returned -?[0-9]+" | head -1)"
    printf 'MISCOMPILE %-22s native=%s vm=%s\n' "$name" "$expected" "${got:-?}"; mis=$((mis+1))
  fi
done

echo "---"
echo "PASS=$pass  GAP=$gap  MISCOMPILE=$mis"
[[ $gap -eq 0 && $mis -eq 0 ]]
