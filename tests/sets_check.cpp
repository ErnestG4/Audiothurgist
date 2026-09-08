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
#include <string>
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
    strcpy(s.name, "kit_1"); s.bars = 2; s.bpm = 120.f; s.nOptions = bb::kSetOptionsToday;   /* EDIT-PERSIST: 24 (OPT8) */
    for(int i = 0; i < bb::kSetOptionsToday; i++) s.options[i] = (uint8_t)(i * 7);
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
    CHECK(strstr(json, "\"grid\":32") && strstr(json, "\"keys\":[") && strstr(json, "\"flags\":0") && s.nGrid == 32 && s.hasKeys && s.keys[5] == 32 && s.keys[20] == 33 && s.keys[0] == 0, "json carries grid, keys, flags (EDIT-PERSIST)");
    CHECK(bb::SetReader::Json(json, n, t), "parse");
    CHECK(t.nSlices == 34 && t.material == len && t.len == s.len && fabsf(t.swing - 0.6f) < 1e-6f && t.nLooper == 3 && t.nOptions == bb::kSetOptionsToday && t.options[21] == 147 && t.options[23] == 161, "parsed header");

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
    /* EDIT-PERSIST: a marker may lie anywhere in [0, len) — inside the break too — keyed or keyless, in any order */
    grid(16, 48000, 96000); rec(3, 47000, 4800);
    CHECK(bb::PlanSet(s, p) && p.nGrid == 16 && p.nRec == 1 && p.rec[0] == 16, "a keyed marker inside the material accepted");
    grid(16, 48000, 96000); rec(-1, 1000, 4800); rec(3, 60000, 4800); rec(-1, 50000, 4800); rec(9, 2000, 4800); s.nGrid = 16;
    CHECK(bb::PlanSet(s, p) && p.nGrid == 16 && p.nRec == 4, "keyless and keyed markers, inside and past the break, in any order (with the explicit grid count every new file carries)");
    s.nGrid = 0;
    CHECK(!bb::PlanSet(s, p), "…without it a keyless in-break marker is read as a grid entry and the grid is out of order: refused (old files have no such markers)");
    grid(16, 48000, 96000); rec(3, 95000, 4800);
    CHECK(!bb::PlanSet(s, p), "marker past the file refused");
    grid(16, 48000, 96000); rec(3, 48000, 400);
    CHECK(!bb::PlanSet(s, p), "marker under kRecMin refused");
    grid(16, 48000, 96000); rec(-1, 48000, 400);
    CHECK(!bb::PlanSet(s, p), "keyless marker under kRecMin refused");
    grid(16, 48000, 96000); rec(3, 48000, 4800); s.slices[s.nSlices] = bb::SetSlice(); s.slices[s.nSlices].start = 1000; s.nSlices++;
    CHECK(!bb::PlanSet(s, p), "a zero-length entry after a recorded one refused");
    grid(16, 48000, 96000); s.slices[5].start = s.slices[4].start;
    CHECK(!bb::PlanSet(s, p), "grid out of order refused");
    grid(16, 48000, 96000); s.slices[5].start = s.slices[4].start + 63;
    CHECK(!bb::PlanSet(s, p), "a 63-sample grid slice refused");
    grid(16, 48000, 96000); s.slices[5].start = s.slices[4].start + 64;
    CHECK(bb::PlanSet(s, p) && p.starts[5] == s.slices[4].start + 64, "a 64-sample grid slice accepted (starts carried)");
    grid(16, 48000, 96000); s.slices[15].start = 48000 - 63;
    CHECK(!bb::PlanSet(s, p), "63 samples before the material's end refused");
    grid(16, 48000, 96000); s.slices[0].start = 10;
    CHECK(!bb::PlanSet(s, p), "grid not starting at 0 refused");
    grid(16, 48000, 96000); s.nGrid = 16; s.slices[15].start = 48000;
    CHECK(!bb::PlanSet(s, p), "grid entry at the material's end refused (explicit count)");
    grid(16, 48000, 96000); s.slices[15].start = 48000; s.slices[15].len = 4800;
    CHECK(bb::PlanSet(s, p) && p.nGrid == 15 && p.nRec == 1, "…inferred: a keyless entry at the material is a marker (the ClearKey case)");
    grid(16, 48000, 96000); s.nGrid = 17;
    CHECK(!bb::PlanSet(s, p), "grid count past the entries refused");
    grid(16, 48000, 96000); s.nGrid = 8;
    CHECK(bb::PlanSet(s, p) && p.nGrid == 8 && p.nRec == 8, "an explicit smaller grid count: the rest are markers");
    grid(16, 48000, 96000); s.hasKeys = true; s.keys[3] = 16;
    CHECK(!bb::PlanSet(s, p), "a key onto an entry the file does not hold refused");
    grid(16, 48000, 96000); rec(-1, 48000, 4800); s.hasKeys = true; s.keys[3] = 16; s.keys[4] = 2; s.keys[5] = -1;
    CHECK(bb::PlanSet(s, p) && p.hasKeys && p.keys[3] == 16 && p.keys[4] == 2 && p.keys[5] == -1 && p.keys[23] == -1, "keys carried as file indices");
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
    for(int k = 0; k < 8; k++) rec(-1, 1000 + k * 4800, 4800);
    CHECK(bb::PlanSet(s, p) && p.nRec == 32, "…plus 8 keyless markers = the whole table");
    rec(-1, 1000, 4800);
    CHECK(!bb::PlanSet(s, p), "one more than kMaxTable refused");

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


