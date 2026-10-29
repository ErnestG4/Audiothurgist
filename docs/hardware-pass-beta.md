# The hardware pass — v0.13.0-beta, 2026-10

One ordered session, about an hour. It replaces `hardware-pass-2026-09.md`
(stage 35, never run): everything that pass covered is still here, after the
new work, because stages 30–35 have never been on hardware either. Each step
proves the ground under the next, so **stop and report at the first
failure**. Write **ok** or what you saw next to each box. "Listen for" is what
tells pass from fail.

Setup: `audiothurgist-beta7-clean-c80709e.bin` from the Desktop's
`Audiothurgist-flash/` flashed (md5 f6d45945…), and the page from the same
commit (GitHub Pages after the push). The page checks the firmware's api
number and refuses any other build, so the two always go together.
**Flashing it drops the saved presets once**: each slot that has a backup on
the card (`Audiothurgist/presets/<slot>.json`) comes back at the first
power-up with its options, knobs, sample and name, but not its motion locks
or the brightness. A slot without a backup is gone. Re-make the locks you
care about and save the slot again.

Bring a card with the Amen and one break with no hats; the page from
https://ernestg4.github.io/Audiothurgist/ on the desktop and, if you have the
OTG adapter, the phone. Close `/program`.

This firmware reads only `Audiothurgist/` on the card, so a card laid out
the old way (WAVs in the root) boots to the synth break with nothing on P5.
**Do §10b's first two boxes first** (the banner, then Copy them in): that
moves your breaks into the new folder and proves the copy, and everything
from §0 on then has the Amen to play.

