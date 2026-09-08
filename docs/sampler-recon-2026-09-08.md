# Sampler recon — slicing, point editing and key assignment, 2026-09-08

*Research only, no code. Ernest wants "the Sample Drum experience" in the
Kit tab: slice in the UI, move slice starts, swap slices between keys —
"do a recon of all the best current samplers and their most loved and
hated features / UX and try to avoid the pitfalls while matching all the
peaks." Sources: manufacturer manuals, SOS / MusicRadar / MusicTech /
Bonedo reviews, ModWiggler, Elektronauts, Gearspace, MPC-Forums,
Image-Line / Renoise / Polyend / Loopy Pro forums, Apple / Ableton /
Bitwig / NI docs. Every non-obvious claim carries its URL; claims that
rest only on a search snippet are marked (snippet). Four parallel
researchers did the device reads; this document is the synthesis.*

## 0. What we already have (so the recommendations land on it)

- One material buffer, one slice table: grid entries `[0, nSlices)` (the
  break, `{start, len}`, what the grammars, clock, bar rule and gates see)
  and recorded entries `[nSlices, nTable)` (one per key that has been
  recorded into), `kMaxTable = 96`. `keySlice[24]` says which table entry
  each Voice-map note plays; `SetKeySlice(key, idx)` already exists
  (control thread). `CommitSlice` publishes a region as a key's entry
  (`docs/packages/sample-engine.md`).
- A voice copies its bounds at trigger (`Voice::bound = ReadEnd(idx)`), so
  a table edit never touches a voice in flight — the property that makes
  edit-while-playing safe for free.
- Per-slice loop points are re-searched on commit; fades are fixed or
  adaptive (keyframe density); choke is 5 ms; Slice Tail = Gate / Loop /
  Ping-pong / Zoom.
- Sets = `sets/<name>.wav` (a `cue ` chunk at every slice start) +
  `.json` (table, keys, class, options, looper events)
  (`docs/packages/sample-sets.md`, `sample-persist.md`). WAV cues are
  the de-facto interchange format for every module surveyed (§A1).
- The page: Kit tab with 24 key cells, `GET_WAVE` windows (≤ 3/s, cached),
  `GET_TABLE` on a table-signature change, `GET_HITS` onset ticks,
  `ACTION` 0–15 (rec arm/clear, key plays, looper undo/clear, set
  save/load). No slice editing yet; `max_body` 1024.
- The panel: six pots with LED rings (pickup behaviour: a dim ring is a
  pot waiting to be swept through its value), three buttons, no screen.
  B3 tap = reslice, hold = load; B2 + B3 held = Settings.

---

# Part A — the survey

## A1. Eurorack

**Erica Synths Sample Drum** — the namesake; see §4 in full. Short form:
OLED (monochrome, not colour), two mono channels, 64 slots each, up to
**32 slices** per sample, auto-slice LIN (equal) or ZC (equal, snapped to
zero-crossings) — **no transient detection at all** — plus WAV-marker
import and manual add/nudge/remove with zoom. Slice select by encoder,
by FWD/BKW/RND step, or by CV (evenly divided, or 1V/oct = 32 notes from
C0). Three assignable CV inputs per channel with mini-scopes and
attenuators. Edits live in RAM projects (nothing autosaves).

