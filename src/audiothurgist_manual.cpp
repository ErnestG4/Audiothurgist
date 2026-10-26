/**
 * audiothurgist_manual.cpp — the firmware-authored interactive manual.
 *
 * Shown live in Hermetic's /program viewer over the front USB-C. Ranges,
 * units and labels come from the control declarations; this file adds the
 * prose. Pattern follows the alchemy-template's stereo_eq_manual.cpp.
 */

#include "alchemy/host_link/host.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/manual.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_knob.h"

using namespace alchemy;

extern VirtualKnob k_chop, k_tempo, k_hist, k_pitch, k_sample, k_vol;
extern VirtualKnob b_x, b_y, b_kick, b_snare, b_hat, b_rot;
extern Page        perform, brain;
extern Settings    settings;
extern Jack        jk1, jk2, jk3, jk4, jk5, jk6, jk7, jk8, jk9, jk10;

static Manual manual =
    Manual()
        .Tagline("A sampling, slicing, and rearranging toolkit")
        .Preamble(
            "**Audiothurgist chops breaks to a grid and rearranges them** "
            "with one knob that travels from *the record, verbatim* to "
            "rolls, bursts, and reverb rubble. Patterns hold until you ask "
            "for new ones. Every sound is a slice of real material played "
            "from its attack, on the grid — no grains, no time-stretch. "
            "If you don't like it, re-slice it.")
        .Section("roles", "Three roles, one engine",
            "**Chopper** — the knobs and buttons: slice, rearrange, reroll, "
            "flow, history, tempo. **Voice** — the rack plays *it*: a v/oct + "
            "gate pair calls the Kit's keys from a keyboard or sequencer. "
            "**Brain** — it plays the rack: kick, snare and hat gates leave on "
            "J6/J7/J8 as the pattern runs, and a clock on J3 locks the loop "
            "to your master. The roles run at once; nothing needs a mode. "
            "**It powers up as a sampler**: Pads, the pattern stopped (B1 "
            "breathes) — the 24 keys (J4, the browser's pads) play, nothing "
            "else does. For a beat, pick a grammar (Classic, Grids…) and hold "
            "**B1** 600 ms (or ▶ in the browser); it starts on the beat. "
            "*Start in* (the web app's Options → Start-up, kept in "
            "`Audiothurgist/startup.json`) = As saved powers up in the boot "
            "preset's grammar, running, as before.")
        .Section("journey", "The knob journey (P1)",
            "Full right: verbatim. ~4 o'clock: whole beats trade places. "
            "~1 o'clock: one-beat chops — listen for the double-time illusion. "
            "~11: half-beat motifs. ~9: sixteenth-note dice and reversals. "
            "Past 8: the cluster zone — bursts of 32nd rolls, hat-ticks and "
            "real silence, reverb blooming toward full left. B3 relocates the "
            "burst centres. Chops respect the kit: kicks trade with kicks, "
            "snares with snares.")
        .Section("keymap", "Playing it from a keyboard (one keymap)",
            "**J4 is the key in every map: 0 V = key 0 (C1)**, a semitone per "
            "key to 23, below 0 V = key 0. A key plays what the Kit's key "
            "table says (in **Pads** the keys follow the markers: time order "
            "on keys 0, 1, 2…). **Voice map**: J6 gate, **J5 = the style** "
            "(0 V plain, then reverse · half · double · ratchet · staccato · "
            "punch/choke flip · 7 Record · 8 Clear, a semitone each; under "
            "~70 mV is plain). The style is read with the hit; changing it "
            "while the key is held re-strikes it. J7 sends a trigger per "
            "call, J8 its velocity. **Brain map**: J5 gate, plain calls, "
            "class gates on J6/J7/J8. Options → Jacks picks the map; *Follow "
            "grammar* gives Play and Pads the Voice map. In Play calls write "
            "the loop (punch flips to momentary), in Pads nothing is written, "
            "elsewhere the Voice option decides.")
        .Section("buttons", "The three buttons",
            "**B1** tap: Perform ↔ Brain page. **B1 held 600 ms, let go** "
            "(B1 goes white; no pot, no other button): start / stop — the "
            "clock runs on, keys still play; B1 breathes while stopped. "
            "A start in Pads (as the module powers up) brings back the last "
            "beat — the boot preset's grammar, or the one you left. "
            "**B1 held 300 ms + turn a pot**: "
            "record that pot's motion as a loop that replays until you hold "
            "and nudge it again (a green pip at 6 o'clock marks a locked pot; "
            "*Clear locks* in the web app removes them all). **B2** tap: dice "
            "(a new arrangement of the same slices; in Play: undo the last "
            "overdub); hold: *flow*, a new bar every loop until released. "
            "**B3** tap: reslice (new cell tree; in Play: clear the loop); "
            "hold: load the highlighted file. **B2 + B3 held 2 s**: Settings "
            "(brightness, preset save/load); B2 or B3 leaves. B1 also shows "
            "the clock: white per pulse, green when locked. **B1 held + B3 "
            "tap: the Edit page** (B1 amber; B1 tap leaves): P1 selects a "
            "slice — its segment pulses on the bar ring as it plays and its "
            "keys pulse on P5, a Voice-map note selects too — P2 / P3 nudge "
            "its start / end ±50 ms once swept through centre, each move "
            "re-playing the new edge; B3 tap drops a marker at the playhead "
            "(onset-snapped, Options → Tap Allowance; near a marker it "
            "selects it), B3 hold reslices; **P5 = autoslice**: 0–32 grey "
            "markers on onsets drawn by loudness — up adds one, down drops "
            "one, 0 and back re-draws; B2 tap undoes, B2 hold puts the "
            "slice back on the grid (a "
            "marker: deletes it). Refusals blink red.")
        .Section("brain", "Clock in, gates out",
            "Patch a clock into J3 and set its PPQN in Options; B1 flashes "
            "white per pulse and sits green when locked (the loop aligns to "
            "the pulse grid at lock). While following, P2 becomes a clock "
            "divider/multiplier (÷4 · ÷2 · ×1 · ×2 · ×4, wide ×1 centre) that "
            "engages once you move it and switches on the next beat, in "
            "phase. Unpatch and the tempo holds until P2 moves. Gates on "
            "J6/J7/J8 follow the classifier's labels: *Gate Fires* chooses "
            "every step or once per hit (as drums do), *Gate Lanes* can mark "
            "every class present in a hit, Gate Length and Gate Level "
            "(velocity from loudness, accents to the ceiling) shape them. "
            "If a break plays at half or double speed against your clock, "
            "set **Bars** in Options — the bar guess is the culprit.")
        .Section("languages", "Pattern languages (Grammar, in Options)",
            "**Classic** is BreakFix's Brocot cell grammar. **Collins** adds "
            "the breakbeat-science upgrades: faithful early in the phrase, "
            "restless toward its end; 3+3+2 odd cuts; a fill on every fourth "
            "loop that snaps back on the downbeat; stutter terminators; "
            "accelerating rolls. **Grids** speaks Mutable Instruments' drum "
            "map through your break's own kicks, snares and hats — Map X/Y "
            "and three densities on the **Brain** page (B1). **Euclid** "
            "spaces each class's hits evenly with Rotation. In both, the Chop "
            "knob sets how faithfully slices are chosen: full right plays "
            "each class's own nearest hit, lower swaps within the class; a "
            "rest lets the break run on underneath (or gates it: *Rests*). "
            "**Camp** rings the bar's cells through change-ringing "
            "permutations — deterministic, B2 rings the next change. "
            "**Play** has no pattern of its own: the bar is what you play "
            "into it from the keyboard or the web page, looped; B3 clears it. "
            "With **Quantize** Off (the default) it is a free looper — every "
            "hit replays at the exact moment you played it, held notes with "
            "their length, overdubbing pass after pass; 8ths snaps hits to "
            "the nearest 8th, 16ths writes the bar step by step as before. "
            "**B2** tap undoes the last pass's overdub, **B3** tap clears. "
            "**Pads** is the instrument with no looper: hits sound as you "
            "play them, nothing is written, keeping the beat is yours.")
        .Section("options", "Options, one line each",
            "**Classifier** — which lens labels kick/snare/hat (Hockman "
            "3-band, the legacy hi/lo split, two-band, keyframe density, or "
            "Adaptive: each step judged at its hit, clustered per break). "
            "**Clock In** — J3 PPQN. **Voice** — Choke or Punch. **Gate "
            "Length / Level / Fires / Lanes** — see the Brain section. "
            "**Grammar** — the pattern language. **Bars** — override the bar "
            "guess (Auto reads the backbeat). **Rests** — continue or gate. "
            "**Fades** — fixed, or adaptive to each slice's brightness. "
            "**Slice Tail** — what a slot does at pitch: zoom (read on into "
            "the next slice, the record's way), gate, loop forward, or ping-pong. **Swaps** — class only, or "
            "class + loudness tier (a ghost never replaces a backbeat). "
            "**Flow Déjà Vu** — some flow bars replay from history. **Swing** "
            "— slice and step at the material's own swing. **Onsets** — the "
            "onset detector behind the bar guess and hit gates. **Jacks** — "
            "Brain (class gates out) or Voice (two v/oct + gate, hit trigger "
            "and velocity out); Follow grammar switches with Play. "
            "**Quantize** — Play's looper: Off (as played), 16ths (punch the "
            "bar), 8ths. **Autosave** — Off, or On: 2 s after an edit the "
            "slice table and key map are written to the set's JSON, or to "
            "sets/<file>.slices.json beside a loose WAV, and read back on load. "
"**Key Plays** — one-shot, or held until the gate falls (a key's own "
            "Plays, in the Kit, overrides it). "
            "**Sampling** — mono, or stereo planes. **Record Source** — the "
            "inputs, or the module's own output. **Record** — arm a loop "
            "take into the break (see Sampling). **Tap Allowance** — how far "
            "back a tap-while-playing mark lands (your reaction time): 0–80 ms. "
            "**Start in** (on the card, not in a preset; the web app sets it) — "
            "Sampler (Pads, stopped; the default, and with no card) or As "
            "saved (the boot preset's grammar, running); read at power-up.")
        .Section("sampling", "Sampling (record into a key, loop-record the break)",
            "One buffer, one slice table: a loaded break and a recorded kit "
            "are the same thing. **Into a key** (Voice map): play J5 style "
            "note **7 = Record** — the buttons go red — and every key you hold "
            "on J4 records from the input for as long as its gate stands "
            "(release: trim, normalise, analyse, live; the key plays it, its "
            "position lights on P5's ring). Style **8 = Clear** puts a key "
            "back on its grid slice; any other style disarms. Records are "
            "appended after the break; a load clears them. **The break "
            "itself**: Options → **Record** = Arm, then **B3 tap** or the "
            "first **J6** edge starts a take into the other buffer. With a "
            "clock on J3 the take is the **Bars** count of bars at the "
            "followed tempo and stops itself on the bar; J6 edges during it "
            "drop **slice markers** (two or more replace the grid). Unclocked, "
            "B3 or J6 stops it. The take runs the load pipeline and plays; "
            "the arm drops after one take, and B3's reslice / load are held "
            "off while armed. **Record Source**: J1/J2, or the module's own "
            "output — play the keys into a take and chop the chop. "
            "**Sampling**: Mono sums J1+J2; Stereo keeps J1 → L, J2 → R "
            "(loads, takes and playback in two planes). Loads keep 28 s of "
            "the 45 s buffer free for keys and pads; a recording plays at its "
            "own pitch (P4 transposes it), the break's slices follow the "
            "tempo. **Pads**: drop an audio file on a pad in the browser "
            "(Play or Kit) and that key plays it, mono or stereo (a stereo "
            "one makes the kit stereo); **New empty kit** clears the break so "
            "the whole buffer is for pads. **Each key** has its own gain "
            "(−24…+6 dB), tune (±24 semitones) and Plays (follow Key Plays · "
            "one-shot · held · loop while held · ratchet while held: the sample "
            "again every whole, ½, ¼ or ⅛ of itself, or every 8th / 16th / "
            "32nd at the tempo), set in the browser's Kit tab "
            "and saved with the set; they ride every call through the key — CV, "
            "the pads, Play's loop — never the pattern's steps. "
            "A dropped sample lives in memory "
            "until you save a set. While a take runs the buttons "
            "are full red and P6 shows the input level. **Cards**: "
            "FAT32 microSD, everything in one folder, `Audiothurgist/`: breaks "
            "in `Audiothurgist/breaks` (WAV, PCM 16/24-bit, mono or stereo, the "
            "first 17 s used), 1–4 bar breaks are ideal (longer files trim to "
            "their first four bars; levels normalise on load). The Kit tab "
            "uploads any audio file the browser can open, and copies an old "
            "card's root WAVs in. Turn **Sample** (P5) to highlight a file, hold **B3** to "
            "load it — or pick it from the Sample dropdown here. New material "
            "opens at its own tempo until P2 moves. **Sets** — a break with "
            "its recorded keys, the Options and Play's loop — live in "
            "`Audiothurgist/sets/` as `<name>.wav` + `<name>.json`, listed as “name [set]” "
            "and loaded the same way; the browser's Kit tab saves and loads "
            "them by name. Every preset save also writes `Audiothurgist/presets/<slot>.json`; "
            "a slot the flash no longer honours after a firmware update comes "
            "back from it at boot.");

