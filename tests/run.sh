#!/usr/bin/env bash
# Host test suite for Audiothurgist's pure core. Run from the repo root:
#   tests/run.sh [path/to/a-break.wav ...]
# 1. grammar_check  — Classic grammar hash must equal the frozen value; all
#                     modes: tiling coverage + source ranges.
# 2. engine_check   — behavioural suite under ASan/UBSan (see the file).
# 3. clock_check    — CLOCK package: multiplier-map continuity, lock alignment,
#                     P2 zones, the jitter meter (ASan/UBSan).
# 4. persist_check — PERSIST package: WAV writer ↔ wav_read.h and the firmware
#                     loader (stdio-shimmed), JSON emitter/parser, set and
#                     preset round trips (ASan/UBSan).
# 5. record_check   — RECORD package: the stereo shell math, take length,
#                     markers → cues → onsets, stereo conditioning, the loop
#                     take copier, the load pipeline on a synthetic take, the
#                     loader's two planes (ASan/UBSan).
# 6. sets_check     — SETS package: set naming, the engine → set → engine
#                     round trip (table, keys, looper, identical audio),
#                     PlanSet's refusals, Looper::Adopt (ASan/UBSan).
# 7. scope_check    — SCOPE package: request parsing and clamps, the trigger
#                     (crossing, hysteresis, ring age), channel alignment,
#                     plain / peak decimation, at-pos frames, the old-mode
#                     reply (ASan/UBSan).
# 8. bars_check     — builds; with WAVs on the command line, prints the bar/
#                     tempo estimate per file (tests/bars_corpus.sh validates
#                     against the corpus truth set).
# 9. classify_check — smoke run of the four classifier lenses on any WAVs
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

echo; echo "== persist_check (ASan/UBSan) =="
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/persist_check.cpp -o "$OUT/pc" || fail=1
"$OUT/pc" "$OUT" || fail=1
echo; echo "== record_check (ASan/UBSan) =="
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/record_check.cpp -o "$OUT/rc" || fail=1
"$OUT/rc" "$OUT" || fail=1
echo; echo "== sets_check (ASan/UBSan) =="
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/sets_check.cpp -o "$OUT/sc" || fail=1
"$OUT/sc" || fail=1
echo; echo "== scope_check (ASan/UBSan) =="
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/scope_check.cpp -o "$OUT/scope" || fail=1
"$OUT/scope" || fail=1
echo; echo "== bars_check (build; corpus run: tests/bars_corpus.sh) =="
$CXX -O2 -std=gnu++17 $INC tests/bars_check.cpp -o "$OUT/bc" || fail=1

if [ $# -gt 0 ]; then
  echo; echo "== classify_check =="
  $CXX -O2 -std=gnu++17 $INC tests/classify_check.cpp -o "$OUT/cc" || fail=1
  "$OUT/cc" "$@"
  echo; echo "== bars_check =="
  "$OUT/bc" "$@"
fi

echo
if [ $fail -eq 0 ]; then echo "ALL GREEN"; else echo "FAILURES"; fi
exit $fail
