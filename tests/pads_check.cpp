// pads_check — PADS package (docs/packages/pads.md): core/bb_pads.h (a drop's
// bytes → the planes, the room, the empty kit), the engine's half
// (EditOp::Record, Engine::AdoptR, Engine::OwnRate) and the loader's
// same-rate path (core/wav_loader.h), over tests/fatfs_shim.h. tests/run.sh
// builds it under ASan/UBSan:
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore -Ivendor/capicola/lib
//       tests/pads_check.cpp -o /tmp/pads && /tmp/pads /tmp     (one line)
//
// 1. The receiver as the shell drives it: BEGIN's check, PCM16 chunks in order
//    straight into the L plane past freePos, the size first, out of sequence
//    drops the drop, nothing below freePos touched.
// 2. PcmToPlanes: stereo forwards, mono backwards, in place, bit-exact against
//    a reference; a mono drop copied into R; a stereo drop with no R folded;
//    the extremes; odd lengths.
// 3. The commit (EditOp::Record): a new pad, a replaced pad (its slot reused,
//    the old region held), one undo step back and forth, freePos never moves
//    back, the refusals; Pads' keys-from-markers keeps a pad; a re-apply of
//    the same planes keeps it.
// 4. The room: free / total / held after drops, a replace, an undo, a split
//    (overlap counted once), a marker inside the break (not a pad).
// 5. OwnRate: a pad plays at its own rate at any tempo, the break's slices
//    varispeed; recOwnRate = false restores the v0.13 rule. AdoptR: a mono
//    kit grows an R plane under a sounding voice without a step.
// 6. KitUnsaved and the empty kit's stub.
// 7. The loader's same-rate path: a 48 kHz file longer than the staging loads
//    whole; 44.1 kHz still resamples through the staging.
// 8. Set round trips at the 45 s planes: a pads-only kit (one stereo pad →
//    the kit stereo) and a 17 s break plus pads filling the plane — saved
//    (SetFromEngine → WAV + JSON on the shim's disk), loaded back through the
//    firmware's own loader, re-planned, re-committed: table, keys and every
//    rendered sample identical.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <new>

#include "fatfs_shim.h"
#include "wav_loader.h"
#include "bb_engine.h"
#include "bb_card.h"
#include "bb_pads.h"
#include "bb_sample.h"
#include "bb_set.h"
#include "bb_sets.h"
#include "bb_wavwrite.h"

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

static std::string gOut = "/tmp";
static constexpr int kSr = 48000, kCap = 48000 * 45;   // the firmware's planes: kMaxSec = 45
static bb::UndoJournal gJ, gJ2;                          // 150 KB each: static, not on the stack

static int16_t S16(float x) { return (int16_t)bbwav::detail::Clip(x, 32768.f, -32768, 32767); }
// PCM16 LE bytes of frames × ch (interleaved), the page's encoding
static std::vector<uint8_t> Pcm(const std::vector<float>& l, const std::vector<float>* r)
{
    const int ch = r ? 2 : 1, n = (int)l.size();
    std::vector<uint8_t> b((size_t)n * ch * 2);
    for(int i = 0; i < n; i++)
    {
        const int16_t a = S16(l[(size_t)i]);
        b[(size_t)i * ch * 2] = (uint8_t)a; b[(size_t)i * ch * 2 + 1] = (uint8_t)((uint16_t)a >> 8);
        if(r) { const int16_t c = S16((*r)[(size_t)i]); b[(size_t)i * 4 + 2] = (uint8_t)c; b[(size_t)i * 4 + 3] = (uint8_t)((uint16_t)c >> 8); }
    }
    return b;
}
static std::vector<float> Wave(int n, float f, float g, float ph = 0.f)
{
    std::vector<float> v((size_t)n);
    for(int i = 0; i < n; i++) v[(size_t)i] = (float)S16(g * sinf(6.2831853f * f * (float)i / (float)kSr + ph)) / 32768.f;   // PCM16-exact
    return v;
}
// The shell's drop, from BEGIN to the commit, on the control thread's side: BEGIN's check against the room, the
// bytes into L past freePos through UploadRx (1000-byte chunks, the page's at max_body 1024), PcmToPlanes, the
// analysis of the head, EditOp::Record. Returns the ApplyEdit status (or 100 + the BEGIN status).
static int Drop(bb::Engine& e, float* R, int key, const std::vector<float>& l, const std::vector<float>* r, int* idx = nullptr)
{
    const int ch = r ? 2 : 1, frames = (int)l.size();
    const uint32_t room = (uint32_t)(e.bufCap - e.freePos);
    const uint8_t b = bb::PadBeginCheck(key, ch, (uint32_t)frames, room);
    if(b) return 100 + b;
    const std::vector<uint8_t> bytes = Pcm(l, r);
    bb::UploadRx rx; rx.buf = (uint8_t*)(e.buf + e.freePos); rx.cap = bb::PadBytes((uint32_t)frames, ch); rx.Reset();
    if(rx.Begin(rx.cap) != bb::kStOk) return 200;
    for(uint32_t off = 0; off < rx.cap; off += 1000)
    {
        const uint32_t n = rx.cap - off < 1000u ? rx.cap - off : 1000u;
        if(rx.Take(rx.cap, off, bytes.data() + off, n) != bb::kStOk) return 201;
    }
    if(!rx.Done()) return 202;
    const int start = e.freePos;
    bb::PcmToPlanes(e.buf, R, start, frames, ch);
    bb::EditReq q; q.op = bb::EditOp::Record; q.key = key; q.start = start; q.len = frames; q.flags = bb::EditReq::kMeta;
    bb::AnalyseRegion(e.buf, start, frames < bb::kPadAnalyseMax ? frames : bb::kPadAnalyseMax, (float)kSr, 0, q.meta.cls, q.meta.sal, q.meta.dens, q.meta.hit);
    int t = -1;
    const int st = (int)e.ApplyEdit(q, &t);
    if(idx) *idx = t;
    return st;
}
static bb::EditStatus Undo(bb::Engine& e, bool redo = false) { bb::EditReq q; q.op = redo ? bb::EditOp::Redo : bb::EditOp::Undo; return e.ApplyEdit(q); }
// An empty kit: the stub as the material, 16 grid slices, class 3, no onsets, the planes' full capacity
static void EmptyKit(bb::Engine& e, float* L, float* R)
{
    bb::MakeEmptyKit(L, R);
    uint8_t cls[64]; memset(cls, 3, sizeof cls);
    e.SetMaterial(L, bb::kPadStubLen, (float)kSr, nullptr, 0, cls, bb::kPadStubSlices, R, kCap);
}

