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

# picolibc fixtures (conf_pico*) link the real picolibc bitcode on the VM side.
# Build picolibc.bc once up front if any such fixture is in the run.
RTLIB="$HERE/../../runtime/lib"
PICO_INC="$HERE/../../external/picolibc/libc/include"
PICOLIBC_BC="$RTLIB/picolibc.bc"
if printf '%s\n' "${fixtures[@]}" | grep -q 'conf_pico'; then
  LLVM_LINK="$(dirname "$CLANG")/llvm-link"
  [[ -x "$LLVM_LINK" || -x "$LLVM_LINK.exe" ]] || LLVM_LINK="llvm-link"
  if ! CLANG="$CLANG" LLVM_LINK="$LLVM_LINK" bash "$RTLIB/build_picolibc.sh" --with-stdio >/dev/null; then
    echo "run_conformance.sh: failed to build picolibc.bc" >&2; exit 1
  fi
fi

pass=0 gap=0 mis=0
for src in "${fixtures[@]}"; do
  [[ -e "$src" ]] || continue
  name="$(basename "$src")"; name="${name%.*}"
  bin="$TMP/$name.bin"
  nat="$TMP/$name.exe"

  # conf_alias relies on __attribute__((alias)), which Darwin/Mach-O does not
  # support, so the NATIVE oracle can't build on macOS. The VM-side GlobalAlias
  # lowering is still exercised on Linux/Windows; skip the fixture on macOS.
  if [[ "$name" == "conf_alias" && "$(uname -s)" == Darwin ]]; then
    printf 'SKIP       %-22s (alias attribute unsupported on darwin)\n' "$name"; continue
  fi

  # picolibc fixtures additionally link picolibc.bc + the machine-port stub on
  # the VM side, with picolibc's headers on the include path. The native oracle
  # uses the host libc (no picolibc include path, so host headers win).
  # C++ fixtures must pass picolibc's C headers with -idirafter (NOT -I) so
  # libc++'s wrapper <math.h>/<cstring>/... shadow them and #include_next through;
  # -I RTLIB stays high-priority (our __config_site / __external_threading win).
  pico=()
  if [[ "$name" == conf_pico* ]]; then
    pico_inc_flag="-I"
    [[ "$src" == *.cpp ]] && pico_inc_flag="-idirafter"
    pico=( "$HERE/pico_machine.c" "$PICOLIBC_BC" -I "$RTLIB" "$pico_inc_flag" "$PICO_INC" )
  fi

  if [[ "$src" == *.cpp ]]; then
    # VM: cvm-cc compiles the .cpp and auto-links runtime/lib/cvm_cxxrt.cpp.
    vmlog="$("$CVMCC" "$src" "$HERE/vm_entry.c" ${pico[@]+"${pico[@]}"} -o "$bin" 2>&1)"; vmrc=$?
    natcmd=("$CLANGXX" -O1 -x c++ "$src" -x c "$HERE/driver.c" -o "$nat")
  else
    vmlog="$("$CVMCC" "$src" "$HERE/vm_entry.c" ${pico[@]+"${pico[@]}"} -o "$bin" 2>&1)"; vmrc=$?
    natcmd=("$CLANG" -O1 "$src" "$HERE/driver.c" -o "$nat")
  fi

  if [[ $vmrc -ne 0 ]]; then
    msg="$(printf '%s\n' "$vmlog" | grep -iE "not yet lowered|outside the supported|unsupported|callee has no|fatal error|error:" | head -1)"
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
