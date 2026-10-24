# The hardware pass — v0.13.0-beta, 2026-10

One ordered session, about an hour. It replaces `hardware-pass-2026-09.md`
(stage 35, never run): everything that pass covered is still here, after the
new work, because stages 30–35 have never been on hardware either. Each step
proves the ground under the next, so **stop and report at the first
failure**. Write **ok** or what you saw next to each box. "Listen for" is what
tells pass from fail.

Setup: `audiothurgist.bin` from the Desktop's `Audiothurgist-flash/` flashed
(0.13.0-beta); a card with the Amen and one break with no hats; the page from
the local server (address in the hand-off message) on the desktop and, if
you have the OTG adapter, the phone. Close `/program`.

## 0. Boot, identity, load (3 min)

- [ ] Boots to the Perform page. The page's status line names
      **0.13.0-beta** and a git hash.
- [ ] Connect works first time; the button reads "connecting…" meanwhile.
      Disconnect, reconnect: first time. **Unplug the USB mid-session**: the
      page says disconnected by itself (it used to keep polling) and the
      readouts clear. Replug, Connect: first time.
- [ ] Play the Amen. The **CPU** meter in the transport bar shows a steady
      mean and a higher peak tick. Note both numbers (the beta's baseline).
      P1 full left (reverb, cluster zone): the numbers rise; overruns stay 0.

## 1. Start / stop (4 min)

- [ ] Page **▶/■**: the pattern stops dead; the clock LED keeps pulsing;
      press again on a downbeat — *listen for* the break coming back **in
      time**, not from step 1.
- [ ] Panel: hold **B1**; at 0.6 s it turns white; let go — the same stop.
      B1 breathes while stopped; the page's button follows within a beat.
- [ ] B1 tap still flips Perform ↔ Brain. B1 held + a pot turned records a
      lock and does **not** toggle. B1 held 2 s, let go clean: a toggle,
      never a page flip. B1 held + B3 tap still enters Edit.
- [ ] Stopped, play a key (page pad or J4): it sounds.

## 2. Modes and the page (5 min)

- [ ] Play tab: the mode chips Classic · Grids · Loop · Pads (+ more). Each
      switches the module within a bar, and the line under the chips says
      which jack map it brings.
- [ ] **Pads**: the slice view shows the kit (keys lit as they fire), **no
      grid ghosts**; the bar strip doesn't take clicks. Dice and Flow are
      greyed.
- [ ] **Loop** (Play grammar): the transport's Dice reads **Undo** and
      Reslice reads **Clear**, which asks before wiping the loop.
- [ ] **Pads from the page**: press three pads with three fingers (phone)
      or one after another (mouse): each plays its key; a pad lights when
      the same key comes from J4. With Key Plays = Held a pad sounds until
      you lift.
- [ ] Options: every option sits in a named group; nothing under "Other".
- [ ] Phone: one status line, tabs scroll sideways, ▶/■ at the bottom under
      the thumb, nothing scrolls the page sideways. ◐ toggles light/dark.

## 3. CV keys — one keymap (6 min)

- [ ] Voice map (Pads or Loop with Jacks = Follow grammar). **Unpatch J5.**
      0 V into J4, gate on J6 — *listen for* key 0's slice **forwards** (the
      floating J5 used to read reverse). The Jacks card shows `C1 · key 0`
      and the live voltage.
- [ ] Walk up a chromatic octave: one key per semitone, the card follows.
      −1 V: key 0, **no mute**.
- [ ] **A sequencer that changes pitch with its gate** (the last-note fix,
      23 Oct): a fast run of different notes into J4 with gates on J6 —
      *listen for* every hit playing **its own** key, never the previous
      one. Then very short triggers (1–2 ms): each still plays.
- [ ] J5 at 1/12 V: reverse. J5 style 7 (7/12 V): record into the key held;
      style 8: clear it.
- [ ] Brain map (Classic, Jacks = Follow grammar): gate on **J5**, the same
      notes on J4 — the same keys, plain. 0 V plays key 0 and never mutes.
      J6/J7/J8 still send kick/snare/hat gates.

## 4. Markers, chop, autoslice (8 min)

- [ ] **Stopped**, nothing playing: the Kit tab's **Chop** pad is greyed
      and says why. In Edit, B3 tap blinks red.
- [ ] Playing: tap Chop on a snare, then again within a moment on the same
      snare — **one** orange marker, the second tap selected it.
- [ ] Autoslice slider to 8: eight grey markers. Up to 9: the eight stay,
      one joins. Down to 7: the last one goes. **Re-roll**: a different set.
      Undo once: the whole drag is undone. Orange markers survive all of it.
- [ ] Panel: Edit page (B1 held + B3 tap). P5's ring shows the grey count
      (dim until you sweep through it). Up a detent at a time — one more
      chop each. Sweep to 0 and back up: a different set. B2 tap: the sweep
      undone. Leave Edit: the Sample highlight did not move.