/* ── 1. the receiver ─────────────────────────────────────────────────── */
static void TestReceiver()
{
    SECTION("the receiver: BEGIN's check, in order into the plane past freePos");
    CHECK(bb::PadBytes(1000, 1) == 2000u && bb::PadBytes(1000, 2) == 4000u && bb::PadBytes(1000, 3) == 0u && bb::PadBytes(1000, 0) == 0u, "PadBytes");
    CHECK(bb::PadBeginCheck(0, 1, 480, 480) == 0 && bb::PadBeginCheck(23, 2, 1000, 5000) == 0, "a fitting drop is taken");
    CHECK(bb::PadBeginCheck(-1, 1, 1000, 5000) == 2 && bb::PadBeginCheck(24, 1, 1000, 5000) == 2, "a key outside 0..23: BAD_ARGS");
    CHECK(bb::PadBeginCheck(0, 0, 1000, 5000) == 2 && bb::PadBeginCheck(0, 3, 1000, 5000) == 2, "0 or 3 channels: BAD_ARGS");
    CHECK(bb::PadBeginCheck(0, 1, 479, 5000) == 2, "under kRecMin (480 frames): BAD_ARGS");
    CHECK(bb::PadBeginCheck(0, 2, 5001, 5000) == 6 && bb::PadBeginCheck(0, 1, 0xFFFFFFFFu, 5000) == 6, "more than the room: TOO_LARGE");

    static std::vector<float> L(kCap, 0.f);
    bb::Engine e; e.Init(3);
    const std::vector<float> brk = Wave(48000, 110.f, 0.5f);
    memcpy(L.data(), brk.data(), brk.size() * sizeof(float));
    e.SetMaterial(L.data(), 48000, (float)kSr, nullptr, 0, nullptr, 16, nullptr, kCap);
    const std::vector<float> below(L.begin(), L.begin() + e.freePos);
    const std::vector<float> pad = Wave(3001, 440.f, 0.7f);
    const std::vector<uint8_t> bytes = Pcm(pad, nullptr);
    bb::UploadRx rx; rx.buf = (uint8_t*)(L.data() + e.freePos); rx.cap = bb::PadBytes(3001, 1); rx.Reset();
    CHECK(rx.Begin(rx.cap + 1) == bb::kStTooLarge && !rx.open, "a total past the drop's bytes refused before a byte is copied");
    CHECK(rx.Begin(rx.cap) == bb::kStOk, "begin");
    CHECK(rx.Take(rx.cap, 0, bytes.data(), 1000) == bb::kStOk && rx.have == 1000 && rx.Progress() > 0, "first chunk");
    CHECK(rx.Take(rx.cap, 2000, bytes.data() + 2000, 1000) == bb::kStBadArgs && !rx.open, "a skipped chunk drops the drop");
    CHECK(rx.Take(rx.cap, 1000, bytes.data() + 1000, 1000) == bb::kStBadState, "nothing more is taken after the drop");
    rx.Begin(rx.cap);
    for(uint32_t off = 0; off < rx.cap; off += 1000) rx.Take(rx.cap, off, bytes.data() + off, rx.cap - off < 1000 ? rx.cap - off : 1000);
    CHECK(rx.Done() && rx.Progress() == 255, "every byte in: done");
    CHECK(memcmp(L.data() + e.freePos, bytes.data(), bytes.size()) == 0, "the bytes lie in L past freePos as they came");
    CHECK(memcmp(L.data(), below.data(), below.size() * sizeof(float)) == 0, "nothing below freePos was touched");
    CHECK(rx.Take(rx.cap, rx.cap, bytes.data(), 1) == bb::kStBadArgs, "past the end: BAD_ARGS");
}

