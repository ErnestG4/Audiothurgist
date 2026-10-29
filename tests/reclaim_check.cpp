// reclaim_check — RECLAIM (docs/packages/pads.md, "Held memory"): core/bb_reclaim.h — the plan, the copy into the
// other buffer, the audio thread's swap, the journal truncated and remapped, the card map beside a set — and the
// packed set save (SetFromEngine through the map, the WAV gathered from the live spans). tests/run.sh builds it
// under ASan/UBSan:
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore -Ivendor/capicola/lib
//       tests/reclaim_check.cpp -o /tmp/reclaim && /tmp/reclaim /tmp     (one line)
//
// 1. The plan: runs merge where they touch, Map is the move (identity over the break, a run's offset, a gap's next
//    kept position, past freePos by the total), Inside, the cap's conservative merge.
// 2. A compaction keeps every live key sounding the same samples: a twin engine that never reclaimed and the
//    reclaimed one render every key (one-shot, held, reversed, a Loop key) bit for bit; both planes in step; the
//    break untouched; freePos down by exactly the held total; held 0 after.
// 3. Undo across it: kReclaimFree keeps the whole history (remapped) — undo / redo all the way, bit for bit
//    against the twin; kReclaimAll drops the steps that need a reclaimed sound (the newest of them and older; the
//    redo half from the first) and keeps the rest working — a NUDGE_ALL, a split, a bulk kSnap step among them.
// 4. Voices through the swap: a held pad, a ratcheting key, a Loop key sounding across it — the output identical
//    to the twin's, block for block; a replaced pad still ringing fades (ReclaimFade) and is gone by the swap; a
//    swap after the table moved is refused, nothing touched.
// 5. The set: a packed save (the held span left out, cues where the JSON says), load → every key the same bits;
//    a save → load after a compaction; the card map following a reclaim — the autosave's JSON still describes the
//    WAV on the card (loaded back: the same bits), KitUnsaved exact before and after.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <memory>
#include <new>

#include "fatfs_shim.h"
#include "wav_loader.h"
#include "bb_engine.h"
#include "bb_card.h"
#include "bb_pads.h"
#include "bb_reclaim.h"
#include "bb_sample.h"
#include "bb_set.h"
#include "bb_sets.h"
#include "bb_wavwrite.h"

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

static std::string gOut = "/tmp";
static constexpr int kSr = 48000, kCap = 48000 * 45;
static bb::UndoJournal gJA, gJB, gJC;   // 150 KB each: static
static bb::ReclaimPlan gPlan;
static bb::CardMap gMap, gMap2;

static int16_t S16(float x) { return (int16_t)bbwav::detail::Clip(x, 32768.f, -32768, 32767); }
static std::vector<float> Wave(int n, float f, float g, float ph = 0.f)
{
    std::vector<float> v((size_t)n);
    for(int i = 0; i < n; i++) v[(size_t)i] = (float)S16(g * sinf(6.2831853f * f * (float)i / (float)kSr + ph) * (1.f - 0.5f * (float)i / (float)n)) / 32768.f;
    return v;
}
// A drop as the shell commits it, minus the wire: the floats into both planes past freePos, EditOp::Record.
static int Drop(bb::Engine& e, int key, const std::vector<float>& l, const std::vector<float>* r = nullptr, int* idx = nullptr)
{
    const int n = (int)l.size(), start = e.freePos;
    if(n > e.bufCap - start) return 100;
    memcpy(e.buf + start, l.data(), sizeof(float) * (size_t)n);
    if(e.bufR) memcpy(e.bufR + start, (r ? *r : l).data(), sizeof(float) * (size_t)n);
    bb::EditReq q; q.op = bb::EditOp::Record; q.key = key; q.start = start; q.len = n; q.flags = bb::EditReq::kMeta;
    bb::AnalyseRegion(e.buf, start, n < bb::kPadAnalyseMax ? n : bb::kPadAnalyseMax, (float)kSr, 0, q.meta.cls, q.meta.sal, q.meta.dens, q.meta.hit);
    int t = -1;
    const int st = (int)e.ApplyEdit(q, &t);
    if(idx) *idx = t;
    return st;
}
static bb::EditStatus Op(bb::Engine& e, bb::EditReq q, int* idx = nullptr) { return e.ApplyEdit(q, idx); }
static bb::EditStatus Undo(bb::Engine& e, bool redo = false) { bb::EditReq q; q.op = redo ? bb::EditOp::Redo : bb::EditOp::Undo; return e.ApplyEdit(q); }

// The shell's reclaim, minus the threads: plan, journal plan, copy (both planes) into the other buffer, swap, journal.
static bool Reclaim(bb::Engine& e, float* L, float* R, uint8_t mode, bb::CardMap* map = nullptr)
{
    if(!bb::BuildReclaimPlan(e, mode, gPlan)) return false;
    bb::ReclaimPlanJournal(e, gPlan);
    bb::ReclaimCopy(gPlan, e.buf, L);
    if(e.bufR) bb::ReclaimCopy(gPlan, e.bufR, R);
    if(!bb::ReclaimSwap(e, gPlan, L, R)) return false;
    bb::ReclaimJournal(e, gPlan);
    if(map) { gMap2.Follow(*map, gPlan); *map = gMap2; }
    return true;
}
// A twin: the same state, its own journal (copied), the same planes.
static std::unique_ptr<bb::Engine> Twin(const bb::Engine& a, bb::UndoJournal& j)
{
    std::unique_ptr<bb::Engine> b(new bb::Engine(a));
    if(a.undo) { memcpy(&j, a.undo, sizeof j); b->undo = &j; }
    return b;
}
// Every key in turn — one-shot, held, reversed (held keys released after 6 blocks) — both engines render the same bits.
static void Silence(bb::Engine& e);
static bool SameKeys(bb::Engine& a, bb::Engine& b, int perKey = 14, int* firstDiff = nullptr)
{
    float al[48], ar[48], bl[48], br[48];
    Silence(a); Silence(b);
    a.muted = b.muted = true;
    int blk = 0;
    for(int k = 0; k < bb::Engine::kKeys; k++)
        for(int style = 0; style < 3; style++)
        {
            const bool held = style == 1, rev = style == 2;
            a.CallSlice(a.KeySlice(k), 1.f, rev, false, false, 1.f, held, k);
            b.CallSlice(b.KeySlice(k), 1.f, rev, false, false, 1.f, held, k);
            for(int i = 0; i < perKey; i++, blk++)
            {
                if(held && i == 6) { a.ReleaseKey(k); b.ReleaseKey(k); }
                a.Process(al, ar, 48); b.Process(bl, br, 48);
                if(memcmp(al, bl, sizeof al) || memcmp(ar, br, sizeof ar)) { if(firstDiff) *firstDiff = blk; printf("    differ: key %d style %d block %d\n", k, style, i); return false; }
            }
        }
    return true;
}
static void Silence(bb::Engine& e) { for(int v = 0; v < bb::Engine::kVoices; v++) e.voices[v] = bb::Voice(); e.keyRatOn = 0; e.callCountdown = -1; }