/* ── EDIT-PERSIST (docs/packages/slice-edit-persist.md) ─────────────────── */
static bb::EditReq Req(bb::EditOp op) { bb::EditReq r; r.op = op; return r; }

// The source engine every EDIT-PERSIST section starts from: the 4 s test
// break on a straight 32-step grid with hits every fourth slice, classes
// cycling, salience rising — and a 1 kHz recording on key 20 past it.
static int SourceEngine(bb::Engine& e, std::vector<float>& A, int cap, int* onsets, int& no, uint8_t* cls)
{
    const int len = bb::MakeTestBreak(A.data(), 48000 * 4, 48000.f);
    no = 0;
    for(int i = 0; i < 32; i++) cls[i] = (uint8_t)(i % 4);
    for(int i = 0; i < 32; i += 4) onsets[no++] = (int)((long long)len * i / 32) + 10;
    e.Init(77);
    for(int i = 0; i < 32; i++) e.sliceSal[i] = (float)i / 31.f;
    e.SetMaterial(A.data(), len, 48000.f, onsets, no, cls, 32, nullptr, cap);
    Tone(A.data(), e.freePos, 6000, 1000.f, 0.5f);
    e.CommitSlice(20, e.freePos, 6000, 2, 0.4f, 0.20f, false);   // 32
    return len;
}
// The loader's sequence on a fresh engine over a copy of the planes:
// SetMaterial → SetGridBoundaries (the audio thread's step) →
// CommitSetRecordings → SetKeyTable → the looper. `dens` looks the entry's
// density up in the source engine, as AnalyseRegion would recompute it.
static void LoadInto(bb::Engine& e2, std::vector<float>& B, int cap, const bb::SetInfo& t, const bb::SetPlan& p, const bb::Engine& src,
                     const int* onsets = nullptr, int no = 0, bool* gridOk = nullptr, bool* keysOk = nullptr, int8_t* liveOut = nullptr)
{
    e2.Init(77);
    for(int i = 0; i < p.nGrid; i++) e2.sliceSal[i] = p.sal[i];
    e2.swing = p.swing;
    e2.SetMaterial(B.data(), p.material, 48000.f, onsets ? onsets : p.onsets, onsets ? no : p.nOnsets, p.cls, p.nGrid, nullptr, cap);
    const bool g = e2.SetGridBoundaries(p.starts, p.nGrid, p.gridFlags);
    if(gridOk) *gridOk = g;
    int8_t live[24];
    bb::CommitSetRecordings(e2, t, p, [&](int start, int lenR) {
        for(int i = src.nSlices; i < src.nTable; i++) if(src.slices[i].start == start && src.slices[i].len == lenR) return src.sliceDens[i];
        return 0.f; }, live);
    if(liveOut) memcpy(liveOut, live, sizeof live);
    const bool k = p.hasKeys ? e2.SetKeyTable(live) : true;
    if(keysOk) *keysOk = k;
    bb::Looper l2; bb::LooperFromSet(t, l2); e2.looper.Adopt(l2);
}
// Both engines render the same bits through the sequencer, five keys and a
// keyless call (idxA on a, idxB on b).
static bool SameAudio(bb::Engine& a, bb::Engine& b, int idxA, int idxB)
{
    float oa[48], ob[48];
    for(int blk = 0; blk < 60; blk++)
    {
        auto call = [&](int key) { a.CallSlice(a.KeySlice(key), 1.f, false, false, false, 1.f, false, key); b.CallSlice(b.KeySlice(key), 1.f, false, false, false, 1.f, false, key); };
        if(blk == 5) call(5);
        if(blk == 12) call(6);
        if(blk == 20) call(7);
        if(blk == 28) call(8);
        if(blk == 36) call(20);
        if(blk == 44 && idxA >= 0) { a.CallSlice(idxA, 1.f, true, false, false, 1.f, false, -1); b.CallSlice(idxB, 1.f, true, false, false, 1.f, false, -1); }
        a.Process(oa, 48); b.Process(ob, 48);
        if(memcmp(oa, ob, sizeof oa) != 0) return false;
    }
    return true;
}
// Strip the EDIT-PERSIST keys from a set's JSON text: what an older firmware
// wrote (no "grid", no "keys", no per-entry "flags").
static std::string OldFormat(const char* json)
{
    std::string o = json;
    auto strip = [&](const char* key, char endc) {
        for(;;)
        {
            const size_t a = o.find(key);
            if(a == std::string::npos) return;
            size_t b = o.find(endc, a);
            if(b == std::string::npos) return;
            b++;
            if(b < o.size() && o[b] == ',') b++;
            o.erase(a, b - a);
        }
    };
    strip("\"grid\":", ',');
    strip("\"keys\":[", ']');
    strip("\"flags\":", ',');
    return o;
}

