# Hike checklist — what landed while you were out (2026-09-07)

Flash `Desktop\Audiothurgist-flash\audiothurgist.bin` = **stage 28, v0.11.1-reviewed**.
Fallbacks: stage 26 (v0.10.1, before sample mode), stage 25 (SDK v0.11 + boost only).
Web app: `cd ~/Daisy/Audiothurgist/web && python3 -m http.server 8080`; the **Kit** tab
is new (`?demo=kit` shows it without the module). Longer scripts per piece:
`docs/packages/sample-record.md`, `sample-looper.md`, `sample-sets.md`, `sample-engine.md`.

## 0. First: the pitch knob
- [ ] Classic, verbatim, pitch +7 and +12: no more doubled attacks. Slice Tail = **Zoom** reads on into the next slice only when that slice does not open with a drum; otherwise it loops this slice's tail for the rest of the slot. Pitch −12: cut at the slot end. Set Slice Tail = Gate to hear the old gap for comparison.

## 1. Voice jack map and the two play grammars (earlier today, in case you skipped them)
- [ ] Options → Jacks = Follow grammar; Grammar = **Pads**: J4 slice note, J5 style note (0 plain · 1 reverse · 2 half · 3 double · 4 ratchet · 5 staccato · 6 punch/choke flip · **7 Record · 8 Clear**), J6 gate, J7 trigger out, J8 velocity out. Change the style while holding a key: it re-strikes.
- [ ] Grammar = **Play**: everything you play loops at its exact timing (Quantize = Off). **B2 = undo the last overdub**, **B3 = clear**. Quantize = 16ths gives the old punch-to-grid; 8ths snaps to 8ths.
- [ ] Options → **Key Plays = Held**: a key plays from its slice to the end of the material while the gate stands; release fades it.

## 2. Sampling into keys
- [ ] Audio into J1/J2, Voice map. Play J5 style **7**: buttons go dim red. Hold a key on J4: buttons full red, P6 shows the input level, the take records until you release (or 8 s). Release: the key plays the recording, its position lights on P5's ring (keys 0–7 on LEDs 0–7, 8–23 paired above).
- [ ] Sample a few keys in a row without leaving style 7. Any other style disarms.
- [ ] Style **8** + a key: back to the break's slice for that key.
- [ ] Options → **Sampling = Stereo**, record from a stereo source: stereo on J9/J10. Mono sums J1+J2.
- [ ] Options → **Record Source = Output**: record the module's own chop into a key.
- [ ] Kit tab: the 24 keys, recorded ones with class colour and length, the waveform with onset ticks; record/clear from the page (a browser take starts at once and a second click stops it).

## 3. Loop take into the break
- [ ] Options → **Record = Arm**. B3 tap (or the first J6 edge) starts a take into the other buffer. **Clocked**: it runs for the Bars count and stops on the bar by itself; J6 edges during it drop slice markers (two or more replace the grid). **Unclocked**: B3 or J6 stops it. The take runs the load pipeline and plays; the arm drops after one take. Note: loads now keep 8 s of the 25 s buffer free for keys (17 s max per file).

## 4. Sets and backups on the card
- [ ] Kit tab → Set → name → **Save**: P5 sweeps, then `sets/<name>.wav` + `sets/<name>.json` exist (directories are created for you). Power-cycle. **Load** it (Kit tab, or P5 + B3 on the "[set]" entry): keys, recordings, options and the looper come back and it sounds the same.
- [ ] Save a preset (Settings gesture or the browser): `presets/<slot>.json` appears. This is the safety net: if a future option-schema bump wipes the QSPI slot, the JSON restores options and knobs at boot.
- [ ] Watch the diag pill during a save: the clock keeps time (a keep-alive ticks it every 40 ms).

## 5. Platform (from before the hike)
- [ ] Running at **480 MHz** on SDK v0.11 with the local boost patch (rebase when Luke's point release lands). Diag pill `audio` µs should read ~20 % lower than before; `regens` should sit still with pots untouched (v0.11's pot filter).

## Review outcome (adversarial pass over the whole sample diff)
GO for the bench. Four must-fixes applied in v0.11.1: a browser take could not be stopped; overflow-safe slice bounds from a hand-edited set file; an interrupted save could leave a stale JSON beside a new WAV; a release posted in the same poll as a re-strike killed the new held note. Plus: a per-block cap on looper fires, releases on map switch, Clear forgets the recorded entry, yields in the trim scans, ±inf phases rejected, velocity out for recorded keys, snapshot timeout refuses rather than saving stale.

**Measure on the bench:** `audio` µs with Sampling = Stereo and a dense Play loop quantized to 8ths (the review's one CPU concern). **Known:** AXI SRAM at 83 % (code + FatFs buffers; 84 KB headroom; the save staging can alias the loader's later); a preset save costs one 10–50 ms poll gap; the QSPI slots re-initialised on this build (schema OPT6) — the JSON backups start from here.