/* ── 1. the plan ─────────────────────────────────────────────────────── */
static void TestPlan()
{
    SECTION("the plan: runs, Map, Inside, the cap");
    bb::ReclaimPlan& p = gPlan;
    p.nRun = 0; p.oldFree = 10000; p.bufLen = 1000;
    p.Add(0, 1000); p.Add(3000, 4000); p.Add(2000, 2500); p.Add(2500, 2600); p.Add(6000, 7000); p.Add(3500, 4200); p.Add(9000, 12000); p.Add(-5, 10);
    p.Finish();
    CHECK(p.nRun == 5 && p.a[0] == 0 && p.b[0] == 1000 && p.a[1] == 2000 && p.b[1] == 2600 && p.a[2] == 3000 && p.b[2] == 4200
          && p.a[3] == 6000 && p.b[3] == 7000 && p.a[4] == 9000 && p.b[4] == 10000, "sorted, merged where they touch or overlap, clamped to [0, freePos)");
    CHECK(p.dst[1] == 1000 && p.dst[2] == 1600 && p.dst[3] == 2800 && p.dst[4] == 3800 && p.newFree == 4800 && p.Reclaimed() == 5200, "each run packed down; freePos 10000 → 4800");
    CHECK(p.Map(0) == 0 && p.Map(999) == 999 && p.Map(2000) == 1000 && p.Map(2599) == 1599 && p.Map(3100) == 1700, "Map: identity over the break, a run's offset inside it");
    CHECK(p.Map(1000) == 1000 && p.Map(1500) == 1000 && p.Map(2600) == 1600 && p.Map(5000) == 2800 && p.Map(10000) == 4800 && p.Map(10500) == 5300, "a gap maps to the next kept position; past freePos by the total");
    bool mono = true; for(int x = 0; x < 11000; x += 7) mono = mono && p.Map(x + 7) >= p.Map(x);
    CHECK(mono, "Map is monotone");
    CHECK(p.Inside(2000, 2600) && p.Inside(3000, 4200) && !p.Inside(2500, 3100) && !p.Inside(1500, 1600) && p.Inside(5, 5) && !p.Inside(9500, 10001), "Inside: one run, whole");
    p.nRun = 0; p.oldFree = 1000000;
    for(int i = 0; i < 200; i++) p.Add(i * 1000, i * 1000 + 10);
    p.Finish();
    bool kept = true; for(int i = 0; i < 200; i++) kept = kept && p.Inside(i * 1000, i * 1000 + 10);
    CHECK(p.nRun == bb::ReclaimPlan::kMaxRuns && kept, "past %d runs: a new span joins the run before it — every span still kept", bb::ReclaimPlan::kMaxRuns);
}

/* ── a kit: a 1 s break, pads, a replaced pad, a split, a marker, Loop / Ratchet keys ── */
static void MakeKit(bb::Engine& e, float* L, float* R, bb::UndoJournal& j, bool stereo)
{
    e.Init(41); e.SetUndoJournal(&j);
    const std::vector<float> b = Wave(48000, 80.f, 0.5f), bR = Wave(48000, 81.f, 0.45f);
    memcpy(L, b.data(), b.size() * sizeof(float));
    if(stereo) memcpy(R, bR.data(), bR.size() * sizeof(float));
    int onsets[16]; for(int i = 0; i < 16; i++) onsets[i] = i * 3000;
    e.SetMaterial(L, 48000, (float)kSr, onsets, 16, nullptr, 16, stereo ? R : nullptr, kCap);
}
static void FillPads(bb::Engine& e)
{
    std::vector<float> r2 = Wave(9000, 700.f, 0.3f), r5 = Wave(30000, 222.f, 0.6f);
    Drop(e, 0, Wave(6000, 300.f, 0.8f));
    Drop(e, 2, Wave(9000, 600.f, 0.5f), &r2);
    Drop(e, 5, Wave(30000, 220.f, 0.7f), &r5);
    Drop(e, 7, Wave(4800, 900.f, 0.4f));
    Drop(e, 2, Wave(12000, 450.f, 0.6f));                       // key 2 replaced: 9000 held
    Drop(e, 9, Wave(20000, 150.f, 0.7f));
    bb::EditReq tb; tb.op = bb::EditOp::Tombstone; tb.t = e.RecordedEntry(7);
    Op(e, tb);                                                   // key 7's pad deleted: 4800 held
    bb::EditReq sp; sp.op = bb::EditOp::Split; sp.t = e.RecordedEntry(5); sp.start = e.slices[e.RecordedEntry(5)].start + 12000;
    int half = -1; Op(e, sp, &half);                             // a split pad: both halves live (one run)
    bb::EditReq sk; sk.op = bb::EditOp::SetKey; sk.key = 11; sk.t = half;
    Op(e, sk);                                                   // key 11 plays the split's second half
    bb::EditReq m; m.op = bb::EditOp::AddEntry; m.start = 9000; m.len = 6000; m.key = 13;
    Op(e, m);                                                    // a marker inside the break on key 13
    Drop(e, 15, Wave(7000, 1200.f, 0.5f));
    Drop(e, 15, Wave(3000, 1300.f, 0.5f));                      // key 15 replaced: 7000 held
    bb::KeyParam lp = {0, bb::KeyParam::kLoop, 0, 0, 0}; e.PutKeyParam(9, lp);
    bb::KeyParam rp = {0, bb::KeyParam::kRatchet, 300, bb::KeyParam::kRatQuarter, 0}; e.PutKeyParam(0, rp);
}