void DescribeManual(hostlink::Host& host)
{
    perform.Help("The performance page: everything BreakFix was, plus tempo "
                 "as a turntable, history as a dial, and file selection.");
    brain.Help("The pattern-language console. Live for Grids and Euclid "
               "(Grammar, in Options); a change regenerates the pattern.");
    b_x.Help("**Map X** — Grids drum-map coordinate.");
    b_y.Help("**Map Y** — Grids drum-map coordinate.");
    b_kick.Help("**Kick density** — Grids threshold / Euclid hits per bar.");
    b_snare.Help("**Snare density** — Grids threshold / Euclid hits per bar.");
    b_hat.Help("**Hat density** — Grids threshold / Euclid hits per bar.");
    b_rot.Help("**Rotation** — Euclid: rotates every class's hits together.");

    k_chop.Help("**Chop character** — the journey from verbatim (full right) "
                "to rolls, bursts and reverb (full left).")
          .SeeAlso(k_hist);
    k_tempo.Help("**Tempo**, 60–200 BPM, varispeed like vinyl. Snaps to the "
                 "break's own tempo within ±2 BPM. While following an "
                 "external clock it becomes a ÷4…×4 multiplier.")
           .SeeAlso(jk3);
    k_hist.Help("**History** — the last sixteen patterns. Counter-clockwise "
                "is older; full clockwise is now. New rolls always append.")
          .SeeAlso(k_chop);
    k_pitch.Help("**Pitch**, ±12 semitones. Onsets stay on the grid; "
                 "the kit retunes.");
    k_sample.Help("**Sample** — highlight a file on the card (sets show as "
                  "“name [set]”); hold **B3** to load it. The browser dropdown "
                  "loads directly. While a set saves, this ring fills with the "
                  "progress; while one loads, a dot sweeps it. On the Edit page "
                  "(B1 held + B3 tap) it is the **autoslice** count.")
            .SeeAlso(jk1);
    k_vol.Help("**Volume**, with the reverb tail riding the cluster zone.");

    jk1.Help("**Audio in, left.** The record input: summed with J2 (Sampling: "
             "Mono) or the L plane (Stereo); a key take (J5 style 7) or a loop "
             "take (Options → Record) hears it unless Record Source = Output.")
       .SeeAlso(jk5);
    jk2.Help("**Audio in, right.** Summed with J1, or the R plane (Sampling: Stereo).")
       .SeeAlso(jk1);
    jk3.Help("**Clock in.** Set PPQN in Options. B1 = pulse/lock indicator.")
       .SeeAlso(k_tempo);
    jk4.Help("**Key** (v/oct) — 0 V = key 0 (C1), a semitone per key to key 23, "
             "below 0 V = key 0; the key plays what the Kit's key table says. "
             "Gated by J6 (Voice map) or J5 (Brain map).")
       .SeeAlso(jk5);
    jk5.Help("**Gate** (Brain map) — each rising edge plays J4's key, plain. "
             "**Style v/oct** (Voice map): 0 plain (anything under ~70 mV) · 1 reverse · "
             "2 half · 3 double · 4 ratchet · 5 staccato · 6 punch/choke flip · "
             "**7 Record** (keys held on J4 record from the input) · **8 Clear** (the "
             "key back to its slice).")
       .SeeAlso(jk4);
    jk6.Help("**Kick gate out** (Brain map) — every kick-class step, or once per hit "
             "(Gate Fires); Gate Lanes can add it to mixed hits. **Gate in** (Voice map): "
             "plays the J4 key; with Record armed its first edge starts the loop take, "
             "later edges drop slice markers (clocked) or stop it (unclocked).");
    jk7.Help("**Snare gate out** (Brain map) — every snare-class step, or once per hit. "
             "**Hit trigger out** (Voice map): 10 ms per call.");
    jk8.Help("**Hat gate out** (Brain map) — every hat-class step, or once per hit. "
             "**Velocity out** (Voice map): 1.5–5 V per call.");
    jk9.Help("Audio out, left.");
    jk10.Help("Audio out, right.");

    settings.Page(0).Help(
        "Hold **B2 + B3** for two seconds to enter settings: brightness and "
        "the preset save/load gesture. B2 or B3 leaves.");

    host.Jacks(jk1, jk2, jk3, jk4, jk5, jk6, jk7, jk8, jk9, jk10)
        .Attach(manual);
}