- [ ] Classic: **Markers → keys** — the keys play the markers in time order;
      undo restores the old keys. **Clear markers** asks, then every marker
      goes and the keys return to the grid; one undo brings all back.
      A recorded key survives both.
- [ ] **Pads** with markers: keys 0, 1, 2… are the markers in time order.
      Add a marker before the first: key 0 now plays it. Undo once: the
      marker and the old keys both come back.

## 5. Clock (5 min) — from the stage-35 pass

- [ ] J3 from Pamela's at 24 PPQN: lock within a bar, no silence while it
      acquires. P2 to ×2 and **hold two full minutes**: it never falls to ×1.
- [ ] ÷2, ×4, back to ×1: each switch on a beat. Stop/start under the
      clock: the restart lands on the clock's beat.
- [ ] Unplug J3: the module waits on the knob, then runs on with no jump.

## 6. Sound with no edits (4 min)

- [ ] Classic, verbatim, native tempo: the break as loaded, no doubled
      attacks, no notch per 16th on the ride.
- [ ] Pitch +7 / +12 with Slice Tail = Zoom: no duplicated attacks; −12
      cuts clean.
- [ ] Grids, Gate Fires = Onsets only: every audible hit gets a gate; a
      snare's decay doesn't re-fire the snare lane.
- [ ] Two minutes unclocked, pots still: no skip; `regens` doesn't move.

## 7. Sampling into keys (4 min)

- [ ] Sampling = Stereo on J9/J10: a take into a key is stereo; Mono sums.
- [ ] Record Source = Output: resample the module's own chop into a key.
- [ ] Kit tab: recorded keys show class colour, length, waveform; the input
      meter moves during a take.

## 8. The Kit editor (6 min)

- [ ] Drag a grid handle slowly — the first 250 ms re-plays as you move,
      then the whole slice once on release; the handle turns orange.
- [ ] Tap inside the break: a marker; tap inside it: split; double-tap the
      second handle: merge. Long-press a handle: the menu; delete works.
- [ ] Hold an end handle still: the tail loops; drag off the canvas to lift:
      **no stuck loop**.
- [ ] Drag a marker onto a key; play it from J4. Drag a key onto a key:
      swap.
- [ ] Undo three times, redo twice: the table and the sound agree.
- [ ] Phone: a handle drag never pans; the strip pans and pinch-zooms.

## 9. The panel Edit page (4 min)

- [ ] B1 held + B3 tap: B1 amber; the page did not flip on release.
- [ ] P1 walks the segment; P2/P3 nudge start/end through centre and **the
      page's handle follows**.
- [ ] B3 tap on a playing snare: an orange marker. B3 hold: reslice, every
      orange mark survives. B2 tap undo; B2 hold: back on the grid.
- [ ] B1 tap leaves; chop, tempo and history didn't jump.

## 10. Persistence (4 min)

- [ ] Two seconds after an edit, P5's ring shows the save; the clock keeps
      time through it.
- [ ] Save a set; power-cycle; load it: boundaries, markers, keys,
      recordings, options and the loop come back and sound the same.
- [ ] A loose WAV edited, power-cycled, reloaded: its edits come back.
- [ ] `/program` with your **fullest card**: the file list loads (the
      manual grew; the descriptor shares a 24 KB buffer with the list).

## 11. Scope (3 min)

- [ ] Scope tab: Out L/R trace the beat (this tab was hidden by a duplicate
      id bug before the beta — the old page never showed it).
- [ ] Gates preset: J6 rising at the left edge with the kick under it.
- [ ] Nothing patched: each input jack within ±0.1 V of 0 (report offsets).

## 12. Adaptive classifier (2 min) — keep or remove

- [ ] Amen, Classifier Hockman → Adaptive: do backbeats turn green and the
      snare lane fire more? No-hat break: two classes, not three.
- [ ] Verdict: keep / remove.

## Calls to make while you're there

1. **P5 autoslice range** 0–32 on the panel, 0–64 on the page — match them?
   And does a whole P5 sweep as **one** undo step feel right?
2. **Entering Pads** rewrites the keys to the markers (as its own undo
   step). Wanted, or only when markers change?
3. **B1's white "let go now"** cue: keep?
4. **Light theme**: the display canvases stay dark like a lit screen — fine?
5. **Pads on the Play tab** in pattern modes (Classic, Grids…): keep them
   second, or hide them there?
6. **Pads mode** still draws the bar strip, dimmed: keep or hide?

## Known and not to be reported

SRAM ~92 %, DTCM ~85 %, SDRAM ~59 % (the undo journal grew to 152 KB); the
Play tab's small scope uses the old request on purpose; a preset save costs
one 10–50 ms poll gap; iOS has no browser USB.