**1010music Bitbox mk2 / Micro.** Hybrid slicer per pad: *manual* (scroll
the waveform under a fixed cursor, touch Split / Join), *Scan* (threshold
%), *Grid* (count); WAV markers import as slice points
([manual](https://1010music.com/wp-content/uploads/2023/03/bitbox-mk2-2.0-user-manual-2023-03-01-clean.pdf)).
Markers are not dragged — the audio scrolls under the cursor; pinch to
zoom. Slices are addressed by a per-pad `Slice` parameter (CV/CC
modulatable), `Slice Seq` (Forward / Backwards / Random / Stagger) or MIDI
note; `Play Thru` continues past the slice end. There is no "explode
slices to pads" — a new owner asked exactly that and got no answer
([ModWiggler](https://modwiggler.com/forum/viewtopic.php?t=280854)).
Loved: "worth it just for the bigger screen"; "Bitbox had the easiest
interface" ([MW](https://modwiggler.com/forum/viewtopic.php?t=263734)).
Hated: Micro's screen ("I'm often pushing things multiple times to get it
to react… running auto-slice isn't quick"), mk2 screen flicker, output
assignment, "unpredictable audio dropouts on samples… incredibly
stressful to use while playing live"
([MW](https://modwiggler.com/forum/viewtopic.php?t=288147)), and the
menu depth that every Sample-Drum-vs-Bitbox thread cites ("Sample drum
wins hands down on UI. Micro wins on versatility",
[MW](https://www.modwiggler.com/forum/viewtopic.php?t=202975&start=325)).

**ALM Squid Salmple.** "Cue Sets" (start/loop/end triplets, 64 since fw
125) on an 11 s mono buffer per channel: `Add` (manual), `Split` ("cut
all sets into equal halves"), `Auto` (transients + zero crossings —
"will delete any existing cue sets even if no new ones are found"),
`Random`, `Step`, `RM ALL` ("There is no undo!"); WAV markers import
([manual](https://assets.busycircuits.com/docs/alm022-manual.pdf)).
Editing: press Cues to cycle start/end/loop, turn the one encoder; Func +
turn zooms *centred on the cue point*, step size follows zoom and encoder
acceleration; sample offset in samples shown at max zoom. Cue-set changes
while a sample plays are *deferred to the next trigger*. Cue select by
CV: "0–5 V divided up evenly across the number of actual cue sets."
Loved for immediacy: "Menu diving is almost non existent… The UI is
brilliant"; "didnt have to read the manual"
([MW](https://www.modwiggler.com/forum/viewtopic.php?t=276992)); "the
most intuitive and user friendly machine for drum based playback"
([MW](https://www.modwiggler.com/forum/viewtopic.php?t=231141)). Hated:
"the screen is soooo tiny"; "the way samples are edited, using the tiny
display and crowded UI is often frustrating"; early firmware forgot cue
sets on power cycle; replace-recording drops cue points
([MW](https://modwiggler.com/forum/viewtopic.php?t=219509&start=100)).

**Make Noise Morphagene.** Manual splices only: press Splice at the
playhead (or a gate on the Splice input — patch a clock to auto-slice);
up to 299 markers; delete = Shift + Splice; no waveform, no moving a
marker, no transient detection
([manual](https://www.makenoise-manuals.com/morphagene/morphagene-manual.pdf)).
`Organize` (0–5 V) selects a splice, "always evenly distributed…
regardless of their relative length"; `Slide` offsets the start and
`Gene Size` shrinks the window non-destructively. Loved for what it is
("It has a life and mind of its own",
[SOS](https://www.soundonsound.com/reviews/make-noise-morphagene)).
Hated for exactly what we are designing against: "so cryptic — how do you
keep track of your slices and reels"; "I'm clearly not hitting splice in
the right places… drum loops end up sounding messy and sometimes out of
time"; "the button press combos are enough to drive anyone bonkers";
recording into the same splice is destructive and "I had all my slices
disappear, without touching any buttons"
([MW](https://www.modwiggler.com/forum/viewtopic.php?t=245584),
[MW](https://modwiggler.com/forum/viewtopic.php?t=252831)).

**Tiptop ONE** (the counter-example). One file per trigger, no
start/end/loop at all, modes set by holding PLAY and turning a knob until
the LED shows a colour; 0.25 ms latency
([manual](https://www.tiptopaudio.com/manuals/Tiptop_Audio_one.pdf)).
What simplicity buys: "as simple to use as it is affordable", live
players swap Pico Drums for two ONEs. What it costs: files under ~1 s
must be padded in Audacity, config by LED colour and `config.txt`
([Gearspace](https://gearspace.com/board/modular-mania-all-things-eurorack-and-modular-synths-effects/1180785-tiptop-one-any-red-flags-issues.html)).

**Qu-Bit Nebulae v2.** No discrete slices: Start and Size knobs/CV set a
window (Start CV ±5 V added to the knob), Reset jumps to loop start;
Start/Size are locked while recording
([manual](https://www.analoguehaven.com/qu-bit-electronix/nebulae-v2/manual.pdf)).
Relevant only as the "window, not slices" model.

**4ms Stereo Triggered Sampler, and the 2025 4ms Sampler.** No slicer:
you *copy a file into several slots with different trim points*
("split up a sample into a bunch of smaller slices",
[manual](https://4mscompany.com/media/STS/manual/STS-manual-1.5.pdf)).
Editing is screenless and audible while looping: hold Edit + turn Scrub
Start / Scrub End (velocity-sensitive), "These edits do not change the
actual WAV file… not saved until/unless you tell the STS to save" —
unsaved trims vanish at power-down. Start Pos / Length performance knobs
are confined to the trim window. v1.4 added fade envelopes after "How do
you get rid of start click". SOS: "lovely as all these coloured LEDs are,
they are no substitute for even a basic numeric display"; "the other CVs
are 0 V to +5 V. Half my LFO cycle was doing nothing!"
([SOS](https://www.soundonsound.com/reviews/4ms-stereo-triggered-sampler)).
The 2025 Sampler adds cue snapping: "If a sample file contains cues…
the Start Pos and Length knobs will snap to these cue points"
([4ms](https://www.4ms.info/sampler)) — Dan Green weighed three marker
designs publicly and shipped "snap the knobs to markers, easy to toggle
off" ([MW](https://modwiggler.com/forum/viewtopic.php?t=175197&start=400)).

**Noise Engineering.** No NE module is a sample player; Melotus / Granulita
Versio are granular effects on Daisy ([NE](https://noiseengineering.us/collections/modules/)).
Nothing to borrow.

**Rossum Assimil8or.** Start/end per channel or per zone (8 zones, each
with a CV range); 2.0 added *Explode* (2–8 equal regions → sequential
channels/zones) and *Chop* (transients)
([Synthtopia](https://www.synthtopia.com/content/2019/12/26/rossum-electro-music-updates-assimil8or-with-new-sampling-editing-features-more/)).
Editing: DATA 2 knob to the neighbourhood, click the encoder into the
numeric field for single-sample steps; the OLED shows the whole waveform
*and* a zoomed region around the point. **Start is latched at trigger**
("Moving the start point… while a sample is playing will have no effect
until the next time the channel is triggered"); end "can be moved while
playing"; 3.0 added double-click-to-zero-crossing, ±500-sample steps and
a playback cursor ([manual](https://www.rossum-electro.com/fqlzron/wp-content/uploads/2018/04/Assimil8or_man_040618-2.pdf)).
Manual warning: a start "in a non-silent portion… will almost certainly
cause a click if the envelope is set to a fast attack." Loved: sound,
100 µs latency. Hated: "Many times it's zoomed in too much to be
useful"; "the DATA2 knob is too sensitive… nudge up/down buttons";
"not as fast as it could be to edit stuff up"
([MW](https://modwiggler.com/forum/viewtopic.php?t=192914&start=700)).

**Expert Sleepers Disting EX.** No on-module slicing; slices come from
files, filename tags (`_CK8`, `_TR4`) and WAV cue chunks (1 marker =
loop-to-end, 2 = loop, 3+ = first is the start)
([manual](https://www.expert-sleepers.co.uk/downloads/manuals/disting_EX_user_manual_1.5.pdf)).
Multisample maps drum folders "from 48 upwards… a new sample on each
semitone". Loved for density and the manual; hated for the tiny display
and text-file configuration.

**Briefly.** *Squarp Rample*: start/length per voice with a `SLICER`
quantiser (/8…/128), start/length CV-assignable, but no screen ("like
working in the dark"), no recording, kits must be built on a PC
([manual](https://squarp.net/rample/manual/),
[MW](https://modwiggler.com/forum/viewtopic.php?t=227999&start=200)).
*OXI Coral*: WAV player, no point editing, "quite a bit of finger and
encoder kung-fu… your fingers are always in the way of the LEDs"
([Perfect Circuit](https://www.perfectcircuit.com/signal/oxi-coral-review)).
*Erica Pico Drums*: semitone-quantised CV sample select, exclusive/choke
switch.

**Eurorack patterns worth naming.**
- *Slice select by CV, two camps*: evenly divided 0–5 V (Squid,
  Morphagene, Bitbox, STS) vs chromatic 1 V/oct (Disting EX, Pico Drums,
  Rample, Sample Drum's 1V/oct mode). Users of the chromatic camp find
  the spread "very annoying to sequence" when the count isn't a power of
  two; users of the even camp cannot address a slice from a keyboard.
  Offer both, and show the slice number. (We already play keys 1 V/oct
  from J4; the even map is what P1's Chop does.)
- *CV-before-trigger race*: every trigger + CV sampler grows a settle
  delay (STS trigger delay, Assimil8or 2 ms Gate Delay, Tiptop `CV
  delay`, Sample Drum TR DELAY "4 ms seems to be the general consensus").
  Squid and Tiptop defer selection changes to the *next* trigger.
- *Nobody has undo*: Squid "There is no undo!", Morphagene auto-writes
  the card, STS's "undo" is not saving. Assimil8or's truncate needs a
  2-second hold. This is the field's biggest open goal.
- *Screen vs no screen is the same complaint from both sides*: STS "no
  substitute for even a basic numeric display" vs Micro "trash", Squid
  "soooo tiny", Assimil8or "zoomed in too much". The praised middle is
  Squid's: minimal screen dependence, a dedicated button per parameter
  group, and "Menu diving is almost non existent." Our split — a phone
  for the eyes, the panel for the hands — is that middle ground if the
  panel keeps the one or two hot gestures.
- *WAV cue chunks are the slice interchange format*: Squid, Bitbox,
  Disting EX, 4ms Sampler and Morphagene (via Reaper) all read them. We
  already write them; reading them on a plain load closes the loop.

## A2. Hardware samplers and grooveboxes

**Roland SP-404 MK2** (v4/v5 firmware). *Chop* = MARK mode: three ways
to place markers — cursor + `[MARK]`; **lazy chop** ("Press the pad [1].
The sample plays back. Press the pads [2]–[16] that are blinking blue, at
the timing where you want to add a marker"); `AUTO MARK` with `TIME
DIVISION 2–16`, `LEVEL 1–10`, `TRANSIENT HARD/MID/SOFT`
([reference manual v4, pp. 53–55](https://files.kraftmusic.com/media/ownersmanual/Roland_SP-404MKII_Reference_Manual_v4.pdf)).
Hard cap **16 chops**. Assignment is a separate step: `ASSIGN TO PAD`,
empty pads blink yellow, chosen turn green, occupied pads that *will be
overwritten turn red*; nothing is written until the final `[VALUE]`
press — the clearest overwrite warning in the survey. Start/end/loop:
`[CTRL 1/2/3]`, `[SHIFT]+[CTRL n]` zooms around that point, `[SHIFT]+
[VALUE]` = numeric entry via the pads, `[RESAMPLE]` lit = snap to
zero-cross (added in v2.00), `[MARK]` during playback sets start then
end. **No audition on turn** — you press the pad to hear the new start.
`EXCHANGE PAD` = `[SHIFT]`+pad 5, press two pads; mute groups (10);
per-pad envelope; skip-back sampler (25 s buffer, lost on `[EXIT]`);
markers persist with the sample since v3.0. TRUNCATE/NORMALIZE "cannot
be restored"; no undo for sample edits.
Loved: chop "will be a huge boon… Instead of having to duplicate a
sample to multiple pads and cut each down individually"
([SOS](https://www.soundonsound.com/reviews/roland-sp-404-mkii));
"Envelopes are a gamechanger for removing clicks/pops. No more trial and
error fumbling around for the zero crossing"
([Elektronauts](https://www.elektronauts.com/t/roland-sp-404-mk2-part-1/161021/1744));
"Chop samples to pads, put them in a mute group, jam out on the pads til
it sounds right, then skipback sample the good bits"
([Elektronauts](https://www.elektronauts.com/t/roland-sp-404-mk2-part-1/161021/4172)).
Hated: "samples take time to draw to the screen every time you change
pads"; "odd changes to button availability depending on what menu you're
in" (same thread); "you edit all your chops in a sort of buffer memory
and then commit them to pads and then if you find you want a wee bit
more at the start of one of them, you're snookered"
([Gearspace](https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1440371-sampler-good-sample-slicing-workflow.html));
LEVEL auto-mark "put[s] the marker after the attack has started"; in
ASSIGN "you *cannot* listen to the pads once you are assigning them"
([NearTao guide](https://neartao.wordpress.com/wp-content/uploads/2023/12/neartaos-sp-404-mk2-guide-0.29.pdf)).
*SP-404 OG/SX*: no waveform, `[MARK]` twice while playing (BPM-snapped
end), then knobs by ear — the Dilla lineage (SP-303 below).

**Akai MPC One / Live II / MPC 3.x.** Chop modes **Manual / Threshold
(+ Min Time) / Regions / BPM** ([MPC One+ manual p. 272](https://www.manuals.co.uk/akai/mpc-one+/manual?p=272));
128 regions demonstrated in 3.7. Manual: "press an unlit pad (usually
Pad 1) to start playback… press an unlit pad during playback to place a
slice marker at each location of the playhead. (If you press a lit pad,
playback will restart from that pad's corresponding slice marker.)";
inserting inside a region ripples slices right on the pads
([SOS](https://www.soundonsound.com/techniques/sample-slicing-beatmaking-hardware)).
Points: Q-Link columns for Start/End/Loop with **coarse on the top knob,
fine on the bottom**; drag `S`/`E` handles; `0 Snap` (Shift + F2) forces
zero crossings; `Link Slices` on = moving a slice's start moves the
previous slice's end. **The only true audition-on-turn in the survey:**
Sample Edit → Settings → **`Slice Preview` Before / After / Off** ("set
if any audio plays as you move a slice marker") plus `Cue Preview`;
descended from 2015's "audio marker preview… plays a small snippet of
audio at the marker point when you adjust it"
([MPC 1.9 notes](https://www.scribd.com/document/464263401/MPC-Software-v1-9-Release-Notes)).
It does *not* extend to a held note in Program Edit: "Visually, the
parameters change, but the SOUND DOES NOT CHANGE"
([MPC-Forums](https://www.mpc-forums.com/viewtopic.php?p=1888767)).
Slices → pads: `Convert` → new program with new samples, or
`Non-Destructive Slice` (pad's Slice field = number), or `Pad Parameters`
(start/end copied to the pad). MPC-Tutor's warning: "'Non-Destructive
Slice' is not an entirely accurate name… it destroys the previous slice
information stored in the sample. 'Pad Parameters' is the real
non-destructive slice" ([MPC-Forums](https://www.mpc-forums.com/viewtopic.php?p=1872575)).
Rearranging: hold Copy → source → dest (overwrite); no hardware swap;
desktop Cmd-drag = swap, Alt-drag = copy
([mpc-samples](https://www.mpc-samples.com/article/rearrange-drum-kit-pad-layout)).
Loved: "you can start hitting empty pads while it's playing to chop it
up, which feels very intuitive and authentic"
([Mixdown](https://mixdownmag.com.au/reviews/review-akai-mpc-one/));
"lazy chop is a godsend"; "touch any on-screen element and the knobs will
jump to that section" ([SOS](https://www.soundonsound.com/reviews/akai-mpc-one));
"The 16 qlinks make chopping enjoyable" (Gearspace thread above).
Hated: "you have to press the qlink buttons to switch values… these
little nags littered everywhere in the OS"
([MPC-Forums](https://www.mpc-forums.com/viewtopic.php?p=1797570));
"the zoom feature kicks in automatically… I'll pinch to zoom in or out
and it'll accidentally move the end position"
([MPC-Forums](https://www.mpc-forums.com/viewtopic.php?p=1886525));
"the marker appears to be having trouble keeping up with my finger"
([mpc-tutor](https://www.mpc-tutor.com/akai-mpc-touch-review/)); "you do
get clicks at the end of most regions"
([MPC-Forums](https://www.mpc-forums.com/viewtopic.php?f=36&t=170757));
"if you decide mid-chop that you want to change the amount of regions…
You lose your progress" ([MPC-Forums](https://www.mpc-forums.com/viewtopic.php?p=1707010));
a 2.8 bug that "permanently changed the start/edit points of the
original sample" ([MPC-Forums](https://www.mpc-forums.com/viewtopic.php?f=48&t=197731));
the 2013 Renaissance wishlist is still the best single list of what a
chop screen needs: "Audition slices should be ON by default"; "the
currently selected slice centered in the editor… when you adjust these
points, the actual sample is shifted left/right"; a Manual mode "with two
F keys… Add or Delete a slice marker"; "one-shot, mono per pad" audition
([MPC-Forums](https://www.mpc-forums.com/viewtopic.php?p=1546117)).

**Elektron Octatrack MKI/MKII.** AED → `CREATE SLICE GRID` (2…64 equal
slices between the trim points, "ALIGN MARKERS TO ZERO-CROSSES?"), then
`SLIC = ON` turns `STRT` into a slice selector to p-lock per trig;
`CREATE LINEAR / RANDOM LOCKS`
([manual](https://www.manualsdir.com/manuals/657165/elektron-octatrack.html?page=111)).
Per slice: knob A start, B loop, C end; `ADD SLICE HERE`, `DELETE`,
`REVERSE SLICE`; **hold `[FUNC]` while moving a marker to snap it to a
zero crossing** — the community's fine mode
([Elektronauts](https://www.elektronauts.com/t/adjusting-slices-manually/91033)).
Audition is explicit (`[FUNC]+[YES]`). The zero-cross option aligns only
the *start* of a slice, not its end
([Elektronauts](https://www.elektronauts.com/t/live-sampling-problem-click-noise-on-slice/785)).
Slice data lives in a `.ot` sidecar and is **lost if you forget SAVE
SAMPLE SETTINGS** ([Elektronauts](https://www.elektronauts.com/t/saving-sample-settings/33512));
no general undo — Elektron: "providing non-destructive (UNDO-able) audio
editing wouldn't really work out"
([Elektronauts](https://www.elektronauts.com/t/audo-editor-undo/16569)).
Live STRT modulation without prep is "an extremely clicky and messy
affair" ([Elektronauts](https://www.elektronauts.com/t/ot-digitakt-sample-start-end/48032)).
Loved: p-locks = slicing by sequencing; "an infinite number of
rearrangements" from an LFO on STRT. Hated: "I don't like that you have
to remember to save and naming is a chore" (Gearspace thread), MKI LCD
"rather inscrutable" ([MusicRadar](https://www.musicradar.com/reviews/elektron-octatrack-mkii)).

**Elektron Digitakt I / II.** DT I: no slice mode for six years — `STRT`
/ `LEN` 0–120 per trig ("slicing by p-lock"); OS 1.50's fixed Slice
machine: "You can't move slice points. All you can do is select the total
number of slices" ([Sunwarper](https://www.sunwarper.com/post/the-original-digitakt-still-slaps-for-beatmaking-even-after-the-digitakt-ii-slice-machine-update));
"no xfade loop… your only hope is to prepare your perfect loops on the
computer" ([Elektronauts](https://www.elektronauts.com/t/clicking-in-looping-sample-zero-crossing/160607)).
DT II: GRID machine (cuts "at the nearest zero amplitude crossing" —
users want it optional because "it changes tuning",
[Elektronauts](https://www.elektronauts.com/t/slice-machine-snapping-to-zero-crossing/191358));
**OS 1.15 (June 2025)** added a true SLICE machine: up to 64 editable
slices, grid or transient detection, per-slice start/end/loop, Slice trig
mode ([Synthtopia](https://www.synthtopia.com/content/2025/06/26/elektron-digitakt-ii-os-1-15-adds-sample-slicing-more/)).
Coarse/fine is inverted vs everyone else (press the knob for *larger*
steps). STRT/LEN are ordinary p-lockable parameters so they are live
while the sequencer runs. "They waited eight years" (Synthtopia
comments); "the sample zooming is great" (1.15 thread).

**Teenage Engineering EP-133 K.O. II.** Chop at launch, equal or attack
mode; live: "press a pad to CHOP. the break will be assigned depending
on the amount of time between pressing pads"
([TE guide §10.3](https://teenage.engineering/guides/ep-133/functions));
max 12 per group; "Auto-chop resets pad assignments in the selected
group". No waveform (segment display); trim = knob X start, knob Y
length, **hold SHIFT for finer control**; press the pad to preview; trim
is non-destructive until Shift+Sound "will overwrite the file and cannot
be undone" ([TE modes](https://teenage.engineering/guides/ep-133/modes)).
Chops reference the parent file and auto-join a mute group. Loved:
"Sampling is more accessible here than with any other sampler I know"
([Gearnews](https://www.gearnews.com/teenage-engineering-ep-133-k-o-ii-review-better-than-an-mpc/)).
Hated: 64 MB, 20 s per sample, no editor undo
([MusicTech](https://musictech.com/reviews/hardware-instruments/teenage-engineering-ep-133-ko-ii-review/)).

**Polyend Tracker / Tracker+ / Play.** Tracker: Slice and Beat Slice
playback, auto-slice by amplitude change, Shift = equal slices, max 48,
**no sensitivity** ("every other sampler has a sensitivity incase it's
not catching some of the hits",
[Backstage](https://backstage.polyend.com/t/auto-beat-slicing-simply-does-not-work/23854));
precise zoom + preview button; beat slicing non-destructive per
instrument. The Gearspace praise that matters most to us: "you can edit
the start point of chops/slices while the sequencer is running… being
able to mess with the chop points live, while the loop is playing, lets
you find the 'pocket' a bit easier"
([Gearspace](https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1440371-sampler-good-sample-slicing-workflow.html)).
Hated: "there is no ability to see the slices on an instrument in the
Sample Editor" ([Backstage](https://backstage.polyend.com/t/see-slices-in-sample-editor/23470));
zoom resets after every operation. Play: no slicing at all — per-step
Sample Start/End, live, reverse by setting end before start; slicing has
been the top request since 2022
([Backstage](https://backstage.polyend.com/t/sample-slicing-mode/1057)).

**Dirtywave M8.** Instrument `SLICE` = equal sections mapped from C-1;
`01 FILE` uses markers stored in the WAV; editor `SLICE MARKER` up to
128; `SLICE:AUTO` (transient), `SLICE:SILENC`; **lazy chop `[PLAY]+
[EDIT]`** since 5.0.0 ([manual](https://cdn.shopify.com/s/files/1/0455/0485/6229/files/m8_operation_manual_v20260421.pdf)).
Hex values, `[EDIT]+[UP/DOWN]` large, `[LEFT/RIGHT]` small steps; tempo
snap, no zero-cross snap; editor exit forces SAVE/OVERWRITE. "one of the
most elegantly implemented [slicers] I've seen. it's ultra fast"
([Elektronauts](https://www.elektronauts.com/t/deluge-vs-m8-tracker/160430)).

**Sonicware Liven Lofi-12 / XT.** "Downbeat Divider": tap a key to set
one beat's length while playing; equal slices; CHOP mode = choke; start
0–9359 / end numeric on a 4-digit LED with coarse-then-fine knobs
([manual](https://www.sonicware.co.jp/DL/LIVEN_Lofi-12/DOC/Lofi-12_v2_1_manual_en.pdf)).
"clicky samples due to lack of no zero editing/setting for sample
start/end points" (Gearspace, snippet).

**Chompi.** Screenless Daisy sampler; fixed start/end by ear on an
encoder page; no chop in stock firmware (a 2026 "TEMPO" alt-firmware adds
a slice engine); designed "while completely blind between eye surgeries"
so all feedback is audible/tactile ([Wikipedia](https://en.wikipedia.org/wiki/Chompi)).
"simply press a button and record."

**Classics: Boss SP-303, Yamaha SU-10 / SU-200 / SU-700.** The SP-303's
MARK-on-the-fly is the ancestor of every lazy chop: press `[MARK]` at
the start, again at the end, and a BPM between the presses snaps the end
to the nearest beat; then centre-detent *relative* knobs trim by ear
([manual pp. 38–41](https://media.sp-forums.com/manuals/SP-303_OM.pdf));
SOS: knob trim "a bit of a hit-and-miss affair" but "every function is
never more than a few button pushes away"
([SOS](https://www.soundonsound.com/reviews/boss-dr-sample-sp303)).
SU-10's ribbon "constantly sampling the input signal" is the proto
skip-back; SU-200 trims "by using your ears: play back the sample,
hitting the Start Point and End Point buttons on the fly"
([SOS](https://www.soundonsound.com/reviews/yamaha-su200)). Lesson:
record to a pad, trim by playing, snap the end to tempo, resample as the
editor. Hate: numeric trimming without a waveform.

**Korg ESX-1 / electribe sampler; Novation Circuit Rhythm.** ESX-1 Time
Slice: threshold page where "The step keys will light at each division…
Press a key to hear the note", sensitivity 0–32, SHIFT + step adds or
deletes a division ([manual](https://cdn.korg.com/us/support/download/files/35a1395f04992adb70f7e12361a3a0e7.pdf)).
Circuit Rhythm **Live Slice Point Recording**: "As the sample plays back,
tap a second pad to set that pad's start point, and the previous pad's
end point" (Novation guide, snippet); 4/8/16 equal, one shared choke,
pitch global; "wrestling with the lack of a screen"
([MusicRadar](https://www.musicradar.com/reviews/novation-circuit-rhythm)),
"trimming a loop to the perfect length is a giant PITA"
([Engadget](https://www.engadget.com/circuit-rhythm-review-a-solid-and-affordable-sampler-groovebox-130026004-130026561.html)).

## A3. Software and apps

**Koala Sampler** (the phone benchmark). AUTO-CHOP: *Transients* and
*Equal* with the count set by **swiping within the chops bar**, and
*Lazy* — "Tap anywhere in the blue stripe below the waveform to add chop
markers" while it plays, then "Drag the markers at the bottom to change
their starting points" ([manual §4.9](https://metawork-studio.github.io/koala-documentation/tablet/4-sample.html)).
Chop is destructive-by-copy (a resampled copy with vol/pitch/pan baked
in). Trim: drag the corners of the red region, pinch to zoom, **"Zoom out
and drag while holding the top stripe of the selected area to move it
around"** — move-the-window, not the edges. Pads: "Tap and drag a pad
with a sample over a free pad to copy it. If the target pad is not empty,
you will be prompted to either merge / swap / add"; hold EDIT + tap pads
for multi-edit; BOUNCE = resample a region to a new pad; 6 choke groups
([manual §4.5–4.7](https://manual.koalasampler.com/mobile/4-sample/)).
Undo lives on the sequencer screen; "Step-based undo for trimming,
normalizing, and slicing" is still a wishlist item
([wishlist](https://hiphopdrumsamples.com/blogs/hip-hop-producer-blog/koala-sampler-feature-wishlist)).
Loved: "the immediacy, the ergonomics and the sheer fun"; "you have a
real gift for intuitive UX… I love how you rethought 'Trim' as an 'Edit'
button" ([Loopy Pro](https://forum.loopypro.com/discussion/31514/koala-the-ultimate-pocket-sized-sampler/p17)).
Hated (early and lasting): no waveform zoom, no undo — "Those of us at
the back would really benefit from undo"; "it's hard to get exact fades"
([Loopy Pro](https://forum.loopypro.com/discussion/60932/editing-koala-vs-sitala-vs-digistix-2)).

**Serato Sample 2.** 32 cue points on pads; *Find Samples* (algorithmic,
"no sensitivity adjustments"), *Set Slicer* (32 consecutive cues of
1/16–16 beats from the playhead), *Set Random*; manual = scrub + hit a
pad, or play and "assign cues in real time as you listen"; drag cue
handles; **Favourite/lock** keeps chosen cues while re-running Find
([musicmarketing.ca](https://musicmarketing.ca/article_serato-sample.php)).
Loved: "Per slice level/filter/env/reverse/key shift/time
stretch/overlapable start and end points, and… favourite… To do all this
is much slower in Simpler since you can't do any of it until you commit
slices to a drum rack" ([Gearspace](https://gearspace.com/board/music-computers/1164495-lets-talk-serato-sample-2.html)).
Hated: "the slicing is fiddly. The 'find samples' function is a total
joke"; "It does really weird things to breaks though – the transients
sound mushy" ([DOA](https://www.dogsonacid.com/threads/serato-sample-trial-your-experiences.810598/)).

**Ableton Live Simpler / Drum Rack.** Slice By Transient (Sensitivity,
max 64) / Beat / Region / Manual; double-click a slice to delete, drag to
move, double-click between slices to add (white = manual); markers reuse
warp transients (orange active, grey inactive); SOS: markers "placed
later than the sound actually starts" cause "a glitch at the end of the
preceding slice, which you can eliminate by dragging the transient a
little to its left" ([SOS](https://www.soundonsound.com/techniques/re-slicing-audio-ableton-live)).
Editing is live while the clip loops. SNAP to zero crossing "has always
been hit or miss" and there is "no snap option in slice mode"
([Ableton forum](https://forum.ableton.com/viewtopic.php?t=245501)).
Drum Sampler (12.1): "Hold Shift while dragging the slider to adjust the
start point in fine increments. This also causes the display to zoom in
on the waveform" ([manual](https://www.ableton.com/en/manual/live-instrument-reference/)).
Drum Rack: **"Dragging a pad to another pad swaps the note mapping"**,
Alt-drag layers, 16 choke groups, Hot-Swap
([manual](https://www.ableton.com/en/manual/instrument-drum-and-effect-racks/)).
Hated: once committed to a Drum Rack "you lose the ability to have your
chops play in thru mode. You also lose the ability to make parameter
changes to multiple slices at once" (Gearspace above); "Who would enjoy
adjusting the same parameter on 16 simplers" ([forum](https://forum.ableton.com/viewtopic.php?t=223743));
the mono-retrigger click: "if you… retrigger the sample before it ends
(Choked), the first note isn't being snapped to zero. So, you'll hear a
click. I also noticed that Battery and Maschine do NOT do this"
([forum](https://forum.ableton.com/viewtopic.php?t=228866)).

**Bitwig Sampler (6.1, June 2026).** Slicing Start/End markers bound the
region; modes Divisions (max 64) / Beats / Onsets (sensitivity) / Pitch /
Manual; "double-click in the top half of the waveform" adds a manual
(mint) slice, double-click a handle removes, drag converts a computed
slice to manual and **manual slices survive a mode switch**; bottom half
of the waveform is zoom/scroll; "Snap to zero crossings"; slices stay
inside the Sampler so playback/looping/modulation remain editable
([release notes](https://downloads.bitwig.com/6.1/Release-Notes-6.1.html)).
Pre-6.1 the clip-level Slice to Drum Machine used an *onset-strength
histogram* with a draggable threshold — the most legible sensitivity UI
seen ([guide](https://www.bitwig.com/userguide/latest/slicing_to_notes/)).

**Renoise.** Click to place markers, auto-slice with a sensitivity box,
Ctrl+K at the cursor, drag numbered tabs, non-destructive (per-slice
volume/pan/loop type), "endless undo/redo"; **Snap menu: 0 Crossing, 0S
Effect, Markers, Beats**; middle-click plays from the click position,
Enter from the cursor ([manual](https://tutorials.renoise.com/wiki/Sampler_Waveform)).
Hated: "automatic sample slice tool detecting marker points bit too late?
I get click/pop in the end of slices with almost every break"
([forum](https://forum.renoise.com/t/transient-detection-offset/33878));
wish for "a single dial [to] gradually alter all slice positions"
([forum](https://forum.renoise.com/t/replace-sample-keep-slice-points/47380)).

**FL Studio Slicex / Fruity Slicer 2.** Dull / Medium / Sharp presets,
grid slicing, `M` add marker, snap menu incl. zero-crossing, drag the
square handle, 10-step undo history; per-region cut group, speed
(negative = reverse), start, envelopes; "Dump Score" to the piano roll
([manual](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/Slicex%20Editor.htm)).
Hated: "The size is way too small for accurate editing"; "Dull, Medium-,
and Sharp auto-slicing is useless… The 'Lazy chops' option should be
possible, like in MPC Software or Maschine"
([IL forum](https://forum.image-line.com/viewtopic.php?t=248806)).

**Logic Pro Quick Sampler (Mac / iPad).** Transient (Sensitivity) / Beat
Divisions / Equal Divisions / Manual; click to insert, drag the handle,
double-click the marker to delete; **a Play icon at the foot of every
slice marker**; Option-drag moves start and end together; a numeric
parameter bar on marker click; fades "applied to all slices"; "Slice
Marker ignores Sensitivity" locks a marker (orange), manual markers
locked by default ([Apple](https://support.apple.com/guide/logicpro/slice-mode-lgcp18515788/mac)).
iPad: tap to insert, double-tap the handle to delete, tap the note name
to play, a Snap pop-up, Zoom buttons
([Apple iPad](https://support.apple.com/en-ca/guide/logicpro-ipad/lpip5007ae10/ipados)).
Hated: "lacking a realtime manual chop option… We need the option to use
our ears, and hit chops with a key command as the song plays"
([Apple Communities](https://discussions.apple.com/thread/255022091)).

**NI Battery 4 / Maschine.** Battery has no slicer (cells, drag to swap,
Shift-drag fine). Maschine Slice page: **Detect** (sensitivity, optional
ZERO-X), **Split** (4/8/16/32), **Grid**, **Manual** — press pads as the
sample plays, with **Auto-Snap "aligns Slice points to the nearest
transient as you manually trigger"**; per-slice START/END on knobs 3/4,
SHIFT = finer; REMOVE joins with the previous slice; pads preview on the
cue bus; APPLY TO Drum / Group / Single
([Maschine+ manual](https://docs.native-instruments.com/ni-tech-manuals/maschine-plus-manual/en/sampling-and-sample-mapping)).
Manual mode became the default in 2.4.5.

**ReCycle / Dr. Octo Rex** (the original). Sensitivity 0–99 with the
number keys; **Lock** tool keeps a marker "even if you drag the
Sensitivity slider all the way to zero"; Mute tool; the pencil snaps to
zero crossings by default (Alt bypasses); "Play Slice and Move to Next"
([Reason docs](https://docs.reasonstudios.com/recycle/setting-slices-and-loops)).
Dr. Octo Rex sets per-slice pitch/pan/level/decay/reverse/filter/alt
group "by clicking, or drawing across several slices".

**Cubase Sampler Track** — Transient / Grid / Transient+Grid / Manual,
threshold + min length, Alt-click add/remove, global Snap to Zero
Crossing; users ask for SP-404-style live chop
([Steinberg forum](https://forums.steinberg.net/t/sp-404-style-sampler-track-manual-slice-mode/1010852)).
**Sitala** — "no zoom in the edit window??? … makes it difficult to get
precise". **Regroover / XO** — not slicing UX.

---

# Part B — synthesis

## 1. Peaks — what is loved across devices

1. **Tap while it plays** (lazy chop). SP-303 MARK → SP-404 MK2 pads
   2–16 → MPC unlit pads → Circuit Rhythm Live Slice → EP-133 → M8
   `[PLAY]+[EDIT]` → Koala Lazy → Maschine Manual. "feels very intuitive
   and authentic"; "lazy chop is a godsend"; its *absence* is the top
   complaint in Logic, Slicex and Cubase threads. Maschine's **Auto-Snap
   to the nearest transient** is the refinement that makes a human tap
   land on the hit.
2. **Auto-chop with a sensitivity you can see and a lock you can trust.**
   Transient detection with sensitivity (MPC Threshold + Min Time, Bitbox
   Scan %, Ableton, Bitwig's onset histogram) *plus* a way to pin the
   markers you like (ReCycle Lock, Logic "ignores Sensitivity", Bitwig
   manual-vs-computed colours, Serato Favourite). Without sensitivity
   (Polyend Tracker) or without transients at all (Sample Drum) users
   grumble for years.
3. **Equal-division grid as the zero-effort default** (everywhere:
   2…64), with the MPC's BPM mode and the SP-303's tempo-snapped end.
   We have this: it is what Audiothurgist *is*.
4. **Edit the start while the loop runs, and hear it.** Polyend Tracker
   praised for exactly this; Elektron STRT p-locks; Assimil8or's end
   moves live; MPC `Slice Preview` is the only true audition-on-turn.
   Users hate committing chops and then being "snookered" (SP-404 MK2).
5. **Fine/coarse without a mode.** Shift-drag = fine + auto-zoom
   (Ableton Drum Sampler), coarse top knob / fine bottom knob (MPC
   Q-Links), Shift+knob (Maschine, EP-133), zoom-centred-on-the-point
   with step following zoom (Squid), numeric readout on marker select
   (Logic, Squid at max zoom, SP-404 pad keypad).
6. **Snap to zero crossing as an opt-in modifier** (Octatrack `[FUNC]`
   while dragging; SP-404 `[RESAMPLE]` lit; Renoise/Bitwig/Cubase Snap
   menu; ReCycle pencil default). Forced snapping (Digitakt II Grid) is
   resented because "it changes tuning".
7. **A short fade instead of zero-cross hunting.** "Envelopes are a
   gamechanger for removing clicks/pops. No more trial and error fumbling
   around for the zero crossing" (SP-404 MK2). FL's declick modes
   ("Transient (no bleeding)": only the first few samples ramped). We
   already have adaptive fades and a 5 ms choke.
8. **Per-marker play affordance.** Logic's play icon at the marker foot,
   ESX-1's lit step keys "Press a key to hear the note", Maschine pads on
   the cue bus, MPC "Audition slices should be ON by default".
9. **Pads as state.** SP-404 ASSIGN colours (yellow empty, green chosen,
   red will-overwrite), Bitwig mint/yellow, Ableton orange/grey/white,
   Logic orange = locked. Colour tells you what a gesture will do
   *before* you do it.
10. **Swap is one gesture.** Drum Rack "dragging a pad to another pad
    swaps the note mapping"; Koala "merge / swap / add" prompt on drop;
    SP-404 EXCHANGE PAD press-two-pads.
11. **Non-destructive slice metadata that travels with the file.** MPC
    slices in the WAV, M8 cue points (128), SP-404 markers since v3.0,
    every Eurorack module reading `cue` chunks. We already write cues.
12. **Choke groups, per-slice reverse, per-slice pitch, resample to a
    pad.** SP-404 mute groups + skip-back ("Genius"), Koala BOUNCE, MPC
    Mute Group 1–16, Octatrack REVERSE SLICE. We have reverse/half/
    double/ratchet/punch as *styles* and Record Source = Output.
13. **"Never more than a few button pushes away"** (SP-303) / "Menu
    diving is almost non existent" (Squid) / "So dead simple. No weird
    button combos to remember" (Sample Drum). The compliment is always
    about *depth*, not feature count.

## 2. Pitfalls — what is hated

1. **No undo for sample edits.** Squid "There is no undo!", Octatrack
   ("wouldn't really work out"), SP-404 MK2 TRUNCATE "cannot be
   restored", EP-133 overwrite "cannot be undone", M8 SAVE/OVERWRITE on
   exit, Koala users begging since 2019, MPC Sample "Editing slices also
   cannot be undone". Nobody in Eurorack has it. **Easiest win in the
   whole survey.**
2. **Losing edits on reload / power cycle.** Octatrack `.ot` not saved,
   STS trims not saved, Sample Drum RAM projects, early Squid, Cubase
   slices vanishing from projects, Drambo's REC-next-to-PLAY wiping
   "the previous sample, loop position and all slices with no
   possibility of Undo" ([BeepStreet](https://forum.beepstreet.com/discussion/105/somehow-prevent-accidental-taps-on-rec-in-samplers)).
   "I don't like that you have to remember to save."
3. **Commit-then-stuck.** SP-404 MK2 chop buffer → pads; Ableton Slice
   to Drum Rack loses thru mode and multi-slice edits; MPC
   "Non-Destructive Slice" overwriting the previous chop; "if you decide
   mid-chop that you want to change the amount of regions… You lose your
   progress".
4. **Can't hear the start move.** Everyone except the MPC makes you
   re-press the pad; SP-404 ASSIGN mutes the pads entirely; MPC Program
   Edit doesn't update a held note.
5. **Clicks at boundaries and on retrigger.** MPC "clicks at the end of
   most regions", Octatrack declick threads, Ableton mono-retrigger
   click, Sample Drum "super clippy", Lofi-12, Digitakt I. Causes: cut
   on non-zero, no fade, late transient marker, voice steal without a
   fade. (Ours: fades + choke geometry already; keep them on every path
   including audition.)
6. **Late transient markers.** Renoise "bit too late… click/pop in the
   end of slices with almost every break", Ableton "drag the transient a
   little to its left", SP-404 LEVEL "after the attack has started". Our
   own SLICE package found the same defect in v0.8.0's onset refine (up
   to 10 ms early/late on hats) — mode 1 fixes it; the editor should
   still offer a global "nudge all starts by N ms".
7. **Laggy waveform / marker not keeping up.** SP-404 MK2 redraw on
   every pad change, MPC Touch "trouble keeping up with my finger",
   Bitbox Micro "pushing things multiple times". A phone page over USB
   serial at ≤ 3 windows/s is exactly the risk; the page must draw the
   marker from local state and never wait on the module.
8. **Zoom hijacking the drag.** MPC "pinch to zoom… accidentally move the
   end position"; Assimil8or "zoomed in too much to be useful"; Polyend
   zoom resetting after each op. Separate the zoom surface from the
   marker surface (Koala's stripe below, Bitwig's bottom half, Ableton
   Note's double-tap-to-range).
9. **Modal states and button-combo mazes.** SP-404 "odd changes to
   button availability depending on what menu you're in"; Morphagene
   "drive anyone bonkers"; MPC Q-Link banking "little nags littered
   everywhere"; Polyend Play "accidental encoder touches jump modes".
10. **Tiny screens** (Squid, Micro, Slicex "way too small for accurate
    editing", Sample Drum burn-in) *and* no screens ("wrestling with the
    lack of a screen"). Both are solved by a phone — if the page is the
    screen and the panel never *needs* it.
11. **Slice-count ceilings and rigid maps.** SP-404's 16 chops, EP-133's
    12, Digitakt I's fixed grid, Circuit's 4/8/16, Sample Drum's 32; and
    Rample/Sample Drum CV maps that are "very annoying to sequence" when
    the count isn't a power of two.
12. **Accidental overwrite.** Drambo's REC next to PLAY; MPC's 2.8 bug;
    Morphagene same-splice overdub; Squid `Auto` wiping existing cues.
    SP-404's *red = will overwrite* is the model answer.
13. **Editing requires the sequencer to stop / the file to reload.**
    "If you change anything to the actual sample in Sample Edit, you
    need to reload it in your program" (MPC); Tracker's "offline"
    editor; Sample Drum DOUBLE-project load interrupts both channels.

## 3. What maps to our constraints — the recommended interaction model

Constraints restated: the page is the screen (desktop or phone, Web
Serial, 10 Hz state, `GET_WAVE` windows ≤ 3/s, `max_body` 1024); the
panel has six pickup pots with rings, three buttons, no display; the
table is the single source of truth on the device; the grammars read
the grid part of the table every step; playback must not glitch during
an edit; a set is WAV + JSON with cues.

### 3.1 Principles (each traceable to a peak or a pitfall above)

- **The device owns the table; the page owns the history.** Every edit
  is a small idempotent command; the firmware bumps a `tableGen` in
  state; the page re-fetches `GET_TABLE` on change (it already does).
  Undo is a stack of *table snapshots* on the page (96 × {start, len,
  key} ≈ 1 KB each — hundreds of levels for nothing), replayed with a
  bulk `SET_TABLE`. The firmware keeps one level itself (the previous
  table) so **panel undo works with no page attached** (pitfall 1).
- **Nothing is destructive.** The buffer is never rewritten by an edit;
  a slice is `{start, len}`; split/merge/move only touch the table.
  Recorded regions are never trimmed in place after commit; a "crop" is
  a table change (peak 11, pitfall 3).
- **Never lose an edit.** The table is autosaved to the card (a `.json`
  beside the loaded file, or the set's own JSON) on an idle timer after
  any edit, and a plain WAV's cues are read on load. A power cycle
  returns what you left (pitfall 2). Sets stay explicit ("Save set") for
  naming; autosave is the safety net.
- **Edits are live and audible.** A grid edit takes effect on the next
  step the grammar plays; a voice in flight keeps its old bounds
  (already true). The page auditions from the new start on every drag
  step (peak 4, pitfall 4) — see 3.3.
- **One surface, two halves.** Waveform top half = markers (tap to add,
  drag the handle, double-tap to delete, long-press for the slice menu);
  bottom strip = zoom/scroll (drag left-right pans, pinch or drag
  up-down zooms, double-tap fits the selected slice). A drag that starts
  on a handle never zooms (pitfall 8). On a phone the handle has a fat
  lollipop below the waveform, Koala-style.
- **Colour is state.** Grid slices white, recorded yellow (already);
  *locked* (user-placed or user-moved) markers orange and immune to a
  re-slice; auto markers grey until touched; onset ticks blue (already);
  the selected slice's key cell and marker pulse together; a drop target
  that would overwrite a recorded key turns red (peaks 9, 10).
- **Snap is a modifier, not a mode.** Default drag snaps to the nearest
  onset within ±12 ms (the magnet), then to a zero crossing within ±2 ms
  if "0-snap" is on (default on for the grid, off for recorded regions
  whose starts were trimmed already); Shift / a second finger held
  disables both; a numeric field (samples, ms, and bar:beat:16th) under
  the marker for typed entry (peaks 5, 6).

### 3.2 Slicing in the UI

Four ways to get markers, all producing the same table, all undoable:

1. **Grid** (what B3 does): a stepper 4 / 8 / 16 / 32 / 64 (bars ×
   16ths), swing-aware — this is `SetMaterial` with `nSlicesHint`; the
   page shows the count and the bars it implies. Locked markers survive
   a regrid (Logic/Bitwig/ReCycle lock, peak 2).
2. **Transient**: a sensitivity slider drawn *as an onset-strength
   histogram* (Bitwig): the page already has `GET_HITS` with salience;
   dragging the threshold shows which ticks would become markers before
   you commit; commit = `SLICE_FROM_ONSETS(threshold, minGap)`; an
   onset table refetch is not needed. Min gap defaults to 35 ms (our
   onset detector's own). Tell the user the count.
3. **Tap while playing** (lazy chop, peak 1): a big "Chop" pad on the
   page; while the break plays verbatim (Chop knob right, or the page
   sends ACTION *audition whole*), each tap sends `TAP_MARK`. **The
   firmware timestamps** (the page's 10 Hz view of the playhead is too
   coarse and USB adds jitter): it takes the playhead at receipt, subtracts
   a fixed reaction+transport allowance (start at 40 ms, make it an
   option), and snaps to the nearest onset within ±60 ms (Maschine
   Auto-Snap) — else places it raw. Insert ripples the grid right (MPC
   behaviour) and the new slice inherits the class of the onset it
   landed on. On the panel the same tap is B2 while in Edit (3.6).
4. **Drag / tap on the waveform**: tap empty waveform = split the slice
   under the finger at that point (snapped as 3.1); double-tap a marker
   = merge with the previous slice (Maschine REMOVE semantics); drag a
   marker = move its start (3.3).

Everything that produces markers goes through the same `SPLIT` /
`MERGE` / `SET_START` primitives so undo, locking and colour are
uniform.

### 3.3 Moving a slice start, with audition

The gesture users love (Polyend, MPC Slice Preview) and the one every
other device gets wrong:

- **Drag the handle.** The page moves the marker locally at 60 fps
  (never waiting for the module), shows ms and samples, and the
  neighbourhood zooms in as the finger slows (Ableton Drum Sampler's
  shift-zoom, but automatic: velocity-scaled fine mode). Snap magnets as
  3.1; a held second finger or Shift disables snapping.
- **Audition on the move.** While dragging, every ~120 ms the page sends
  `AUDITION(start, len ≤ 250 ms, retrig)`; the firmware plays a scratch
  voice from `start` with the normal attack fade and the choke geometry
  on retrigger, so you hear the transient walk under your finger (MPC
  `Slice Preview: After`). On release it plays the whole slice once and
  sends `SET_START(idx, pos, linked)`. This needs a new engine call
  (`CallRegion(start, len, rate)` — a one-shot voice with explicit
  bounds, tagged so the sequencer's continuity branch ignores it, like a
  held voice). It costs one voice.
- **Linked or free.** `Link Slices` (MPC) is the default for the grid:
  moving slice *i*'s start also sets slice *i−1*'s end, so the grid
  keeps tiling the bar and Slice Tail = Zoom still reads on "never
  through an attack". Recorded keys are free (overlaps allowed).
- **Live under the grammar.** The edit applies on the next call of that
  slice; the bar ring keeps running. The step's loop points are
  re-searched on the control thread before publication (as `CommitSlice`
  does now), and start/len are published in the order the audio thread
  expects (len then start, or a single 64-bit store).
- **Nudge all.** A "shift all starts ±N ms" control (Renoise's wish,
  Reaper MK Slicer's Offset) for the "every marker is 4 ms late" case.

### 3.4 Moving a slice end

Same handle mechanics from the right edge of the selected slice; the
audition plays the *last* 250 ms up to the new end (MPC `Slice Preview:
Before` for ends). Free slices may overlap; linked grid ends are the next
start. Add an **end preview hold** (SP-404 `[ROLL]`): press-and-hold the
end handle without moving = loop the tail.

### 3.5 Split, merge, assign, swap

- **Split**: tap the waveform in a slice → `SPLIT(idx, pos)`; the new
  slice is inserted after `idx`, the grid ripples right, `keySlice`
  entries above are re-pointed (the firmware does it; the page just
  refetches). Class/salience/density for the two halves are re-analysed
  with `AnalyseRegion` on the control thread.
- **Merge**: double-tap a marker or pinch two adjacent slices → `MERGE
  (idx)` joins with the previous slice. Both are one undo level.
- **Assign**: drag a slice from the waveform onto a key cell, or drag a
  key cell onto another cell: `SET_KEY(key, idx)`. Drop on an occupied
  key → prompt *swap / replace / cancel* (Koala); a drop that would
  orphan a *recording* is red and defaults to swap (SP-404). Dragging a
  cell onto another cell = `SWAP_KEYS(a, b)` (Drum Rack). Tap a cell =
  audition that key (one-shot, mono — the MPC wishlist) and select its
  slice in the waveform; the cell and marker pulse together.
- **Copy**: long-press a cell → duplicate to another key (two keys, one
  slice — allowed, `keySlice` is just an index; the styles give reverse
  / half / double per hit already).
- **Reset**: "keys follow the grid" restores the span mapping
  (`SetMaterial`'s default).

### 3.6 Undo

- Page: snapshot stack per edit, "Undo / Redo" buttons and a swipe;
  replay = `SET_TABLE` (chunked, ≤ 1 KB per request) then `SET_KEYS`.
  A snapshot label ("split 7", "move start 3 −4 ms") in a history list.
- Firmware: one level — the previous table + key map — restored by
  `UNDO` (ACTION 16) so B-button undo works without the page; on an
  incoming `SET_TABLE` the firmware's one level is the state before it.
- Autosave the table after 2 s idle; a set's JSON is the same format.
- The looper's undo (ACTION 12) stays separate: it is the *newest
  overdub*, a different stack.

### 3.7 What the panel alone must do (no page)

Pick **one** hot gesture set and refuse to grow it: the most common
sampler edit after loading is "this chop is late — pull it back a
touch", and the second is "I want a marker *there*".

- **Edit page** (B1 hold + B3 tap, ring colours shift to the edit
  palette; B1 tap leaves): P1 = select slice (ring = index, the bar ring
  pulses the slice as it plays — you hear it without a screen); P2 =
  start nudge ±50 ms, centre-detent semantics via pickup (the ring shows
  the offset from centre in blue-grey until picked up, SP-303's relative
  knob); P3 = end nudge likewise; P4 = pitch as now; **the audition rule
  of 3.3 applies to the pot**: every move retriggers the slice's first
  250 ms, so the panel *hears the start move* — the thing the MPC alone
  does. Releasing the page (B1 tap) commits; B2 tap = undo the last
  panel edit; B2 hold = revert the slice to the grid.
- **B3 tap while the break plays in Edit** = drop a marker at the
  playhead, onset-snapped (lazy chop, 3.2.3). B3 hold = reslice to the
  grid as today (with locked markers kept).
- **Keys as pads**: a J4/J5 note while in Edit selects that key's slice
  (so a sequencer or keyboard can drive selection), and a J6 edge with
  the Record style still records into it as now.
- **Rings as feedback**: P1's ring already maps 24 keys over 16 LEDs for
  recordings; in Edit the selected slice's LED pulses on each hit.
- Nothing else. No swap on the panel (it needs two selections and a
  confirm — the page does it in one drag); no transient threshold on the
  panel (needs eyes). This is the Squid lesson: dedicated buttons for the
  few things, the page for the rest.

### 3.8 Protocol sketch (for the integrator, not a spec)

One new command class `0x67 SLICE` with a `u8 op` and a `u16 gen` (the
page's view of `tableGen`; the firmware refuses with STALE if it moved,
the page refetches and retries — no lost updates from two clients):

| op | args | notes |
|---|---|---|
| `SET_START` | idx, u32 pos, u8 linked | control thread; re-search loop points; publish |
| `SET_END` | idx, u32 pos, u8 linked | |
| `SPLIT` | idx, u32 pos | ripples grid right; re-points keys; re-analyses both halves |
| `MERGE` | idx | joins with idx−1 |
| `SET_KEY` / `SWAP_KEYS` | key, idx / a, b | `SetKeySlice` exists |
| `LOCK` | idx, u8 on | survives regrid / re-slice-from-onsets |
| `REGRID` | u8 n | `SetMaterial` with hint, locked kept |
| `FROM_ONSETS` | u8 thresh, u16 minGapMs | |
| `TAP_MARK` | u8 snapWindowMs | firmware timestamps |
| `AUDITION` | u32 start, u16 lenMs, u8 flags | scratch voice, throttled ≤ 10/s |
| `NUDGE_ALL` | i16 ms | |
| `UNDO` | — | firmware's one level |
| `SET_TABLE` / `SET_KEYS` | chunked | replay from the page's history |

State gains `tableGen` (u16) and `selSlice` (u8, the panel's selection,
so the page follows the knobs). `GET_TABLE` gains a `lock` bit per
entry. Sets' JSON gains `"lock"` per slice; the WAV cue list stays the
starts (every module can read it).

### 3.9 Engine/audio constraints to honour

- A table entry is written by the control thread only; the audio thread
  reads `start`/`len` at trigger and copies them into the voice (already
  the design). Publish `len` before `start` (or both in one aligned
  64-bit store) so a call between the two writes never reads past the
  material.
- Loop-point re-search and `AnalyseRegion` are control-thread work with
  `bb::Yield()` — never in the audio callback; a `SPLIT` during a busy
  bar must not starve the poll loop (chunk the analysis).
- Audition voices use the same attack fade and 5 ms choke as any hit and
  never adopt continuity; cap one audition voice (steal itself).
- `TAP_MARK` reads the playhead in the callback (a volatile phase
  snapshot) so the mark is sample-accurate before the reaction allowance.
- Grid slices stay sorted and non-overlapping when linked (the bar rule
  and the swing map assume it); free recorded entries may overlap.
- A regrid keeps recordings (invariant 7) *and* now keeps locked grid
  markers: locked starts are passed to `SetMaterial` as fixed onsets.

## 4. Erica Synths Sample Drum — what "the Sample Drum experience" means

Facts first (from the manual and the 17-page ModWiggler thread; the
Sample Drum researcher's notes corrected three of my priors): 14 HP, two
mono channels, a **monochrome OLED**, six push-encoders whose functions
are labelled on screen per page (A = select/data, D = back, everywhere),
a 1/2 toggle to swap the edited channel without leaving the page, three
assignable CV inputs per channel with a mini-scope and attenuator each,
sampling via channel 2's CV3 jack at 12-bit, 64 slots per channel, **up
to 32 slices per sample**, project save/autoload, ~1 ms trigger latency
([manual](https://www.ericasynths.lv/media/sample_drum_user_manual.pdf),
[Bonedo](https://www.bonedo.de/artikel/erica-synths-sample-drum-test/),
[MusicRadar](https://www.musicradar.com/reviews/erica-synths-sample-drum)).

### What Ernest is pointing at — the strengths

- **Slice-then-play immediacy.** SAMPLE page: turn C for LIN or ZC, turn
  D for the count, push F — sliced; then FWD / BKW / RND / CV step
  modes, a first/last slice range, and triggers do the rest. "prepare 1
  bar long loops and slice up them to 16 slices and then trigger them on
  every clock tick" is the manual's own recipe. "ease of recording in a
  break and getting it sliced up and sounding like part of an MPC-style
  arrangement very very quickly" ([MW p.10](https://www.modwiggler.com/forum/viewtopic.php?t=202975&start=225)).
- **The display as a slicing aid**: "the display is extremly helpful when
  slicing samples" ([MW](https://www.modwiggler.com/forum/viewtopic.php?t=231141));
  numbered slice lines, the selected slice dashed, zoom with C, cursor
  with E, push A to add, B to nudge, push C to remove.
- **Per-slice / per-parameter CV with visible scopes**: "the CV assign
  process is also a lot better on the Sample Drum, with the mini scopes
  + built-in attenuation"; "CV the slice index — which is the party trick
  of the Sample Drum" ([MW t=215023](https://www.modwiggler.com/forum/viewtopic.php?t=215023));
  slice select evenly over the range or 1V/oct (32 notes from C0).
- **UI depth, not feature count**: "So dead simple. No weird button
  combos to remember, there's nothing cryptic about it"; "Even after
  weeks you don't need to reread the manual"; Bonedo 5/5 "currently the
  best available sample module"; the Jan-2026 verdict "The Ui on Sample
  Drum is fantastic and intuitive. Bitbot UI was infuriating"
  ([MW](https://www.modwiggler.com/forum/viewtopic.php?t=298640)).
- **Performance page**: the six encoders become macros with snap-back;
  the TR DELAY setting for CV-before-trigger (Bonedo: proof the designers
  play modular).
- **RELATIVE envelopes**: envelope times scale with slice length so a
  regrid doesn't leave decays hanging.

### The known frustrations — what we avoid

- **No transient detection at all** (MusicRadar's headline con; a top
  request for years). LIN/ZC are both equal divisions.
- **Clicks**: G-Storm "it does not process its loops and slices using
  zero crossing… sometimes you'll have clicks and pops"; "you have to be
  very hands on with the envelopes to keep slices from sounding super
  clippy all the time"; "sliced them via zero crossing and a few with
  linear… no difference whatsoever the drum breaks sound rather steppy"
  ([MW](https://www.modwiggler.com/forum/viewtopic.php?t=202975&start=225)).
- **No clock, no tempo awareness, no time-stretch**: "the lack of a clock
  as an input option really is a bummer. Without that, slicing and
  syncing of drum loops has to be manually dialed in by pitch"; tune per
  channel not per sample.
- **Encoder editing is slow and doesn't audition**: MusicRadar "a bit
  fiddly using the encoders… it still takes a while"; Bonedo: adjusting
  individual slices "works OK but is not really satisfying"; TRG1 plays
  the sample, nothing plays *as* you nudge.
- **Edits live in RAM projects**: "Do you have to do it from scratch
  every time you start a new project (due to slice settings being stored
  in RAM)?"; nothing autosaves; the NaN template bug.
- **Recording is an afterthought**: 12-bit, via a CV jack, crop screen
  without preview looping ("near impossible to accurately crop a perfect
  loop"), no trigger-to-record, no resampling; 2025: "Does anyone have a
  viable workflow for recording loops directly into sample drum?" —
  unanswered.
- **Reset audibly fires slice 1**, OLED burn-in with no screensaver,
  two mono channels with no summed out, firmware essentially frozen
  since 2019 (1.10 in Jan 2026 was a one-bug build).

### Borrow / avoid, in one table

| Borrow | Avoid |
|---|---|
| Count → push → sliced; step modes FWD/BKW/RND/CV with a first/last range (our Chop knob + styles already cover most of this) | Equal-only slicing: add transient + lock + lazy chop (§3.2) |
| "A = select, D = back, everywhere" → one Edit page on the panel with fixed pot roles (§3.7) | Encoder-only nudging with no audition → audition on every move (§3.3) |
| Numbered slice lines, selected slice dashed, zoom + cursor → the page's waveform | Edits in RAM only → autosave the table, cues in the WAV (§3.1) |
| Slice select evenly *or* 1V/oct, and show the number | CV maps you can't sequence when the count is odd: keep the 24-key map explicit and visible |
| Per-input scope + attenuator: we have the Scope tab; show J4/J5 in the Kit tab while assigning | Clicks: keep adaptive fades and the choke on every path, snap as a modifier, "nudge all" for late markers |
| TR DELAY (we have Gate Length/level; add a settle delay if J4→J6 races appear) | No clock: we have J3 and the file's own tempo — keep slices on the grid |
| RELATIVE envelopes (ours: adaptive fades) | A silent-reset that plays: reset never triggers |
| Performance page with snap-back (ours: locks + presets) | Two channels, mono, no resample: keep Record Source = Output |
| Project autoload | A slicer with no undo |

The short version of "the Sample Drum experience" for our purposes: *one
page, count, push, play; slices you can see; every slice addressable by
CV; nothing cryptic.* We keep that shape and add the four things its
owners have asked for since 2019 — transients, audition while nudging,
edits that persist, and undo.
