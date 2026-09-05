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
        .Section("keymap", "Playing it from a keyboard (J4 v/oct, J5 gate)",
            "Each gate edge samples v/oct once. Relative to the key that "
            "outputs 0 V: that key **mutes/unmutes** the loop; the next is a "
            "gap; the following ten are **latched modifier keyswitches** "
            "(reverse, half-speed, double, choke/punch override, ratchet); "
            "two octaves up, **sixteen keys call sixteen slices** spread "
            "across the loop. Calls replace what is sounding. In *Punch* mode "
            "(Options) a call also writes itself into the bar at the current "
            "step — playing is editing. Mute + punch composes a loop from "
            "silence.")
        .Section("brain", "Clock in, gates out",
            "Patch a clock into J3 and set its PPQN in Options; B1 flashes "
            "white per pulse and sits green when locked. While following, P2 "
            "becomes a clock divider/multiplier (÷4 · ÷2 · ×1 · ×2 · ×4, wide "
            "×1 centre) and engages once you move it. Unpatch and the tempo "
            "holds until P2 moves. Gates on J6/J7/J8 follow the classifier's "
            "labels; Gate Length and Gate Level (velocity from each slice's "
            "loudness) live in Options.")
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
            "each class's own nearest hit, lower swaps within the class.")
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
    k_tempo.Help("**Tempo**, 70–180 BPM, varispeed like vinyl. Snaps to the "
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
    jk5.Help("**Gate** — each rising edge samples J4 and dispatches.")
       .SeeAlso(jk4);
    jk6.Help("**Kick gate out** — fires with every kick-class step.");
    jk7.Help("**Snare gate out** — fires with every snare-class step.");
    jk8.Help("**Hat gate out** — fires with every hat-class step.");
    jk9.Help("Audio out, left.");
    jk10.Help("Audio out, right.");

    settings.Page(0).Help(
        "Hold **B2 + B3** for two seconds to enter settings: brightness and "
        "the preset save/load gesture. B2 or B3 leaves.");

    host.Jacks(jk1, jk2, jk3, jk4, jk5, jk6, jk7, jk8, jk9, jk10)
        .Attach(manual);
}