/* ── 2. every live key the same samples ─────────────────────────────── */
static void TestKeysSame()
{
    SECTION("a compaction keeps every live key sounding the same samples; both planes in step");
    static std::vector<float> LA(kCap), RA(kCap), LB(kCap), RB(kCap);
    for(int stereo = 0; stereo < 2; stereo++)
    {
        bb::Engine a; MakeKit(a, LA.data(), RA.data(), gJA, stereo);
        FillPads(a);
        const bb::PadRoom r0 = bb::PadRoomOf(a);
        CHECK(r0.held == 9000 + 4800 + 7000, "%s: held 20800 before (got %u)", stereo ? "stereo" : "mono", r0.held);
        for(int t = a.nSlices; t < a.nTable; t++) if(!a.IsTomb(t) && a.slices[t].len > 0) a.FindLoop(t);   /* the tails' cached loop points */
        auto b = Twin(a, gJB);
        CHECK(Reclaim(*b, LB.data(), stereo ? RB.data() : nullptr, bb::kReclaimAll), "reclaimed");
        bool loops = true;
        for(int t = a.nSlices; t < a.nTable; t++)
        {
            if(a.IsTomb(t) || a.slices[t].len <= 0 || a.loopState[t] != 1) continue;
            const double d = (double)(b->slices[t].start - a.slices[t].start);
            loops = loops && b->loopState[t] == 1 && b->loopA[t] == a.loopA[t] + d && b->loopB[t] == a.loopB[t] + d;
        }
        CHECK(loops, "each entry's cached loop points moved with it (no re-search on the audio thread)");
        CHECK(b->freePos == a.freePos - (int)r0.held && bb::PadRoomOf(*b).held == 0 && bb::PadRoomOf(*b).free == r0.free + r0.held, "freePos down by exactly the held total; nothing held after");
        CHECK(b->buf == LB.data() && (stereo ? b->bufR == RB.data() : b->bufR == nullptr), "the engine reads the new planes");
        CHECK(memcmp(LB.data(), LA.data(), sizeof(float) * 48000) == 0 && (!stereo || memcmp(RB.data(), RA.data(), sizeof(float) * 48000) == 0), "the break copied as it was");
        bool data = true, gridSame = true, step = true;
        for(int t = 0; t < a.nTable; t++)
        {
            if(t < a.nSlices) { gridSame = gridSame && b->slices[t].start == a.slices[t].start && b->slices[t].len == a.slices[t].len; continue; }
            if(a.IsTomb(t) || a.slices[t].len <= 0) continue;
            const int s0 = a.slices[t].start, s1 = b->slices[t].start, n = a.slices[t].len;
            data = data && b->slices[t].len == n && memcmp(LB.data() + s1, LA.data() + s0, sizeof(float) * (size_t)n) == 0;
            if(stereo) step = step && memcmp(RB.data() + s1, RA.data() + s0, sizeof(float) * (size_t)n) == 0;
        }
        CHECK(gridSame && data, "every live entry holds the same samples where it now starts");
        CHECK(step, "the R plane moved with L: every entry's R samples beside its L ones");
        int lowest = kCap; for(int t = a.nSlices; t < a.nTable; t++) if(!a.IsTomb(t) && a.slices[t].start >= a.bufLen && a.slices[t].start < lowest) lowest = a.slices[t].start;
        bool moved = false; for(int t = a.nSlices; t < a.nTable; t++) if(!a.IsTomb(t) && b->slices[t].start != a.slices[t].start) moved = true;
        CHECK(moved && b->slices[a.KeySlice(13)].start == 9000, "pads moved down; the marker inside the break stayed");
        CHECK(SameKeys(a, *b), "every key — one-shot, held, reversed; Loop and Ratchet keys among them — renders the same bits");
        CHECK(b->RecordedEntry(2) == a.RecordedEntry(2) && b->KeySlice(11) == a.KeySlice(11), "the same table indices, the same key map");
        CHECK(!Reclaim(*b, LA.data(), stereo ? RA.data() : nullptr, bb::kReclaimAll) || b->freePos == a.freePos - (int)r0.held, "a second reclaim has nothing to do");
    }
}

