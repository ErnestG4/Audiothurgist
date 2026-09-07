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
            "gate pair on J4/J5 calls slices from a keyboard or sequencer. "
            "**Brain** — it plays the rack: kick, snare and hat gates leave on "
            "J6/J7/J8 as the pattern runs, and a clock on J3 locks the loop "
            "to your master. The roles run at once; nothing needs a mode.")
        .Section("journey", "The knob journey (P1)",
            "Full right: verbatim. ~4 o'clock: whole beats trade places. "
            "~1 o'clock: one-beat chops — listen for the double-time illusion. "
            "~11: half-beat motifs. ~9: sixteenth-note dice and reversals. "
            "Past 8: the cluster zone — bursts of 32nd rolls, hat-ticks and "
            "real silence, reverb blooming toward full left. B3 relocates the "
            "burst centres. Chops respect the kit: kicks trade with kicks, "
            "snares with snares.")
        .Section("keymap", "Playing it from a keyboard (the Voice jack map)",
            "Options → **Jacks** picks the map; *Follow grammar* gives the "
            "Play grammar the Voice map. **Voice map**: J6 gate, **J4 = the "
            "slice** (one key per slice region, up to two octaves from 0 V; "
            "one semitone below 0 V toggles mute), **J5 = the style** (0 V "
            "plain, then reverse · half-speed · double · ratchet · staccato · "
            "punch/choke flip, one semitone each). The style is read with "
            "the hit, and changing it while the key is held re-strikes the "
            "slice — the left hand plays rhythm. J7 sends a trigger per "
            "call, J8 its velocity. In Play, calls write the bar (the punch "
            "style flips to momentary); elsewhere the Voice option decides. "
            "**Brain map** (J5 gate, J4 v/oct): the older single-keyboard "
            "layout — 0 V mutes, ten latched modifier keys, sixteen slice "
            "keys two octaves up — with the class gates on J6/J7/J8.")
        .Section("buttons", "The three buttons",
            "**B1** tap: Perform ↔ Brain page. **B1 held 300 ms + turn a pot**: "
            "record that pot's motion as a loop that replays until you hold "
            "and nudge it again (a green pip at 6 o'clock marks a locked pot; "
            "*Clear locks* in the web app removes them all). **B2** tap: dice "
            "(a new arrangement of the same slices); hold: *flow*, a new bar "
            "every loop until released. **B3** tap: reslice (new cell tree); "
            "hold: load the highlighted file. **B2 + B3 held 2 s**: Settings "
            "(brightness, preset save/load); B2 or B3 leaves. B1 also shows "
            "the clock: white per pulse, green when locked.")
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
            "into it from the keyboard or the web page, and B3 clears it.")
        .Section("options", "Options, one line each",
            "**Classifier** — which lens labels kick/snare/hat (Hockman "
            "3-band, the legacy hi/lo split, two-band, or keyframe density). "
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
            "and velocity out); Follow grammar switches with Play.")
        .Section("cards", "Cards and loading",
            "FAT32 microSD, WAVs in the root, 1–4 bar breaks are ideal "
            "(longer files trim to their first four bars; levels normalise on "
            "load). Turn **Sample** (P5) to highlight a file, hold **B3** to "
            "load it — or pick it from the Sample dropdown here. New material "
            "opens at its own tempo until P2 moves.");

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
    k_sample.Help("**Sample** — highlight a file on the card; hold **B3** to "
                  "load it. The browser dropdown loads directly.")
            .SeeAlso(jk1);
    k_vol.Help("**Volume**, with the reverb tail riding the cluster zone.");

    jk1.Help("Audio in, left (record/resample — coming).");
    jk2.Help("Audio in, right.");
    jk3.Help("**Clock in.** Set PPQN in Options. B1 = pulse/lock indicator.")
       .SeeAlso(k_tempo);
    jk4.Help("**V/Oct** — the keymap's pitch: mute key, modifiers, sixteen "
             "slice keys. Pair with J5.")
       .SeeAlso(jk5);
    jk5.Help("**Gate** (Brain map) — each rising edge samples J4 and dispatches. "
             "**Style v/oct** (Voice map).")
       .SeeAlso(jk4);
    jk6.Help("**Kick gate out** (Brain map) — every kick-class step, or once per hit "
             "(Gate Fires); Gate Lanes can add it to mixed hits. **Gate in** (Voice map).");
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
