#!/usr/bin/env bash
# Differential conformance runner for the CronoVM translator.
#
# For each fixture tests/conformance/conf_*.c:
#   1. Build it on the VM    (cvm-cc fixture.c -> fixture.bin)
#   2. Build it natively     (clang fixture.c driver.c -> native oracle)
#   3. Run native -> the expected int32 checksum
#   4. Run the VM bin via test_e2e and compare to the native checksum
#
# Outcomes per fixture:
#   PASS       — VM result == native result (intrinsic/op lowered correctly)
#   GAP        — cvm-cc failed to translate (unlowered construct)
#   MISCOMPILE — VM translated but returned a different value than native
#
# The native build is the ORACLE: fixtures use only fixed-width types + an
# int32 checksum, so host-64 and VM-32 agree bit-for-bit. Exits non-zero if any
# fixture is GAP or MISCOMPILE.
#
# Usage: run_conformance.sh <cvm-cc> <test_e2e> <clang> [fixture.c ...]
#   With no fixture args, runs every tests/conformance/conf_*.c.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CVMCC="${1:?usage: run_conformance.sh <cvm-cc> <test_e2e> <clang> [fixtures...]}"
E2E="${2:?missing test_e2e path}"
CLANG="${3:?missing clang path}"
shift 3

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fixtures=( "$@" )
if [[ ${#fixtures[@]} -eq 0 ]]; then
  fixtures=( "$HERE"/conf_*.c )
fi

pass=0 gap=0 mis=0
for src in "${fixtures[@]}"; do
  name="$(basename "$src" .c)"
  bin="$TMP/$name.bin"
  nat="$TMP/$name.exe"

  # 1+2: VM translate (capture the gap message) + native oracle build.
  vmlog="$("$CVMCC" "$src" -o "$bin" 2>&1)"; vmrc=$?
  if [[ $vmrc -ne 0 ]]; then
    msg="$(printf '%s\n' "$vmlog" | grep -iE "not yet lowered|outside the supported|unsupported" | head -1)"
    printf 'GAP        %-22s %s\n' "$name" "${msg:-(cvm-cc exit $vmrc)}"
    gap=$((gap+1)); continue
  fi
  if ! "$CLANG" -O1 "$src" "$HERE/driver.c" -o "$nat" 2>"$TMP/nat.err"; then
    printf 'GAP        %-22s (native oracle build failed)\n' "$name"
    cat "$TMP/nat.err"; gap=$((gap+1)); continue
  fi

  # 3: native checksum.
  expected="$("$nat")"

  # 4: VM run vs oracle.
  if "$E2E" "$bin" "$expected" >/dev/null 2>&1; then
    printf 'PASS       %-22s %s\n' "$name" "$expected"
    pass=$((pass+1))
  else
    got="$("$E2E" "$bin" "$expected" 2>&1 | grep -oE "returned -?[0-9]+" | head -1)"
    printf 'MISCOMPILE %-22s native=%s vm=%s\n' "$name" "$expected" "${got:-?}"
    mis=$((mis+1))
  fi
done

echo "---"
echo "PASS=$pass  GAP=$gap  MISCOMPILE=$mis"
[[ $gap -eq 0 && $mis -eq 0 ]]
