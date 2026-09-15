# The hardware pass — stage 35 (v0.12.3), 2026-09

One ordered session, about 45 minutes, replacing the hike checklist, morning
checklist 2, and the scope / slice-edit checklists. Each step proves the
ground under the next, so stop and report at the first failure rather than
pushing on; the later steps assume the earlier ones. Write the result next
to each step: **ok**, or what you saw. "Listen for" is the thing that
distinguishes pass from fail.

Setup: stage 35 flashed (`audiothurgist.bin` on the Desktop), a card with
the Amen and one break with no hats, the page open from Codeberg Pages on
the desktop and, if the OTG adapter arrived, on the phone. Close `/program`.

## 0. Boot and identity (2 min)

- [ ] Boots to the Perform page; the diag pill reads `audio` under ~60 %.
- [ ] The module pill says **0.12.3-harden** and, after it, the **git hash**
      — the build stamp. If it says `local` the Makefile stamp did not run.
- [ ] Connect works **the first time**. Disconnect, reconnect: first time
      again. (The lone-zero fix; before it, one in two connects timed out.)
- [ ] Unplug the USB mid-session, replug, Connect: first time.

## 1. Clock (5 min) — the GetUs wrap and the multiplier

- [ ] J3 from Pamela's at 24 PPQN, Clock In set to match. Lock within a bar,
      no silence while it acquires, the loop aligns to the pulse grid.
- [ ] P2 to ×2. **Hold two full minutes.** Listen for: it never falls back
      to ×1. (The wrap used to hit at 21.5 s.)
- [ ] ÷2, ×4, back to ×1: each switch lands on a beat, phase continuous.
- [ ] Unplug J3: the module waits on the knob, then runs on its own tempo
      with no jump when P2 is touched.

## 2. Sound with no edits (5 min) — nothing regressed under SLICE-EDIT

- [ ] Classic, verbatim, native tempo: the break as loaded, no doubled
      attacks, no 6 ms notch per 16th on the ride.
- [ ] Pitch +7 and +12 under Slice Tail = Zoom: no duplicated attacks; −12
      cuts clean at the slot end.
- [ ] Grids with Density keyframes and Gate Fires = Onsets only: every
      audible hit gets a gate; a snare's decay steps do **not** re-fire the
      snare lane.
- [ ] Two minutes unclocked in Classic with the pots still: no skip, no
      dropped moment, `regens` on the diag pill does not move.

## 3. Sampling into keys (5 min)

- [ ] Voice map, Grammar = Pads. J4 note picks the slice, J5 style 0–6 as
      before; style 7 arms record (buttons dim red), a gate takes, release
      plays the take back; style 8 clears. Three keys in a row.
- [ ] Sampling = Stereo on J9/J10: the take is stereo; Mono sums J1+J2.
- [ ] Record Source = Output: resample the module's own chop into a key.
- [ ] Kit tab: the recorded keys show class colour, length, waveform with
      onset ticks; the input meter moves during a take.

## 4. The Kit editor (8 min) — the thing nobody has seen

- [ ] Load the Amen. The Kit waveform shows white grid boundaries, blue
      onset ticks, and a **fat handle row** under it.
- [ ] Drag a grid handle slowly. Listen for: the first 250 ms from the new
      start, re-played as you move (≤ 10 a second), then the **whole slice
      once** on release. The handle turns **orange** and stays orange after
      the table refetch.
- [ ] Tap inside the break away from the grid: a **marker** appears; tap
      inside it again: it splits; double-tap the second handle: it merges.
- [ ] Long-press a handle: the menu (audition · merge · promote · copy ·
      delete). Delete one; it is gone from the page and the key that used
      it plays its grid slice.
- [ ] Hold an end handle still: the tail loops; lift: it stops. Now lift
      by dragging off the canvas (a pointercancel): **no stuck loop**.
- [ ] Drag a marker onto a key cell; play that key from J4. Drag a cell
      onto a cell: the prompt; choose swap; both keys swapped. Drop onto a
      **recorded** key: the cell goes red, default is swap.
- [ ] The Chop pad: while the break plays, tap on four snares. Four orange
      markers on the snares' onsets. Change Tap Allowance to 0 and to 80 in
      Options: the readout follows, the marks land earlier / later.
- [ ] The onset histogram: drag the threshold; the count updates; **Make
      markers**; that many grey markers appear; orange ones untouched.
- [ ] Undo three times, redo twice: the table and the audio agree with what
      you did. The history list strikes the undone entries.
- [ ] **On the phone**: a drag from a handle never pans; the strip pans and
      pinch-zooms; a 500 ms long-press raises no context menu; the drop
      finds the cell under your thumb.

## 5. The panel Edit page (6 min)

- [ ] B1 held + B3 tap: B1 amber, the bar ring amber-on-dark with one
      bright segment. **The page did not flip** when B1 was released.
- [ ] P1 walks the segment; each step is silent; the segment pulses on its
      hit. A J4 note selects that key's entry.
- [ ] P2 through centre then left a little: the slice's first 250 ms
      re-plays on every step, and **the page's handle follows the knob**.
      P3 the same on the end.
- [ ] Slice 0's start, the last slice's end: a red blink, nothing moves.
- [ ] B3 tap on a snare while it plays: an orange marker on its onset;
      B3 hold: reslice, and every orange boundary and marker survives.
- [ ] B2 tap: undo (B2 was blue); B2 hold: the slice back on the grid.
- [ ] B1 tap leaves. Chop, tempo and history did not jump.
- [ ] B1 + B3 held long (no Edit) still loads the next file; B2 + B3 for
      2 s in Edit still opens Settings.

## 6. Persistence (5 min)

- [ ] Two seconds after the last edit, P5's ring shows the save; the diag
      pill's worst poll gap during it stays under ~50 ms and the clock
      keeps time.
- [ ] Save a set by name; power-cycle; load it: boundaries, markers, key
      assignments, recordings, options, the loop — and it sounds the same.
- [ ] Load a **loose WAV**, edit it, power-cycle, load it again: the
      sidecar brings the edits back.
- [ ] A Bars change on a set: does the resulting autosave write cause an
      audible hiccup? (Decision 15 in the plan: if yes, autosave narrows to
      hand edits.)

## 7. The scope (4 min)

- [ ] Scope tab: Out L/R trace the beat, kicks stand still under the
      trigger, the fps pill reads 20+.
- [ ] Gates preset: J6 red rising at the left edge with the kick's first
      cycles under it; J7/J8 further along; the button reads the jack's
      role and voltage.
- [ ] Nothing patched: every input-mode jack reads within **±0.1 V of 0**.
      (If a jack shows a fixed offset, that is the calibration transform —
      report the number.)
- [ ] LFO into J4, CV preset, time ×32: the whole LFO over 8 s.
- [ ] Back to the Play tab: state polling returns to 10 Hz, the small
      scope still moves.

## 8. The Adaptive lens (3 min) — keep or remove

- [ ] Amen: Classifier Hockman → Adaptive. Listen for: backbeats that
      Hockman painted yellow turn green and the snare lane fires more.
- [ ] The no-hat break: Adaptive shows two classes, not three (the guard).
- [ ] Verdict: keep as option 4 / remove.

## Known and not to be reported

DTCM ~76 %, SRAM ~83 %; the Play tab's small scope uses the old request on
purpose; under Swing Auto odd steps' class can sit up to 0.3 step off; a
preset save costs one 10–50 ms poll gap; iOS has no browser USB.