static void TestEditRoundTrip()
{
    SECTION("EDIT-PERSIST: engine → set → engine with edits");
    const int cap = 48000 * 8;
    std::vector<float> A(cap, 0.f), B(cap, 0.f);
    int onsets[8], no = 0; uint8_t cls[64];
    bb::Engine e1;
    const int len = SourceEngine(e1, A, cap, onsets, no, cls);
    CHECK(e1.nTable == 33 && e1.KeySlice(20) == 32, "source: 32 grid + a recording on key 20");
    // edits, as the audio thread applies them
    bb::EditReq r;
    const int s5 = e1.slices[5].start, d5 = e1.slices[4].len / 2;   // half a step to the left: slice 4 keeps half
    r = Req(bb::EditOp::SetBoundary); r.i = 5; r.start = s5 - d5;
    CHECK(e1.ApplyEdit(r) == bb::EditStatus::Ok && e1.slices[5].start == s5 - d5 && (e1.TableFlags(5) & bb::Engine::kFlagOrange), "boundary 5 moved −%d, orange", d5);
    int tMark = -1;
    r = Req(bb::EditOp::AddEntry); r.key = 5; r.start = e1.slices[7].start; r.len = 6000; r.flags = bb::EditReq::kOrange | bb::EditReq::kMeta; r.meta = {1, 0.9f, 0.10f, true};
    CHECK(e1.ApplyEdit(r, &tMark) == bb::EditStatus::Ok && tMark == 33 && e1.KeySlice(5) == 33, "an in-break marker over slice 7 on key 5 → 33");
    r = Req(bb::EditOp::SetKey); r.key = 6; r.t = 33;
    CHECK(e1.ApplyEdit(r) == bb::EditStatus::Ok && e1.KeySlice(6) == 33, "key 6 on the same marker");
    int tDead = -1;
    r = Req(bb::EditOp::AddEntry); r.key = -1; r.start = e1.slices[9].start; r.len = 5000; r.flags = bb::EditReq::kOrange;
    CHECK(e1.ApplyEdit(r, &tDead) == bb::EditStatus::Ok && tDead == 34, "a keyless marker → 34");
    int tLast = -1;
    r = Req(bb::EditOp::AddEntry); r.key = 7; r.start = e1.slices[11].start; r.len = 5000; r.flags = bb::EditReq::kOrange | bb::EditReq::kMeta; r.meta = {2, 0.5f, 0.15f, false};
    CHECK(e1.ApplyEdit(r, &tLast) == bb::EditStatus::Ok && tLast == 35 && e1.KeySlice(7) == 35, "a marker on key 7 → 35");
    r = Req(bb::EditOp::SetKey); r.key = 8; r.t = 2;
    CHECK(e1.ApplyEdit(r) == bb::EditStatus::Ok && e1.KeySlice(8) == 2, "key 8 on grid slice 2");
    // the loop: keyless on 33, keyless on 34 (dies with it), keyless on 35, keyed on 5, keyless on the recording
    e1.looper.Add(0.10, -1, 33, 0, 1.f);
    e1.looper.Add(0.30, -1, 34, 0, 1.f);
    e1.looper.Add(0.50, -1, 35, bb::Looper::kRev, 0.8f);
    e1.looper.Add(0.70, 5, -1, 0, 1.f);
    e1.looper.Add(0.90, -1, 32, 0, 1.f);
    e1.looper.Seal();
    r = Req(bb::EditOp::Tombstone); r.t = 34;
    CHECK(e1.ApplyEdit(r) == bb::EditStatus::Ok && e1.IsTomb(34) && e1.looper.n == 4 && e1.nTable == 36, "34 tombstoned: its loop event dropped, the table keeps its count");
    bb::Looper snap; snap.Adopt(e1.looper);

    static bb::SetInfo s, t;
    bb::SetFromEngine(e1, snap, s, false, 48000);
    strcpy(s.name, "edited"); s.bars = 2; s.bpm = 120.f; s.nOptions = bb::kSetOptionsToday;
    CHECK(s.nSlices == 35 && s.nGrid == 32 && s.len == e1.freePos && s.material == len, "compacted: 35 entries (the tombstone skipped), grid 32");
    CHECK(s.slices[5].start == s5 - d5 && (s.slices[5].flags & bb::kSetFlagOrange) && !(s.slices[4].flags & bb::kSetFlagOrange), "the moved boundary and its orange bit");
    CHECK(s.slices[32].key == 20 && s.slices[32].flags == 0 && s.slices[33].key == -1 && s.slices[33].start == e1.slices[7].start && (s.slices[33].flags & 1)
          && s.slices[34].key == -1 && s.slices[34].start == e1.slices[11].start && (s.slices[34].flags & 1),
          "entries: the recording (key 20, white), the marker over 7 (orange, no owner), the marker over 11 re-indexed 35 → 34");
    CHECK(s.hasKeys && s.keys[5] == 33 && s.keys[6] == 33 && s.keys[7] == 34 && s.keys[8] == 2 && s.keys[20] == 32 && s.keys[0] == 0 && s.keys[23] == e1.SpanSlice(23), "keys re-indexed to the file's order");
    CHECK(s.nLooper == 4 && s.looper[0].slice == 33 && s.looper[1].slice == 34 && s.looper[2].key == 5 && s.looper[3].slice == 32, "looper events re-indexed (35 → 34)");
    static char json[bb::kSetJsonMax]; size_t n = 0;
    CHECK(bb::SetWriter::Json(s, json, sizeof json, &n) && strstr(json, "\"grid\":32") && strstr(json, "\"keys\":[0,") && strstr(json, "\"flags\":1"), "json carries grid, keys, flags");
    CHECK(bb::SetReader::Json(json, n, t) && t.nGrid == 32 && t.hasKeys && t.keys[6] == 33 && t.slices[33].flags == 1 && t.slices[32].flags == 0, "parsed back");
    static bb::SetPlan p;
    CHECK(bb::PlanSet(t, p) && p.nGrid == 32 && p.nRec == 3 && p.starts[5] == s5 - d5 && (p.gridFlags[5] & 1) && !(p.gridFlags[4] & 1) && p.hasKeys && p.keys[6] == 33, "planned: starts, flags, keys");

    memcpy(B.data(), A.data(), (size_t)s.len * sizeof(float));
    bb::Engine e2;
    bool gridOk = false, keysOk = false; int8_t live[24];
    LoadInto(e2, B, cap, t, p, e1, nullptr, 0, &gridOk, &keysOk, live);
    CHECK(gridOk && e2.slices[5].start == s5 - d5 && e2.slices[4].len == e1.slices[4].len && (e2.TableFlags(5) & 1) && !(e2.TableFlags(4) & 1), "grid starts applied at the swap, orange where the file says");
    CHECK(e2.nTable == 35 && live[5] == 33 && live[6] == 33 && live[7] == 34 && live[8] == 2 && live[20] == 32 && live[0] == 0 && live[23] == e1.SpanSlice(23), "markers committed; keys → live indices");
    CHECK(keysOk && e2.KeySlice(5) == 33 && e2.KeySlice(6) == 33 && e2.KeySlice(7) == 34 && e2.KeySlice(8) == 2 && e2.KeySlice(20) == 32, "key table applied");
    CHECK(e2.tableKey[32] == 20 && e2.tableKey[33] == -1 && e2.tableKey[34] == -1 && e2.RecordedEntry(20) == 32 && e2.RecordedEntry(5) == -1, "provenance: the recording owned by 20, the markers by nobody");
    CHECK((e2.TableFlags(33) & 1) && (e2.TableFlags(34) & 1) && !(e2.TableFlags(32) & 1) && !e2.IsTomb(34) && e2.freePos == e1.freePos, "orange restored on the markers, the recording white, freePos as saved");
    CHECK(e2.looper.n == 4 && e2.looper.ev[0].slice == 33 && e2.looper.ev[1].slice == 34 && e2.looper.ev[2].key == 5 && e2.looper.ev[3].slice == 32, "looper: the events point at the live entries");
    auto m = [](int i) { return i < 34 ? i : i - 1; };
    bool same = e2.nSlices == 32;
    for(int i = 0; i < e1.nTable; i++)
    {
        if(i == 34) continue;
        const int j = m(i);
        same = same && e2.slices[j].start == e1.slices[i].start && e2.slices[j].len == e1.slices[i].len && e2.stepClass[j] == e1.stepClass[i]
               && e2.stepHit[j] == e1.stepHit[i] && fabsf(e2.sliceSal[j] - e1.sliceSal[i]) < 1e-6f && fabsf(e2.sliceDens[j] - e1.sliceDens[i]) < 1e-6f
               && e2.TableFlags(j) == e1.TableFlags(i);
    }
    CHECK(same, "every live entry identical through the compaction map");
    bool keys = true;
    for(int k = 0; k < 24; k++) keys = keys && e2.KeySlice(k) == m(e1.KeySlice(k));
    CHECK(keys, "every key plays the same entry");
    CHECK(SameAudio(e1, e2, 35, 34), "identical audio through the edited grid, the markers, the recording and a keyless call");

    // the overlay came back with the load: a same-count re-apply keeps the moved boundary and the markers
    e2.SetMaterial(B.data(), p.material, 48000.f, p.onsets, p.nOnsets, p.cls, p.nGrid, nullptr, cap);
    CHECK(e2.slices[5].start == s5 - d5 && (e2.TableFlags(5) & 1) && e2.nTable == 35 && e2.KeySlice(6) == 33 && e2.KeySlice(7) == 34, "a re-apply after the load keeps the edit, the markers and their keys");
}

