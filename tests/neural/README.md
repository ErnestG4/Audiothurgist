# tests/neural — neural opinions and public truth for the corpus tools

Nothing in this directory runs on the module. These scripts give the host
harnesses (`tests/bars_check.cpp`, `tests/classify_bench.cpp`) something to
measure against on the 1,231-break corpus, which carries no labels of its
own. Everything they download or build (venvs, corpora, CSVs) is gitignored.

## Beat This! — beats and downbeats (SLICE package)

`beatthis_corpus.py` (setup line in its header, ~3 GB venv `.bt`) runs
Foscarin, Schlüter & Widmer's Beat This! tracker over the corpus;
`compare_bt.py` compares its tempo and downbeats with `bars_check --csv`.
See `docs/packages/slice.md`.

## ADTOF — kick / snare / hat hits (CLASSIFY package)

`adt_corpus.py` runs the **ADTOF Frame_RNN** drum transcriber (Zehren,
Alunno & Bientinesi, ISMIR 2021 / Signals 2023; the PyTorch port at
github.com/xavriley/ADTOF-pytorch, weights bundled, MDBDrums++ F 88.5) over
every corpus file on the GPU — about 3 minutes for 1,230 files on an RTX
4090 — and writes `adt.csv`: `file, t, cls, act, midi`, one row per hit,
`t` in seconds of the original file (first 25 s, the module's cap), `cls`
0 kick · 1 snare · 2 hat, `act` the model's activation at the peak, `midi`
the model's own class (35 kick, 38 snare, 47 tom → snare, 42 hi-hat, 49
crash/ride → hat).

```sh
cd tests/neural
uv venv .adt --python 3.12
uv pip install --python .adt/bin/python torch torchaudio --index-url https://download.pytorch.org/whl/cu124
git clone --depth 1 https://github.com/xavriley/ADTOF-pytorch
uv pip install --python .adt/bin/python -e ./ADTOF-pytorch soundfile pandas
.adt/bin/python adt_corpus.py ../../analysis/breaks adt.csv        # or wherever the corpus is
```

## IDMT-SMT-Drums — hand-annotated truth (CLASSIFY package)

Dittmar & Gärtner's IDMT-SMT-Drums (Fraunhofer IDMT, zenodo record
7544164, CC BY-NC-ND 4.0; 287 MB): 95 polyphonic drum loops — real kits,
drum machines, sampled kits — with every kick, snare and hi-hat onset
annotated by hand. `idmt_truth.py` turns the XML into the same CSV shape
(`idmt/idmt_truth.csv`) and links the mixes into `idmt/mix/`. It is the
only real ground truth here; the neural CSV is an opinion measured against
it (`compare_adt_truth.py`, hits matched per class within ±30 ms):

```sh
curl -L -o idmt/IDMT-SMT-DRUMS-V2.zip https://zenodo.org/api/records/7544164/files/IDMT-SMT-DRUMS-V2.zip/content
(cd idmt && unzip -q IDMT-SMT-DRUMS-V2.zip) && python3 idmt_truth.py
.adt/bin/python adt_corpus.py idmt/mix idmt/idmt_adt.csv
python3 compare_adt_truth.py idmt/idmt_adt.csv idmt/idmt_truth.csv
```

| ADTOF vs the annotations | precision | recall | F1 |
|---|---|---|---|
| kick | 97.6 % | 93.1 % | 95.3 % |
| snare | 72.2 % | 94.7 % | 81.9 % |
| hat | 91.4 % | 91.0 % | 91.2 % |

The model over-reports snares (546 false snares on 1,499 real ones — most
are hi-hat or kick hits it also hears as a snare); corpus tables built on
it inherit that bias.

## Fitting the Adaptive lens's anchors

`adapt_fit.py` reads a feature dump of a **training** sample
(`classify_bench --sample 200 --seed 2 --modes 04 --dump train.csv`), drops
every file that appears in the measurement dumps, and writes
`core/bb_classify_anchors.h` (per-feature mean and deviation, the three
class means, the diagonal weights found by coordinate descent). Refit
whenever the lens's features change; the doc's tables were measured with
the header as committed.

```sh
/tmp/cb --truth adt.csv --root <breaks> --sample 200 --seed 2 --modes 04 --dump /tmp/train.csv
/tmp/cb --truth adt.csv --root <breaks> --sample 200 --seed 1 --modes 04 --dump /tmp/s1.csv
/tmp/cb --truth adt.csv --root <breaks> --list ../bars_truth.txt --modes 04 --dump /tmp/canon.csv
.adt/bin/python adapt_fit.py /tmp/train.csv ../../core/bb_classify_anchors.h /tmp/s1.csv /tmp/canon.csv
```

Then `tests/classify_bench.sh` prints the standard tables.
