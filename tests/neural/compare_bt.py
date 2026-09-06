#!/usr/bin/env python3
"""Compare the firmware's grid (bars_check --csv) with Beat This! (beatthis_corpus.py) over the corpus.
Usage: compare_bt.py est.csv bt.csv
Tempo agreement classes vs the neural beat tracker: same (±4 %), x2, /2, x3/2, x2/3, x4/3, x3/4, other,
reported separately for files where the estimator spoke and where the prior stood.
Downbeat: for spoken files whose tempo agrees, the cut position (head_trim_s, in the original file) vs
Beat This!'s downbeats in the original file: distance to the nearest neural downbeat modulo the bar,
as a fraction of the bar; a hit is within ±1/16 bar (one 16th)."""
import sys, csv
from collections import Counter
est = {r['file']: r for r in csv.DictReader(open(sys.argv[1]), delimiter='\t')}
bt = {r['file']: r for r in csv.DictReader(open(sys.argv[2]))}

def cls(r):
    if 0.96 < r < 1.04: return 'same'
    for name, v in (('x2', 2), ('/2', 0.5), ('x3/2', 1.5), ('x2/3', 2 / 3), ('x4/3', 4 / 3), ('x3/4', 0.75)):
        if abs(r / v - 1) < 0.04: return name
    return 'other'

spoke, prior = Counter(), Counter()
down_hit = down_n = 0
down_hist = Counter()
n = 0
for f, e in est.items():
    b = bt.get(f)
    if not b or float(b['bpm']) <= 0 or int(b['nbeats']) < 6: continue
    n += 1
    r = float(e['grid_bpm']) / float(b['bpm'])
    c = cls(r)
    (spoke if e['spoke'] == '1' else prior)[c] += 1
    if e['spoke'] == '1' and c == 'same' and float(b['first_down_s']) >= 0 and int(b['ndown']) >= 2:
        bar_s = float(b['bar_s']) if float(b['bar_s']) > 0 else 240.0 / float(b['bpm'])
        cut = float(e['head_trim_s'])
        # the neural downbeats form a lattice first_down + k·bar_s; distance of the cut to that lattice
        d = (cut - float(b['first_down_s'])) / bar_s
        frac = d - round(d)
        down_n += 1
        if abs(frac) <= 1 / 16: down_hit += 1
        down_hist[round(frac * 16) / 16] += 1
tot_s, tot_p = sum(spoke.values()), sum(prior.values())
print(f"{n} files with a Beat This! opinion")
print(f"estimator spoke ({tot_s}):", ', '.join(f"{k} {v} ({100*v/tot_s:.0f}%)" for k, v in spoke.most_common()))
print(f"prior stood     ({tot_p}):", ', '.join(f"{k} {v} ({100*v/tot_p:.0f}%)" for k, v in prior.most_common()))
if down_n:
    print(f"downbeat: of {down_n} spoken files agreeing on tempo, the cut lands within a 16th of a neural downbeat on {down_hit} ({100*down_hit/down_n:.0f}%)")
    print("  offset histogram (bars, 1/16 bins):", ', '.join(f"{k:+.3f}:{v}" for k, v in sorted(down_hist.items())))
