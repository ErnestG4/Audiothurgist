#!/usr/bin/env python3
"""Neural beat/downbeat opinion for every corpus file (Beat This!, Foscarin et al. 2024, GPU).
Setup (once, ~3 GB): uv venv .bt --python 3.12 && uv pip install --python .bt/bin/python torch torchaudio \
  --index-url https://download.pytorch.org/whl/cu124 && uv pip install --python .bt/bin/python git+https://github.com/CPJKU/beat_this soundfile
The whole 1,230-file corpus takes ~60 s on an RTX 4090. Then: bars_check --csv … > est.csv; compare_bt.py est.csv bt.csv
Output CSV: file, dur, nbeats, ndown, bpm (median inter-beat), bar_s (median inter-downbeat),
first_downbeat_s, beat_cv (jitter of inter-beat intervals).
Usage: beatthis_corpus.py <corpus-dir> <out.csv> [speed]
"""
import os, sys, csv
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from wavio import load_wav
import torch
from beat_this.inference import Audio2Beats

root, out = sys.argv[1], sys.argv[2]
speed = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
a2b = Audio2Beats(checkpoint_path="final0", device="cuda" if torch.cuda.is_available() else "cpu", dbn=False)
files = sorted(f for f in os.listdir(root) if f.lower().endswith('.wav'))
with open(out, 'w', newline='') as fo:
    w = csv.writer(fo)
    w.writerow(['file', 'speed', 'dur', 'nbeats', 'ndown', 'bpm', 'bar_s', 'first_down_s', 'beat_cv'])
    for i, f in enumerate(files):
        try:
            x, sr = load_wav(os.path.join(root, f))
            if x is None or len(x) < sr: raise ValueError('short')
            if speed != 1.0:
                idx = np.arange(0, len(x) - 1, speed); k = idx.astype(int); fr = idx - k
                x = x[k] * (1 - fr) + x[k + 1] * fr
            x = x[:int(25 * sr)]
            x = (x / (np.max(np.abs(x)) + 1e-9)).astype(np.float32)
            beats, downs = a2b(x, sr)
            ib = np.diff(beats) if len(beats) > 1 else np.array([])
            idb = np.diff(downs) if len(downs) > 1 else np.array([])
            bpm = 60.0 / np.median(ib) if len(ib) else 0.0
            bar = float(np.median(idb)) if len(idb) else 0.0
            cv = float(np.std(ib) / np.mean(ib)) if len(ib) > 1 else 0.0
            w.writerow([f, speed, round(len(x) / sr, 2), len(beats), len(downs), round(bpm, 2), round(bar, 4),
                        round(float(downs[0]), 4) if len(downs) else -1, round(cv, 3)])
        except Exception as e:
            w.writerow([f, speed, 0, 0, 0, 0, 0, -1, 0])
        if i % 100 == 0: print(i, f, flush=True)
print('done', len(files))