static void TestClearKeyRegression()
{
    SECTION("EDIT-PERSIST: a set saved after ClearKey loads (pre-existing bug)");
    const int cap = 48000 * 8;
    std::vector<float> A(cap, 0.f), B(cap, 0.f);
    int onsets[8], no = 0; uint8_t cls[64];
    bb::Engine e1;
    const int len = SourceEngine(e1, A, cap, onsets, no, cls);
    Tone(A.data(), e1.freePos, 4800, 3000.f, 0.5f);
    const int t5 = e1.CommitSlice(5, e1.freePos, 4800, 1, 0.9f, 0.1f, true);
    CHECK(t5 == 33 && e1.KeySlice(5) == 33, "a second recording on key 5");
    e1.ClearKey(5);
    CHECK(e1.KeySlice(5) == e1.SpanSlice(5) && e1.tableKey[33] == -1 && e1.nTable == 34 && e1.RecordedEntry(5) == -1, "ClearKey: the key is back on its span slice, the entry stays keyless");
    bb::Looper empty;
    static bb::SetInfo s, t;
    bb::SetFromEngine(e1, empty, s, false, 48000);
    CHECK(s.nSlices == 34 && s.slices[33].key == -1 && s.slices[33].start >= len && s.keys[5] == e1.SpanSlice(5), "saved: a keyless entry past the material");
    static char json[bb::kSetJsonMax]; size_t n = 0;
    CHECK(bb::SetWriter::Json(s, json, sizeof json, &n) && bb::SetReader::Json(json, n, t), "json");
    static bb::SetPlan p;
    CHECK(bb::PlanSet(t, p) && p.nGrid == 32 && p.nRec == 2 && p.rec[1] == 33, "PlanSet accepts it (it refused 'everything after the first keyed entry must be keyed' before)");
    const std::string old = OldFormat(json);
    static bb::SetInfo o;
    CHECK(bb::SetReader::Json(old.data(), old.size(), o) && o.nGrid == 0 && !o.hasKeys, "the same set as the current firmware writes it: no grid, keys or flags");
    static bb::SetPlan po;
    CHECK(bb::PlanSet(o, po) && po.nGrid == 32 && po.nRec == 2 && !po.hasKeys, "…loads too: the keyless recording is a marker after the inferred grid");
    memcpy(B.data(), A.data(), (size_t)s.len * sizeof(float));
    bb::Engine e2;
    LoadInto(e2, B, cap, o, po, e1);
    CHECK(e2.nTable == 34 && e2.KeySlice(5) == e2.SpanSlice(5) && e2.RecordedEntry(5) == -1 && e2.tableKey[33] == -1 && e2.KeySlice(20) == 32 && e2.slices[33].start == e1.slices[33].start,
          "loaded: key 5 on its span slice, the orphaned region present but unowned, key 20 on its recording");
    CHECK(SameAudio(e1, e2, 33, 33), "identical audio");
}