/* ── 2. PcmToPlanes ──────────────────────────────────────────────────── */
static void TestConvert()
{
    SECTION("PcmToPlanes: in place, stereo forwards, mono backwards, bit-exact");
    for(int n : {1, 2, 3, 480, 4095, 4096, 4097, 9001})
    {
        std::vector<float> l = Wave(n, 523.f, 0.9f), r = Wave(n, 97.f, -0.6f, 1.f);
        l[0] = -1.f; if(n > 1) l[(size_t)n - 1] = 32767.f / 32768.f;
        // mono into a mono plane
        {
            std::vector<float> L(64 + (size_t)n + 8, 9.f);
            const std::vector<uint8_t> b = Pcm(l, nullptr);
            memcpy(L.data() + 64, b.data(), b.size());
            bb::PcmToPlanes(L.data(), nullptr, 64, n, 1);
            bool ok = true;
            for(int i = 0; i < n; i++) ok = ok && L[64 + (size_t)i] == l[(size_t)i];
            CHECK(ok && L[63] == 9.f && L[64 + (size_t)n] == 9.f, "mono, %d frames: every sample exact, the neighbours untouched", n);
        }
        // stereo into two planes
        {
            std::vector<float> L(16 + (size_t)n, 9.f), R(16 + (size_t)n, 7.f);
            const std::vector<uint8_t> b = Pcm(l, &r);
            memcpy(L.data() + 16, b.data(), b.size());
            bb::PcmToPlanes(L.data(), R.data(), 16, n, 2);
            bool ok = true;
            for(int i = 0; i < n; i++) ok = ok && L[16 + (size_t)i] == l[(size_t)i] && R[16 + (size_t)i] == r[(size_t)i];
            CHECK(ok && L[15] == 9.f && R[15] == 7.f, "stereo, %d frames: both planes exact", n);
        }
        // a mono drop onto a stereo kit: R is L's copy; a stereo drop with no R: the fold
        {
            std::vector<float> L((size_t)n), R((size_t)n, 7.f);
            const std::vector<uint8_t> b = Pcm(l, nullptr);
            memcpy(L.data(), b.data(), b.size());
            bb::PcmToPlanes(L.data(), R.data(), 0, n, 1);
            CHECK(memcmp(L.data(), R.data(), (size_t)n * 4) == 0 && L[0] == -1.f, "mono into a stereo kit: R = L (%d)", n);
            std::vector<float> M((size_t)n);
            const std::vector<uint8_t> c = Pcm(l, &r);
            memcpy(M.data(), c.data(), c.size());
            bb::PcmToPlanes(M.data(), nullptr, 0, n, 2);
            bool ok = true;
            for(int i = 0; i < n; i++) ok = ok && M[(size_t)i] == 0.5f * (l[(size_t)i] + r[(size_t)i]);
            CHECK(ok, "stereo with no R plane folds to (L + R) / 2 (%d)", n);
        }
    }
    float x = 5.f; bb::PcmToPlanes(&x, nullptr, 0, 0, 1); bb::PcmToPlanes(&x, nullptr, 0, 1, 3); bb::PcmToPlanes(nullptr, nullptr, 0, 1, 1);
    CHECK(x == 5.f, "nothing to do: nothing written (0 frames, 3 channels, no plane)");
}

