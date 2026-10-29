#pragma once
// RECLAIM — held sample memory comes back (docs/packages/pads.md, "Held memory"). The pure half; the shell's
// plumbing (0x68 op 4, the main loop's copy, the audio thread's swap, the card map beside a set) is
// src/audiothurgist.cpp. Host-tested in tests/reclaim_check.cpp.
//
//   * A PLAN is the kept runs over [0, freePos): the break [0, bufLen), every live entry's span and — when Undo is
//     to keep what it needs — every span the undo journal could bring back; sorted, merged where they touch. Run i
//     moves down to dst[i]; the gaps between runs (replaced, deleted, undone recordings) are reclaimed and freePos
//     comes down by their total. Map(p) is the move as a function: monotone, the identity over the break, a run's
//     offset inside it, the next kept position inside a gap.
//   * The COPY is never in place: the kept runs go into the OTHER buffer (the load buffer, free whenever no load,
//     loop take or drop is in flight), so the planes the voices read stay whole until the audio thread swaps them in
//     at a block's start (ReclaimSwap) and moves every position with them in the same block: the table, its loop
//     points, freePos, each sounding voice. A voice reading a region that goes (a replaced pad still ringing, an
//     audition across a gap) is faded first (ReclaimFade, the choke geometry) and cut at the swap if it still sounds.
//   * UNDO: the journal's records hold spans. Mode kReclaimFree keeps every span a record holds, so the history is
//     whole and only remapped; mode kReclaimAll keeps only what is live and drops the steps that would bring back a
//     reclaimed sound — the oldest steps through the newest such step in the undo half, the redo half from the first
//     such step — and remaps the rest (ReclaimPlanJournal / ReclaimJournal, on the control thread once the swap is
//     done: nothing edits in between).
//   * The CARD MAP: where each live span lies in the card's copy of the kit (a set's WAV). A set save writes only the
//     live runs, packed (CardMap::FromPlan, the WAV through the map's segments); a reclaim moves the live side
//     (Follow); the autosave and the unsaved note read positions through it (ToCard), so the JSON always describes
//     the WAV on the card whatever the memory did since.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "bb_engine.h"
#include "bb_yield.h"

namespace bb {

constexpr uint8_t kReclaimFree = 0;   // what nothing needs — not even Undo: the history stays whole
constexpr uint8_t kReclaimAll  = 1;   // everything held: Undo forgets the steps that need a reclaimed sound

// Trivially constructible on purpose (the shell keeps it in SDRAM, NOLOAD).
struct ReclaimPlan
{
    static constexpr int kMaxRuns = 128;
    int     nRun;
    int     a[kMaxRuns], b[kMaxRuns], dst[kMaxRuns];   // kept [a, b) → [dst, dst + b − a)
    int     bufLen, oldFree, newFree;
    uint8_t gen;          // Engine::tableGen the plan was made for (the swap refuses any other)
    uint8_t mode;         // kReclaimFree · kReclaimAll
    bool    undoLive;     // the journal was live at the plan (a stale one is cleared at the swap)
    int     keepFrom, keepTo, dropSteps;   // the journal: records [keepFrom, keepTo) survive; the steps that go