static void TestOldJson()
{
    SECTION("EDIT-PERSIST: an old JSON (no grid, keys, flags) loads exactly as the new one");
    const int cap = 48000 * 8;
    std::vector<float> A(cap, 0.f), B(cap, 0.f), C(cap, 0.f), D(cap, 0.f);
    int onsets[8], no = 0; uint8_t cls[64];
    bb::Engine e1;
    SourceEngine(e1, A, cap, onsets, no, cls);
    e1.looper.Add(0.25, 20, -1, bb::Looper::kHeld, 1.f);
    e1.looper.Add(0.50, -1, 7, 0, 0.7f);
    e1.looper.Seal();
    bb::Looper snap; snap.Adopt(e1.looper);
    static bb::SetInfo s, tn, to;
    bb::SetFromEngine(e1, snap, s, false, 48000);
    static char json[bb::kSetJsonMax]; size_t n = 0;
    CHECK(bb::SetWriter::Json(s, json, sizeof json, &n), "json");
    const std::string old = OldFormat(json);
    CHECK(old.find("\"grid\"") == std::string::npos && old.find("\"keys\"") == std::string::npos && old.find("\"flags\"") == std::string::npos && old.find("\"slice\":7") != std::string::npos, "stripped to the old shape");
    CHECK(bb::SetReader::Json(json, n, tn) && bb::SetReader::Json(old.data(), old.size(), to), "both parse");
    CHECK(tn.nGrid == 32 && tn.hasKeys && to.nGrid == 0 && !to.hasKeys && to.keys[5] == -1 && to.slices[3].flags == 0 && to.nSlices == 33 && to.nLooper == 2, "old: grid inferred later, keys absent, flags 0");
    static bb::SetPlan pn, po;
    CHECK(bb::PlanSet(tn, pn) && bb::PlanSet(to, po) && po.nGrid == 32 && po.nRec == 1 && !po.hasKeys && !memcmp(pn.starts, po.starts, sizeof(int) * 32), "both plans: the same grid starts");
    memcpy(B.data(), A.data(), (size_t)s.len * sizeof(float));
    memcpy(C.data(), A.data(), (size_t)s.len * sizeof(float));
    bb::Engine e2, e3;
    bool g2 = false, g3 = false;
    LoadInto(e2, B, cap, tn, pn, e1, nullptr, 0, &g2);
    LoadInto(e3, C, cap, to, po, e1, nullptr, 0, &g3);
    CHECK(g2 && g3 && e2.ovOrange == 0 && e3.ovOrange == 0, "the computed grid applied as a no-op on both: nothing orange, no overlay");
    bool same = e2.nTable == e3.nTable && e2.nTable == 33;
    for(int i = 0; i < e2.nTable; i++) same = same && e2.slices[i].start == e3.slices[i].start && e2.slices[i].len == e3.slices[i].len && e2.TableFlags(i) == 0 && e3.TableFlags(i) == 0 && e2.stepHit[i] == e3.stepHit[i];
    for(int k = 0; k < 24; k++) same = same && e2.KeySlice(k) == e3.KeySlice(k) && e2.KeySlice(k) == e1.KeySlice(k);
    CHECK(same && e2.looper.n == 2 && e3.looper.n == 2 && e3.looper.ev[1].slice == 7, "tables, keys and loops identical (and equal to the source's)");
    CHECK(SameAudio(e2, e3, 7, 7), "identical audio: the old file and the new one");
    memcpy(D.data(), A.data(), (size_t)s.len * sizeof(float));
    bb::Engine e4;
    LoadInto(e4, D, cap, to, po, e1);
    CHECK(SameAudio(e1, e4, 7, 7), "identical audio: the source and the old file");
}