/* ── 3. the commit ───────────────────────────────────────────────────── */
static void TestCommit()
{
    SECTION("the commit: EditOp::Record — a pad, a replaced pad, one undo step, the refusals");
    static std::vector<float> L(kCap), R(kCap);
    bb::Engine e; e.Init(5); e.SetUndoJournal(&gJ);
    const std::vector<float> brk = Wave(96000, 80.f, 0.5f);
    memcpy(L.data(), brk.data(), brk.size() * sizeof(float));
    int onsets[4] = {0, 24000, 48000, 72000};
    e.SetMaterial(L.data(), 96000, (float)kSr, onsets, 4, nullptr, 16, nullptr, kCap);
    CHECK(e.bufCap == kCap && e.freePos == 96000 && e.nTable == 16, "a 2 s break in a 45 s plane");
    const uint8_t gen0 = e.tableGen;
    int t = -1;
    const std::vector<float> kick = Wave(9600, 60.f, 0.9f);
    CHECK(Drop(e, nullptr, 3, kick, nullptr, &t) == 0 && t == 16, "a drop onto key 3: entry %d", t);
    CHECK(e.tableKey[16] == 3 && e.KeySlice(3) == 16 && e.RecordedEntry(3) == 16 && e.slices[16].start == 96000 && e.slices[16].len == 9600, "key 3 owns [96000, +9600)");
    CHECK(e.freePos == 96000 + 9600 && e.tableFlags[16] == 0 && e.tableGen != gen0 && e.UndoDepth() == 1, "freePos past it, white, one undo step");
    CHECK(e.stepHit[16] && e.stepClass[16] != 3 && e.sliceSal[16] == 1.f, "analysed: a hit, a class, full salience");
    bool exact = true;
    for(int i = 0; i < 9600; i++) exact = exact && L[96000 + (size_t)i] == kick[(size_t)i];
    CHECK(exact, "the plane holds the drop sample for sample (no trim, no normalise)");

    const std::vector<float> snare = Wave(4800, 200.f, 0.3f);
    CHECK(Drop(e, nullptr, 3, snare, nullptr, &t) == 0 && t == 16, "a second drop onto key 3 reuses its slot");
    CHECK(e.slices[16].start == 105600 && e.slices[16].len == 4800 && e.nTable == 17 && e.freePos == 110400 && e.UndoDepth() == 2, "the new region, the table no longer");
    bb::PadRoom room = bb::PadRoomOf(e);
    CHECK(room.held == 9600 && room.free == (uint32_t)(kCap - 110400) && room.total == (uint32_t)(kCap - 96000), "the old region is held: %u (free %u, total %u)", room.held, room.free, room.total);

    CHECK(Undo(e) == bb::EditStatus::Ok && e.slices[16].start == 96000 && e.slices[16].len == 9600 && e.KeySlice(3) == 16, "undo: the first pad is back");
    CHECK(e.freePos == 110400, "freePos stays where it went (the redo needs its region)");
    CHECK(Undo(e) == bb::EditStatus::Ok && e.nTable == 16 && e.RecordedEntry(3) < 0 && e.KeySlice(3) == e.SpanSlice(3), "undo again: no pad, key 3 on its grid slice");
    CHECK(Undo(e) == bb::EditStatus::BadArgs, "nothing more to undo");
    CHECK(bb::PadRoomOf(e).held == 14400, "every dropped sample is held now: %u", bb::PadRoomOf(e).held);
    CHECK(Undo(e, true) == bb::EditStatus::Ok && Undo(e, true) == bb::EditStatus::Ok && e.slices[16].start == 105600 && e.KeySlice(3) == 16 && bb::PadRoomOf(e).held == 9600, "redo twice: the second pad again");

    // refusals
    bb::EditReq q; q.op = bb::EditOp::Record; q.key = 4; q.start = e.freePos + 1; q.len = 1000;
    CHECK(e.ApplyEdit(q) == bb::EditStatus::BadArgs, "a span past freePos (a gap) refused");
    q.start = e.freePos; q.len = 479;
    CHECK(e.ApplyEdit(q) == bb::EditStatus::BadArgs, "under kRecMin refused");
    q.len = kCap - e.freePos + 1;
    CHECK(e.ApplyEdit(q) == bb::EditStatus::BadArgs, "past bufCap refused");
    q.len = 1000; q.key = 24;
    CHECK(e.ApplyEdit(q) == bb::EditStatus::BadArgs, "key 24 refused");
    CHECK(Drop(e, nullptr, 5, Wave(kCap, 1.f, 0.1f), nullptr) == 106, "a drop bigger than the room: TOO_LARGE at BEGIN");

    // Pads' keys follow markers — a pad keeps its key; a re-apply of the same planes keeps the pad
    bb::EditReq m; m.op = bb::EditOp::AddEntry; m.start = 30000; m.len = 6000; m.flags = bb::EditReq::kOrange;
    CHECK(e.ApplyEdit(m) == bb::EditStatus::Ok && e.MarkerCount() == 1, "a marker inside the break");
    int8_t keys[24];
    e.KeysFromMarkers(keys);
    CHECK(keys[3] == 16 && keys[0] != 16, "keys from markers: key 3 keeps its pad, the marker goes to key 0");
    e.SetMaterial(L.data(), 96000, (float)kSr, onsets, 4, nullptr, 16, nullptr, kCap);   /* a re-apply (Bars, Swing…) */
    const int pt = e.RecordedEntry(3);
    CHECK(pt >= 16 && e.KeySlice(3) == pt && e.slices[pt].start == 105600 && e.freePos == 110400, "a re-apply keeps the pad and freePos");

    // the table fills: Full, nothing changes
    bb::Engine f; f.Init(9); f.SetUndoJournal(&gJ2);
    f.SetMaterial(L.data(), 96000, (float)kSr, onsets, 4, nullptr, 64, nullptr, kCap);
    int k = 0; bb::EditStatus st = bb::EditStatus::Ok;
    for(; k < 40 && st == bb::EditStatus::Ok; k++)
    {
        bb::EditReq a; a.op = bb::EditOp::AddEntry; a.start = 100 * k; a.len = 600;
        st = f.ApplyEdit(a);
    }
    CHECK(f.nTable == bb::Engine::kMaxTable, "64 grid + 32 markers: the table is full");
    const uint8_t g = f.tableGen; const int fp = f.freePos;
    bb::EditReq rq; rq.op = bb::EditOp::Record; rq.key = 7; rq.start = f.freePos; rq.len = 1000;
    CHECK(f.ApplyEdit(rq) == bb::EditStatus::Full && f.tableGen == g && f.freePos == fp && f.RecordedEntry(7) < 0, "Full: nothing written");
}

