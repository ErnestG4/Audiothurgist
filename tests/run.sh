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
# 6b. card_check   — CARD package: the one folder's paths, the upload name
#                     rule, the upload receiver, the copy plan, and the FatFs
#                     half (scan, survey, copy, the upload's write) over
#                     tests/fatfs_shim.h with a scratch card (ASan/UBSan).
# 6c. pads_check   — PADS package: a drop's receiver and its in-place PCM16 →
#                     planes conversion, EditOp::Record (one undo step, a
#                     replaced pad held), the room, recordings at their own
#                     rate, AdoptR, the unsaved note, the loader's same-rate
#                     path, set round trips of a pads-only kit and of a 17 s
#                     break plus pads at the 45 s planes (ASan/UBSan).
# 6e. reclaim_check — RECLAIM: held memory back — the plan, the copy into
#                     the other buffer, the swap (every live key the same
#                     bits, both planes, voices sounding across it), undo
#                     across it (Free: whole, remapped; All: the steps that
#                     need a reclaimed sound forgotten), the packed set save,
#                     the card map through a reclaim (ASan/UBSan).
# 6d. keyp_check   — KEYP package: per-key gain / tune / plays — gain and
#                     tune at the call (steps, keyless calls, auditions
#                     untouched; a looper replay follows the key), Follow /
#                     One-shot / Held / Loop, the edit op and its journal (a
#                     drag = one step), the set JSON (written only when set,
#                     old / odd files) and a round trip; ENV: the amp
#                     envelope (times, shape, no click, one-shots, Loop,
#                     Ratchet, the steps untouched) and choke groups (the cut
#                     within the choke time, never the key itself) (ASan/UBSan).
# 7. scope_check    — SCOPE package: request parsing and clamps, the trigger
#                     (crossing, hysteresis, ring age), channel alignment,
#                     plain / peak decimation, at-pos frames, the old-mode
#                     reply (ASan/UBSan).
# 8. panel_check    — EDIT-PANEL package: the selection walker, the centred
#                     pickup, pot → nudge request and its clamps, the rate
#                     window, the computed grid, the ring states (ASan/UBSan).
# 9. keys_check     — BETA: the one CV keymap — J4 → key (0 V = key 0), J5 →
#                     style with the wide zero, the call plan per style ×
#                     grammar × Voice option, the tail's millivolts (ASan/UBSan).
# 10. bars_check    — builds; with WAVs on the command line, prints the bar/
#                     tempo estimate per file (tests/bars_corpus.sh validates
#                     against the corpus truth set).
# 9. classify_check — smoke run of the five classifier lenses on any WAVs
#                     given on the command line (prints, no assertions).
# 9. classify_bench — builds; tests/classify_bench.sh measures every lens
#                     against ground truth (CLASSIFY package).
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
echo; echo "== card_check (ASan/UBSan) =="
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/card_check.cpp -o "$OUT/cardchk" || fail=1
"$OUT/cardchk" "$OUT" || fail=1
echo; echo "== pads_check (ASan/UBSan) =="
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/pads_check.cpp -o "$OUT/padschk" || fail=1
"$OUT/padschk" "$OUT" || fail=1
echo; echo "== reclaim_check (ASan/UBSan) =="
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/reclaim_check.cpp -o "$OUT/reclaimchk" || fail=1
"$OUT/reclaimchk" "$OUT" || fail=1
echo; echo "== keyp_check (ASan/UBSan) =="
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/keyp_check.cpp -o "$OUT/keypchk" || fail=1
"$OUT/keypchk" "$OUT" || fail=1
echo; echo "== scope_check (ASan/UBSan) =="
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/scope_check.cpp -o "$OUT/scope" || fail=1
"$OUT/scope" || fail=1
echo; echo "== panel_check (ASan/UBSan) =="
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/panel_check.cpp -o "$OUT/panel" || fail=1
"$OUT/panel" || fail=1
echo; echo "== keys_check (ASan/UBSan) =="
$CXX -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $INC tests/keys_check.cpp -o "$OUT/keys" || fail=1
"$OUT/keys" || fail=1
echo; echo "== bars_check (build; corpus run: tests/bars_corpus.sh) =="
$CXX -O2 -std=gnu++17 $INC tests/bars_check.cpp -o "$OUT/bc" || fail=1
echo; echo "== classify_bench (build; truth run: tests/classify_bench.sh) =="
$CXX -O2 -std=gnu++17 $INC tests/classify_bench.cpp -o "$OUT/cb" || fail=1

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