    int Reclaimed() const { return oldFree > newFree ? oldFree - newFree : 0; }
    // the run holding p (a[i] <= p < b[i]), else the last run before it, else −1
    BB_COLD int RunAt(int p) const
    {
        int lo = 0, hi = nRun - 1, r = -1;
        while(lo <= hi) { const int m = (lo + hi) >> 1; if(a[m] <= p) { r = m; lo = m + 1; } else hi = m - 1; }
        return r;
    }
    BB_COLD int Map(int p) const
    {
        if(p >= oldFree) return p - Reclaimed();
        const int i = RunAt(p);
        if(i < 0) return p;
        return p < b[i] ? dst[i] + (p - a[i]) : dst[i] + (b[i] - a[i]);
    }
    // [lo, hi) lies inside one kept run (an empty span anywhere is inside)
    BB_COLD bool Inside(int lo, int hi) const
    {
        if(hi <= lo) return true;
        const int i = RunAt(lo);
        return i >= 0 && lo < b[i] && hi <= b[i];
    }
    // keep [lo, hi) (clamped to [0, oldFree)); merges with what it touches. Past kMaxRuns a span that touches
    // nothing joins the run before it (the gap between them is kept too: less is reclaimed, nothing breaks).
    BB_COLD void Add(int lo, int hi)
    {
        if(lo < 0) lo = 0;
        if(hi > oldFree) hi = oldFree;
        if(hi <= lo) return;
        int i = 0;
        while(i < nRun && b[i] < lo) i++;
        if(i == nRun || a[i] > hi)
        {
            if(nRun < kMaxRuns)
            {
                for(int j = nRun; j > i; j--) { a[j] = a[j - 1]; b[j] = b[j - 1]; }
                a[i] = lo; b[i] = hi; nRun++;
                return;
            }
            if(i == 0) { a[0] = lo; return; }
            i--;
        }
        if(lo < a[i]) a[i] = lo;
        if(hi > b[i]) b[i] = hi;
        int j = i + 1;
        while(j < nRun && a[j] <= b[i]) { if(b[j] > b[i]) b[i] = b[j]; j++; }
        const int gone = j - (i + 1);
        if(gone > 0)
        {
            for(int k = i + 1; k + gone < nRun; k++) { a[k] = a[k + gone]; b[k] = b[k + gone]; }
            nRun -= gone;
        }
    }
    void Finish()
    {
        int at = 0;
        for(int i = 0; i < nRun; i++) { dst[i] = at; at += b[i] - a[i]; }
        newFree = nRun > 0 ? at : oldFree;
    }
};

inline bool ReclaimJournalLive(const Engine& e) { return e.undo && e.undo->gen == e.tableGen && e.undo->count > 0; }
// Every span the live journal can bring back — of each record, [start, start + len) of every entry past the grid it
// holds inside its own nTable (a slot past it is not live in the state the record restores), a tombstone never; a
// NUDGE_ALL record holds starts only, beside the length the slot will have when the record is restored, so the undo
// half is walked back from the cursor and the redo half forward from it, each slot's length followed through the
// records on the way. `add`: each span joins the plan's kept runs (kReclaimFree). Else (kReclaimAll) the walk finds
// the records that need a span the plan does not keep: keepFrom past the newest of them in the undo half, keepTo at
// the first in the redo half. One body for both (the module's code lives in SRAM).
BB_COLD inline void ReclaimJournalSpans(const Engine& e, ReclaimPlan& p, bool add)
{
    if(!ReclaimJournalLive(e)) return;
    const UndoJournal& j = *e.undo;
    int len[Engine::kMaxTable];
    for(int pass = 0; pass < 2; pass++)
    {
        for(int t = 0; t < Engine::kMaxTable; t++) len[t] = t < e.nTable ? e.slices[t].len : 0;
        const int n0 = pass == 0 ? j.cur : j.count - j.cur;
        for(int k = 0; k < n0; k++)
        {
            const int i = pass == 0 ? j.cur - 1 - k : j.cur + k;
            const UndoRec& r = j.Slot(i);
            if(r.kind == UndoRec::kKeyP) continue;
            const bool starts = r.kind == UndoRec::kStarts;
            const int n = starts || r.kind == UndoRec::kSnap ? (r.n < Engine::kMaxTable ? r.n : Engine::kMaxTable) : (r.n < 2 ? r.n : 2);
            for(int q = 0; q < n; q++)
            {
                int t, lo, l;
                if(starts) { t = r.first + q; lo = r.u.nudge.start[q]; l = t < Engine::kMaxTable ? len[t] : 0; }
                else { const UndoEntry& u = r.u.e[q]; t = u.idx; lo = u.start; l = (u.flags & Engine::kFlagTomb) ? 0 : u.len; }
                if(t < e.nSlices || t >= r.nTable || t >= Engine::kMaxTable || l <= 0) continue;
                if(add) p.Add(lo, lo + l);
                else if(!p.Inside(lo, lo + l)) { if(i < j.cur) { if(i + 1 > p.keepFrom) p.keepFrom = i + 1; } else if(i < p.keepTo) p.keepTo = i; }
            }
            if(!starts) for(int q = 0; q < n; q++) if(r.u.e[q].idx < Engine::kMaxTable) len[r.u.e[q].idx] = r.u.e[q].len;
        }
    }
}

// The plan: the break, the live entries and (kReclaimFree) every span the live journal holds. False when there is
// no material (nothing to keep or move). CONTROL thread; reads the table and the journal, writes nothing but `p`.
BB_COLD inline bool BuildReclaimPlan(const Engine& e, uint8_t mode, ReclaimPlan& p)
{
    p.nRun = 0; p.mode = mode; p.gen = e.tableGen;
    p.bufLen = e.bufLen; p.oldFree = e.freePos; p.newFree = e.freePos;
    p.undoLive = ReclaimJournalLive(e);
    p.keepFrom = 0; p.keepTo = p.undoLive ? e.undo->count : 0; p.dropSteps = 0;
    if(!e.buf || e.bufLen <= 0 || e.freePos < e.bufLen) return false;
    p.Add(0, e.bufLen);
    for(int t = e.nSlices; t < e.nTable && t < Engine::kMaxTable; t++)
        if(!e.IsTomb(t) && e.slices[t].len > 0) p.Add(e.slices[t].start, e.slices[t].start + e.slices[t].len);
    if(mode == kReclaimFree) ReclaimJournalSpans(e, p, true);
    p.Finish();
    return true;
}

// kReclaimAll: which records survive. The undo half keeps the steps newer than the newest record that needs a
// reclaimed span; the redo half keeps the steps before the first such record. A record "needs" a span that does not
// lie inside one kept run. kReclaimFree keeps them all by construction (its plan kept every span they hold).
BB_COLD inline void ReclaimPlanJournal(const Engine& e, ReclaimPlan& p)
{
    if(!p.undoLive) { p.keepFrom = p.keepTo = 0; p.dropSteps = 0; return; }
    const UndoJournal& j = *e.undo;
    p.keepFrom = 0; p.keepTo = j.count;
    if(p.mode == kReclaimAll)
    {
        ReclaimJournalSpans(e, p, false);
        while(p.keepFrom < j.cur && !j.Slot(p.keepFrom).head) p.keepFrom++;                   // the rest of that step goes with it
        while(p.keepTo < j.count && p.keepTo > j.cur && !j.Slot(p.keepTo).head) p.keepTo--;   // back to the step's head
    }
    p.dropSteps = (p.keepFrom > 0 ? j.Steps(0, p.keepFrom) : 0) + (p.keepTo < j.count ? j.Steps(p.keepTo, j.count) : 0);
}

// The kept runs from `src` into `dst` (another buffer of the same capacity), each to its new place. Yields every
// 16 Ki samples. CONTROL thread: `src` is the playing plane (read only), `dst` the free one.
BB_COLD inline void ReclaimCopy(const ReclaimPlan& p, const float* src, float* dst)
{
    if(!src || !dst) return;
    constexpr int kChunk = 16384;
    for(int i = 0; i < p.nRun; i++)
        for(int at = p.a[i]; at < p.b[i]; at += kChunk)
        {
            const int n = p.b[i] - at < kChunk ? p.b[i] - at : kChunk;
            memcpy(dst + p.dst[i] + (at - p.a[i]), src + at, sizeof(float) * (size_t)n);
            Yield();
        }
}

// AUDIO, at block start, every sounding voice against the plan. Its reach — from where it started (or where it is,
// if lower; a loop's start) to its read limit — inside one kept run: `swap` moves it with the run, else it is left.
// Outside (a replaced pad still ringing, an audition across a gap): without `swap` it fades (a held one lets go; a
// reversed one turns forward first, as a release does — value-continuous — so the ramp runs; one already in its
// ramp is left to finish); with `swap` it is cut, its last output carried on as the steal's ~1 ms bridge. Returns
// how many sound outside (the shell waits for 0 before the swap, a while at most). One body for both: SRAM.
BB_COLD inline int ReclaimVoices(Engine& e, const ReclaimPlan& p, bool swap)
{
    int outside = 0;
    for(int i = 0; i < Engine::kVoices; i++)
    {
        Voice& v = e.voices[i];
        if(!v.active) continue;
        int lo = v.start < (int)v.pos ? v.start : (int)v.pos;
        if(v.tail && (int)v.loopA < lo) lo = (int)v.loopA;
        const int hi = v.bound > v.end ? v.bound : v.end;
        if(p.Inside(lo, hi))
        {
            if(!swap) continue;
            const int d = p.Map(lo) - lo;
            v.pos += (double)d; v.start += d; v.natEnd += d; v.end += d; v.bound += d;
            v.loopA += (double)d; v.loopB += (double)d;
            continue;
        }
        outside++;
        if(swap)
        {
            v.clickComp += v.lastOut; v.clickCompR += v.lastOutR;
            v.lastOut = v.lastOutR = 0.f;
            v.active = false; v.held = false; v.key = -1; v.tail = 0;
        }
        else if(v.held || v.tail || v.rate < 0.f || v.pos < (double)v.natEnd)
        {
            v.held = false; v.key = -1;
            if(!v.tail && v.rate < 0.f) v.rate = -v.rate;
            v.Fade(v.epShape);
        }
    }
    return outside;
}
inline int ReclaimFade(Engine& e, const ReclaimPlan& p) { return ReclaimVoices(e, p, false); }

// AUDIO, at block start: the compacted planes in, every position moved with them — the entries past the grid (and
// their loop points), freePos, each sounding voice (ReclaimVoices). False, and nothing touched, when the table or the
// material moved since the plan (the control thread plans again). `R` = nullptr for mono material.
BB_COLD inline bool ReclaimSwap(Engine& e, const ReclaimPlan& p, float* L, float* R)
{
    if(!L || e.tableGen != p.gen || e.freePos != p.oldFree || e.bufLen != p.bufLen || e.recArmed || e.recPending) return false;
    for(int t = e.nSlices; t < Engine::kMaxTable; t++)
    {
        const int s = e.slices[t].start, len = e.slices[t].len;
        const bool live = t < e.nTable && !e.IsTomb(t) && len > 0;
        const int to = p.Map(s);
        if(live && e.loopState[t] == 1 && p.Inside(s, s + len)) { const double d = (double)(to - s); e.loopA[t] += d; e.loopB[t] += d; }
        else e.loopState[t] = 0;
        e.slices[t].start = to;
    }
    ReclaimVoices(e, p, true);
    e.freePos = p.newFree;
    e.buf  = L;
    e.bufR = e.bufR ? R : nullptr;
    e.BumpGen();
    return true;
}

// CONTROL, after the swap (nothing edits in between: the shell holds every edit off while a reclaim runs): the
// journal truncated to [keepFrom, keepTo) and every position it holds moved; then bound to the new generation. A
// journal that was stale at the plan is cleared — its gen could otherwise come round to the new one.
BB_COLD inline void ReclaimJournal(Engine& e, const ReclaimPlan& p)
{
    if(!e.undo) return;
    UndoJournal& j = *e.undo;
    if(!p.undoLive || p.keepTo <= p.keepFrom) { j.Clear(); return; }
    j.base  = (j.base + p.keepFrom) % UndoJournal::kDepth;
    j.cur   = j.cur < p.keepFrom ? 0 : (j.cur > p.keepTo ? p.keepTo : j.cur) - p.keepFrom;
    j.count = p.keepTo - p.keepFrom;
    for(int i = 0; i < j.count; i++)
    {
        UndoRec& r = j.Slot(i);
        if(r.kind == UndoRec::kStarts) { for(int k = 0; k < r.n; k++) if(r.first + k >= e.nSlices) r.u.nudge.start[k] = p.Map(r.u.nudge.start[k]); continue; }
        if(r.kind == UndoRec::kKeyP) continue;
        const int n = r.kind == UndoRec::kSnap ? (r.n < Engine::kMaxTable ? r.n : Engine::kMaxTable) : (r.n < 2 ? r.n : 2);
        for(int k = 0; k < n; k++) if(r.u.e[k].idx >= e.nSlices) r.u.e[k].start = p.Map(r.u.e[k].start);
    }
    j.armed = 0;
    j.gen   = e.tableGen;
}

/* ── the card map ───────────────────────────────────────────────────────── */

// Where the live planes' samples lie in the card's copy of the kit: segments live[i] … + seg[i] ↔ card[i] … ,
// sorted (in both), never overlapping. Trivially constructible (SDRAM, NOLOAD). `ok` false: a reclaim split it past
// kMax pieces, and the card's copy can no longer be described (nothing maps: the kit reads unsaved until a save).
struct CardMap
{
    static constexpr int kMax = ReclaimPlan::kMaxRuns;
    int  n, len;          // segments; the card copy's frames per plane
    bool ok;
    int  live[kMax], card[kMax], seg[kMax];