/* ── 4. the room ─────────────────────────────────────────────────────── */
static void TestRoom()
{
    SECTION("the room: free, total, held (union of live recordings, markers excluded)");
    static std::vector<float> L(kCap);
    bb::Engine e; e.Init(11); e.SetUndoJournal(&gJ);
    e.SetMaterial(L.data(), 48000, (float)kSr, nullptr, 0, nullptr, 16, nullptr, kCap);
    bb::PadRoom r = bb::PadRoomOf(e);
    CHECK(r.free == (uint32_t)(kCap - 48000) && r.total == r.free && r.held == 0, "fresh: free = total, nothing held");
    Drop(e, nullptr, 0, Wave(4800, 300.f, 0.5f), nullptr);
    Drop(e, nullptr, 1, Wave(4800, 300.f, 0.5f), nullptr);
    r = bb::PadRoomOf(e);
    CHECK(r.free == (uint32_t)(kCap - 57600) && r.held == 0 && bb::LiveRecordedSamples(e) == 9600, "two pads: 9600 live, nothing held");
    bb::EditReq s; s.op = bb::EditOp::Split; s.t = e.RecordedEntry(0); s.start = 48000 + 2400;
    CHECK(e.ApplyEdit(s) == bb::EditStatus::Ok && bb::LiveRecordedSamples(e) == 9600, "a split pad counts once");
    bb::EditReq se; se.op = bb::EditOp::SetEntry; se.t = e.RecordedEntry(1); se.start = 48000; se.len = 9600;
    CHECK(e.ApplyEdit(se) == bb::EditStatus::Ok && bb::LiveRecordedSamples(e) == 9600 && bb::PadRoomOf(e).held == 0, "overlapping spans: the union");
    bb::EditReq tb; tb.op = bb::EditOp::Tombstone; tb.t = e.RecordedEntry(1);
    CHECK(e.ApplyEdit(tb) == bb::EditStatus::Ok && bb::PadRoomOf(e).held == 4800, "a pad deleted: its tail is held (%u)", bb::PadRoomOf(e).held);
    bb::EditReq m; m.op = bb::EditOp::AddEntry; m.start = 1000; m.len = 5000;
    CHECK(e.ApplyEdit(m) == bb::EditStatus::Ok && bb::LiveRecordedSamples(e) == 4800, "a marker inside the break is no pad");
    CHECK(bb::PadMask(e) == 1u, "PadMask: key 0 (key 1's pad deleted, the split half keyless)");
    bb::Engine z;
    r = bb::PadRoomOf(z);
    CHECK(r.free == 0 && r.total == 0 && r.held == 0, "no material: no room");
}

/* ── 5. OwnRate, AdoptR ──────────────────────────────────────────────── */
static void TestRateAndPlanes()
{
    SECTION("OwnRate: pads keep their pitch at any tempo; AdoptR: a mono kit turns stereo under a voice");
    static std::vector<float> L(kCap), R(kCap);
    bb::Engine e; e.Init(13); e.SetUndoJournal(&gJ);
    const std::vector<float> brk = Wave(96000, 90.f, 0.5f);
    memcpy(L.data(), brk.data(), brk.size() * sizeof(float));
    e.SetMaterial(L.data(), 96000, (float)kSr, nullptr, 0, nullptr, 16, nullptr, kCap);   /* 2 s, 16 steps: 120 BPM native */
    int t = -1;
    Drop(e, nullptr, 2, Wave(24000, 440.f, 0.6f), nullptr, &t);
    e.SetBpm(150.f);   /* the clock at 150: the break varispeeds ×1.25 */
    e.pitch = 1.f;
    auto rateOf = [&](int idx) {
        for(int v = 0; v < bb::Engine::kVoices; v++) e.voices[v].active = false;
        e.CallSlice(idx, 1.f, false, false, false);
        for(int v = 0; v < bb::Engine::kVoices; v++) if(e.voices[v].active && e.voices[v].start == e.slices[idx].start && e.voices[v].age == 0) return e.voices[v].rate;
        return -1.f; };
    const float rp = rateOf(t), rg = rateOf(3);
    CHECK(fabsf(rp - 1.f) < 1e-6f, "a pad at 150 BPM plays at 1.0 (got %g)", rp);
    CHECK(fabsf(rg - 1.25f) < 1e-3f, "a grid slice at 150 BPM varispeeds ×1.25 (got %g)", rg);
    e.pitch = 2.f;
    CHECK(fabsf(rateOf(t) - 2.f) < 1e-6f, "P4 still transposes a pad (an octave: 2.0)");
    e.pitch = 1.f;
    bb::EditReq m; m.op = bb::EditOp::AddEntry; m.start = 12000; m.len = 6000;
    int mi = -1; e.ApplyEdit(m, &mi);
    CHECK(!e.OwnRate(mi) && fabsf(rateOf(mi) - 1.25f) < 1e-3f, "a marker inside the break follows the tempo");
    e.recOwnRate = false;
    CHECK(fabsf(rateOf(t) - 1.25f) < 1e-3f, "recOwnRate = false: the v0.13 rule (every call varispeeds)");
    e.recOwnRate = true;

    // AdoptR: a voice sounding on a pad while the kit grows an R plane — L unchanged, R = L, no step
    bb::Engine a; a.Init(17); a.SetUndoJournal(&gJ2);
    static std::vector<float> L2(kCap), R2(kCap);
    memcpy(L2.data(), brk.data(), brk.size() * sizeof(float));
    a.SetMaterial(L2.data(), 96000, (float)kSr, nullptr, 0, nullptr, 16, nullptr, kCap);
    a.muted = true;
    int pt = -1;
    Drop(a, nullptr, 0, Wave(48000, 330.f, 0.6f), nullptr, &pt);
    a.CallSlice(pt, 1.f, false, false, false);
    float ol[48], orr[48], ref[48];
    for(int b = 0; b < 4; b++) a.Process(ol, orr, 48);
    bb::Engine c = a;   /* a twin that stays mono */
    for(int i = 0; i < a.freePos; i++) R2[(size_t)i] = L2[(size_t)i];
    CHECK(a.AdoptR(R2.data()) && a.bufR == R2.data() && !a.AdoptR(R2.data()), "AdoptR once (a second is refused)");
    bool same = true, lr = true;
    for(int b = 0; b < 30; b++)
    {
        a.Process(ol, orr, 48); c.Process(ref, 48);
        for(int i = 0; i < 48; i++) { same = same && ol[i] == ref[i]; lr = lr && orr[i] == ol[i]; }
    }
    CHECK(same && lr, "after AdoptR: L renders exactly as the mono twin, R = L, through the sounding voice");
    CHECK(a.RecordedEntry(0) == pt && a.UndoDepth() == 1, "the pad and the undo history stay");
}

