#!/usr/bin/env python3
"""How good is the neural opinion? Hit-level precision / recall / F1 per class of one CSV (adt_corpus.py on the
IDMT mixes) against another (idmt_truth.py, hand-annotated), matching hits of the same class within ±tol s.
Usage: compare_adt_truth.py idmt/idmt_adt.csv idmt/idmt_truth.csv [tol=0.03]"""
import sys, csv
from collections import defaultdict
def load(p):
    d = defaultdict(lambda: defaultdict(list))
    for r in csv.DictReader(open(p)): d[r['file']][int(r['cls'])].append(float(r['t']))
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
tol = float(sys.argv[3]) if len(sys.argv) > 3 else 0.03
for c, name in enumerate(['kick', 'snare', 'hat']):
    tp = fp = fn = 0
    for f in b:
        est, ref = sorted(a[f][c]), sorted(b[f][c])
        used = [False] * len(ref)
        for t in est:
            j = min(range(len(ref)), key=lambda i: abs(ref[i] - t) if not used[i] else 1e9, default=None)
            if j is not None and not used[j] and abs(ref[j] - t) <= tol: used[j] = True; tp += 1
            else: fp += 1
        fn += used.count(False)
    p = tp / (tp + fp) if tp + fp else 0; r = tp / (tp + fn) if tp + fn else 0
    print(f"{name:5s} P {100*p:5.1f}%  R {100*r:5.1f}%  F1 {200*p*r/(p+r) if p+r else 0:5.1f}%  (tp {tp} fp {fp} fn {fn})")