static void TestSidecar()
{
    SECTION("EDIT-PERSIST: the loose-file sidecar (the set JSON without the material)");
    const int cap = 48000 * 8;
    std::vector<float> A(cap, 0.f), B(cap, 0.f);
    int onsets[8], no = 0; uint8_t cls[64];
    bb::Engine e1;
    const int len = SourceEngine(e1, A, cap, onsets, no, cls);
    bb::EditReq r;
    const int s9 = e1.slices[9].start, d9 = e1.slices[9].len / 3;
    r = Req(bb::EditOp::SetBoundary); r.i = 9; r.start = s9 + d9;
    CHECK(e1.ApplyEdit(r) == bb::EditStatus::Ok, "boundary 9 moved");
    int tMark = -1;
    r = Req(bb::EditOp::AddEntry); r.key = 5; r.start = e1.slices[3].start; r.len = 4800; r.flags = bb::EditReq::kOrange | bb::EditReq::kMeta; r.meta = {0, 0.7f, 0.05f, true};
    CHECK(e1.ApplyEdit(r, &tMark) == bb::EditStatus::Ok && tMark == 33, "an in-break marker on key 5 → 33");
    r = Req(bb::EditOp::SetKey); r.key = 6; r.t = 32;
    CHECK(e1.ApplyEdit(r) == bb::EditStatus::Ok && e1.KeySlice(6) == 32, "key 6 on the recording");
    e1.looper.Add(0.20, -1, 32, 0, 1.f);   // keyless on the recording: not in the sidecar's world
    e1.looper.Add(0.40, -1, 33, 0, 1.f);
    e1.looper.Seal();
    bb::Looper snap; snap.Adopt(e1.looper);
    static bb::SetInfo s, t;
    bb::SetFromEngine(e1, snap, s, false, 48000, e1.bufLen);   // the sidecar: bounded by the material
    CHECK(s.len == len && s.material == len && s.nSlices == 33 && s.nGrid == 32 && s.slices[32].start == e1.slices[3].start && s.slices[32].key == -1 && (s.slices[32].flags & 1),
          "the break and its marker only: the recording past it is not in the file");
    CHECK(s.keys[5] == 32 && s.keys[6] == -1 && s.keys[20] == -1 && s.keys[0] == 0, "keys on the recording → the default");
    CHECK(s.nLooper == 1 && s.looper[0].slice == 32, "the keyless event on the recording dropped, the marker's re-indexed 33 → 32");
    static char json[bb::kSetJsonMax]; size_t n = 0;
    s.nLooper = 0; s.nOptions = 0;   // as the shell writes a sidecar: no looper, no options
    CHECK(bb::SetWriter::Json(s, json, sizeof json, &n) && bb::SetReader::Json(json, n, t) && t.len == len && t.nGrid == 32 && t.hasKeys, "sidecar json round trip");
    static bb::SetPlan p;
    CHECK(bb::PlanSet(t, p) && p.material == len && p.len == len && p.nGrid == 32 && p.nRec == 1 && p.starts[9] == s9 + d9 && p.keys[5] == 32 && p.keys[6] == -1, "planned");
    // the shell's guard: the sidecar fits only the same conditioned length and slice count
    CHECK(p.material == len && p.nGrid == e1.nSlices, "fits this material");
    memcpy(B.data(), A.data(), (size_t)len * sizeof(float));
    bb::Engine e2;
    bool gridOk = false, keysOk = false;
    LoadInto(e2, B, cap, t, p, e1, onsets, no, &gridOk, &keysOk);   // a plain load: the detector's onsets, the file's starts
    CHECK(gridOk && keysOk && e2.nTable == 33 && e2.slices[9].start == s9 + d9 && (e2.TableFlags(9) & 1) && e2.slices[32].start == e1.slices[3].start && (e2.TableFlags(32) & 1),
          "the moved boundary and the marker are back");
    CHECK(e2.KeySlice(5) == 32 && e2.KeySlice(6) == e2.SpanSlice(6) && e2.KeySlice(20) == e2.SpanSlice(20) && e2.RecordedEntry(20) == -1 && e2.freePos == len, "key 5 on the marker; 6 and 20 default (no recording to point at)");
    bool grid = true;
    for(int i = 0; i < 32; i++) grid = grid && e2.slices[i].start == e1.slices[i].start && e2.slices[i].len == e1.slices[i].len && e2.stepHit[i] == e1.stepHit[i];
    CHECK(grid, "the grid identical to the source's");
    // a sidecar made under another slice count: the markers come back, the grid keys default
    static bb::SetInfo u; u = t; u.nGrid = 16; u.nSlices = 17; u.slices[16] = t.slices[32];
    for(int i = 0; i < 16; i++) { u.slices[i].start = i * (len / 16); u.slices[i].len = len / 16; }
    for(int k = 0; k < 24; k++) u.keys[k] = -1;
    u.keys[5] = 16; u.keys[8] = 3;
    static bb::SetPlan pu;
    CHECK(bb::PlanSet(u, pu) && pu.nGrid == 16, "a 16-grid sidecar plans");
    bb::Engine e3; std::vector<float> C(A.begin(), A.begin() + len);
    C.resize(cap, 0.f);
    e3.Init(77); e3.SetMaterial(C.data(), len, 48000.f, onsets, no, cls, 32, nullptr, cap);
    CHECK(!e3.SetGridBoundaries(pu.starts, pu.nGrid, pu.gridFlags), "…its grid starts are refused on a 32-slice grid");
    int8_t live[24];
    bb::CommitSetRecordings(e3, u, pu, [](int, int) { return 0.f; }, live);
    CHECK(live[5] == 32 && live[8] == -1 && e3.SetKeyTable(live) && e3.KeySlice(5) == 32 && e3.KeySlice(8) == e3.SpanSlice(8), "…the marker and its key come back, the grid key defaults");
}

int main()
{
    TestNaming();
    TestRoundTrip();
    TestRefusals();
    TestEditRoundTrip();       /* EDIT-PERSIST */
    TestClearKeyRegression();
    TestOldJson();
    TestSidecar();
    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
