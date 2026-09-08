#!/usr/bin/env python3
"""IDMT-SMT-Drums (Dittmar & Gärtner 2014; zenodo.org/record/7544164, CC BY-NC-ND) → the adt_corpus.py CSV
shape, from the manual annotations of the 95 #MIX loops (KD / SD / HH → 0 / 1 / 2, act = 1, midi = 35/38/42).
Also builds idmt/mix/ (symlinks to the mixes) so the same tools run on it.
Setup: curl -L -o idmt/IDMT-SMT-DRUMS-V2.zip https://zenodo.org/api/records/7544164/files/IDMT-SMT-DRUMS-V2.zip/content
       && (cd idmt && unzip -q IDMT-SMT-DRUMS-V2.zip) && python3 idmt_truth.py
"""
import os, csv, glob, xml.etree.ElementTree as ET
here = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'idmt')
os.makedirs(os.path.join(here, 'mix'), exist_ok=True)
M = {'KD': (0, 35), 'SD': (1, 38), 'HH': (2, 42)}
n = 0
with open(os.path.join(here, 'idmt_truth.csv'), 'w', newline='') as fo:
    w = csv.writer(fo); w.writerow(['file', 't', 'cls', 'act', 'midi'])
    for x in sorted(glob.glob(os.path.join(here, 'annotation_xml', '*#MIX.xml'))):
        root = ET.parse(x).getroot()
        f = root.find('globalParameter/audioFileName').text
        src = os.path.join(here, 'audio', f); dst = os.path.join(here, 'mix', f)
        if not os.path.exists(dst): os.symlink(src, dst)
        rows = []
        for ev in root.iter('event'):
            ins = ev.find('instrument').text.strip()
            if ins not in M: continue
            rows.append((round(float(ev.find('onsetSec').text), 4), M[ins][0], 1.0, M[ins][1]))
        rows.sort()
        for r in rows: w.writerow([f, *r]); n += 1
print('events', n)