/* ── 6. the unsaved note, the stub ──────────────────────────────────── */
static void TestUnsaved()
{
    SECTION("KitUnsaved and the empty kit's stub");
    static std::vector<float> L(kCap), R(kCap);
    bb::Engine e; e.Init(19); e.SetUndoJournal(&gJ);
    EmptyKit(e, L.data(), R.data());
    CHECK(e.bufLen == bb::kPadStubLen && e.nSlices == 16 && e.bufR == R.data() && e.freePos == bb::kPadStubLen, "the stub: ¼ s, 16 slices");
    CHECK(bb::IsEmptyKit(L.data(), e.bufLen) && !bb::IsEmptyKit(L.data(), e.bufLen - 1), "IsEmptyKit: exactly the stub's length of silence");
    CHECK(!bb::KitUnsaved(e, 0, true), "an empty kit with no pads: nothing to lose");
    Drop(e, R.data(), 0, Wave(4800, 300.f, 0.5f), nullptr);
    CHECK(bb::KitUnsaved(e, 0, true), "a pad: unsaved");
    CHECK(!bb::KitUnsaved(e, e.freePos, true), "saved (the set's WAV holds it)");
    Drop(e, R.data(), 1, Wave(4800, 300.f, 0.5f), nullptr);
    CHECK(bb::KitUnsaved(e, e.freePos - 4800, true), "a pad since the save: unsaved");
    L[100] = 0.001f;
    CHECK(!bb::IsEmptyKit(L.data(), bb::kPadStubLen), "a stub with a sound in it is a break");
    bb::Engine b; b.Init(1);
    b.SetMaterial(L.data(), 48000, (float)kSr, nullptr, 0, nullptr, 16, nullptr, kCap);
    CHECK(!bb::KitUnsaved(b, 48000, false) && bb::KitUnsaved(b, 0, false), "a card break: saved; a take (nothing on the card): unsaved");
}

/* ── 7. the loader's same-rate path ──────────────────────────────────── */
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
static void TestLoader()
{
    SECTION("the loader: a 48 kHz file decodes straight into the plane (no staging), 44.1 kHz resamples");
    const int n = 48000 * 3;
    const std::vector<float> l = Wave(n, 220.f, 0.5f), r = Wave(n, 330.f, -0.4f);
    bbwav::WriteSpec w; w.left = l.data(); w.right = r.data(); w.len = n; w.sr = 48000; w.fmt = bbwav::kPcm16;
    std::vector<uint8_t> b; static uint8_t staging[4096];
    CHECK(bbwav::Write({MemSink, &b}, w, staging, sizeof staging), "a 3 s stereo 48 kHz WAV");
    const std::string p48 = gOut + "/pads_48.wav";
    CHECK(WriteFile(p48, b), "written");
    std::vector<float> stg(1000), stgR(1000), out((size_t)n), outR((size_t)n);
    int cues[8], nc = 0, err = 0; bool gotR = false;
    const int got = bbwav::Load(p48.c_str(), stg.data(), (int)stg.size(), out.data(), (int)out.size(), 48000.f, 45, cues, &nc, 8, &err, outR.data(), stgR.data(), &gotR);
    bool exact = got == n;
    for(int i = 0; i < n && exact; i++) exact = out[(size_t)i] == l[(size_t)i] && outR[(size_t)i] == r[(size_t)i];
    CHECK(got == n && gotR && err == 0 && exact, "3 s through a 1000-sample staging: whole, both planes bit-exact (n %d)", got);
    const int capd = bbwav::Load(p48.c_str(), stg.data(), (int)stg.size(), out.data(), 48000, 48000.f, 45, cues, &nc, 8, &err);
    CHECK(capd == 48000, "capped at the output plane (a 17 s load into a 45 s plane: kLoadMax)");
    const int capS = bbwav::Load(p48.c_str(), stg.data(), (int)stg.size(), out.data(), (int)out.size(), 48000.f, 2, cues, &nc, 8, &err);
    CHECK(capS == 96000, "and at maxSec (%d)", capS);
    // 44.1 kHz: through the staging, which caps it
    w.sr = 44100; b.clear(); bbwav::Write({MemSink, &b}, w, staging, sizeof staging);
    const std::string p44 = gOut + "/pads_44.wav";
    WriteFile(p44, b);
    std::vector<float> big((size_t)n);
    const int g44 = bbwav::Load(p44.c_str(), big.data(), (int)big.size(), out.data(), (int)out.size(), 48000.f, 45, cues, &nc, 8, &err);
    CHECK(g44 == (int)((float)n * (48000.f / 44100.f)) || g44 == n, "44.1 kHz resampled (n %d)", g44);
    std::vector<float> mid(30000);
    const int gs = bbwav::Load(p44.c_str(), mid.data(), (int)mid.size(), out.data(), (int)out.size(), 48000.f, 45, cues, &nc, 8, &err);
    CHECK(gs == (int)(30000.f * (48000.f / 44100.f)), "44.1 kHz is capped by the staging, as ever (n %d)", gs);
}

