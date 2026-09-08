// panel_check — EDIT-PANEL's pure half (core/bb_panel_edit.h): the selection
// walker, the centred pickup, the nudge → request mapping and its clamps,
// the rate window, the computed grid against the engine's own, the revert
// mask, the bar-ring states. Build + run: tests/run.sh, or by hand:
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore -Ivendor/capicola/lib \
//       tests/panel_check.cpp -o /tmp/panel && /tmp/panel
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "bb_engine.h"
#include "bb_panel_edit.h"

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

using namespace bb::panel;
static constexpr int kSr = 48000;

static bb::Engine gEng;
static float      gMat[96000];

int main()
{
    SECTION("walker: live entries, tombstones skipped");
    {
        /* 16 grid + 4 markers, markers 17 and 19 tombstoned */
        uint8_t fl[20] = {};
        fl[17] = kFlagTomb; fl[19] = kFlagTomb;
        CHECK(LiveCount(fl, 20) == 18, "live count 18 (got %d)", LiveCount(fl, 20));
        CHECK(LiveAt(fl, 20, 0) == 0 && LiveAt(fl, 20, 15) == 15, "ordinals 0..15 are the grid");
        CHECK(LiveAt(fl, 20, 16) == 16 && LiveAt(fl, 20, 17) == 18, "ordinal 16 → 16, 17 → 18 (17 skipped)");
        CHECK(LiveAt(fl, 20, 18) == -1 && LiveAt(fl, 20, -1) == -1, "past the end / negative → −1");
        CHECK(OrdinalOf(fl, 20, 18) == 17 && OrdinalOf(fl, 20, 16) == 16, "OrdinalOf inverts LiveAt");
        CHECK(OrdinalOf(fl, 20, 17) == -1 && OrdinalOf(fl, 20, 20) == -1 && OrdinalOf(fl, 20, -1) == -1, "tombstone / out of range → −1");
        CHECK(!Live(fl, 20, 19) && Live(fl, 20, 18) && !Live(fl, 20, 20), "Live()");
        CHECK(SelectByPot(0.f, fl, 20) == 0, "P1 at 0 → entry 0");
        CHECK(SelectByPot(1.f, fl, 20) == 18, "P1 at 1 → the last live (18)");
        CHECK(SelectByPot(2.f, fl, 20) == 18 && SelectByPot(-1.f, fl, 20) == 0, "P1 clamps");
        CHECK(SelectByPot(0.5f, fl, 20) == 9, "P1 at 0.5 → ordinal 9 (got %d)", SelectByPot(0.5f, fl, 20));
        CHECK(SelectByPot(17.5f / 18.f, fl, 20) == 18, "P1 in the last bin skips the tombstone");
        uint8_t none[4] = {kFlagTomb, kFlagTomb, kFlagTomb, kFlagTomb};
        CHECK(SelectByPot(0.5f, none, 4) == -1 && SelectByPot(0.5f, fl, 0) == -1, "nothing live → −1");
        /* the ordinal is even over the travel: every live entry is reachable */
        bool all = true;
        for(int o = 0; o < 18; o++) { const float v = ((float)o + 0.5f) / 18.f; if(OrdinalOf(fl, 20, SelectByPot(v, fl, 20)) != o) all = false; }
        CHECK(all, "every ordinal has a bin");
    }

    SECTION("walker: key → entry");
    {
        uint8_t fl[20] = {}; fl[17] = kFlagTomb;
        int8_t ks[kKeys];
        for(int k = 0; k < kKeys; k++) ks[k] = (int8_t)SpanSlice(k, 16);
        CHECK(KeyEntry(ks, 0, 16, 20, fl) == 0 && KeyEntry(ks, 15, 16, 20, fl) == 15, "the span default");
        CHECK(KeyEntry(ks, 23, 16, 20, fl) == 15, "keys past the span play the last slice");
        CHECK(KeyEntry(ks, -3, 16, 20, fl) == 0 && KeyEntry(ks, 40, 16, 20, fl) == 15, "key clamps");
        ks[5] = 18;
        CHECK(KeyEntry(ks, 5, 16, 20, fl) == 18, "a key on a marker");
        ks[6] = 17;
        CHECK(KeyEntry(ks, 6, 16, 20, fl) == SpanSlice(6, 16), "a key on a tombstone → its span slice");
        ks[7] = 30;
        CHECK(KeyEntry(ks, 7, 16, 20, fl) == 19, "an index past the table → the last entry");
        ks[8] = -1;
        CHECK(KeyEntry(ks, 8, 16, 20, fl) == SpanSlice(8, 16), "−1 = the default");
        CHECK(KeyEntry(ks, 0, 0, 0, fl) == -1, "an empty table → −1");
        CHECK(SpanSlice(3, 32) == 4 && SpanSlice(23, 32) == 30 && SpanSlice(0, 0) == 0, "SpanSlice = Engine::SpanSlice");
    }

    SECTION("walker: reselect after a table change");
    {
        uint8_t fl[20] = {};
        CHECK(Reselect(18, 18, fl, 20) == 18, "still live → the same");
        fl[18] = kFlagTomb;
        CHECK(Reselect(18, 18, fl, 20) == 19, "tombstoned → the live entry at its ordinal (the next one)");
        CHECK(Reselect(25, 30, fl, 20) == 19, "past the table, ordinal past the end → the last live");
        CHECK(Reselect(-1, -1, fl, 20) == 0, "no selection with a live table → the first");
        uint8_t none[2] = {kFlagTomb, kFlagTomb};
        CHECK(Reselect(0, 0, none, 2) == -1, "nothing live → −1");
    }

    SECTION("pickup: the SDK's catch, centred");
    {
        Pickup p;
        ArmPickup(p, 0.5f);
        CHECK(p.caught && p.sign == 0, "armed at centre → caught at once");
        ArmPickup(p, 0.503f);
        CHECK(p.caught, "within the tolerance → caught");
        ArmPickup(p, 0.8f);
        CHECK(!p.caught && p.sign == 1, "armed above centre → inert, sign +");
        CHECK(!UpdatePickup(p, 0.7f) && !UpdatePickup(p, 0.52f), "moving toward centre on the same side stays inert");
        CHECK(UpdatePickup(p, 0.5f) && p.caught, "reaching centre picks up (proximity)");
        ArmPickup(p, 0.2f);
        CHECK(!p.caught && p.sign == -1, "armed below → sign −");
        CHECK(UpdatePickup(p, 0.62f), "jumping across centre picks up (crossover)");
        CHECK(UpdatePickup(p, 0.9f), "once caught it stays caught");
        ArmPickup(p, 0.9f);
        CHECK(!UpdatePickup(p, 0.99f), "away from centre: still inert");
    }

    SECTION("offset: pot → ms, dead band, clamps");
    {
        CHECK(OffsetMs(0.5f) == 0 && OffsetMs(0.505f) == 0 && OffsetMs(0.495f) == 0, "dead band ±0.01 → 0");
        CHECK(OffsetMs(1.f) == 50 && OffsetMs(0.f) == -50, "the ends are ±50 ms");
        CHECK(OffsetMs(1.5f) == 50 && OffsetMs(-0.5f) == -50, "clamped past the ends");
        CHECK(OffsetMs(0.75f) == 25 && OffsetMs(0.25f) == -25, "quarter turns are ±25 ms");
        CHECK(OffsetMs(0.6f) == -OffsetMs(0.4f), "symmetric");
        bool mono = true; int last = -51;
        for(int i = 0; i <= 200; i++) { const int m = OffsetMs((float)i / 200.f); if(m < last) mono = false; last = m; }
        CHECK(mono, "monotonic over the travel");
        CHECK(MsToSamples(50, kSr) == 2400 && MsToSamples(-7, kSr) == -336, "ms → samples at 48 k");
        CHECK(fabsf(RingValue(25) - 0.5f) < 1e-6f && RingValue(-50) == -1.f && RingValue(80) == 1.f, "ring value −1..+1");
    }

    SECTION("rate window: ~20/s, last value wins");
    {
        RateGate g; int out = -1;
        CHECK(Offer(g, 10, 1000u, &out) && out == 10, "the first offer posts");
        CHECK(!Offer(g, 11, 1000u + 10000u, &out) && g.pend, "10 ms later: held");
        CHECK(!Offer(g, 12, 1000u + 30000u, &out) && g.val == 12, "a later offer replaces the held one");
        CHECK(!Drain(g, 1000u + 40000u, &out), "not yet");
        CHECK(Drain(g, 1000u + 50000u, &out) && out == 12 && !g.pend, "the window ends: the last value goes out");
        CHECK(!Drain(g, 1000u + 60000u, &out), "nothing left");
        CHECK(Offer(g, 13, 1000u + 100000u, &out) && out == 13, "past the window an offer posts at once");
        /* µs wrap */
        RateGate w; w.armed = true; w.lastUs = 0xFFFFFF00u;
        CHECK(!Offer(w, 1, 0x0000C000u, &out), "49 408 µs across the 2^32 wrap: still held");
        CHECK(Drain(w, 0x0000C400u, &out) && out == 1, "50 176 µs across the wrap: the window ends");
    }

    SECTION("nudge → request: the grid");
    {
        NudgeBase b; b.nSlices = 16; b.freePos = 96000;
        /* slice 5 of a 6000-sample grid, neighbours at the computed places */
        b.t = 5; SetSpan(b, 30000, 36000); b.prevStart = 24000; b.nextEnd = 42000;
        NudgePlan p = PlanNudge(b, 0, MsToSamples(10, kSr), kSr);
        CHECK(p.kind == NudgeKind::Boundary && p.i == 5 && p.start == 30480, "start +10 ms → SET_BOUNDARY(5, 30480) (got i %d pos %d)", p.i, p.start);
        CHECK(p.audStart == 30480 && p.audLen == 5520 && !p.fromEnd, "audition: the new start, the whole (short) slice (len %d)", p.audLen);
        p = PlanNudge(b, 0, MsToSamples(-50, kSr), kSr);
        CHECK(p.kind == NudgeKind::Boundary && p.start == 27600 && p.audLen == 8400, "start −50 ms: the audition is the whole 175 ms slice (len %d)", p.audLen);
        { NudgeBase L = b; SetSpan(L, 30000, 60000); L.nextEnd = 66000; p = PlanNudge(L, 0, 0, kSr); CHECK(p.kind == NudgeKind::Boundary && p.audLen == 12000, "a long slice: the audition caps at 250 ms"); p = PlanNudge(L, 1, 0, kSr); CHECK(p.fromEnd && p.audStart == 60000 && p.audLen == 12000, "…from the end too"); }
        p = PlanNudge(b, 1, MsToSamples(20, kSr), kSr);
        CHECK(p.kind == NudgeKind::Boundary && p.i == 6 && p.start == 36960 && p.fromEnd && p.audStart == 36960 && p.audLen == 6960, "end +20 ms → SET_BOUNDARY(6, 36960), audition from the end");
        /* clamps against the neighbours: 64 each side */
        NudgeBase c = b; c.prevStart = 30000 - 100;
        p = PlanNudge(c, 0, MsToSamples(-50, kSr), kSr);
        CHECK(p.kind == NudgeKind::Boundary && p.start == 29964, "a start move keeps the predecessor 64 (got %d)", p.start);
        p = PlanNudge(b, 0, MsToSamples(50, kSr), kSr);
        CHECK(p.start == 32400 && p.start <= b.end - kGridMin, "+50 ms inside the slice");
        NudgeBase d = b; SetSpan(d, 35900, 36000);
        p = PlanNudge(d, 0, MsToSamples(50, kSr), kSr);
        CHECK(p.kind == NudgeKind::Boundary && p.start == 36000 - 64, "a start move keeps the slice itself 64");
        NudgeBase e = b; e.nextEnd = 36000 + 100;
        p = PlanNudge(e, 1, MsToSamples(50, kSr), kSr);
        CHECK(p.kind == NudgeKind::Boundary && p.start == 36036, "an end move keeps the successor 64");
        /* the edges of the grid */
        NudgeBase f = b; f.t = 0; SetSpan(f, 0, 6000); f.prevStart = 0;
        CHECK(PlanNudge(f, 0, 100, kSr).kind == NudgeKind::Refused, "slice 0's start is the material's: refused");
        CHECK(PlanNudge(f, 1, 100, kSr).kind == NudgeKind::Boundary && PlanNudge(f, 1, 100, kSr).i == 1, "slice 0's end is boundary 1");
        NudgeBase g = b; g.t = 15; SetSpan(g, 90000, 96000);
        CHECK(PlanNudge(g, 1, 100, kSr).kind == NudgeKind::Refused, "the last slice's end is the material's: refused");
        CHECK(PlanNudge(g, 0, 100, kSr).kind == NudgeKind::Boundary && PlanNudge(g, 0, 100, kSr).i == 15, "the last slice's start is boundary 15");
        NudgeBase h; h.t = -1;
        CHECK(PlanNudge(h, 0, 100, kSr).kind == NudgeKind::Refused, "no selection: refused");
        NudgeBase tight = b; tight.prevStart = 30000 - 64; SetSpan(tight, 30000, 30064);
        p = PlanNudge(tight, 0, 0, kSr);
        CHECK(p.kind == NudgeKind::Boundary && p.start == 30000, "a slice already at both minima can only stay");
        NudgeBase imp = b; imp.prevStart = 30000; SetSpan(imp, 30000, 30064);
        CHECK(PlanNudge(imp, 0, 0, kSr).kind == NudgeKind::Refused, "no legal position → refused, nothing posted");
        CHECK(PlanNudge(b, 0, 0, kSr).start == 30000 && PlanNudge(b, 1, 0, kSr).start == 36000, "centre = no change");
        /* origin vs current: a landed start move does not shift the start pot's origin, and the end pot clamps against the NEW start */
        NudgeBase L = b; SetSpan(L, 30000, 30000 + 3000); L.nextEnd = 39000;
        p = PlanNudge(L, 0, MsToSamples(50, kSr), kSr);
        CHECK(p.kind == NudgeKind::Boundary && p.start == 32400, "start +50 ms on a 3000-sample slice → 32400");
        Landed(L, p, 0);
        CHECK(L.start == 32400 && L.origStart == 30000 && L.end == 33000, "landed: current start follows, the origin stays");
        p = PlanNudge(L, 0, MsToSamples(25, kSr), kSr);
        CHECK(p.start == 31200, "the next start offset is still from the origin (not cumulative)");
        p = PlanNudge(L, 1, MsToSamples(-50, kSr), kSr);
        CHECK(p.kind == NudgeKind::Boundary && p.i == 6 && p.start == 32400 + kGridMin, "an end move clamps against the moved start, not the origin (got %d)", p.start);
        Landed(L, p, 1);
        CHECK(L.end == 32464 && L.origEnd == 33000, "landed end");
        NudgeBase M; M.t = 17; M.nSlices = 16; M.freePos = 100000; SetSpan(M, 50000, 56000);
        p = PlanNudge(M, 1, MsToSamples(-50, kSr), kSr); Landed(M, p, 1);
        CHECK(M.start == 50000 && M.end == 53600, "a marker's landed end");
        p = PlanNudge(M, 0, MsToSamples(50, kSr), kSr);
        CHECK(p.kind == NudgeKind::Entry && p.start == 52400 && p.len == 1200, "a marker's start move against its moved end");
    }

    SECTION("nudge → request: markers");
    {
        NudgeBase b; b.nSlices = 16; b.freePos = 100000; b.t = 17; SetSpan(b, 50000, 56000);
        NudgePlan p = PlanNudge(b, 0, MsToSamples(-10, kSr), kSr);
        CHECK(p.kind == NudgeKind::Entry && p.t == 17 && p.start == 49520 && p.len == 6480, "start −10 ms → SET_ENTRY(17, 49520, 6480)");
        CHECK(p.audStart == 49520 && p.audLen == 6480 && !p.fromEnd, "audition from the new start");
        p = PlanNudge(b, 1, MsToSamples(30, kSr), kSr);
        CHECK(p.kind == NudgeKind::Entry && p.start == 50000 && p.len == 7440 && p.fromEnd && p.audStart == 57440, "end +30 ms, audition up to the new end");
        NudgeBase c = b; SetSpan(c, 1000, 56000);
        p = PlanNudge(c, 0, MsToSamples(-50, kSr), kSr);
        CHECK(p.kind == NudgeKind::Entry && p.start == 0 && p.len == 56000, "a marker's start clamps at 0");
        NudgeBase d = b; SetSpan(d, 50000, 99000);
        p = PlanNudge(d, 1, MsToSamples(50, kSr), kSr);
        CHECK(p.kind == NudgeKind::Entry && p.start + p.len == 100000, "a marker's end clamps at freePos");
        NudgeBase e = b; SetSpan(e, 50000, 50600);
        p = PlanNudge(e, 0, MsToSamples(50, kSr), kSr);
        CHECK(p.kind == NudgeKind::Entry && p.len == kRecMin && p.start == e.end - kRecMin, "a start move keeps kRecMin");
        p = PlanNudge(e, 1, MsToSamples(-50, kSr), kSr);
        CHECK(p.kind == NudgeKind::Entry && p.len == kRecMin && p.start == e.start, "an end move keeps kRecMin");
        NudgeBase f = b; f.freePos = 55000;
        p = PlanNudge(f, 1, 0, kSr);
        CHECK(p.kind == NudgeKind::Entry && p.start + p.len == 55000, "a base past freePos (a stale base) clamps in");
        f.freePos = 50100;
        CHECK(PlanNudge(f, 1, 0, kSr).kind == NudgeKind::Refused, "…and with no room for kRecMin: refused");
        p = PlanNudge(b, 1, MsToSamples(-100, kSr), kSr);
        CHECK(p.kind == NudgeKind::Entry && p.len == 1200 && p.start == 50000, "−100 ms on a 6000-sample marker: 1200 left");
        p = PlanNudge(b, 1, MsToSamples(-200, kSr), kSr);
        CHECK(p.kind == NudgeKind::Entry && p.len == kRecMin, "an offset past the range clamps at kRecMin");
    }

    SECTION("the computed grid = the engine's");
    {
        memset(gMat, 0, sizeof gMat);
        for(int i = 0; i < 96000; i++) gMat[i] = 0.1f * sinf((float)i * 0.01f);
        gEng.swing = 0.5f;
        gEng.SetMaterial(gMat, 96000, (float)kSr, nullptr, 0, nullptr, 16);
        bool ok = gEng.nSlices == 16;
        for(int i = 0; i <= 16 && ok; i++)
        {
            const int want = i < 16 ? gEng.slices[i].start : 96000;
            if(GridStart(i, 96000, 16, gEng.swing, Swung(gEng.swing, 16)) != want) ok = false;
        }
        CHECK(ok && !Swung(0.5f, 16), "straight grid: every boundary matches SetMaterial");
        gEng.swing = 0.62f;
        gEng.SetMaterial(gMat, 96000, (float)kSr, nullptr, 0, nullptr, 16);
        ok = gEng.nSlices == 16 && gEng.swung;
        for(int i = 0; i <= 16 && ok; i++)
        {
            const int want = i < 16 ? gEng.slices[i].start : 96000;
            if(GridStart(i, 96000, 16, gEng.swing, Swung(gEng.swing, 16)) != want) ok = false;
        }
        CHECK(ok && Swung(0.62f, 16), "swung grid: the odd boundaries match SetMaterial");
        CHECK(GridStart(3, 96000, 16, 0.62f, true) == 2 * 6000 + (int)(12000.f * 0.62f + 0.5f), "odd boundary formula");
        CHECK(GridStart(0, 96000, 16, 0.62f, true) == 0 && GridStart(16, 96000, 16, 0.62f, true) == 96000 && GridStart(3, 96000, 0, 0.5f, false) == 0, "the ends and an empty grid");
        CHECK(!Swung(0.75f, 16) && !Swung(0.6f, 15) && !Swung(0.6f, 1), "swing applies to even counts inside (0.5, 0.75)");
        /* an unswung odd count: plain i·step */
        gEng.swing = 0.5f;
        gEng.SetMaterial(gMat, 96000, (float)kSr, nullptr, 0, nullptr, 32);
        ok = gEng.nSlices == 32;
        for(int i = 0; i < 32 && ok; i++) if(GridStart(i, 96000, 32, 0.5f, false) != gEng.slices[i].start) ok = false;
        CHECK(ok, "32 steps: matches");
    }

    SECTION("revert mask");
    {
        CHECK(RevertMask(5, 16, 30000, 36000, 30000, 36000) == 0, "on the grid: nothing to do");
        CHECK(RevertMask(5, 16, 30480, 36000, 30000, 36000) == 1, "a moved start → boundary 5");
        CHECK(RevertMask(5, 16, 30000, 36500, 30000, 36000) == 2, "a moved end → boundary 6");
        CHECK(RevertMask(5, 16, 29000, 37000, 30000, 36000) == 3, "both");
        CHECK(RevertMask(0, 16, 0, 6100, 0, 6000) == 2, "slice 0: only its end can move");
        CHECK(RevertMask(15, 16, 90100, 96000, 90000, 96000) == 1, "the last slice: only its start");
        CHECK(RevertMask(17, 16, 1, 2, 3, 4) == 0, "a marker: no boundary (the shell tombstones)");
    }

    SECTION("bar ring states");
    {
        CHECK(BarBase(5, 32, 20) == 0 && BarBase(20, 32, 5) == 16, "the selection's bar is shown");
        CHECK(BarBase(40, 32, 20) == 16 && BarBase(-1, 32, 3) == 0, "a marker / no selection: the playing bar");
        CHECK(BarLed(5, 5, 5, 2, -1, 16) == kLedSel, "the selected slice's own segment");
        CHECK(BarLed(5, 5, 5, 5, -1, 16) == kLedPulse, "…pulses while it plays");
        CHECK(BarLed(9, 5, 5, 2, -1, 16) == kLedSelDim, "a step that plays the selection is dim");
        CHECK(BarLed(9, 5, 5, 9, -1, 16) == kLedPulse, "…and pulses at its turn");
        CHECK(BarLed(5, 5, 9, 2, 5, 16) == kLedPulse, "a Voice call of the selection pulses its segment");
        CHECK(BarLed(2, 5, 2, 2, -1, 16) == kLedHead, "the playhead elsewhere");
        CHECK(BarLed(2, 5, 2, 7, -1, 16) == kLedPlain && BarLed(2, -1, 2, 7, -1, 16) == kLedPlain, "plain / no selection");
        CHECK(BarLed(20, 5, 5, 20, -1, 16) == kLedPlain, "past the grid: plain");
        CHECK(BarLed(3, 17, 17, 3, -1, 16) == kLedPulse && BarLed(3, 17, 17, 1, -1, 16) == kLedSelDim, "a marker punched into a step");
        int8_t ks[kKeys]; uint8_t fl[20] = {};
        for(int k = 0; k < kKeys; k++) ks[k] = (int8_t)SpanSlice(k, 16);
        ks[4] = 17;
        CHECK(KeyOnSelection(ks, 4, 17, 16, 20, fl) && !KeyOnSelection(ks, 5, 17, 16, 20, fl) && KeyOnSelection(ks, 3, 3, 16, 20, fl) && !KeyOnSelection(ks, 3, -1, 16, 20, fl), "the keys on the selection");
        CHECK(Pulse(0, 16) == 0.f && Pulse(8, 16) == 1.f && fabsf(Pulse(4, 16) - 0.5f) < 1e-6f && Pulse(12, 16) == 0.5f && Pulse(5, 1) == 1.f, "the triangle");
    }

    printf("\npanel_check: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
