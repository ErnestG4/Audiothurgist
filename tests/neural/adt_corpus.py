#!/usr/bin/env python3
"""Neural drum-hit opinion for every corpus file: ADTOF Frame_RNN (Zehren, Alunno & Bientinesi 2021/2023),
the PyTorch port by xavriley (github.com/xavriley/ADTOF-pytorch, weights bundled), on the GPU.
Setup (once, ~3 GB): uv venv .adt --python 3.12 && uv pip install --python .adt/bin/python torch torchaudio \
  --index-url https://download.pytorch.org/whl/cu124 && git clone --depth 1 https://github.com/xavriley/ADTOF-pytorch \
  && uv pip install --python .adt/bin/python -e ./ADTOF-pytorch soundfile
Then: .adt/bin/python adt_corpus.py <corpus-dir> <out.csv>   (the 1,231-file corpus takes ~3 min on an RTX 4090)
Output CSV: file, t (seconds in the ORIGINAL file, first 25 s only — the module's cap), cls (0 kick, 1 snare,
2 hat), act (the model's activation at the peak, 0..1), midi (the model's own class: 35 kick, 38 snare,
47 tom -> snare, 42 hi-hat, 49 crash/ride -> hat). Per-class thresholds are the port's Frame_RNN defaults.
Audio is resampled to 44.1 kHz and peak-normalised before the model (the module normalises too).
"""
import os, sys, csv
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from wavio import load_wav
import torch, librosa
from adtof_pytorch import create_frame_rnn_model, calculate_n_bins, load_pytorch_weights, get_default_weights_path
from adtof_pytorch.audio import create_adtof_processor
from adtof_pytorch.post_processing import PeakPicker, FRAME_RNN_THRESHOLDS, LABELS_5

CLS = {35: 0, 38: 1, 47: 1, 42: 2, 49: 2}

def main():
    root, out = sys.argv[1], sys.argv[2]
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    model = create_frame_rnn_model(calculate_n_bins()).eval()
    model = load_pytorch_weights(model, get_default_weights_path(), strict=False).to(dev)
    proc = create_adtof_processor()
    picker = PeakPicker(thresholds=FRAME_RNN_THRESHOLDS, fps=100)
    if os.path.isdir(root):
        files = sorted(f for f in os.listdir(root) if f.lower().endswith('.wav'))
        paths = [os.path.join(root, f) for f in files]
    else:
        files, paths = [os.path.basename(root)], [root]
    with open(out, 'w', newline='') as fo:
        w = csv.writer(fo)
        w.writerow(['file', 't', 'cls', 'act', 'midi'])
        for i, (f, p) in enumerate(zip(files, paths)):
            try:
                x, sr = load_wav(p)
                if x is None or len(x) < sr // 4: raise ValueError('short')
                x = x[:int(25 * sr)]
                if sr != proc.sample_rate: x = librosa.resample(x.astype(np.float32), orig_sr=sr, target_sr=proc.sample_rate)
                x = (x / (np.max(np.abs(x)) + 1e-9)).astype(np.float32)
                spec = proc.apply_filterbank(proc.compute_stft(x)).T[:, :, None]
                with torch.no_grad():
                    act = model(torch.from_numpy(spec).float().unsqueeze(0).to(dev)).cpu().numpy()[0]   # [T, 5]
                peaks = picker.pick(act, labels=LABELS_5)[0]
                rows = []
                for k, lab in enumerate(LABELS_5):
                    for t in peaks[lab]:
                        fr = min(int(round(t * 100)), act.shape[0] - 1)
                        rows.append((round(t, 3), CLS[lab], round(float(act[fr, k]), 3), lab))
                rows.sort()
                for r in rows: w.writerow([f, *r])
            except Exception as e:
                print('skip', f, e, file=sys.stderr)
            if i % 100 == 0: print(i, f, flush=True)
    print('done', len(files))

if __name__ == '__main__': main()