**This build powers up** as a sampler (Pads, the
pattern stopped). Do **§10d's first two steps right after flashing** (they
check the new start-up); then hold B1 (the boot preset's beat comes back) or pick Classic on the page and
press ▶ before §0 — or set Start In to *As saved* (Options → Power-up, then
save slot 0) and power-cycle, and the
rest of this pass boots as written.

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
      one. Then very short triggers (1–2 ms): each still plays — and with
      the key on **Held**, **Loop** or **Ratchet** (Kit → the key's Plays, or
      Key Plays = Held) each short trigger still **releases**: a short note,
      never a key left sounding.
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

## 10b. The card — one folder, uploads, an old card copied in (8 min)

`docs/packages/card.md`. The first two boxes
you did at the start; do the rest here.
Bring the **old card** (WAVs in the root, a `sets/` and a `presets/`) and,
for the last box, a blank one. A clock on J3 and a break playing throughout.

- [ ] Boot on the old card: the synth break plays, P5 lists nothing; the page
      shows the banner ("keeps its breaks the old way … moves nothing for
      you") and the Kit's Card panel says the same with the counts.
- [ ] **Copy them in** while a break plays. *Listen for*: no click, no
      dropout, no tempo wobble against the clock; P5's ring fills with the
      progress. Then P5 and the page list the breaks and sets; on a computer
      the root still holds every original and `Audiothurgist/` the copies.
- [ ] **Copy them in** again (from the panel): everything "already there".
- [ ] Drop three files on the page while the break plays — a 44.1 kHz
      stereo WAV, an MP3, a 30 s file. *Listen for*: **no glitch and no drift
      against the clock** while they transfer and while the module writes
      (P5's ring). Write down the KB/s the rows show: ____ KB/s.
- [ ] **Load it now** on each: the stereo one plays stereo (Sampling →
      Stereo), the 30 s one is its first 17 s, the MP3 sounds right (no
      pitch shift: the 48 kHz conversion).
- [ ] The same file again: "already on the card"; **Replace it** replaces it.
- [ ] Start a set save while an upload transfers: one of them waits or is
      refused, never a broken file; the upload still lands.
- [ ] Unplug USB mid-upload: no `<name>.wav` on the card afterwards; plug
      back, connect, upload again.
- [ ] No card in: the panel says "no card", an upload is refused plainly.
- [ ] Blank card: the first upload creates `Audiothurgist/breaks` itself;
      a preset save adds `Audiothurgist/presets/<slot>.json`.

## 10c. Samples onto pads, an empty kit, 45 s of memory (10 min)

`docs/packages/pads.md`. Nothing of this has run
on hardware. A clock on J3 and a break playing, Classic, for the first three
boxes; bring a few one-shots (a mono kick, a **stereo** pad or loop, a 30 s
file) on the computer.

- [ ] Boot: the page's Pads row reads "pads: 28.00 s of 28.00 s free" with a
      17 s break loaded (more with a shorter one). A 17 s break still loads
      whole; a take into a key (J5 style 7) still records — path 2.
- [ ] Drop the kick on a pad **while the break plays with the clock on J3**.
      *Listen for*: **no glitch, no dropout and no drift against the clock**
      while it transfers and while the module places it (a fraction of a
      second); the J4 keys keep answering during it. The pad says "✓".
- [ ] Play that key from the page, from J4 (Voice map: J6 gate) and from the
      panel (Pads, any key the ring shows): it plays the kick as the file
      is — no trim, no level change — and at the same pitch at 90 and 170 BPM
      (P2) and with the clock: recordings keep their pitch now; P4 still
      transposes. *Call*: is that right for takes too (path 2)? ____
- [ ] Drop the **stereo** file: the line says the kit is stereo now, Options →
      Sampling reads Stereo, the pad plays in stereo (pan it), and the break
      sounds exactly as before on both sides. *Listen for*: no click at the
      moment the kit turns stereo, with a pad held.
- [ ] Undo (Kit → Edit): the drop is gone, the key plays what it did before;
      Redo brings it back.
- [ ] Drop onto the same pad again: the new one plays; the room line now says
      "… held by replaced pads". Save the set and load it back: still held
      (not reclaimed — expected).
- [ ] Drop the 30 s file with less room than that: "the first … of 30.0 s,
      all the room left"; then any drop: "no room left".
- [ ] While a drop transfers, start a set save / a card upload / B3-hold load:
      each refused or held, never both at once; the drop still lands.
- [ ] **New empty kit** (asks first): the break goes, Pads, "44.75 s free · no
      break", every pad empty (silent), the clock keeps its tempo, no gate on
      J6–J8. Drop three samples on three pads (one stereo).
- [ ] The **unsaved kit** note shows; **Save set** as `padkit`; the note goes.
- [ ] **Power-cycle**. The boot material plays (the kit was in memory); load
      `padkit` (P5 + B3 or the page): no break, the three pads on their keys,
      stereo, no tempo jump, playable from the page, J4 and the panel.
- [ ] Load a break after it: the break plays at its own tempo; the pads are
      gone (new material); a take into a key works.

## 10d. Start as a sampler; import and play single samples (10 min)

`docs/packages/pads.md` § Start In. Bring a mono one-shot, a **stereo** sample and a quiet file
(a soft hat) on the computer; a clock on J3 for step 8.

1. **Power up.** *See*: B1 breathing cool white (stopped), the P1 bar ring
   dimmed with only the playhead going round (Pads writes nothing), no
   gates out. Nothing plays until a key does. (The first power-up after
   flashing: Start In takes its default, Sampler, even for a slot restored
   from its backup.)
2. **The preset is untouched**: Settings (B2 + B3 2 s) → load a slot with a
   grammar other than Classic. *See*: its grammar, **stopped** (B1 still
   breathes); ▶ (or B1 held 600 ms) starts it on the beat. Power-cycle
   again: Pads, stopped — Start In overrides the boot preset's grammar, not
   the preset. (A preset **saved** in this sampler state keeps the beat it
   came with, not Pads; one you put on Pads yourself saves as Pads.)
3. **Connect the page.** *See*: the Play tab opens on the pads, a line over
   them "Sampler — the pads play samples…" with **Classic** / **Grids**
   buttons, the transport "**sampler** · Pads", the Pads chip lit, the Jacks
   card "Voice map" with J4 live.
4. **Keys play while stopped**: a page pad, and J4 + a J6 gate. Each sounds
   its key; the transport stays stopped. (The panel has no way to play a key.)
5. **Drop the mono one-shot on C1, the stereo sample on C♯1.** *See*: "✓",
   the line "mono, …" then "stereo, … — the kit is stereo now"; Options →
   Sampling reads Stereo. Play both from the page and from J4 (C1 = 0 V,
   C♯1 = 1/12 V): each as the file is; C♯1 in stereo (pan it).
6. **Normalise drops**: tick it (beside *Choose samples…*), drop the quiet
   file on D1. *See*: "normalised +x dB" in the line and D1 near full
   level. Untick, reload the page: it stays unticked (and ticked if you leave
   it ticked).
7. **Save set** (the Unsaved kit note) as `smpkit`. Power-cycle: Pads,
   stopped again, the pads back on the break (memory). Load `smpkit`: C1,
   C♯1, D1 back, stereo, still a sampler.
8. **The flip**: with the clock on J3, press **Classic** in the sampler line.
   *See*: nothing starts; B1 still breathes; under the modes "Stopped —
   press ▶ to start Classic", ▶ ringed in orange. Press ▶ (or hold B1
   600 ms): *listen for* the beat coming in **on the beat**, in time with
   the clock, not from step 1 at a random moment.
9. **Start In = As saved**: Options → Power-up → Start In → As saved; the
   sampler line stops saying it powers up like this. Power-cycle without
   saving: Pads, stopped (Start In is the boot preset's, and the preset still
   says Sampler). Set As saved again and **save slot 0** (Settings, or
   `/program`); power-cycle: the boot preset's grammar, **running**. Take the
   card out and power-cycle: the same (no card changes nothing now). Set it
   back to Sampler and save slot 0 again. In `/program` the Options panel
   shows Start In with the other options.

10. **No computer**: unplug USB, power-cycle (Sampler). Play a few keys
    from J4, then **hold B1 600 ms, let go**. *Listen for*: the boot preset's
    beat (its grammar comes back) starting on the beat. Hold B1 again: it
    stops and stays on that grammar. On the page the same holds for ▶ while
    the mode chips show Pads.

*Call*: a start made in Pads brings back the last drum grammar (built
overnight, e35b0e7). Keep it, or should ▶ / B1 in Pads stay a silent
no-op and the beat come only from choosing a grammar? ____

## 10e. Each key's own gain, tune and Plays (10 min)

`docs/packages/keyp.md`. Nothing of this has run
on hardware. A break loaded and two pads dropped (a kick on C1, a long pad or
vocal on D1), the page on the Kit tab, the clock on J3.

1. [ ] Untouched: everything sounds exactly as before this firmware — the
       break in Classic, a key from J4, Play's loop. No key cell or pad shows
       any orange words.
2. [ ] C1 selected, Gain to −12 dB: C1 is clearly quieter from the page's
       pads and from J4; the break's own kick (the pattern's
       steps) is **not** quieter. Gain to +6 dB: louder, no distortion on a
       normalised take beyond the output's own clip (report if it clips).
3. [ ] Drag the Gain slider through its range and back: it follows live, and
       **one** Undo (Kit → Edit) puts it back where the drag started; the
       history shows one line.
4. [ ] D1 Tune +7 st, then cents −50: up a fifth less a quarter tone, at the
       same pitch at 90 and 170 BPM; P4 still transposes on top. A grid key
       (any key not holding a pad) tuned −12 st plays an octave down **at
       the tempo's varispeed** (move P2: it follows the tempo as before).
5. [ ] D1 Plays → **Held** with Options → Key Plays on One-shot: it sounds as
       long as J6's gate (or the page pad) is held, then fades. Another key
       on One-shot still plays one-shot.
6. [ ] D1 Plays → **Loop**: hold it for several seconds — it loops D1's own
       sample (not into the next pad), the seam is smooth enough to use
       (*listen for*: a click at the wrap; report how bad), and it fades on
       the release. While it loops, play other keys and run the pattern:
       the loop keeps going. **Reversed** (Voice map, J5 at style 1): it
       loops D1 **backward** from its end, the same seam, the same release.
       Turn J5 between 0 and style 1 while the gate stays up: each change
       re-strikes D1 and the new loop **replaces** the old one (a short fade
       under it) — one loop at a time, never two stacked. Release right as
       it wraps: the fade is as long as anywhere else (no click).
7. [ ] C1 Plays → **Ratchet**, every ½ sample: hold C1 — the kick retriggers
       from its start twice per kick length, *no click at the seams*, until
       you let go; J7 sends a trigger per hit. Try ¼ and ⅛ (faster), and the
       tempo rates 8th / 16th / 32nd with the clock on J3: they lock to the
       clock (a hit ≤ 0.5 ms late at most).
8. [ ] Ratchet + a pitched key (Tune +12): the ½-sample ratchet runs twice as
       fast (the sample is half as long as it plays).
9. [ ] Play mode (Quantize Off): play the ratcheting C1 into the loop for a
       beat and let go — the loop replays the burst for as long as you held
       it. Now set a key to Ratchet that was played into the loop *before*
       (as a one-shot): on replay it fills its 16th (two hits at the 32nd
       rate), then stops by itself. *Call*: right rule? ____
10. [ ] Save the set, change every setting, load the set back: the settings
        return (cells show the same words). Power-cycle and load it: same.
        Load a loose break: every key back to neutral.
11. [ ] CPU meter (transport): with eight voices sounding (Held keys) and
        four keys ratcheting at ⅛, note loadAvg / loadMax ____ (expected:
        within a few ‰ of the same scene with neutral keys).

## 10f. One sample, played chromatically (8 min)

See `docs/packages/chroma.md`. Nothing of this has run on hardware. A tonal
sample with a clear pitch (a bass pluck, a piano note, a sung vowel) dropped
on **G1**, nothing else on a pad; a V/oct sequencer into **J4** and its gate
into J6; a tuner on the output if you have one; the page on the Kit tab.

1. [ ] Select G1, press **Spread G1 chromatically (C1 … B2)**: no prompt; the
       cells now read −7 st … −1 st under C1 … F♯1, nothing under G1, +1 st …
       +16 st under G♯1 … B2. One history line.
2. [ ] Sequence a melody on J4 (a major scale up and down, then a tune you
       know). *Listen for*: one sample, every note in tune with the
       sequencer's other voices — G1 (≈ 0.583 V) is the sample at its own
       pitch, G2 (≈ 1.583 V) exactly an octave up (tuner: same note, ±5
       cents), C1 (0 V) a fifth below G1.
3. [ ] The same melody at **90 BPM, then 170 BPM** (P2, or the clock on J3):
       the pitch does **not** move — only the notes' timing. P4 still
       transposes the whole spread together.
4. [ ] G1's own settings came along: set G1 Plays → Held *before* spreading
       (Undo, change it, spread again) and every note holds for its gate.
5. [ ] **Undo** once (Kit → Edit): every key back as it was (the words gone,
       J4 plays the kit again). Redo: the melody again.
6. [ ] Drop a kick on C1, spread G1 again: the red prompt names C1. *spread
       around them* — C1 is still the kick, the rest the melody; Undo,
       spread, *spread over them too* — C1 is G1's sample −7 st (the kick is
       still in the kit: Undo brings it back). *Call*: which should be the
       default? ____
