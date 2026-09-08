// sets_check — SETS package host suite: core/bb_sets.h (the pure half of the
// card side of persistence; docs/packages/sample-sets.md). Build + run
// (tests/run.sh does this under ASan/UBSan):
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore -Ivendor/capicola/lib
//       tests/sets_check.cpp -o /tmp/sc && /tmp/sc
//
// 1. Naming: set names, list entries, labels, card paths, lock paths, the
//    preset backup path, the case-insensitive entry lookup.
// 2. The round trip that matters: a real Engine with a break, two recorded
//    keys and three looper events → SetFromEngine → JSON → SetReader →
//    PlanSet → a second Engine (SetMaterial + CommitSetRecordings +
//    LooperFromSet/Adopt) whose table, keys, hits, classes, looper AND
//    rendered audio are identical to the first.
// 3. PlanSet's refusals; MergeOptions; Looper::Adopt.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "bb_engine.h"
#include "bb_testbreak.h"
#include "bb_set.h"
#include "bb_sets.h"

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

static void TestNaming()
{
    SECTION("naming");
    CHECK(bb::SetNameOk("amen") && bb::SetNameOk("Amen_170-v2") && bb::SetNameOk("a") && bb::SetNameOk("0123456789012345678901234567890"), "valid names");
    CHECK(!bb::SetNameOk("") && !bb::SetNameOk(nullptr) && !bb::SetNameOk("01234567890123456789012345678901"), "empty / 32 chars refused");
    CHECK(!bb::SetNameOk("a b") && !bb::SetNameOk("a/b") && !bb::SetNameOk("a.wav") && !bb::SetNameOk("..") && !bb::SetNameOk("\xc3\xa9") && !bb::SetNameOk("a\"b"), "space, slash, dot, unicode, quote refused");

    CHECK(bb::IsSetEntry("sets/amen.wav") && bb::IsSetEntry("SETS/AMEN.WAV") && !bb::IsSetEntry("amen.wav") && !bb::IsSetEntry("sets/amen.json") && !bb::IsSetEntry("sets/.wav") && !bb::IsSetEntry(nullptr), "IsSetEntry");
    char n[32];
    CHECK(bb::SetStem("sets/amen.wav", n, sizeof n) && !strcmp(n, "amen"), "stem: %s", n);
    CHECK(bb::SetStem("SETS/Amen_170.WAV", n, sizeof n) && !strcmp(n, "Amen_170"), "stem keeps the card's case: %s", n);
    CHECK(!bb::SetStem("sets/a b.wav", n, sizeof n) && n[0] == 0, "stem of a foreign file refused");
    CHECK(!bb::SetStem("sets/01234567890123456789012345678901.wav", n, sizeof n), "stem of 32 chars refused");
    char e[64];
    CHECK(bb::SetEntry("amen", e, sizeof e) && !strcmp(e, "sets/amen.wav"), "entry: %s", e);
    CHECK(!bb::SetEntry("a b", e, sizeof e) && e[0] == 0, "entry of a bad name refused");
    CHECK(!bb::SetEntry("amen", e, 10), "entry that does not fit refused");
    char l[64];
    bb::SetLabel("sets/amen.wav", l, sizeof l);   CHECK(!strcmp(l, "amen [set]"), "label: %s", l);
    bb::SetLabel("AMEN_170.WAV", l, sizeof l);    CHECK(!strcmp(l, "AMEN_170.WAV"), "loose file label verbatim: %s", l);
    bb::SetLabel("sets/a b.wav", l, sizeof l);    CHECK(!strcmp(l, "sets/a b.wav"), "foreign file in sets/ shown as is: %s", l);
    bb::SetLabel("sets/amen.wav", l, 8);          CHECK(!strcmp(l, "amen [s"), "label truncates cleanly: %s", l);
    char w[64], j[64];
    CHECK(bb::SetPaths("amen", w, j, sizeof w) && !strcmp(w, "0:/sets/amen.wav") && !strcmp(j, "0:/sets/amen.json"), "paths: %s %s", w, j);
    CHECK(!bb::SetPaths("amen", w, j, 17) && !bb::SetPaths("a/b", w, j, sizeof w), "paths refuse a short buffer and a bad name");
    CHECK(bb::SetLockPath("amen", w, sizeof w) && !strcmp(w, "/sets/amen.wav"), "lock path: %s", w);
    CHECK(bb::PresetPath(0, w, sizeof w) && !strcmp(w, "0:/presets/0.json") && bb::PresetPath(15, j, sizeof j) && !strcmp(j, "0:/presets/15.json"), "preset paths: %s %s", w, j);
    CHECK(bb::PresetPath(123, w, sizeof w) && !strcmp(w, "0:/presets/123.json") && !bb::PresetPath(-1, w, sizeof w) && !bb::PresetPath(0, w, 12), "preset path edges");

    static char files[4][128] = {"AMEN_170.WAV", "apache.wav", "sets/amen.wav", "sets/Kit_2.wav"};
    CHECK(bb::FindEntry(&files[0][0], 128, 4, "sets/amen.wav") == 2 && bb::FindEntry(&files[0][0], 128, 4, "SETS/KIT_2.WAV") == 3, "FindEntry case-insensitive");
    CHECK(bb::FindEntry(&files[0][0], 128, 4, "amen.wav") == -1 && bb::FindEntry(&files[0][0], 128, 4, "") == -1 && bb::FindEntry(&files[0][0], 128, 0, "apache.wav") == -1, "FindEntry misses");
}

