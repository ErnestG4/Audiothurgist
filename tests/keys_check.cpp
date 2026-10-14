// keys_check — BETA's one CV keymap (core/bb_keys.h): J4 → the key (0 V = key
// 0, a semitone a key, negatives → 0, the note-centre window), J5 → the style
// (the wide zero: an unpatched / offset jack plays plain), the call plan per
// style × grammar × Voice option, the tail's millivolts. Build + run:
// tests/run.sh, or by hand:
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore tests/keys_check.cpp -o /tmp/keys && /tmp/keys
#include <cmath>
#include <cstdio>
#include <cstdint>
#include "bb_keys.h"

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

using namespace bb::keys;
static float St(int n) { return (float)n / 12.f; }   // a semitone's volts

int main()
{
    SECTION("J4 → the key: 0 V = key 0, a semitone a key, negatives → 0, past 23 → 23");
    {
        int last = -99;
        bool all = true;
        for(int k = 0; k < 24; k++) all &= KeyOf(St(k), last) == k;
        CHECK(all, "every key 0..23 at its own semitone");
        CHECK(KeyOf(0.f, last) == 0, "0 V = key 0 (C1)");
        CHECK(KeyOf(-0.5f, last) == 0 && KeyOf(-5.f, last) == 0 && KeyOf(St(-1), last) == 0, "negatives clamp to key 0 (the old −1 st mute key is gone)");
        CHECK(KeyOf(St(30), last) == 23 && KeyOf(5.f, last) == 23, "past key 23 → 23");
        CHECK(KeyOf(St(5) + 0.3f / 12.f, last) == 5 && KeyOf(St(5) - 0.3f / 12.f, last) == 5, "±0.3 st of a note reads the note");
        last = -99; KeyOf(St(5), last);
        CHECK(KeyOf(St(5) + 0.5f / 12.f, last) == 5, "exactly between 5 and 6: the last reading holds (5)");
        KeyOf(St(6), last);
        CHECK(KeyOf(St(5) + 0.5f / 12.f, last) == 6, "…coming from 6 it stays 6 (no knife edge)");
        last = -99;
        CHECK(KeyOf(St(5) + 0.5f / 12.f, last) == 0, "a first reading on a boundary: nothing yet → key 0");
    }
    SECTION("J5 → the style: semitone steps, a wide zero (an unpatched / offset jack plays plain)");
    {
        int last = 0;
        CHECK(StyleOf(0.f, last) == 0 && StyleOf(-1.f, last) == 0, "0 V and below: plain");
        CHECK(StyleOf(0.060f, last) == 0, "60 mV (0.72 st — the offset an unpatched V2 jack reads): plain (the old quantiser said 1 = reverse)");
        CHECK(StyleOf(0.070f, last) == 0, "70 mV (0.84 st): still plain");
        CHECK(StyleOf(0.075f, last) == 1, "75 mV (0.9 st): reverse");
        bool steps = true; for(int s = 1; s <= 8; s++) steps &= StyleOf(St(s), last) == s;
        CHECK(steps, "styles 1..8 at their semitones");
        StyleOf(St(3), last);
        CHECK(StyleOf(St(3) + 0.5f / 12.f, last) == 3, "between 3 and 4: holds 3");
        CHECK(StyleOf(0.02f, last) == 0 && last == 0, "back under the zero: 0 at once, and the memory with it");
        CHECK(StyleByte(-3) == 0 && StyleByte(5) == 5 && StyleByte(11) == 8, "the tail's style byte: 0..8");
    }
    SECTION("the call plan: style × grammar × Voice option (Pads never writes, Play writes, 6 flips)");
    {
        Call c = CallFor(0, 0, 0);
        CHECK(c.mul == 1.f && c.len == 1.f && !c.rev && !c.rat && !c.punch, "plain, Classic, Choke: a plain one-shot, not written");
        CHECK(CallFor(1, 0, 0).rev && CallFor(2, 0, 0).mul == 0.5f && CallFor(3, 0, 0).mul == 2.f && CallFor(4, 0, 0).rat && CallFor(5, 0, 0).len == 0.5f,
              "1 reverse · 2 half · 3 double · 4 ratchet · 5 staccato");
        CHECK(CallFor(0, 0, 1).punch && !CallFor(6, 0, 1).punch && CallFor(6, 0, 0).punch, "Voice = Punch writes; style 6 flips either way");
        CHECK(CallFor(0, 5, 0).punch && !CallFor(6, 5, 0).punch, "Play: writes (the loop), 6 makes it momentary");
        bool pads = true; for(int s = 0; s <= 6; s++) for(int v = 0; v < 2; v++) pads &= !CallFor(s, 6, v).punch;
        CHECK(pads, "Pads: nothing is ever written, whatever the style or option");
        CHECK(CallFor(9, 0, 0).punch == CallFor(6, 0, 0).punch && CallFor(-2, 0, 0).mul == 1.f, "styles clamp to 0..6 for the call");
    }
    SECTION("the tail's J4 millivolts");
    {
        CHECK(MilliVolts(0.f) == 0 && MilliVolts(1.f) == 1000 && MilliVolts(-0.0834f) == -83 && MilliVolts(0.0004f) == 0, "volts → mV, rounded");
        CHECK(MilliVolts(40.f) == 32767 && MilliVolts(-40.f) == -32768, "saturating");
    }

    printf("\nkeys_check: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