7. [ ] A slice of the break instead (Undo first; the Amen loaded, select a
       key that plays a snare slice, spread it, *around* the pads): J4 plays
       the snare chromatically, and here moving P2 from 90
       to 170 BPM **does** raise the whole spread with the tempo (expected:
       a break's slice varispeeds).
8. [ ] Save the set with a spread, load something else, load it back (and
       once after a power-cycle): the melody comes back on J4.
9. [ ] The ends: spread from C1 (B2 = +23 st) and from B2 (C1 = −23 st) —
       note how the extremes sound (aliasing high up, grain low down) ____.

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
7. **The card folder at boot**: make `Audiothurgist/breaks` on a card that
   has none (so a computer shows where breaks go), or only on the first
   write (now)?
8. **Spread across keys and pads** (§10f.6): the prompt's default is *spread
   around them* (keys with their own pads keep them). Flip to *over them*?
9. **What a spread copies**: every key takes the root's gain and Plays (and
   its cents on top of the semitones). Keep, or tune only and leave each
   key's gain / Plays?
10. **Spread in Pads with markers**: like a hand-made key map, it stands until
    the markers next change — then the keys follow the markers again (their
    tunes stay). Wanted?

## Known and not to be reported

SRAM ~93 %, DTCM ~85 %, SDRAM ~64 % (the undo journal grew to 152 KB; the
card's upload buffer is 3.3 MB); the
Play tab's small scope uses the old request on purpose; a preset save costs
one 10–50 ms poll gap; iOS has no browser USB.
