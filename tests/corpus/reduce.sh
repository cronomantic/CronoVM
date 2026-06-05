#!/usr/bin/env bash
# ddmin line reducer for a corpus MISCOMPILE.
# "Interesting" = native is self-consistent (-O0 == -O2, i.e. well-defined) AND
# the VM at -O2 disagrees with native. Shrinks the program while preserving that,
# using delta-debugging with increasing granularity (O(n log n), not O(n^2)).
# Usage: reduce.sh <cvm-cc> <test_e2e> <clang> <input.c> <out.c>
set -u
CVMCC="$1"; E2E="$2"; CLANG="$3"; IN="$4"; OUT="$5"
CONF="$(cd "$(dirname "${BASH_SOURCE[0]}")/../conformance" && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT

interesting() { # $1 = candidate .c
  local c="$1"
  "$CLANG" -O0 -w "$c" "$CONF/driver.c" -o "$W/n0" -lm 2>/dev/null || return 1
  "$CLANG" -O2 -w "$c" "$CONF/driver.c" -o "$W/n2" -lm 2>/dev/null || return 1
  local r0 r2; r0="$("$W/n0")"; r2="$("$W/n2")"
  [ "$r0" = "$r2" ] || return 1                      # must stay well-defined
  "$CVMCC" -O2 "$c" "$CONF/vm_entry.c" -o "$W/v.bin" 2>/dev/null || return 1
  local vm; vm="$("$E2E" "$W/v.bin" "$r2" 2>&1 | grep -oE 'returned -?[0-9]+' | grep -oE '\-?[0-9]+')"
  [ -n "$vm" ] && [ "$vm" != "$r2" ]                 # VM must still be wrong
}

write_without() { # $1=out  $2=start(0-based)  $3=count   ; drop L[start..start+count)
  local o="$1" s="$2" c="$3" j
  : > "$o"
  for ((j=0;j<${#L[@]};j++)); do
    if (( j < s || j >= s+c )); then printf '%s\n' "${L[$j]}" >> "$o"; fi
  done
}

cp "$IN" "$W/cur.c"
interesting "$W/cur.c" || { echo "reduce: input not interesting"; exit 1; }
mapfile -t L < "$W/cur.c"
echo "start: ${#L[@]} lines"

n=2
while (( ${#L[@]} >= 2 )); do
  len=${#L[@]}
  (( n > len )) && n=$len
  chunk=$(( (len + n - 1) / n ))
  removed=0
  s=0
  while (( s < len )); do
    write_without "$W/cand.c" "$s" "$chunk"
    if interesting "$W/cand.c"; then
      mapfile -t L < "$W/cand.c"
      removed=1
      echo "  -$chunk lines -> ${#L[@]}"
      break
    fi
    s=$(( s + chunk ))
  done
  if (( removed )); then
    n=$(( n > 2 ? n-1 : 2 ))
  else
    (( n >= len )) && break
    n=$(( n*2 < len ? n*2 : len ))
  fi
done

printf '%s\n' "${L[@]}" > "$OUT"
echo "reduced -> $OUT ($(wc -l < "$OUT") lines)"
