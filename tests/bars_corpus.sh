#!/usr/bin/env bash
# Corpus validation of the bar/tempo estimator (SLICE package). Not part of
# tests/run.sh (needs the 2 GB corpus); run by hand from the repo root:
#   tests/bars_corpus.sh [corpus-dir] [onset-mode]
# 1. Truth set (tests/bars_truth.txt, Frane 2017 tempos) at 1.0/1.25/1.5/1.7×:
#    prior vs new, wrong-confident count — the numbers in docs/packages/slice.md.
# 2. Whole corpus at 1.0×: how often the estimator speaks and how its grid
#    relates to the duration prior (same / ×2 / ×½ / other).
# 3. A 200-file sample at 1.5×: the same, for the sped-up case.
set -u
cd "$(dirname "$0")/.."
ROOT=${1:-/home/combust/Daisy/BeatBlaster/analysis/breaks}
MODE=${2:-0}
OUT=${TMPDIR:-/tmp}/audiothurgist-tests
mkdir -p "$OUT"
CXX=${CXX:-g++}
$CXX -O2 -std=gnu++17 -Icore -Ivendor/capicola/lib tests/bars_check.cpp -o "$OUT/bc" || exit 1

echo "== truth set (tests/bars_truth.txt) =="
for sp in 1.0 1.25 1.5 1.7; do
  "$OUT/bc" --quiet --onsets "$MODE" --speed "$sp" --truth tests/bars_truth.txt --root "$ROOT" | grep SUMMARY
done

corpus_stats() {  # $1 = speed, $2 = file list
  "$OUT/bc" --onsets "$MODE" --speed "$1" --root "$ROOT" --list "$2" > "$OUT/corpus_$1.txt" 2>/dev/null
  # columns: … | prior <bars> bars <BPM> BPM | est <BPM> grid <BPM> BPM (<bars> bars, est|prior) …
  awk -v sp="$1" '
    /grid/ { n++;
      for(i = 1; i <= NF; i++) { if($i == "prior" && $(i+2) == "bars") p = $(i+3); if($i == "grid") g = $(i+1); }
      spoke = ($0 ~ /bars, est\)/); if(spoke) s++;
      if(p > 0) { r = g / p; if(spoke) { if(r > 0.96 && r < 1.04) same++; else if(r > 1.9 && r < 2.1) dbl++; else if(r > 0.48 && r < 0.52) half++; else other++; } }
    }
    END { printf("speed %s: %d files, estimator spoke on %d (%.0f%%): same grid as prior %d, prior x2 %d, prior /2 %d, other %d\n", sp, n, s, n ? 100*s/n : 0, same, dbl, half, other) }' "$OUT/corpus_$1.txt"
}
echo; echo "== whole corpus, 1.0x =="
ls "$ROOT" | grep -i '\.wav$' > "$OUT/all.txt"
corpus_stats 1.0 "$OUT/all.txt"
echo; echo "== 200-file sample, 1.5x =="
shuf -n 200 --random-source=<(yes) "$OUT/all.txt" > "$OUT/sample.txt"
corpus_stats 1.5 "$OUT/sample.txt"
