#!/usr/bin/env bash
# Host test suite for Audiothurgist's pure core. Run from the repo root:
#   tests/run.sh [path/to/a-break.wav ...]
# 1. grammar_check  — Classic grammar hash must equal the frozen value; all
#                     modes: tiling coverage + source ranges.
# 2. engine_check   — behavioural suite under ASan/UBSan (see the file).
# 3. clock_check    — CLOCK package: multiplier-map continuity, lock alignment,
#                     P2 zones, the jitter meter (ASan/UBSan).
# 4. classify_check — smoke run of the four classifier lenses on any WAVs
#                     given on the command line (prints, no assertions).
set -u
cd "$(dirname "$0")/.."
OUT=${TMPDIR:-/tmp}/audiothurgist-tests
mkdir -p "$OUT"
fail=0
CXX=${CXX:-g++}
INC="-Icore -Ivendor/capicola/lib"

echo "== grammar_check =="
$CXX -O2 -std=gnu++17 -DHAS_MODE $INC tests/grammar_check.cpp -o "$OUT/gc" || fail=1
for m in 0 1 2 3 4; do "$OUT/gc" $m | tee "$OUT/gc$m.txt"; done
grep -q "hash c2081c20c9d7fbbf" "$OUT/gc0.txt" || { echo "  FAIL: Classic grammar hash changed"; fail=1; }
for m in 0 1 2 3 4; do grep -qE "badsrc 0 +coverErr 0" "$OUT/gc$m.txt" || { echo "  FAIL: mode $m range/coverage"; fail=1; }; done

echo; echo "== engine_check (ASan/UBSan) =="
rm -f "$OUT/ec"
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/engine_check.cpp -o "$OUT/ec" || fail=1
"$OUT/ec" || fail=1

echo; echo "== clock_check (ASan/UBSan) =="
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/clock_check.cpp -o "$OUT/clk" || fail=1
"$OUT/clk" || fail=1

if [ $# -gt 0 ]; then
  echo; echo "== classify_check =="
  $CXX -O2 -std=gnu++17 $INC tests/classify_check.cpp -o "$OUT/cc" || fail=1
  "$OUT/cc" "$@"
fi

echo
if [ $fail -eq 0 ]; then echo "ALL GREEN"; else echo "FAILURES"; fi
exit $fail