/* ── 3. undo across it ───────────────────────────────────────────────── */
static void Steps(bb::Engine& e, int n, bool redo) { for(int i = 0; i < n; i++) Undo(e, redo); }
static void TestUndo()
{
    SECTION("undo across a compaction: kReclaimFree remaps the whole history, kReclaimAll forgets what needs a reclaimed sound");
    static std::vector<float> LA(kCap), RA(kCap), LB(kCap), RB(kCap);
    bb::Engine a; MakeKit(a, LA.data(), RA.data(), gJA, true);
    FillPads(a);
    /* a NUDGE_ALL of the markers (it moves the pads too: kStarts records), then steps after the last one that needs a
     * held sound, each holding a moved pad's span — two splits — and a bulk step (kSnap, then kQuiet) and a grid nudge,
     * undone below: the redo half */
    { bb::EditReq q; q.op = bb::EditOp::NudgeAll; q.a = -5; q.b = 1; Op(a, q); }
    { bb::EditReq q; q.op = bb::EditOp::Split; q.t = a.RecordedEntry(9); q.start = a.slices[q.t].start + 6000; Op(a, q); }
    { bb::EditReq q; q.op = bb::EditOp::Split; q.t = a.RecordedEntry(15); q.start = a.slices[q.t].start + 1500; Op(a, q); }
    { bb::EditReq q; q.op = bb::EditOp::AddEntry; q.start = 30000; q.len = 3000; q.flags = bb::EditReq::kSnap; Op(a, q);
      bb::EditReq q2; q2.op = bb::EditOp::AddEntry; q2.start = 36000; q2.len = 3000; q2.flags = bb::EditReq::kQuiet; Op(a, q2); }
    { bb::EditReq q; q.op = bb::EditOp::NudgeAll; q.a = 9; q.b = 0; Op(a, q); }
    Undo(a); Undo(a);   /* two steps undone: a redo half too */
    const int ud = a.UndoDepth(), rd = a.RedoDepth();
    CHECK(ud > 8 && rd == 2, "a history: %d back, %d forward", ud, rd);

    {   /* kReclaimFree: nothing the history holds goes */
        auto b = Twin(a, gJB);
        CHECK(Reclaim(*b, LB.data(), RB.data(), bb::kReclaimFree), "reclaimed (Free)");
        CHECK(gPlan.dropSteps == 0 && b->UndoDepth() == ud && b->RedoDepth() == rd, "the history whole: %d back, %d forward", b->UndoDepth(), b->RedoDepth());
        CHECK(gPlan.Reclaimed() == 0 && bb::PadRoomOf(*b).held == bb::PadRoomOf(a).held, "every held span is one Undo can bring back here: nothing came back (the tail's undoHeld = all %u)", bb::PadRoomOf(a).held);
        bool same = SameKeys(a, *b);
        for(int i = 0; i < ud && same; i++) { Silence(a); Silence(*b); same = Undo(a) == bb::EditStatus::Ok && Undo(*b) == bb::EditStatus::Ok && SameKeys(a, *b); }
        CHECK(same, "undo all the way back: every key the same bits at every step (replaced and deleted pads come back)");
        for(int i = 0; i < ud + rd && same; i++) { Silence(a); Silence(*b); same = Undo(a, true) == bb::EditStatus::Ok && Undo(*b, true) == bb::EditStatus::Ok && SameKeys(a, *b); }
        CHECK(same && b->RedoDepth() == 0, "and redo all the way forward, the same at every step");
        Steps(a, rd, false); Steps(*b, rd, false);   // back where the twin started
    }
    {   /* kReclaimAll: the steps that need a reclaimed sound go */
        static std::vector<float> LC(kCap), RC(kCap);
        Silence(a);
        auto b = Twin(a, gJB);
        CHECK(Reclaim(*b, LC.data(), RC.data(), bb::kReclaimAll), "reclaimed (All)");
        const int left = b->UndoDepth(), fwd = b->RedoDepth();
        CHECK(gPlan.dropSteps > 0 && left == 2 && fwd == 2 && left + fwd + gPlan.dropSteps == ud + rd, "%d steps forgotten: %d back, %d forward left (the two splits; the bulk add and the grid nudge)", gPlan.dropSteps, left, fwd);
        CHECK(bb::PadRoomOf(*b).held == 0, "everything held came back");
        bool same = true;
        for(int i = 0; i < left && same; i++) { Silence(a); Silence(*b); same = Undo(a) == bb::EditStatus::Ok && Undo(*b) == bb::EditStatus::Ok && SameKeys(a, *b); }
        CHECK(same, "the steps left undo bit for bit with the twin");
        CHECK(Undo(*b) == bb::EditStatus::BadArgs, "and the history ends there");
        for(int i = 0; i < left + fwd && same; i++) { Silence(a); Silence(*b); same = Undo(a, true) == bb::EditStatus::Ok && Undo(*b, true) == bb::EditStatus::Ok && SameKeys(a, *b); }
        CHECK(same && b->RedoDepth() == 0, "redo them and the redo half (a bulk kSnap step, a NUDGE_ALL of the markers) bit for bit");
        Steps(a, fwd, false); Steps(*b, fwd, false);
        bb::EditReq m; m.op = bb::EditOp::AddEntry; m.start = 15000; m.len = 3000;
        CHECK(Op(*b, m) == bb::EditStatus::Ok && b->UndoDepth() == left + 1 && Undo(*b) == bb::EditStatus::Ok, "a new edit after it journals and undoes as ever");
    }
    {   /* a trim leaves its edges held: reclaiming them (All) forgets the trim's undo; Free keeps it */
        static std::vector<float> LD(kCap), RD(kCap);
        Silence(a);
        auto b = Twin(a, gJB), c = Twin(a, gJC);
        auto trim = [](bb::Engine& x) { bb::EditReq q; q.op = bb::EditOp::SetEntry; q.t = x.RecordedEntry(5); q.start = x.slices[q.t].start + 1000; q.len = x.slices[q.t].len - 3000; return Op(x, q); };
        trim(*b); trim(*c);
        const int d0 = b->UndoDepth();
        CHECK(Reclaim(*b, LD.data(), RD.data(), bb::kReclaimAll) && b->UndoDepth() == 0 && bb::PadRoomOf(*b).held == 0, "a trimmed pad's edges come back with All; the trim's undo (and everything before it) goes");
        CHECK(Reclaim(*c, LD.data(), RD.data(), bb::kReclaimFree) && c->UndoDepth() == d0 && Undo(*c) == bb::EditStatus::Ok && c->slices[c->RecordedEntry(5)].len == a.slices[a.RecordedEntry(5)].len, "with Free the trim undoes: the edges were kept");
    }
    {   /* a drop undone: its take is held and only the redo half needs it — All drops that redo step, Free keeps it */
        static std::vector<float> LD(kCap), RD(kCap);
        Silence(a);
        auto b = Twin(a, gJB), c = Twin(a, gJC);
        for(bb::Engine* x : {b.get(), c.get()}) { Drop(*x, 20, Wave(6000, 640.f, 0.5f)); Undo(*x); }
        const int back = b->UndoDepth();
        CHECK(b->RedoDepth() == 1 && bb::PadRoomOf(*b).held == bb::PadRoomOf(a).held + 6000, "a drop undone: 6000 more held, one step to redo");
        CHECK(Reclaim(*b, LD.data(), RD.data(), bb::kReclaimAll) && b->RedoDepth() == 0 && Undo(*b, true) == bb::EditStatus::BadArgs && b->RecordedEntry(20) < 0, "All: that redo step forgotten (Redo has nothing), key 20 stays empty");
        CHECK(Reclaim(*c, LD.data(), RD.data(), bb::kReclaimFree) && c->RedoDepth() == 1 && c->UndoDepth() == back && Undo(*c, true) == bb::EditStatus::Ok && c->RecordedEntry(20) >= 0, "Free: the redo kept — Redo brings the drop back");
    }
    {   /* Free with memory no record holds (a history begun after the replacements): that comes back, the rest of the
         * history — a NUDGE_ALL of the pads among it — is remapped and undoes bit for bit */
        static std::vector<float> LE(kCap), RE(kCap), LF(kCap), RF(kCap);
        bb::Engine c; MakeKit(c, LE.data(), RE.data(), gJC, true);
        FillPads(c);
        const uint32_t orphan = bb::PadRoomOf(c).held;
        c.UndoClear();
        { bb::EditReq q; q.op = bb::EditOp::NudgeAll; q.a = -3; q.b = 1; Op(c, q); }
        { bb::EditReq q; q.op = bb::EditOp::Split; q.t = c.RecordedEntry(9); q.start = c.slices[q.t].start + 5000; Op(c, q); }
        Drop(c, 2, Wave(8000, 520.f, 0.6f));                            /* key 2 replaced again: this one Undo needs */
        { bb::EditReq q; q.op = bb::EditOp::NudgeAll; q.a = 4; q.b = 1; Op(c, q); }
        static bb::UndoJournal jd;
        auto d = Twin(c, jd);
        const int cd = c.UndoDepth();
        CHECK(Reclaim(*d, LF.data(), RF.data(), bb::kReclaimFree) && gPlan.dropSteps == 0 && d->UndoDepth() == cd, "Free: the history whole (%d steps)", cd);
        CHECK(gPlan.Reclaimed() > 0 && gPlan.Reclaimed() <= (int)orphan && bb::PadRoomOf(*d).held > 0, "what no record holds came back (%d of the %u held before the history began); what Undo needs stayed (%u)", gPlan.Reclaimed(), orphan, bb::PadRoomOf(*d).held);
        bool same = SameKeys(c, *d);
        for(int i = 0; i < cd && same; i++) same = Undo(c) == bb::EditStatus::Ok && Undo(*d) == bb::EditStatus::Ok && SameKeys(c, *d);
        CHECK(same, "undo every step — two NUDGE_ALLs of the moved pads, a split, the replacement — bit for bit with the twin");
        for(int i = 0; i < cd && same; i++) same = Undo(c, true) == bb::EditStatus::Ok && Undo(*d, true) == bb::EditStatus::Ok && SameKeys(c, *d);
        CHECK(same, "and redo them");
    }
    {   /* a stale journal (a take's commit went past it) is cleared, never revived */
        static std::vector<float> LD(kCap), RD(kCap);
        auto b = Twin(a, gJB);
        b->BumpGen();   /* something went past the journal */
        const uint32_t h = bb::PadRoomOf(*b).held;
        CHECK(Reclaim(*b, LD.data(), RD.data(), bb::kReclaimFree) && gPlan.Reclaimed() == (int)h && b->undo->count == 0 && b->UndoDepth() == 0, "a stale journal: nothing it holds counts — all %u held came back (Free), the journal cleared", h);
        CHECK(SameKeys(a, *b), "and every live key the same bits");
    }
}

