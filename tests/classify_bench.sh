#!/usr/bin/env bash
# CLASSIFY: the standard measurement tables — every lens against ground truth
# on (1) IDMT-SMT-Drums, hand-annotated; (2) the 45-file tempo truth set and
# (3) a fixed random 200-file corpus sample, both against the ADTOF neural
# transcription. Needs the truth CSVs (tests/neural/README.md) and the corpus
# at $BREAKS (default ../BeatBlaster/analysis/breaks). ≈ 1 min.
set -u
cd "$(dirname "$0")/.."
BREAKS=${BREAKS:-/home/combust/Daisy/BeatBlaster/analysis/breaks}
OUT=${TMPDIR:-/tmp}/audiothurgist-tests; mkdir -p "$OUT"
g++ -O2 -std=gnu++17 -Icore -Ivendor/capicola/lib tests/classify_bench.cpp -o "$OUT/cb" || exit 1
[ -f tests/neural/idmt/idmt_truth.csv ] && { echo "== IDMT-SMT-Drums (manual truth) =="; "$OUT/cb" --truth tests/neural/idmt/idmt_truth.csv --root tests/neural/idmt/mix "$@"; }
[ -f tests/neural/adt.csv ] || { echo "no tests/neural/adt.csv — run tests/neural/adt_corpus.py"; exit 0; }
echo; echo "== tempo truth set, 45 files (ADTOF truth) =="; "$OUT/cb" --truth tests/neural/adt.csv --root "$BREAKS" --list tests/bars_truth.txt "$@"
echo; echo "== random 200-file sample, seed 1 (ADTOF truth) =="; "$OUT/cb" --truth tests/neural/adt.csv --root "$BREAKS" --sample 200 --seed 1 "$@"
