#pragma once
// BETA (docs/packages/beta-fw.md): the one CV keymap — Combust's call of
// 2026-10-14, "one map, 0 V = key 0". Pure: the shell reads the jacks, these
// turn volts into a key and a style and a style into the call the audio
// thread makes; tests/keys_check.cpp holds them to it.
//
//   J4 = the key, in BOTH jack maps: 0 V is key 0 (C1), one semitone per key
//        up to key 23, negatives clamp to key 0, and a key always plays what
//        the Kit's key table says (Engine::KeySlice) — the grid's span slice
//        by default, a marker or a recording once the Kit points it there.
//   J5 = the style (Voice map; the Brain map's J5 is the gate and its calls
//        are plain): 0 plain · 1 reverse · 2 half · 3 double · 4 ratchet ·
//        5 staccato · 6 punch/choke flip · 7 Record · 8 Clear, a semitone
//        each — with a wide zero: anything under 0.85 st (71 mV) reads style
//        0, so an unpatched or offset J5 (≈ 60 mV, which the old quantiser
//        read as 1 = reverse) plays plain.
//
// Gone with the old maps: the Voice map's −1 st mute toggle, the Brain map's
// 0 V mute and its ten latched modifier keyswitches (start / stop is the
// transport now: B1 hold, ACTION 35).
#include <cmath>
#include <cstdint>

namespace bb {
namespace keys {

constexpr int   kKeys      = 24;      // Engine::kKeys
constexpr float kNoteTol   = 0.4f;    // note-centre window, semitones (spec §6.3: boundaries are the knife-edge bug class)
constexpr float kStyleZero = 0.85f;   // J5: below this (semitones) is style 0
constexpr int   kStyleMax  = 8;       // the highest style that means something (8 Clear)

/* The note-centre quantiser: n = round(volts · 12), taken only inside ±kNoteTol
 * of the note; between notes the last reading holds (`last`, the caller's). */
inline int Note(float volts, int& last)
{
    const float nf = volts * 12.f;
    const int   n  = (int)floorf(nf + 0.5f);
    if(fabsf(nf - (float)n) < kNoteTol) last = n;
    return last;
}
/* J4 → the key 0..23: negatives clamp to 0, past 23 to 23. */
inline int KeyOf(float volts, int& last)
{
    const int n = Note(volts, last);
    return n < 0 ? 0 : (n >= kKeys ? kKeys - 1 : n);
}
/* J5 → the style, ≥ 0 (styles past 8 act as Clear, as before: the shell's
 * record branch takes every style ≥ 7, 7 records, the rest clear). The zero
 * window resets `last`, so a jack drifting back down reads 0 at once. */
inline int StyleOf(float volts, int& last)
{
    if(volts * 12.f < kStyleZero) { last = 0; return 0; }
    const int n = Note(volts, last);
    return n < 0 ? 0 : n;
}
/* What the state tail reports for the style (271): 0..8. */
inline uint8_t StyleByte(int style) { return (uint8_t)(style < 0 ? 0 : (style > kStyleMax ? kStyleMax : style)); }

/* A playing style (0..6) → the call. `grammar` is Options → Grammar (5 Play,
 * 6 Pads), `voiceSel` Options → Voice (1 Punch). Pads never writes; Play
 * writes unless the style flips it (6); elsewhere the Voice option decides,
 * flipped by 6. The Brain map, PLAY_KEY and the page's pads call style 0. */
struct Call
{
    float mul   = 1.f;   // rate multiplier (half / double)
    float len   = 1.f;   // staccato: the fraction of the slice
    bool  rev   = false;
    bool  rat   = false; // ratchet
    bool  punch = false; // write the step (Play: the loop)
};
inline Call CallFor(int style, int grammar, int voiceSel)
{
    const int s = style < 0 ? 0 : (style > 6 ? 6 : style);
    Call c;
    c.rev   = s == 1;
    c.mul   = s == 2 ? 0.5f : (s == 3 ? 2.f : 1.f);
    c.rat   = s == 4;
    c.len   = s == 5 ? 0.5f : 1.f;
    c.punch = grammar == 6 ? false : (((grammar == 5) || voiceSel == 1) != (s == 6));
    return c;
}
/* J4 now in millivolts for the tail (268), saturating to i16. */
inline int16_t MilliVolts(float volts)
{
    float mv = volts * 1000.f;
    if(mv > 32767.f) mv = 32767.f;
    if(mv < -32768.f) mv = -32768.f;
    return (int16_t)(mv >= 0.f ? mv + 0.5f : mv - 0.5f);
}

} // namespace keys
} // namespace bb