// A 1 kHz burst into [start, start + len) of a plane.
static void Tone(float* b, int start, int len, float f, float g)
{
    for(int i = 0; i < len; i++) b[start + i] = g * sinf(6.2831853f * f * (float)i / 48000.f);
}

static void TestRoundTrip()
{
    SECTION("engine → set → engine");
    const int cap = 48000 * 8;
    std::vector<float> A(cap, 0.f), B(cap, 0.f);
    const int len = bb::MakeTestBreak(A.data(), 48000 * 4, 48000.f);
    CHECK(len > 48000, "test break %d samples", len);

    // the source engine: a 32-step grid with hits on every fourth slice, classes cycling,
    // salience rising, a swung grid — then two takes into keys 5 and 20, and a loop
    int     onsets[8]; int no = 0;
    uint8_t cls[64];
    for(int i = 0; i < 32; i++) cls[i] = (uint8_t)(i % 4);
    for(int i = 0; i < 32; i += 4) onsets[no++] = (int)((long long)len * i / 32) + 10;
    bb::Engine e1; e1.Init(77);
    for(int i = 0; i < 32; i++) e1.sliceSal[i] = (float)i / 31.f;
    e1.swing = 0.6f;
    e1.SetMaterial(A.data(), len, 48000.f, onsets, no, cls, 32, nullptr, cap);
    CHECK(e1.nSlices == 32 && e1.swung && e1.freePos == len, "source grid: 32 swung slices, freePos = len");
    Tone(A.data(), e1.freePos + 300, 12000, 1000.f, 0.8f);
    const int t5 = e1.CommitSlice(5, e1.freePos + 300, 12000, 1, 0.9f, 0.10f, true);
    Tone(A.data(), e1.freePos, 6000, 3000.f, 0.5f);
    const int t20 = e1.CommitSlice(20, e1.freePos, 6000, 2, 0.4f, 0.20f, false);
    CHECK(t5 == 32 && t20 == 33 && e1.nTable == 34 && e1.KeySlice(5) == 32 && e1.KeySlice(20) == 33, "two takes committed");
    e1.looper.Add(0.25, 5, -1, bb::Looper::kHeld, 1.f);
    e1.looper.Add(0.50, -1, 7, bb::Looper::kRev | bb::Looper::kHalf, 0.7f);
    e1.looper.Add(0.75, 5, -1, bb::Looper::kRelease, 0.f);
    e1.looper.Seal();
    bb::Looper snap; snap.Adopt(e1.looper);
    CHECK(snap.n == 3 && snap.ev[1].slice == 7 && snap.ev[1].key == -1 && snap.gen == e1.looper.gen, "looper snapshot");

    // → SetInfo → JSON → SetInfo
    static bb::SetInfo s, t;
    bb::SetFromEngine(e1, snap, s, false, 48000);
    strcpy(s.name, "kit_1"); s.bars = 2; s.bpm = 120.f; s.nOptions = 22;
    for(int i = 0; i < 22; i++) s.options[i] = (uint8_t)(i * 7);
    CHECK(s.nSlices == 34 && s.material == len && s.len == e1.freePos && fabsf(s.swing - 0.6f) < 1e-6f && !s.stereo, "SetFromEngine header: %d entries, material %d, len %d", s.nSlices, s.material, s.len);
    CHECK(s.slices[0].key == -1 && s.slices[31].key == -1 && s.slices[32].key == 5 && s.slices[33].key == 20, "keys: grid -1, recorded their key");
    CHECK(s.slices[32].start == len + 300 && s.slices[32].len == 12000 && s.slices[32].cls == 1 && fabsf(s.slices[32].sal - 0.9f) < 1e-6f && s.slices[32].hit && !s.slices[33].hit, "recorded entry metadata");
    CHECK(s.slices[1].start == e1.slices[1].start && s.slices[1].len == e1.slices[1].len, "swung grid boundaries carried");
    int hits = 0; for(int i = 0; i < 32; i++) if(s.slices[i].hit) hits++;
    CHECK(hits == 8 && s.slices[0].hit && !s.slices[1].hit && s.slices[4].hit, "grid hits carried (%d)", hits);
    CHECK(s.nLooper == 3 && s.looper[1].key == -1 && s.looper[1].slice == 7 && s.looper[1].style == (bb::Looper::kRev | bb::Looper::kHalf) && s.looper[2].style == bb::Looper::kRelease, "looper events carried");
    static char json[bb::kSetJsonMax];
    size_t n = 0;
    CHECK(bb::SetWriter::Json(s, json, sizeof json, &n) && n > 0, "json %zu bytes", n);
    CHECK(strstr(json, "\"material\":") && strstr(json, "\"swing\":0.6") && strstr(json, "\"slice\":7") && strstr(json, "\"key\":-1"), "json carries material, swing, slice, -1 keys");
    CHECK(bb::SetReader::Json(json, n, t), "parse");
    CHECK(t.nSlices == 34 && t.material == len && t.len == s.len && fabsf(t.swing - 0.6f) < 1e-6f && t.nLooper == 3 && t.nOptions == 22 && t.options[21] == 147, "parsed header");

    // → plan → the second engine
    static bb::SetPlan p;
    CHECK(bb::PlanSet(t, p), "PlanSet");
    CHECK(p.nGrid == 32 && p.material == len && p.len == s.len && p.nRec == 2 && p.rec[0] == 32 && p.rec[1] == 33 && p.nOnsets == 8 && fabsf(p.swing - 0.6f) < 1e-6f, "plan: %d grid, %d rec, %d onsets", p.nGrid, p.nRec, p.nOnsets);
    CHECK(p.onsets[0] == 0 && p.onsets[1] == e1.slices[4].start && p.cls[3] == 3 && fabsf(p.sal[31] - 1.f) < 1e-6f, "plan onsets at hit slice starts, classes, salience");
    memcpy(B.data(), A.data(), (size_t)s.len * sizeof(float));
    bb::Engine e2; e2.Init(77);
    for(int i = 0; i < p.nGrid; i++) e2.sliceSal[i] = p.sal[i];
    e2.swing = p.swing;
    e2.SetMaterial(B.data(), p.material, 48000.f, p.onsets, p.nOnsets, p.cls, p.nGrid, nullptr, cap);
    int densCalls = 0;
    const int committed = bb::CommitSetRecordings(e2, t, p, [&](int start, int lenR) { densCalls++; return start == len + 300 && lenR == 12000 ? 0.10f : 0.20f; });
    CHECK(committed == 2 && densCalls == 2 && e2.nTable == 34 && e2.freePos == e1.freePos, "recordings committed: freePos %d vs %d", e2.freePos, e1.freePos);
    bool same = e2.nSlices == e1.nSlices && e2.swung == e1.swung;
    for(int i = 0; i < e1.nTable; i++)
        same = same && e2.slices[i].start == e1.slices[i].start && e2.slices[i].len == e1.slices[i].len
               && e2.stepClass[i] == e1.stepClass[i] && e2.stepHit[i] == e1.stepHit[i] && fabsf(e2.sliceSal[i] - e1.sliceSal[i]) < 1e-6f
               && fabsf(e2.sliceDens[i] - e1.sliceDens[i]) < 1e-6f;
    CHECK(same, "table identical: slices, classes, hits, salience, density");
    bool keys = true;
    for(int k = 0; k < bb::Engine::kKeys; k++) keys = keys && e2.KeySlice(k) == e1.KeySlice(k);
    CHECK(keys && e2.tableKey[32] == 5 && e2.tableKey[33] == 20 && e2.RecordedEntry(5) == 32 && e2.RecordedEntry(20) == 33, "key table identical");
    bb::Looper l2;
    CHECK(bb::LooperFromSet(t, l2) == 3, "looper rebuilt");
    e2.looper.Adopt(l2);
    bool lsame = e2.looper.n == 3;
    for(int i = 0; i < 3 && lsame; i++)
        lsame = fabs(e2.looper.ev[i].phase - e1.looper.ev[i].phase) < 1e-6 && e2.looper.ev[i].key == e1.looper.ev[i].key
                && e2.looper.ev[i].slice == e1.looper.ev[i].slice && e2.looper.ev[i].style == e1.looper.ev[i].style && fabsf(e2.looper.ev[i].vel - e1.looper.ev[i].vel) < 1e-6f;
    CHECK(lsame && e2.looper.ev[0].gen == e2.looper.ev[2].gen && e2.looper.gen == e2.looper.ev[0].gen + 1, "looper events identical, sealed as one generation");

    // the proof: both engines render the same bits — the sequencer over the grid,
    // then a held call on a recorded key (reads the take), then a keyless call
    float oa[48], ob[48];
    bool bits = true;
    for(int blk = 0; blk < 40 && bits; blk++)
    {
        if(blk == 10) { e1.CallSlice(e1.KeySlice(5), 1.f, false, false, false, 1.f, true, 5); e2.CallSlice(e2.KeySlice(5), 1.f, false, false, false, 1.f, true, 5); }
        if(blk == 25) { e1.ReleaseKey(5); e2.ReleaseKey(5); }
        if(blk == 30) { e1.CallSlice(33, 1.f, true, false, false, 1.f, false, -1); e2.CallSlice(33, 1.f, true, false, false, 1.f, false, -1); }
        e1.Process(oa, 48); e2.Process(ob, 48);
        bits = memcmp(oa, ob, sizeof oa) == 0;
    }
    CHECK(bits, "both engines render identical audio through the grid, a held take and a one-shot take");

    // a set with no recordings and no looper is a plain grid
    bb::Engine e3; e3.Init(1);
    e3.SetMaterial(A.data(), len, 48000.f, onsets, no, cls, 32, nullptr, cap);
    bb::Looper empty;
    bb::SetFromEngine(e3, empty, s, true, 48000);
    CHECK(s.nSlices == 32 && s.nLooper == 0 && s.stereo && s.len == len && s.material == len, "grid-only set");
    CHECK(bb::PlanSet(s, p) && p.nRec == 0 && p.nGrid == 32, "grid-only plan");
    CHECK(bb::LooperFromSet(s, l2) == 0 && l2.n == 0, "empty looper");
}