/* ── 8. set round trips at the 45 s planes ───────────────────────────── */
// Save e (planes L, R) as <name>: SetFromEngine → SetWriter::Wav + Json to gOut. Returns the set's frames.
static int SaveKit(const bb::Engine& e, const char* name, bb::SetInfo& s)
{
    bb::Looper none;
    new(&s) bb::SetInfo();
    bb::SetFromEngine(e, none, s, e.bufR != nullptr, kSr);
    strcpy(s.name, name); s.bars = 1; s.bpm = e.NativeBpm(); s.nOptions = 0;
    std::vector<uint8_t> wav, js;
    static uint8_t staging[16384];
    static char json[bb::kSetJsonMax];
    if(!bb::SetWriter::Wav({MemSink, &wav}, s, e.buf, e.bufR, staging, sizeof staging)) return -1;
    if(!bb::SetWriter::Json({MemSink, &js}, s, json, sizeof json)) return -2;
    if(!WriteFile(gOut + "/" + name + ".wav", wav) || !WriteFile(gOut + "/" + name + ".json", js)) return -3;
    return s.len;
}
// The firmware's LoadSet + ServiceSets, minus the card: the JSON, the WAV straight into fresh 45 s planes (a small
// staging: the same-rate path), PlanSet, SetMaterial, CommitSetRecordings, SetKeyTable.
static bool LoadKit(bb::Engine& e, float* L, float* R, const char* name, bool* stub)
{
    std::vector<uint8_t> js;
    {
        FILE* f = fopen((gOut + "/" + name + ".json").c_str(), "rb"); if(!f) return false;
        int c; while((c = fgetc(f)) != EOF) js.push_back((uint8_t)c);
        fclose(f);
    }
    static bb::SetInfo t; static bb::SetPlan p;
    if(!bb::SetReader::Json((const char*)js.data(), js.size(), t)) return false;
    static std::vector<float> stg(4096), stgR(4096);
    int cues[128], nc = 0, err = 0; bool gotR = false;
    const int len = bbwav::Load((gOut + "/" + name + ".wav").c_str(), stg.data(), (int)stg.size(), L, kCap, 48000.f, 45, cues, &nc, 128, &err,
                                t.stereo ? R : nullptr, t.stereo ? stgR.data() : nullptr, &gotR);
    if(len <= 0 || len < t.len || !bb::PlanSet(t, p)) { printf("  load: len %d (want %d) err %d\n", len, t.len, err); return false; }
    *stub = bb::IsEmptyKit(L, p.material);
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
    return true;
}
// Every key called in turn (one-shot, then held) — both engines render the same bits.
static bool SameKeys(bb::Engine& a, bb::Engine& b, int blocks)
{
    float al[48], ar[48], bl[48], br[48];
    a.muted = b.muted = true;
    for(int blk = 0; blk < blocks; blk++)
    {
        if(blk % 20 == 0) { const int k = (blk / 20) % 24; a.CallSlice(a.KeySlice(k), 1.f, false, false, false, 1.f, (blk / 20) & 1, k); b.CallSlice(b.KeySlice(k), 1.f, false, false, false, 1.f, (blk / 20) & 1, k); }
        a.Process(al, ar, 48); b.Process(bl, br, 48);
        if(memcmp(al, bl, sizeof al) || memcmp(ar, br, sizeof ar)) { printf("  differ at block %d\n", blk); return false; }
    }
    return true;
}
static void TestRoundTrips()
{
    SECTION("set round trips at the 45 s planes: a pads-only kit, a 17 s break plus pads");
    static std::vector<float> L(kCap), R(kCap), L2(kCap), R2(kCap);
    static bb::SetInfo s;
    {
        bb::Engine e; e.Init(23); e.SetUndoJournal(&gJ);
        EmptyKit(e, L.data(), nullptr);   /* a mono empty kit */
        CHECK(Drop(e, nullptr, 0, Wave(9600, 60.f, 0.9f), nullptr) == 0, "pad 0: a mono kick");
        /* a stereo drop onto the mono kit: the shell copies L into R over [0, freePos), converts, adopts */
        const std::vector<float> sl = Wave(24000, 440.f, 0.5f), sr = Wave(24000, 660.f, 0.4f);
        for(int i = 0; i < e.freePos; i++) R[(size_t)i] = L[(size_t)i];
        const int start = e.freePos;
        const std::vector<uint8_t> by = Pcm(sl, &sr);
        memcpy(L.data() + start, by.data(), by.size());
        bb::PcmToPlanes(L.data(), R.data(), start, 24000, 2);
        CHECK(e.AdoptR(R.data()), "the kit turns stereo");
        bb::EditReq q; q.op = bb::EditOp::Record; q.key = 5; q.start = start; q.len = 24000;
        CHECK(e.ApplyEdit(q) == bb::EditStatus::Ok, "pad 5: a stereo pad");
        CHECK(Drop(e, R.data(), 23, Wave(48000 * 10, 110.f, 0.3f), nullptr) == 0, "pad 23: ten seconds");
        CHECK(Drop(e, R.data(), 5, Wave(4800, 990.f, 0.3f), nullptr) == 0, "pad 5 replaced (the stereo one held)");
        const int frames = SaveKit(e, "pads_only", s);
        CHECK(frames == e.freePos && s.stereo && s.material == bb::kPadStubLen && s.nGrid == 16, "saved: %d frames, stereo, the stub as the material", frames);
        bb::Engine e2; e2.Init(23);
        bool stub = false;
        CHECK(LoadKit(e2, L2.data(), R2.data(), "pads_only", &stub), "loaded back");
        CHECK(stub && e2.bufLen == bb::kPadStubLen && e2.bufR == R2.data() && e2.freePos == e.freePos, "an empty kit again: the stub, stereo, freePos %d", e2.freePos);
        bool keys = true;
        for(int k = 0; k < 24; k++) keys = keys && (e2.RecordedEntry(k) >= 0) == (e.RecordedEntry(k) >= 0)
                                       && e2.slices[e2.KeySlice(k)].start == e.slices[e.KeySlice(k)].start && e2.slices[e2.KeySlice(k)].len == e.slices[e.KeySlice(k)].len;
        CHECK(keys && bb::PadMask(e2) == ((1u << 0) | (1u << 5) | (1u << 23)), "the same pads on the same keys");
        CHECK(bb::PadRoomOf(e2).held == bb::PadRoomOf(e).held && bb::PadRoomOf(e2).held == 24000, "the held region came along (the set is [0, freePos) whole): not reclaimed");
        CHECK(SameKeys(e, e2, 24 * 20), "every key renders the same bits, both planes");
    }
    {
        bb::Engine e; e.Init(29); e.SetUndoJournal(&gJ);
        const int brk = 48000 * 17;
        const std::vector<float> b = Wave(brk, 70.f, 0.5f), bR = Wave(brk, 71.f, 0.5f);
        memcpy(L.data(), b.data(), b.size() * sizeof(float)); memcpy(R.data(), bR.data(), bR.size() * sizeof(float));
        int onsets[16]; for(int i = 0; i < 16; i++) onsets[i] = i * (brk / 16);
        e.SetMaterial(L.data(), brk, 48000.f, onsets, 16, nullptr, 32, R.data(), kCap);
        CHECK(bb::PadRoomOf(e).free == (uint32_t)(28 * 48000), "a 17 s break leaves 28 s for pads");
        int k = 0;
        while(bb::PadRoomOf(e).free >= 48000 * 3u && k < 24) { CHECK(Drop(e, R.data(), k, Wave(48000 * 3, 200.f + 50.f * (float)k, 0.4f), (k & 1) ? nullptr : nullptr) == 0, "pad %d", k); k++; }
        const uint32_t left = bb::PadRoomOf(e).free;
        CHECK(Drop(e, R.data(), k, Wave((int)left, 500.f, 0.2f), nullptr) == 0 && bb::PadRoomOf(e).free == 0 && e.freePos == kCap, "the last pad takes the room to the sample: freePos = bufCap");
        CHECK(Drop(e, R.data(), k + 1, Wave(480, 500.f, 0.2f), nullptr) == 106, "then nothing more fits: TOO_LARGE");
        const int frames = SaveKit(e, "break_pads", s);
        CHECK(frames == kCap && s.material == brk && s.nSlices == 32 + k + 1, "saved: the whole 45 s plane (%d frames), %d entries", frames, s.nSlices);
        bb::Engine e2; e2.Init(29);
        bool stub = true;
        CHECK(LoadKit(e2, L2.data(), R2.data(), "break_pads", &stub), "a 45 s set loads through a 4096-sample staging");
        CHECK(!stub && e2.bufLen == brk && e2.freePos == kCap && e2.nTable == e.nTable && e2.nSlices == 32, "the break, the pads, freePos at the cap");
        bool same = true;
        for(int i = 0; i < kCap && same; i++) same = L2[(size_t)i] == L[(size_t)i] && R2[(size_t)i] == R[(size_t)i];
        CHECK(same, "both planes bit-identical after the round trip");
        CHECK(SameKeys(e, e2, 24 * 20), "every key renders the same bits");
    }
}

int main(int argc, char** argv)
{
    if(argc > 1) gOut = argv[1];
    TestReceiver();
    TestConvert();
    TestCommit();
    TestRoom();
    TestRateAndPlanes();
    TestUnsaved();
    TestLoader();
    TestRoundTrips();
    printf("\npads_check: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