    void Clear() { n = 0; len = 0; ok = true; }
    // the card holds [0, frames) as the planes do (a loaded set or break, a set just saved whole)
    void Identity(int frames)
    {
        Clear();
        if(frames <= 0) return;
        live[0] = card[0] = 0; seg[0] = frames; n = 1; len = frames;
    }
    // a save of the plan's kept runs, packed: the card's copy is the planes as the reclaim would leave them
    void FromPlan(const ReclaimPlan& p)
    {
        Clear();
        for(int i = 0; i < p.nRun && i < kMax; i++) { live[n] = p.a[i]; card[n] = p.dst[i]; seg[n] = p.b[i] - p.a[i]; n++; }
        len = p.nRun > 0 ? p.newFree : 0;
    }
    // [start, start + frames) in the card's copy, when it lies inside one segment
    BB_COLD bool ToCard(int start, int frames, int& out) const
    {
        if(!ok || frames < 0) return false;
        int lo = 0, hi = n - 1, r = -1;
        while(lo <= hi) { const int m = (lo + hi) >> 1; if(live[m] <= start) { r = m; lo = m + 1; } else hi = m - 1; }
        if(r < 0 || start + frames > live[r] + seg[r] || (frames == 0 && start >= live[r] + seg[r])) return false;
        out = card[r] + (start - live[r]);
        return true;
    }
    // `in` with its live side moved by a reclaim: each segment cut at the plan's runs, what fell in a gap dropped,
    // pieces contiguous on both sides joined again.
    BB_COLD void Follow(const CardMap& in, const ReclaimPlan& p)
    {
        Clear();
        ok = in.ok; len = in.len;
        if(!ok) return;
        for(int s = 0; s < in.n; s++)
        {
            const int s0 = in.live[s], s1 = in.live[s] + in.seg[s];
            for(int i = 0; i < p.nRun; i++)
            {
                const int lo = s0 > p.a[i] ? s0 : p.a[i], hi = s1 < p.b[i] ? s1 : p.b[i];
                if(hi <= lo) continue;
                const int l = p.Map(lo), c = in.card[s] + (lo - s0);
                if(n > 0 && live[n - 1] + seg[n - 1] == l && card[n - 1] + seg[n - 1] == c) { seg[n - 1] += hi - lo; continue; }
                if(n == kMax) { ok = false; n = 0; return; }
                live[n] = l; card[n] = c; seg[n] = hi - lo; n++;
            }
        }
    }
};

// A set save's layout: the break and the live spans, packed — the plan of a kReclaimAll (its runs; the journal is not
// consulted) as a map. The WAV is then written through m's segments and the JSON through m.
BB_COLD inline bool PackForCard(const Engine& e, ReclaimPlan& p, CardMap& m)
{
    if(!BuildReclaimPlan(e, kReclaimAll, p)) { m.Clear(); return false; }
    m.FromPlan(p);
    return m.n > 0;
}

// Does the kit hold audio the card does not? The material must lie in the card's copy where it lies in the planes
// (an empty kit's silent stub counts as on the card: it is nothing), and every live entry past the grid somewhere
// in it.
BB_COLD inline bool KitUnsaved(const Engine& e, const CardMap& m, bool stub)
{
    if(!e.buf) return false;
    if(!m.ok) return true;
    int c = 0;
    if(!stub && e.bufLen > 0 && !(m.ToCard(0, e.bufLen, c) && c == 0)) return true;
    for(int t = e.nSlices; t < e.nTable && t < Engine::kMaxTable; t++)
    {
        if(e.IsTomb(t) || e.slices[t].len <= 0) continue;
        const int s = e.slices[t].start, l = e.slices[t].len;
        if(stub && s + l <= e.bufLen) continue;
        if(!m.ToCard(s, l, c)) return true;
    }
    return false;
}

} // namespace bb
