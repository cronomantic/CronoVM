#!/usr/bin/env bash
# Differential CORPUS runner for the CronoVM translator/VM.
#
# Unlike the hand-written conformance suite (run_conformance.sh), this drives the
# cvm-fuzz GENERATOR over a range of seeds, so it exercises a broad, randomised
# slice of the integer/control-flow/spill surface and catches translator/VM
# miscompiles PROACTIVELY (the recurring spill / narrow-int / phi / etc. bugs that
# were historically found late, via a game OOB). It reuses the SAME differential
# machinery: each generated program exports `int conf_main(void)` returning an
# int32 FNV checksum; the native build (host clang) is the oracle, the VM build
# (cvm-cc) is compared against it via test_e2e.
#
# Each seed is built on the VM at EVERY requested optimisation level (the IR — and
# thus the translator path — differs per level; e.g. the -O2 vector lowering gap
# was -O2-only). The native oracle is computed ONCE (the checksum is opt-level
# independent for a UB-free program) and every VM build is compared to it.
#
# Generated programs are UB-free BY CONSTRUCTION (see cvm-fuzz.c); a divergence is
# therefore a genuine translator/VM bug, not undefined behaviour. Failing programs
# (GAP = cvm-cc couldn't translate; MISCOMPILE = wrong checksum) are SAVED under
# tests/corpus/fails/ for triage/minimisation.
#
# Usage:
#   run_corpus.sh <cvm-cc> <test_e2e> <clang> <cvm-fuzz> [COUNT] [SEED_BASE] [OPTS]
#     COUNT      number of seeds to run            (default 100)
#     SEED_BASE  first seed                          (default 1)
#     OPTS       space-separated cvm-cc opt levels   (default "O1 O2")
# Exit status: 0 iff every seed PASSed at every opt level.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF="$HERE/../conformance"            # reuse driver.c + vm_entry.c
CVMCC="${1:?usage: run_corpus.sh <cvm-cc> <test_e2e> <clang> <cvm-fuzz> [count] [base] [opts]}"
E2E="${2:?missing test_e2e path}"
CLANG="${3:?missing clang path}"
FUZZ="${4:?missing cvm-fuzz path}"
COUNT="${5:-100}"
BASE="${6:-1}"
OPTS="${7:-O1 O2}"

FAILDIR="$HERE/fails"
mkdir -p "$FAILDIR"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0 gap=0 mis=0 skip=0 nfail=0
first_fail=""

for ((i = 0; i < COUNT; i++)); do
  seed=$((BASE + i))
  src="$TMP/s$seed.c"
  # FUZZ_FLAGS (env) forwards extra generator flags, e.g. FUZZ_FLAGS=--no-rot64
  # for a GAP-free baseline (skips the known llvm.fshl.i64 gap).
  "$FUZZ" --seed "$seed" ${FUZZ_FLAGS:-} -o "$src" || { echo "GENFAIL seed=$seed"; continue; }

  # native oracle (once) — opt-level independent for a UB-free program
  if ! "$CLANG" -O1 -w "$src" "$CONF/driver.c" -o "$TMP/s$seed.exe" -lm 2>"$TMP/nat.err"; then
    printf 'NATIVE-SKIP  seed=%-6s (oracle build failed)\n' "$seed"
    grep -i error "$TMP/nat.err" | head -1
    cp "$src" "$FAILDIR/seed${seed}_nativefail.c"; nfail=$((nfail+1)); skip=$((skip+1)); continue
  fi
  expected="$("$TMP/s$seed.exe")"

  for opt in $OPTS; do
    bin="$TMP/s${seed}_$opt.bin"
    if ! vmlog="$("$CVMCC" "-$opt" "$src" "$CONF/vm_entry.c" -o "$bin" 2>&1)"; then
      msg="$(printf '%s\n' "$vmlog" | grep -iE "not yet lowered|outside the supported|unsupported|callee has no|fatal error|error:" | head -1)"
      printf 'GAP          seed=%-6s -%s  %s\n' "$seed" "$opt" "${msg:-(cvm-cc failed)}"
      cp "$src" "$FAILDIR/seed${seed}_${opt}_gap.c"; gap=$((gap+1))
      [[ -z "$first_fail" ]] && first_fail="seed=$seed -$opt GAP"
      continue
    fi
    if "$E2E" "$bin" "$expected" >/dev/null 2>&1; then
      pass=$((pass+1))
    else
      got="$("$E2E" "$bin" "$expected" 2>&1 | grep -oE "returned -?[0-9]+" | head -1)"
      printf 'MISCOMPILE   seed=%-6s -%s  native=%s vm=%s\n' "$seed" "$opt" "$expected" "${got:-?}"
      cp "$src" "$FAILDIR/seed${seed}_${opt}_miscompile.c"; mis=$((mis+1))
      [[ -z "$first_fail" ]] && first_fail="seed=$seed -$opt MISCOMPILE (native=$expected vm=${got:-?})"
    fi
  done
done

echo "---"
echo "seeds=$COUNT base=$BASE opts=[$OPTS]"
echo "PASS=$pass  GAP=$gap  MISCOMPILE=$mis  NATIVE-SKIP=$skip"
[[ -n "$first_fail" ]] && echo "first failure: $first_fail  (saved under tests/corpus/fails/)"
[[ $gap -eq 0 && $mis -eq 0 && $nfail -eq 0 ]]
