#pragma once
// PADS — the pure pieces of a sample dropped onto a pad, and of a kit with no
// break (docs/packages/pads.md). The shell's plumbing (the 0x68 command, the
// state tail, the main-loop commit) is src/audiothurgist.cpp; the engine's
// half is EditOp::Record (core/bb_edit.h), Engine::AdoptR and Engine::OwnRate
// (core/bb_engine.h). Host-tested in tests/pads_check.cpp.
//
//   * A drop is PCM16, 48 kHz, mono or stereo (interleaved L R), received in
//     order (bb::UploadRx, core/bb_card.h) straight into the L plane past
//     freePos — where nothing reads: every table entry ends at or before
//     freePos, a take into a key is refused while a drop is open, GET_WAVE
//     and the audition stop at freePos. A stereo frame is 4 bytes and a mono
//     sample 2, so the bytes always fit the floats' room.
//   * The commit converts the bytes to floats IN PLACE (PcmToPlanes: stereo
//     forwards — frame i's four bytes become float i; mono backwards — float i
//     covers samples 2i and 2i + 1, which a backward walk has already read),
//     writes the R plane, analyses the head, and posts EditOp::Record: one
//     undo step, the key owns the region, freePos moves past it.
//   * Room: what is left past freePos, the pads' whole share (the plane past
//     the break), and what replaced recordings still hold below freePos — the
//     held memory a reclaim gives back (core/bb_reclaim.h: the kept runs moved
//     down, every position following; a set save writes the live spans only).
//   * An empty kit: kStubLen samples of digital silence as the "break" — 16
//     grid slices of 750, the shortest material the loader takes back (¼ s) —
//     so every path that wants material (the grid, the clock, the set format)
//     has one, and the pads get the rest.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "bb_engine.h"
#include "bb_yield.h"