/* ── 4. voices through the swap ──────────────────────────────────────── */
static void TestVoices()
{
    SECTION("voices through the swap: a held pad, a ratchet, a Loop key — the same bits; a replaced pad still ringing fades");
    static std::vector<float> LA(kCap), RA(kCap), LB(kCap), RB(kCap);
    bb::Engine a; MakeKit(a, LA.data(), RA.data(), gJA, true);
    FillPads(a);
    a.muted = true;
    float al[48], ar[48], bl[48], br[48];
    struct Case { const char* what; int key; };
    const Case cs[] = {{"a held pad (key 5's split half)", 5}, {"a ratcheting key (key 0, ¼ of itself)", 0}, {"a Loop key held (key 9)", 9}, {"the marker inside the break (key 13)", 13}};
    for(const Case& c : cs)
    {
        Silence(a);
        auto b = Twin(a, gJB);
        a.CallSlice(a.KeySlice(c.key), 1.f, false, false, false, 1.f, true, c.key);
        b->CallSlice(b->KeySlice(c.key), 1.f, false, false, false, 1.f, true, c.key);
        bool same = true, swapped = false;
        for(int blk = 0; blk < 300 && same; blk++)
        {
            if(blk == 40)
            {
                if(!bb::BuildReclaimPlan(*b, bb::kReclaimAll, gPlan)) break;
                bb::ReclaimPlanJournal(*b, gPlan);
                CHECK(bb::ReclaimFade(*b, gPlan) == 0, "%s: nothing to fade", c.what);
                bb::ReclaimCopy(gPlan, b->buf, LB.data()); bb::ReclaimCopy(gPlan, b->bufR, RB.data());
            }
            if(blk == 45) { swapped = bb::ReclaimSwap(*b, gPlan, LB.data(), RB.data()); bb::ReclaimJournal(*b, gPlan); }
            if(blk == 200) { a.ReleaseKey(c.key); b->ReleaseKey(c.key); }
            a.Process(al, ar, 48); b->Process(bl, br, 48);
            same = !memcmp(al, bl, sizeof al) && !memcmp(ar, br, sizeof ar);
            if(!same) printf("    %s: differ at block %d\n", c.what, blk);
        }
        float pk = 0.f; for(int i = 0; i < 48; i++) pk = fmaxf(pk, fabsf(al[i]));
        CHECK(swapped && same, "%s: sounding across the swap, the same bits block for block", c.what);
    }
    {   /* key 2 replaced while its old take still sounds: that region goes */
        Silence(a);
        static std::vector<float> LC(kCap), RC(kCap);
        bb::Engine& e = a;
        e.CallSlice(e.KeySlice(15), 1.f, false, false, false, 1.f, true, 15);   /* key 15's current pad, held */
        Drop(e, 15, Wave(5000, 1500.f, 0.5f));                                   /* replaced under the held voice */
        int ringing = 0; for(int v = 0; v < bb::Engine::kVoices; v++) if(e.voices[v].active && e.voices[v].held) ringing++;
        CHECK(ringing == 1, "the replaced pad's held voice still rings");
        if(!bb::BuildReclaimPlan(e, bb::kReclaimAll, gPlan)) return;
        bb::ReclaimPlanJournal(e, gPlan);
        const int out0 = bb::ReclaimFade(e, gPlan);
        CHECK(out0 == 1, "ReclaimFade: one voice outside the kept runs, released");
        bool finite = true; int blocks = 0; float peak = 0.f;
        while(bb::ReclaimFade(e, gPlan) > 0 && blocks < 100) { e.Process(al, ar, 48); blocks++; for(int i = 0; i < 48; i++) { finite = finite && std::isfinite(al[i]); peak = fmaxf(peak, fabsf(al[i])); } }
        CHECK(blocks > 0 && blocks < 20 && finite && peak < 1.f, "it fades out in %d blocks (the choke geometry), finite", blocks);
        bb::ReclaimCopy(gPlan, e.buf, LC.data()); bb::ReclaimCopy(gPlan, e.bufR, RC.data());
        CHECK(bb::ReclaimSwap(e, gPlan, LC.data(), RC.data()), "then the swap");
        bb::ReclaimJournal(e, gPlan);
        bool inside = true;
        for(int v = 0; v < bb::Engine::kVoices; v++) if(e.voices[v].active) inside = inside && e.voices[v].bound <= e.freePos && e.voices[v].start >= 0;
        CHECK(inside, "no voice reads past the new freePos");
        /* a voice the fade did not finish is cut at the swap, not left reading a gap */
        Silence(a);
        Drop(e, 15, Wave(6000, 1600.f, 0.5f));
        e.CallSlice(e.KeySlice(15), 1.f, false, false, false, 1.f, true, 15);
        Drop(e, 15, Wave(5000, 1700.f, 0.5f));
        bb::BuildReclaimPlan(e, bb::kReclaimAll, gPlan); bb::ReclaimPlanJournal(e, gPlan);
        bb::ReclaimCopy(gPlan, e.buf, LA.data()); bb::ReclaimCopy(gPlan, e.bufR, RA.data());
        CHECK(bb::ReclaimSwap(e, gPlan, LA.data(), RA.data()), "a swap with the old take still held");
        int act = 0; for(int v = 0; v < bb::Engine::kVoices; v++) if(e.voices[v].active) act++;
        e.Process(al, ar, 48);
        bool fin = true; for(int i = 0; i < 48; i++) fin = fin && std::isfinite(al[i]) && fabsf(al[i]) < 1.f;
        CHECK(act == 0 && fin, "the voice is cut (the steal's bridge carries its last sample), the output finite");
    }
    {   /* a swap after the table moved is refused */
        bb::BuildReclaimPlan(a, bb::kReclaimAll, gPlan);
        a.BumpGen();
        const int fp = a.freePos; float* buf = a.buf;
        CHECK(!bb::ReclaimSwap(a, gPlan, LB.data(), RB.data()) && a.freePos == fp && a.buf == buf, "the table moved since the plan: the swap refuses, nothing touched");
    }
}