static void TestRefusals()
{
    SECTION("PlanSet refusals, MergeOptions, Adopt");
    static bb::SetInfo s;
    static bb::SetPlan p;
    auto grid = [&](int nGrid, int material, int len) {
        s = bb::SetInfo(); s.len = len; s.material = material; s.nSlices = nGrid;
        const int m = material > 0 ? material : len;
        for(int i = 0; i < nGrid; i++) { s.slices[i] = bb::SetSlice(); s.slices[i].start = m * i / nGrid; s.slices[i].len = m / nGrid; s.slices[i].hit = (i & 1) == 0; }
    };
    auto rec = [&](int key, int start, int len) { bb::SetSlice& r = s.slices[s.nSlices++]; r = bb::SetSlice(); r.key = (int8_t)key; r.start = start; r.len = len; };
    grid(16, 48000, 96000); rec(3, 48000, 4800); rec(9, 60000, 4800);
    CHECK(bb::PlanSet(s, p) && p.nGrid == 16 && p.nRec == 2 && p.nOnsets == 8, "baseline accepted");
    grid(16, 48000, 96000); rec(3, 48000, 4800); rec(3, 60000, 4800);
    CHECK(!bb::PlanSet(s, p) && p.nGrid == 0, "duplicate key refused, plan zeroed");
    grid(16, 48000, 96000); rec(24, 48000, 4800);
    CHECK(!bb::PlanSet(s, p), "key 24 refused");
    grid(16, 48000, 96000); rec(3, 47000, 4800);
    CHECK(!bb::PlanSet(s, p), "recording inside the material refused");
    grid(16, 48000, 96000); rec(3, 95000, 4800);
    CHECK(!bb::PlanSet(s, p), "recording past the file refused");
    grid(16, 48000, 96000); rec(3, 48000, 400);
    CHECK(!bb::PlanSet(s, p), "recording under kRecMin refused");
    grid(16, 48000, 96000); rec(3, 48000, 4800); s.slices[s.nSlices] = bb::SetSlice(); s.slices[s.nSlices].start = 1000; s.nSlices++;
    CHECK(!bb::PlanSet(s, p), "a grid entry after a recorded one refused");
    grid(16, 48000, 96000); s.slices[5].start = s.slices[4].start;
    CHECK(!bb::PlanSet(s, p), "grid out of order refused");
    grid(16, 48000, 96000); s.slices[15].start = 48000;
    CHECK(!bb::PlanSet(s, p), "grid entry at the material's end refused");
    grid(0, 48000, 96000);
    CHECK(!bb::PlanSet(s, p), "no grid refused");
    grid(16, 48000, 96000); rec(3, 48000, 4800);
    CHECK(!bb::PlanSet(s, p, 8), "more grid entries than the engine's max refused");
    grid(16, 0, 96000);
    CHECK(bb::PlanSet(s, p) && p.material == 96000, "material 0 = the whole file");
    grid(16, 48000, 96000); s.swing = 7.f;
    CHECK(bb::PlanSet(s, p) && p.swing == 0.5f, "absurd swing → straight");
    grid(16, 48000, 0);
    CHECK(!bb::PlanSet(s, p), "empty file refused");
    // 32 recorded keys (24 possible) — every key once is the most a set can carry
    grid(64, 480000, 960000);
    for(int k = 0; k < 24; k++) rec(k, 480000 + k * 4800, 4800);
    CHECK(bb::PlanSet(s, p) && p.nRec == 24 && p.nGrid == 64, "64 grid + 24 keys accepted");

    // MergeOptions: the set's bytes, then the live ones
    s = bb::SetInfo(); s.nOptions = 3; s.options[0] = 9; s.options[1] = 8; s.options[2] = 7;
    uint8_t live[6] = {1, 2, 3, 4, 5, 6}, out[6];
    bb::MergeOptions(s, live, out, 6);
    CHECK(out[0] == 9 && out[1] == 8 && out[2] == 7 && out[3] == 4 && out[5] == 6, "merge: set first, live past its count");
    bb::MergeOptions(s, live, live, 6);
    CHECK(live[0] == 9 && live[3] == 4, "merge in place");
    s.nOptions = 0; bb::MergeOptions(s, live, out, 6);
    CHECK(out[0] == 9 && out[5] == 6, "no options → live untouched");

    // Adopt: a full list, generation and dirty carried, window and held mask reset
    bb::Looper a, b;
    for(int i = 0; i < bb::Looper::kMax; i++) a.Add((double)i / bb::Looper::kMax, i % 24, -1, bb::Looper::kHeld, 1.f);
    a.heldMask = 0xFFFu; a.frontier = 0.3;
    CHECK(a.n == bb::Looper::kMax && !a.Add(0.5, 0, -1, 0, 1.f), "source full");
    b.quant = 2; b.divisions = 32;
    b.Adopt(a);
    CHECK(b.n == bb::Looper::kMax && b.ev[255].key == 255 % 24 && b.gen == a.gen && b.dirty == a.dirty && b.heldMask == 0 && b.frontier < 0. && b.quant == 2 && b.divisions == 32, "adopt: events, gen, dirty; window/held reset; quant/divisions kept");
    b.Adopt(bb::Looper());
    CHECK(b.n == 0, "adopt an empty looper");

    // LooperFromSet drops what cannot fire and seals what it added
    s = bb::SetInfo(); s.nLooper = 4;
    s.looper[0] = {0.1f, 3, bb::Looper::kHeld, 1.f, -1};
    s.looper[1] = {1.7f, -1, bb::Looper::kRev, 1.f, 40};      // wraps to 0.7
    s.looper[2] = {0.2f, 24, 0, 1.f, -1};                     // key 24: dropped
    s.looper[3] = {NAN, 0, 0, 1.f, -1};                       // NaN phase: dropped
    CHECK(bb::LooperFromSet(s, b) == 2 && b.n == 2 && fabs(b.ev[1].phase - 0.7) < 1e-6 && b.ev[1].slice == 40 && b.ev[0].key == 3 && !b.dirty, "LooperFromSet: kept 2, wrapped, sealed");
}

int main()
{
    TestNaming();
    TestRoundTrip();
    TestRefusals();
    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