namespace bb {

constexpr int kPadStubLen     = 12000;    // an empty kit's silent material: ¼ s, the loader's floor (16 × 750)
constexpr int kPadStubSlices  = 16;
constexpr int kPadAnalyseMax  = 48000;    // a pad's class / salience / density / hit come from its first second
constexpr uint8_t kPadMaxCh   = 2;

/* 0x68 PADS op state (state tail 293) and the last op's result (295) */
enum PadState : uint8_t { kPadIdle = 0, kPadReceiving = 1, kPadCommitting = 2, kPadClearing = 3, kPadReclaiming = 4 /* RECLAIM: op 4 */ };
enum PadResult : uint8_t
{
    kPadOk = 0, kPadNoRoom = 1, kPadBusy = 2, kPadBadArgs = 3, kPadFull = 4, kPadTimeout = 5,
    kPadAborted = 6, kPadMoved = 7,   // the planes changed under the drop (a load, a re-apply): nothing committed
};

// Bytes of a drop: frames × channels × 2. 0 for a bad channel count.
inline uint32_t PadBytes(uint32_t frames, int ch)
{
    return (ch == 1 || ch == 2) ? frames * (uint32_t)ch * 2u : 0u;
}

// BEGIN's check: kStOk (0), kStBadArgs (2: key, channels, fewer than kRecMin frames), kStTooLarge (6: more than
// `room` frames — the reply carries the room either way).
inline uint8_t PadBeginCheck(int key, int ch, uint32_t frames, uint32_t room)
{
    if(key < 0 || key >= Engine::kKeys || (ch != 1 && ch != 2) || frames < (uint32_t)Engine::kRecMin) return 2u;
    if(frames > room) return 6u;
    return 0u;
}

// The drop's bytes (PCM16 LE, interleaved when ch == 2) lying in L's memory from L + start → floats in
// L[start, start + frames), and R[start, …) when R is given: a stereo drop's right channel, a mono drop's copy of
// L. A stereo drop with no R plane folds to (L + R) / 2. Yields every 4096 frames. In place: the bytes are read
// before the float that overwrites them is written (see the header).
inline void PcmToPlanes(float* L, float* R, int start, int frames, int ch)
{
    if(!L || start < 0 || frames <= 0 || (ch != 1 && ch != 2)) return;
    const uint8_t* raw = (const uint8_t*)(L + start);
    float* l = L + start;
    float* r = R ? R + start : nullptr;
    constexpr float k = 1.f / 32768.f;
    auto s16 = [](const uint8_t* p) { return (int16_t)(uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); };
    if(ch == 2)
    {
        for(int i = 0; i < frames; i++)
        {
            const uint8_t* p = raw + (size_t)i * 4u;
            const float a = (float)s16(p) * k, b = (float)s16(p + 2) * k;
            if(r) { r[i] = b; l[i] = a; }
            else  l[i] = 0.5f * (a + b);
            if((i & 4095) == 4095) Yield();
        }
        return;
    }
    for(int i = frames - 1; i >= 0; i--)
    {
        const float a = (float)s16(raw + (size_t)i * 2u) * k;
        l[i] = a;
        if(r) r[i] = a;
        if((i & 4095) == 0) Yield();
    }
}

// The pads' room, in samples per plane.
struct PadRoom
{
    uint32_t free;    // bufCap − freePos: what the next drop (or take) can have
    uint32_t total;   // bufCap − bufLen: the pads' whole share
    uint32_t held;    // [bufLen, freePos) no live entry covers: replaced / undone / cleared recordings
};
// Live entries past the grid (not tombstones) covering [bufLen, freePos) — the union, so overlapping spans
// (a split recording, an entry re-spanned over another) count once. O(n²) in the table's ≤ 96 entries.
inline int LiveRecordedSamples(const Engine& e)
{
    int a[Engine::kMaxTable], b[Engine::kMaxTable], n = 0;
    const int lo = e.bufLen, hi = e.freePos;
    for(int t = e.nSlices; t < e.nTable && t < Engine::kMaxTable; t++)
    {
        if(e.IsTomb(t) || e.slices[t].len <= 0) continue;
        int s = e.slices[t].start, f = e.slices[t].start + e.slices[t].len;
        if(s < lo) s = lo;
        if(f > hi) f = hi;
        if(f <= s) continue;
        int j = n++;
        while(j > 0 && a[j - 1] > s) { a[j] = a[j - 1]; b[j] = b[j - 1]; j--; }
        a[j] = s; b[j] = f;
    }
    int sum = 0, curA = -1, curB = -1;
    for(int i = 0; i < n; i++)
    {
        if(curB < 0 || a[i] > curB) { if(curB > curA) sum += curB - curA; curA = a[i]; curB = b[i]; }
        else if(b[i] > curB) curB = b[i];
    }
    if(curB > curA) sum += curB - curA;
    return sum;
}
inline PadRoom PadRoomOf(const Engine& e)
{
    PadRoom r = {0u, 0u, 0u};
    if(!e.buf || e.bufCap <= 0) return r;
    r.free  = e.bufCap > e.freePos ? (uint32_t)(e.bufCap - e.freePos) : 0u;
    r.total = e.bufCap > e.bufLen ? (uint32_t)(e.bufCap - e.bufLen) : 0u;
    const int used = e.freePos - e.bufLen, live = LiveRecordedSamples(e);
    r.held = used > live ? (uint32_t)(used - live) : 0u;
    return r;
}

// (KitUnsaved — does the kit hold audio the card does not? — reads the card map: core/bb_reclaim.h.)

// An empty kit's material: kPadStubLen zeros into L (and R when given).
inline void MakeEmptyKit(float* L, float* R)
{
    if(L) memset(L, 0, sizeof(float) * (size_t)kPadStubLen);
    if(R) memset(R, 0, sizeof(float) * (size_t)kPadStubLen);
}
// A set's material is an empty kit's: exactly kPadStubLen samples of digital silence (a break the loader took is
// ¼ s at the least and never all zeros).
inline bool IsEmptyKit(const float* L, int len)
{
    if(!L || len != kPadStubLen) return false;
    for(int i = 0; i < len; i++) if(L[i] != 0.f) return false;
    return true;
}

// Keys holding a recording (a pad), as a bit mask.
inline uint32_t PadMask(const Engine& e)
{
    uint32_t m = 0;
    for(int k = 0; k < Engine::kKeys; k++) if(e.RecordedEntry(k) >= 0) m |= 1u << k;
    return m;
}

} // namespace bb