/* ── 5. the set ──────────────────────────────────────────────────────── */
static bool MemSink(void* ctx, const void* p, size_t n)
{
    auto* v = (std::vector<uint8_t>*)ctx;
    v->insert(v->end(), (const uint8_t*)p, (const uint8_t*)p + n);
    return true;
}
static bool WriteFile(const std::string& path, const std::vector<uint8_t>& b)
{
    FILE* f = fopen(path.c_str(), "wb"); if(!f) return false;
    const bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
    return fclose(f) == 0 && ok;
}
static bool WriteJson(const char* name, const bb::SetInfo& s)
{
    std::vector<uint8_t> js; static char json[bb::kSetJsonMax];
    return bb::SetWriter::Json({MemSink, &js}, s, json, sizeof json) && WriteFile(gOut + "/" + name + ".json", js);
}
// The firmware's SaveSet: pack (the live spans), the JSON through the map, the WAV gathered through its segments.
static int SaveKit(const bb::Engine& e, const char* name, bb::SetInfo& s, bb::CardMap& m)
{
    static bb::ReclaimPlan pk;
    bb::Looper none;
    new(&s) bb::SetInfo();
    if(!bb::PackForCard(e, pk, m) || !bb::SetFromEngine(e, none, s, e.bufR != nullptr, kSr, m)) return -4;
    strcpy(s.name, name); s.bars = 1; s.bpm = e.NativeBpm();
    std::vector<uint8_t> wav; static uint8_t staging[16384];
    if(!bb::SetWriter::Wav({MemSink, &wav}, s, e.buf, e.bufR, staging, sizeof staging, bbwav::kPcm16, m.live, m.seg, m.n)) return -1;
    if(!WriteFile(gOut + "/" + name + ".wav", wav) || !WriteJson(name, s)) return -3;
    return s.len;
}
// The firmware's LoadSet + ServiceSets: the JSON, the WAV through the loader, PlanSet, SetMaterial, the markers, keys.
static bool LoadKit(bb::Engine& e, float* L, float* R, const char* name, bb::UndoJournal& j)
{
    std::vector<uint8_t> js;
    { FILE* f = fopen((gOut + "/" + name + ".json").c_str(), "rb"); if(!f) return false; int c; while((c = fgetc(f)) != EOF) js.push_back((uint8_t)c); fclose(f); }
    static bb::SetInfo t; static bb::SetPlan p;
    new(&t) bb::SetInfo();
    if(!bb::SetReader::Json((const char*)js.data(), js.size(), t)) return false;
    static std::vector<float> stg(4096), stgR(4096);
    int cues[128], nc = 0, err = 0; bool gotR = false;
    const int len = bbwav::Load((gOut + "/" + name + ".wav").c_str(), stg.data(), (int)stg.size(), L, kCap, 48000.f, 45, cues, &nc, 128, &err,
                                t.stereo ? R : nullptr, t.stereo ? stgR.data() : nullptr, &gotR);
    if(len <= 0 || len < t.len || !bb::PlanSet(t, p)) { printf("    load: len %d (want %d) err %d\n", len, t.len, err); return false; }
    e.Init(41); e.SetUndoJournal(&j);
    for(int i = 0; i < p.nGrid; i++) e.sliceSal[i] = p.sal[i];
    e.swing = p.swing;
    e.SetMaterial(L, p.material, 48000.f, p.onsets, p.nOnsets, p.cls, p.nGrid, gotR ? R : nullptr, kCap);
    e.SetGridBoundaries(p.starts, p.nGrid, p.gridFlags);
    int8_t live[24];
    bb::CommitSetRecordings(e, t, p, [&](int start, int l) {
        uint8_t c = 3; float sal = 0.f, d = 0.f; bool h = false;
        bb::AnalyseRegion(L, start, l < bb::kPadAnalyseMax ? l : bb::kPadAnalyseMax, 48000.f, 0, c, sal, d, h);
        return d; }, live);
    if(p.hasKeys) e.SetKeyTable(live);
    bb::KeyParam kp[24]; bb::KeyParamsFromSet(t, kp); e.SetKeyParams(kp);
    return true;
}
// Same keys, but the loaded engine's analysis may differ by a hair from the live one's (density comes from the
// file's audio; a dropped pad's from its first second): compare with Fades = Fixed on both, where it does not count.
static bool SameKeysFixed(bb::Engine& a, bb::Engine& b) { a.fadeMode = b.fadeMode = 0; return SameKeys(a, b); }
static void TestSet()
{
    SECTION("the set: a packed save, load; after a compaction; the card map through a reclaim (autosave, unsaved)");
    static std::vector<float> LA(kCap), RA(kCap), LB(kCap), RB(kCap), LL(kCap), RL(kCap);
    static bb::SetInfo s;
    bb::Engine a; MakeKit(a, LA.data(), RA.data(), gJA, true);
    FillPads(a);
    const uint32_t held = bb::PadRoomOf(a).held;
    {   /* a packed save: the live spans only */
        const int frames = SaveKit(a, "rc_packed", s, gMap);
        CHECK(frames == a.freePos - (int)held && gMap.n >= 2 && gMap.live[0] == 0 && gMap.card[0] == 0, "saved %d frames: freePos %d less the %u held", frames, a.freePos, held);
        CHECK(!bb::KitUnsaved(a, gMap, false), "the kit is on the card");
        bool cueOk = true; std::vector<float> w; w.resize((size_t)frames);
        for(int i = 0; i < s.nSlices; i++) cueOk = cueOk && s.slices[i].start + s.slices[i].len <= frames;
        CHECK(cueOk, "every entry inside the file");
        bb::Engine e2;
        CHECK(LoadKit(e2, LL.data(), RL.data(), "rc_packed", gJC), "loaded back");
        CHECK(e2.freePos == frames && bb::PadRoomOf(e2).held == 0 && e2.bufR == RL.data(), "freePos %d, nothing held, stereo", e2.freePos);
        bool data = true;
        for(int t = a.nSlices; t < a.nTable; t++)
        {
            if(a.IsTomb(t) || a.slices[t].len <= 0) continue;
            int c = 0; gMap.ToCard(a.slices[t].start, a.slices[t].len, c);
            data = data && memcmp(LL.data() + c, LA.data() + a.slices[t].start, sizeof(float) * 8) == 0;
        }
        CHECK(data, "each entry's samples where the map says the card holds them");
        CHECK(SameKeysFixed(a, e2), "every key renders the same bits from the loaded set");
    }
    {   /* a compaction, then a save and a load */
        auto b = Twin(a, gJB);
        CHECK(Reclaim(*b, LB.data(), RB.data(), bb::kReclaimAll), "reclaimed");
        static bb::CardMap m;
        const int frames = SaveKit(*b, "rc_after", s, m);
        CHECK(frames == b->freePos && m.n == 1 && m.seg[0] == frames, "after a compaction the save is the planes as they lie (one segment, %d frames)", frames);
        bb::Engine e2;
        CHECK(LoadKit(e2, LL.data(), RL.data(), "rc_after", gJC) && SameKeysFixed(*b, e2) && SameKeysFixed(a, e2), "save → load after a compaction: the same bits as before it");
    }
    {   /* the card map through a reclaim: the autosave still describes the WAV on the card */
        Silence(a);
        CHECK(SaveKit(a, "rc_auto", s, gMap) > 0, "saved (packed)");
        Drop(a, 3, Wave(5000, 2000.f, 0.5f));                         /* a new pad, not on the card */
        Drop(a, 0, Wave(4000, 350.f, 0.7f));                          /* key 0 replaced: its card copy is now held */
        bb::EditReq m; m.op = bb::EditOp::SetEntry; m.t = a.RecordedEntry(9); m.start = a.slices[m.t].start + 2000; m.len = a.slices[m.t].len - 4000;
        CHECK(Op(a, m) == bb::EditStatus::Ok, "key 9's pad trimmed (inside its card span)");
        CHECK(bb::KitUnsaved(a, gMap, false), "two new takes: unsaved");
        auto b = Twin(a, gJB);
        CHECK(Reclaim(*b, LB.data(), RB.data(), bb::kReclaimAll, &gMap), "reclaimed: the map follows");
        CHECK(gMap.ok && gMap.len == s.len && bb::KitUnsaved(*b, gMap, false), "the map still says what the card holds: still unsaved (the new pads)");
        static bb::SetInfo auto_; bb::Looper none; new(&auto_) bb::SetInfo();
        CHECK(bb::SetFromEngine(*b, none, auto_, true, kSr, gMap), "the autosave's JSON through the map");
        strcpy(auto_.name, "rc_auto"); auto_.bars = 1; auto_.bpm = b->NativeBpm();
        bool noNew = true; for(int i = 0; i < auto_.nSlices; i++) noNew = noNew && auto_.slices[i].start + auto_.slices[i].len <= auto_.len;
        CHECK(auto_.len == s.len && noNew, "it describes the card's WAV (%d frames), nothing past it", auto_.len);
        CHECK(WriteJson("rc_auto", auto_), "written over the set's JSON (the WAV untouched)");
        bb::Engine e2;
        CHECK(LoadKit(e2, LL.data(), RL.data(), "rc_auto", gJC), "the card's set loads");
        bool same = true;
        e2.fadeMode = b->fadeMode = 0; e2.muted = b->muted = true;
        float al[48], ar[48], bl[48], br[48];
        for(int k : {2, 5, 9, 11, 13, 15})
        {
            Silence(e2); Silence(*b);
            e2.CallSlice(e2.KeySlice(k), 1.f, false, false, false, 1.f, false, k); b->CallSlice(b->KeySlice(k), 1.f, false, false, false, 1.f, false, k);
            for(int i = 0; i < 30; i++) { e2.Process(al, ar, 48); b->Process(bl, br, 48); same = same && !memcmp(al, bl, sizeof al) && !memcmp(ar, br, sizeof ar); }
            if(!same) { printf("    key %d differs\n", k); break; }
        }
        CHECK(same, "every key the card holds plays the same bits as the live kit (key 9 trimmed, the moved pads)");
        CHECK(e2.RecordedEntry(3) < 0, "the pad dropped since the save is not on the card");
        static bb::CardMap m2;
        CHECK(SaveKit(*b, "rc_auto", s, m2) > 0 && !bb::KitUnsaved(*b, m2, false), "Save set: on the card again");
    }
    {   /* a gathered WAV is the segments, sample for sample */
        std::vector<float> L(1000), R(1000);
        for(int i = 0; i < 1000; i++) { L[(size_t)i] = (float)S16(0.001f * (float)i - 0.5f) / 32768.f; R[(size_t)i] = -L[(size_t)i]; }
        const int from[3] = {0, 400, 900}, len[3] = {100, 0, 50};
        bbwav::WriteSpec w; w.left = L.data(); w.right = R.data(); w.len = 150; w.segFrom = from; w.segLen = len; w.nSeg = 3;
        std::vector<uint8_t> out; static uint8_t stg[256];
        CHECK(bbwav::Write({MemSink, &out}, w, stg, sizeof stg), "a gathered write (a zero-length segment among them)");
        const uint8_t* d = out.data() + out.size() - 150 * 4;
        bool ok = true;
        for(int i = 0; i < 150; i++)
        {
            const int src = i < 100 ? i : 900 + (i - 100);
            const int16_t l = (int16_t)(d[i * 4] | (d[i * 4 + 1] << 8)), r = (int16_t)(d[i * 4 + 2] | (d[i * 4 + 3] << 8));
            ok = ok && l == S16(L[(size_t)src]) && r == S16(R[(size_t)src]);
        }
        CHECK(ok, "the frames are the segments' in order, both channels");
        w.len = 151;
        CHECK(!bbwav::Write({MemSink, &out}, w, stg, sizeof stg), "segments that do not add up to the length: refused");
    }
    {   /* the card map's own rules */
        bb::CardMap& m = gMap;
        m.Identity(5000);
        int c = -1;
        CHECK(m.ToCard(100, 200, c) && c == 100 && !m.ToCard(4900, 200, c) && m.ToCard(4999, 1, c), "Identity: inside maps to itself, past the end does not");
        gPlan.nRun = 0; gPlan.oldFree = 5000; gPlan.bufLen = 1000; gPlan.Add(0, 1000); gPlan.Add(2000, 3000); gPlan.Add(4000, 5000); gPlan.Finish();
        gMap2.Follow(m, gPlan); m = gMap2;
        CHECK(m.n == 3 && m.ToCard(1000, 1000, c) && c == 2000 && m.ToCard(2000, 500, c) && c == 4000 && !m.ToCard(900, 200, c), "Follow: the kept runs keep their card spans, a span across a reclaimed gap maps nowhere");
        bb::CardMap big; big.Identity(1000000);
        gPlan.nRun = 0; gPlan.oldFree = 1000000; gPlan.bufLen = 10; gPlan.Add(0, 10);
        for(int i = 1; i < 200; i++) gPlan.Add(i * 5000, i * 5000 + 100);
        gPlan.Finish();
        gMap2.Follow(big, gPlan);
        CHECK(gMap2.ok == (gPlan.nRun <= bb::CardMap::kMax), "a map that would split past %d pieces says so (ok = %d)", bb::CardMap::kMax, (int)gMap2.ok);
    }
}

int main(int argc, char** argv)
{
    if(argc > 1) gOut = argv[1];
    TestPlan();
    TestKeysSame();
    TestUndo();
    TestVoices();
    TestSet();
    printf("\nreclaim_check: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
