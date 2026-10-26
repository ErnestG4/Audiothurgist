#pragma once
// EDIT (docs/packages/slice-edit-core.md): the bodies of Engine's slice
// editing — included at the foot of bb_engine.h, never on its own. Pure:
// no allocation, no I/O, no analysis (a new region's class / salience /
// density arrive in EditReq::meta from bb::AnalyseEntry on the control
// thread). Every op validates first and returns without touching anything
// on refusal; on success it refreshes what the geometry change made stale
// (loop points, the hit flag, the recorded-entry shadow, the gas well) and
// bumps tableGen. All of it runs on the AUDIO thread at block start.
#include <cstdint>
#include <cmath>

namespace bb {

/* The per-entry form of SetMaterial's hit rule: an onset inside the first
 * quarter of the span, or within 10 ms before its start. No onset table →
 * every slice is a hit (the pre-onset behaviour). ≤ nOnsetTab compares. */
inline bool Engine::OnsetHit(int start, int len) const
{
    if(!onsetsGiven) return true;
    const int pre = (int)(0.010f * sr);
    for(int o = 0; o < nOnsetTab; o++)
    {
        const int p = onsetTab[o];
        if(p >= start) { if(p - start <= len / 4) return true; }
        else if(start - p <= pre) return true;
    }
    return false;
}

/* ── EDIT-AUDITION: the TAP_MARK rule, pure (docs/packages/slice-edit-proto.md) ──
 * A tap while the break plays: the audio thread's material position at the
 * tap (`srcPos`), minus the reaction allowance (`allow` samples, the Tap
 * Allowance option), snapped to the nearest onset within ±`snap` samples
 * (the earlier onset wins a tie), else left raw. The marker runs to the next
 * onset after its start, capped at `maxLen` (the shell's 250 ms) and never
 * under `minLen` (kRecMin); a span past `limit` (the shell's freePos) is
 * shortened to it, and only when that leaves less than minLen is the start
 * pulled back. With limit < minLen the result is what ApplyEdit will refuse
 * (BadArgs) — the caller need not special-case it. */
struct TapMark { int start, len; bool snapped; };
inline TapMark TapMarkAt(int srcPos, int allow, int snap, const int* onsets, int n, int limit,
                         int maxLen = 12000, int minLen = Engine::kRecMin)
{
    TapMark m = {0, 0, false};
    int p = srcPos - allow;
    if(p < 0) p = 0;
    int best = -1, bestD = snap + 1;
    for(int i = 0; i < n; i++)
    {
        int d = onsets[i] - p; if(d < 0) d = -d;
        if(d < bestD) { bestD = d; best = i; }
    }
    if(best >= 0) { p = onsets[best]; m.snapped = true; }
    int len = maxLen;
    for(int i = 0; i < n; i++)
        if(onsets[i] > p && onsets[i] - p < len) len = onsets[i] - p;
    if(len < minLen) len = minLen;
    if(p + len > limit)
    {
        len = limit - p;
        if(len < minLen) { len = minLen; p = limit - len; if(p < 0) { p = 0; len = limit; } }
    }
    m.start = p; m.len = len;
    return m;
}

/* ── BETA: AUTOSLICE's draw (docs/packages/beta-fw.md) ─────────────────────
 * Weighted sampling without replacement over the onsets, by salience
 * (Efraimidis & Spirakis 2006): each onset gets key u^(1/w), u uniform in
 * (0, 1] and w its weight; the k largest keys are the sample. Compared as
 * ln(u) / w (the same order, no pow). u is a hash of (seed, the onset's
 * POSITION), so for one seed the order is fixed and the first k are always
 * a subset of the first k + 1 — turning the count up adds the next drawn
 * onset, down removes the last one — and an onset keeps its key when others
 * join or leave the candidate list. A re-roll is a new seed. `w8` 0..255
 * (the shell's onset strength); 0 still weighs 1/256, so every onset is
 * eventually drawn. order[0..n) = candidate indices, the sample's order;
 * ties keep the lower index first. O(n²) insertion, n ≤ 128, control thread. */
inline float AutoSliceKey(int pos, uint8_t w8, uint32_t seed)
{
    uint32_t x = seed ^ ((uint32_t)pos * 0x9E3779B9u);
    x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16;
    const float u = (float)((x >> 8) + 1u) * (1.f / 16777216.f);   // (0, 1]
    const float w = ((float)w8 + 1.f) * (1.f / 256.f);              // (0, 1]
    return logf(u) / w;                                             // ≤ 0; larger = drawn earlier
}
inline void AutoSliceOrder(const int* pos, const uint8_t* w8, int n, uint32_t seed, int* order)
{
    float key[Engine::kMaxOnsets];   // 512 B of stack
    if(n > Engine::kMaxOnsets) n = Engine::kMaxOnsets;
    for(int i = 0; i < n; i++)
    {
        const float k = AutoSliceKey(pos[i], w8[i], seed);
        int j = i;
        while(j > 0 && key[j - 1] < k) { key[j] = key[j - 1]; order[j] = order[j - 1]; j--; }
        key[j] = k; order[j] = i;
    }
}

/* ── BETA: a bulk change of the grey markers as the fewest ops ───────────
 * The target: grey markers at `starts[0..n)` (ascending), each spanning to
 * the next target (the last to the break's end; a span under kRecMin is
 * dropped, as FROM_ONSETS does). The plan against the table now: every grey
 * marker that starts at no target goes (tomb), a grey one already at a
 * target start keeps its slot and keys and only re-spans if its length
 * differs (set — the shell posts SetEntry with kKeep, so it stays grey), and
 * the rest are added (add). Orange markers, recordings, the grid: never
 * touched. Applying the plan leaves the grey set equal to the target, so a
 * count turned 5 → 6 → 5 posts one add + one re-span, then one tomb + one
 * re-span. Pure (reads the table), control thread. */
struct GreyPlan   // a POD (the shell keeps one in SDRAM): PlanGrey sets every count
{
    static constexpr int kMax = Engine::kMaxTable;
    int nTomb, tomb[kMax];
    int nSet, setT[kMax], setLen[kMax];
    int nAdd, addStart[kMax], addLen[kMax];
    int nTarget;   // spans the target keeps (≤ n)
};
inline void PlanGrey(const Engine& e, const int* starts, int n, GreyPlan& p)
{
    p.nTomb = p.nSet = p.nAdd = p.nTarget = 0;
    bool used[Engine::kMaxTable] = {};
    for(int j = 0; j < n; j++)
    {
        const int s = starts[j];
        const int end = j + 1 < n ? starts[j + 1] : e.bufLen;
        if(s < 0 || s >= e.bufLen || end - s < Engine::kRecMin) continue;
        if(p.nTarget >= GreyPlan::kMax) break;
        p.nTarget++;
        int hit = -1;
        for(int t = e.nSlices; t < e.nTable && hit < 0; t++)
            if(!used[t] && e.IsGrey(t) && e.slices[t].start == s) hit = t;
        if(hit >= 0)
        {
            used[hit] = true;
            if(e.slices[hit].len != end - s) { p.setT[p.nSet] = hit; p.setLen[p.nSet] = end - s; p.nSet++; }
        }
        else { p.addStart[p.nAdd] = s; p.addLen[p.nAdd] = end - s; p.nAdd++; }
    }
    for(int t = e.nSlices; t < e.nTable; t++)
        if(e.IsGrey(t) && !used[t]) p.tomb[p.nTomb++] = t;
}

/* ── BETA: markers ────────────────────────────────────────────────────── */
inline bool Engine::IsMarker(int t) const
{
    return t >= nSlices && t < nTable && !(tableFlags[t] & kFlagTomb) && tableKey[t] < 0 && slices[t].start < bufLen;
}
inline bool Engine::IsGrey(int t) const
{
    return IsMarker(t) && !(tableFlags[t] & kFlagOrange);
}
inline int Engine::MarkerCount() const
{
    int n = 0;
    for(int t = nSlices; t < nTable; t++) if(IsMarker(t)) n++;
    return n;
}
// TAP_MARK's dedupe: a mark within `tol` of a live marker's (or recording's)
// start selects it instead of stacking a new one. The nearest wins; −1 none.
inline int Engine::MarkerNear(int pos, int tol) const
{
    int best = -1, bestD = tol + 1;
    for(int t = nSlices; t < nTable; t++)
    {
        if(tableFlags[t] & kFlagTomb) continue;
        int d = slices[t].start - pos; if(d < 0) d = -d;
        if(d < bestD) { bestD = d; best = t; }
    }
    return best;
}
// KEYS_FROM_MARKERS (and Pads, whenever the markers change): the markers in
// time order (ties by index) → keys 0, 1, 2…; a key that owns a recording is
// skipped and keeps what it plays; keys past the markers go back to their
// span slice. `out` is the whole map (kKeys); returns how many keys got a
// marker. Pure: the shell posts it as SetKeyMap.
inline int Engine::KeysFromMarkers(int8_t* out) const
{
    int order[kMaxTable]; int n = 0;
    for(int t = nSlices; t < nTable; t++)
    {
        if(!IsMarker(t)) continue;
        int j = n++;
        while(j > 0 && slices[order[j - 1]].start > slices[t].start) { order[j] = order[j - 1]; j--; }
        order[j] = t;
    }
    int m = 0, given = 0;
    for(int k = 0; k < kKeys; k++)
    {
        if(RecordedEntry(k) >= 0) { out[k] = keySlice[k]; continue; }
        if(m < n) { out[k] = (int8_t)order[m++]; given++; }
        else out[k] = (int8_t)SpanSlice(k);
    }
    return given;
}
inline EditStatus Engine::EdSetKeyMap(const int8_t* keys)
{
    for(int k = 0; k < kKeys; k++)
        if(keys[k] >= 0 && (keys[k] >= nTable || (tableFlags[keys[k]] & kFlagTomb))) return EditStatus::BadArgs;
    for(int k = 0; k < kKeys; k++)
    {
        if(keys[k] >= 0) { keySlice[k] = keys[k]; continue; }
        const int r = RecordedEntry(k);
        keySlice[k] = (int8_t)(r >= 0 ? r : SpanSlice(k));
    }
    return EditStatus::Ok;
}

/* PADS (docs/packages/pads.md): a sample dropped onto a pad — CommitSlice's publication, through the journal so a
 * drop is one undo step. Key `key` owns [start, start + len): its recording's slot is reused in place (the old
 * region stays in the planes, orphaned — the room readout says so), else the first free slot; the entry is
 * white, carries `meta` (kMeta), and freePos moves past it. The span must start at or before freePos (the shell
 * wrote the samples at freePos, where nothing reads) and end inside bufCap. Undo puts the slot, the key map and
 * nTable back; freePos stays where it went, so the region a redo needs is still there. */
inline EditStatus Engine::EdRecord(const EditReq& r, int& idx)
{
    if(r.key < 0 || r.key >= kKeys) return EditStatus::BadArgs;
    if(r.start < 0 || r.len < kRecMin || r.start > freePos || r.start > bufCap || r.len > bufCap - r.start) return EditStatus::BadArgs;
    int t = RecordedEntry(r.key);
    if(t < 0) { t = FreeSlot(); if(t < 0) return EditStatus::Full; }
    const EntryMeta m = (r.flags & EditReq::kMeta) ? r.meta : EntryMeta{3, 0.f, 0.f, false};
    recMeta[t]    = m;
    slices[t]     = {r.start, r.len};
    tableKey[t]   = (int8_t)r.key;
    stepClass[t]  = m.cls;
    sliceSal[t]   = m.sal;
    sliceDens[t]  = m.dens;
    stepHit[t]    = m.hit;
    stepBands[t]  = 0;
    loopState[t]  = 0;
    tableFlags[t] = 0;   // a recording: neither orange nor a tombstone (a reused slot comes alive here)
    if(r.start + r.len > freePos) freePos = r.start + r.len;
    if(t == nTable) nTable = t + 1;
    keySlice[r.key] = (int8_t)t;
    idx = t;
    return EditStatus::Ok;
}

inline int Engine::FreeSlot() const
{
    for(int t = nSlices; t < nTable; t++)
        if(tableFlags[t] & kFlagTomb) return t;
    return nTable < kMaxTable ? nTable : -1;
}

/* After an entry's span changed: forget its loop points (FindLoop re-searches
 * at the next tail), re-derive its hit flag (a marker past the material has
 * no onsets there — its recorded flag stands), mirror it into recMeta so
 * SyncRecordedMeta re-asserts the new value, and re-centre the gas well of
 * a grid slice. */
inline void Engine::RefreshEntry(int t)
{
    if(t < 0 || t >= kMaxTable) return;
    loopState[t] = 0;
    const Slice& s = slices[t];
    if(t < nSlices)
    {
        stepHit[t] = OnsetHit(s.start, s.len);
        if(bufLen > 0) slicePhase[t] = ((float)s.start + 0.5f * (float)s.len) / (float)bufLen * 6.28318531f;
    }
    else
    {
        if(s.start < bufLen) stepHit[t] = OnsetHit(s.start, s.len);
        else stepHit[t] = recMeta[t].hit;
        recMeta[t].hit = stepHit[t];
    }
}

/* ── the boundary overlay ─────────────────────────────────────────────── */

// Remember the whole grid and which boundaries are orange, for re-application.
inline void Engine::SaveOverlay()
{
    ovN   = nSlices;
    ovLen = bufLen;
    ovOrange = 0;
    for(int i = 0; i < nSlices; i++)
    {
        ovStart[i] = slices[i].start;
        if(i > 0 && (tableFlags[i] & kFlagOrange)) ovOrange |= (uint64_t)1 << i;
    }
}

// SetMaterial, after the fresh grid and its hits: the same planes, length
// and count → every orange boundary goes back where the user put it,
// clamped left to right so each slice keeps kGridMin against its (already
// placed) predecessor and leaves kGridMin for every slice after it; the
// hits of the slices whose span changed are re-derived. Anything else
// (new material, another count, no edits) drops the overlay.
inline void Engine::ApplyOverlay(bool samePlanes)
{
    if(!(samePlanes && ovOrange != 0 && ovN == nSlices && ovLen == bufLen)) { ovOrange = 0; return; }
    if(kGridMin * nSlices > bufLen) { ovOrange = 0; return; }   // no room for any clamp to hold
    bool changed[kMaxSlices] = {};
    for(int i = 1; i < nSlices; i++)
    {
        if(!((ovOrange >> i) & 1)) continue;
        const int lo = slices[i - 1].start + kGridMin;
        const int hi = bufLen - kGridMin * (nSlices - i);
        int p = ovStart[i];
        if(p > hi) p = hi;
        if(p < lo) p = lo;
        if(p != slices[i].start) { slices[i].start = p; changed[i] = changed[i - 1] = true; }
        tableFlags[i] |= kFlagOrange;
    }
    for(int i = 0; i < nSlices; i++)
        slices[i].len = (i + 1 < nSlices ? slices[i + 1].start : bufLen) - slices[i].start;
    for(int i = 0; i < nSlices; i++)
        if(changed[i]) RefreshEntry(i);
    SaveOverlay();   // the clamped positions are the overlay now
}

/* ── the set loader's entry points (package EDIT-PERSIST) ─────────────── */

// The grid's starts wholesale: n == nSlices, starts[0] == 0, ascending by
// ≥ kGridMin, the last one ≥ kGridMin before bufLen. Boundaries that differ
// from the current grid become orange (a file carrying the computed grid
// is a no-op and leaves nothing orange). `flags` (EDIT-PERSIST, optional):
// the file's per-slice flags — an orange bit on a boundary that already sits
// where the file puts it is restored too (so the overlay pins it), boundary
// 0 never. false = refused, nothing written.
inline bool Engine::SetGridBoundaries(const int* starts, int n, const uint8_t* flags)
{
    if(!buf || !starts || n != nSlices || n < 1 || starts[0] != 0) return false;
    for(int i = 1; i < n; i++) if(starts[i] - starts[i - 1] < kGridMin) return false;
    if(bufLen - starts[n - 1] < kGridMin) return false;
    bool any = false, changed[kMaxSlices] = {};
    for(int i = 0; i < n; i++)
        if(starts[i] != slices[i].start) { slices[i].start = starts[i]; tableFlags[i] |= kFlagOrange; changed[i] = true; if(i > 0) changed[i - 1] = true; any = true; }
    bool anyFlag = false;
    if(flags)
        for(int i = 1; i < n; i++)
            if((flags[i] & kFlagOrange) && !(tableFlags[i] & kFlagOrange)) { tableFlags[i] |= kFlagOrange; anyFlag = true; }
    if(!any && !anyFlag) return true;
    if(any)
    {
        for(int i = 0; i < n; i++) slices[i].len = (i + 1 < n ? slices[i + 1].start : bufLen) - slices[i].start;
        for(int i = 0; i < n; i++) if(changed[i]) RefreshEntry(i);
    }
    SaveOverlay();
    BumpGen();
    return true;
}

// The key map wholesale: keys[k] = the entry key k plays, or −1 = its
// default (the key's own recording if it has one, else the span slice).
// false = an index ≥ nTable or a tombstone; nothing written then.
inline bool Engine::SetKeyTable(const int8_t* keys)
{
    if(!keys) return false;
    for(int k = 0; k < kKeys; k++)
        if(keys[k] >= 0 && (keys[k] >= nTable || (tableFlags[keys[k]] & kFlagTomb))) return false;
    for(int k = 0; k < kKeys; k++)
    {
        if(keys[k] >= 0) { keySlice[k] = keys[k]; continue; }
        const int r = RecordedEntry(k);
        keySlice[k] = (int8_t)(r >= 0 ? r : SpanSlice(k));
    }
    BumpGen();
    return true;
}

/* ── the ops ───────────────────────────────────────────────────────────── */

inline EditStatus Engine::EdMoveBoundary(int i, int pos)
{
    if(i <= 0 || i >= nSlices) return EditStatus::NotGrid;
    if(pos < 0 || pos >= bufLen) return EditStatus::BadArgs;
    const int lo = slices[i - 1].start + kGridMin;
    const int hi = slices[i].start + slices[i].len - kGridMin;
    if(pos < lo || pos > hi) return EditStatus::BadArgs;
    if(pos == slices[i].start) return EditStatus::Ok;   // nothing moves; still a user op (orange, gen)
    const int end       = slices[i].start + slices[i].len;
    slices[i - 1].len   = pos - slices[i - 1].start;
    slices[i].start     = pos;
    slices[i].len       = end - pos;
    RefreshEntry(i - 1);
    RefreshEntry(i);
    return EditStatus::Ok;
}

inline EditStatus Engine::EdSetEntry(int t, int start, int len)
{
    if(t < nSlices || t >= nTable) return EditStatus::NotMarker;
    if(tableFlags[t] & kFlagTomb) return EditStatus::Tomb;
    if(start < 0 || len < kRecMin || start > freePos || len > freePos - start) return EditStatus::BadArgs;
    slices[t] = {start, len};
    RefreshEntry(t);
    return EditStatus::Ok;
}

inline EditStatus Engine::EdAdd(const EditReq& r, int& idx)
{
    if(r.start < 0 || r.len < kRecMin || r.start > freePos || r.len > freePos - r.start) return EditStatus::BadArgs;
    const int t = FreeSlot();
    if(t < 0) return EditStatus::Full;
    const bool withMeta = (r.flags & EditReq::kMeta) != 0;
    recMeta[t]    = withMeta ? r.meta : EntryMeta{3, 0.f, 0.f, false};
    slices[t]     = {r.start, r.len};
    tableKey[t]   = -1;   // not a recording: no owner (a take into its key appends, it does not replace)
    stepClass[t]  = recMeta[t].cls;
    sliceSal[t]   = recMeta[t].sal;
    sliceDens[t]  = recMeta[t].dens;
    stepBands[t]  = 0;
    loopState[t]  = 0;
    tableFlags[t] = (uint8_t)(r.flags & EditReq::kOrange);   // clears a reused slot's tombstone
    if(withMeta) { stepHit[t] = r.meta.hit; recMeta[t].hit = r.meta.hit; }
    else RefreshEntry(t);
    if(t == nTable) nTable = t + 1;
    if(r.key >= 0 && r.key < kKeys) keySlice[r.key] = (int8_t)t;
    idx = t;
    return EditStatus::Ok;
}

inline EditStatus Engine::EdSplit(const EditReq& r, int& idx)
{
    const int t = r.t, pos = r.start;
    if(t < nSlices || t >= nTable) return EditStatus::NotMarker;
    if(tableFlags[t] & kFlagTomb) return EditStatus::Tomb;
    const Slice s = slices[t];
    if(pos - s.start < kRecMin || s.start + s.len - pos < kRecMin) return EditStatus::BadArgs;
    const int u = FreeSlot();
    if(u < 0) return EditStatus::Full;
    const bool withMeta = (r.flags & EditReq::kMeta) != 0;
    recMeta[u]    = withMeta ? r.meta : EntryMeta{stepClass[t], sliceSal[t], sliceDens[t], recMeta[t].hit};   // the right half inherits t's
    slices[u]     = {pos, s.start + s.len - pos};
    tableKey[u]   = -1;
    stepClass[u]  = recMeta[u].cls;
    sliceSal[u]   = recMeta[u].sal;
    sliceDens[u]  = recMeta[u].dens;
    stepBands[u]  = 0;
    loopState[u]  = 0;
    tableFlags[u] = kFlagOrange;
    if(withMeta) { stepHit[u] = r.meta.hit; recMeta[u].hit = r.meta.hit; }
    else RefreshEntry(u);
    if(u == nTable) nTable = u + 1;
    slices[t].len = pos - s.start;   // keys stay on t
    RefreshEntry(t);
    idx = u;
    return EditStatus::Ok;
}

// The marker Merge(t) swallows: the live one whose span ends at t's start,
// else the previous live one by index; −1 when there is none (or t is no
// live marker). Safe on any t: the journal asks before the op validates.
inline int Engine::MergeTarget(int t) const
{
    if(t < nSlices || t >= nTable || (tableFlags[t] & kFlagTomb)) return -1;
    int u = -1;
    for(int v = nSlices; v < nTable && u < 0; v++)   // the marker whose span ends at t's start
        if(v != t && !(tableFlags[v] & kFlagTomb) && slices[v].start + slices[v].len == slices[t].start) u = v;
    for(int v = t - 1; v >= nSlices && u < 0; v--)   // else the previous live marker
        if(!(tableFlags[v] & kFlagTomb)) u = v;
    return u;
}

inline EditStatus Engine::EdMerge(int t)
{
    if(t < nSlices || t >= nTable) return EditStatus::NotMarker;
    if(tableFlags[t] & kFlagTomb) return EditStatus::Tomb;
    const int u = MergeTarget(t);
    if(u < 0) return EditStatus::BadArgs;
    const int a = slices[u].start < slices[t].start ? slices[u].start : slices[t].start;
    const int eu = slices[u].start + slices[u].len, et = slices[t].start + slices[t].len;
    const int e = eu > et ? eu : et;
    slices[t] = {a, e - a};
    if(tableKey[t] < 0) tableKey[t] = tableKey[u];       // a recording merged in keeps its owner
    for(int k = 0; k < kKeys; k++) if(keySlice[k] == u) keySlice[k] = (int8_t)t;
    int8_t map[kMaxTable];
    for(int v = 0; v < kMaxTable; v++) map[v] = (int8_t)v;
    map[u] = (int8_t)t;
    looper.RemapSlices(map, nTable);                     // keyless loop events on u now play t
    for(int k = 0; k < nSlices; k++) { if(stepSrc[k] == u) stepSrc[k] = (int8_t)t; if(playSrc[k] == u) playSrc[k] = (int8_t)t; }
    tableKey[u]   = -1;
    slices[u].len = 0;
    tableFlags[u] = kFlagTomb;
    loopState[u]  = 0;
    stepHit[u]    = false;
    recMeta[u].hit = false;
    RefreshEntry(t);
    return EditStatus::Ok;
}

inline EditStatus Engine::EdTomb(int t)
{
    if(t < nSlices || t >= nTable) return EditStatus::NotMarker;
    if(tableFlags[t] & kFlagTomb) return EditStatus::Tomb;
    for(int k = 0; k < kKeys; k++) if(keySlice[k] == t) keySlice[k] = (int8_t)SpanSlice(k);
    int8_t map[kMaxTable];
    for(int v = 0; v < kMaxTable; v++) map[v] = (int8_t)v;
    map[t] = -1;
    looper.RemapSlices(map, nTable);                     // keyless loop events on t are dropped
    for(int k = 0; k < nSlices; k++)                     // a punched step of it becomes a rest
        if(stepSrc[k] == t || playSrc[k] == t)
        {
            if(stepSrc[k] == t) { stepSrc[k] = -1; stepCont[k] = false; stepRatN[k] = 0; }
            if(playSrc[k] == t) playSrc[k] = -1;
        }
    tableKey[t]    = -1;
    slices[t].len  = 0;   // the start is kept: the region stays in the buffer
    tableFlags[t]  = kFlagTomb;
    loopState[t]   = 0;
    stepHit[t]     = false;
    recMeta[t].hit = false;
    return EditStatus::Ok;
}

inline EditStatus Engine::EdSetKey(int key, int t)
{
    if(key < 0 || key >= kKeys || t < 0 || t >= nTable) return EditStatus::BadArgs;
    if(tableFlags[t] & kFlagTomb) return EditStatus::Tomb;
    keySlice[key] = (int8_t)t;
    return EditStatus::Ok;
}

inline EditStatus Engine::EdSwapKeys(int a, int b)
{
    if(a < 0 || a >= kKeys || b < 0 || b >= kKeys || a == b) return EditStatus::BadArgs;
    const int8_t ta = keySlice[a], tb = keySlice[b];
    keySlice[a] = tb;
    keySlice[b] = ta;
    // the recorded entries trade owners too (tableKey is "the key that recorded it")
    if(ta >= nSlices && ta < nTable && tableKey[ta] == a) tableKey[ta] = (int8_t)b;
    if(tb >= nSlices && tb < nTable && tableKey[tb] == b) tableKey[tb] = (int8_t)a;
    return EditStatus::Ok;
}

inline EditStatus Engine::EdNudge(int delta, int which)
{
    if(delta == 0) return EditStatus::BadArgs;
    if(which == 0)
    {
        if(nSlices < 2 || kGridMin * nSlices > bufLen) return EditStatus::BadArgs;
        for(int i = 1; i < nSlices; i++)
        {
            const int lo = slices[i - 1].start + kGridMin;
            const int hi = bufLen - kGridMin * (nSlices - i);
            int p = slices[i].start + delta;
            if(p > hi) p = hi;
            if(p < lo) p = lo;
            slices[i].start = p;
            tableFlags[i] |= kFlagOrange;
        }
        for(int i = 0; i < nSlices; i++)
            slices[i].len = (i + 1 < nSlices ? slices[i + 1].start : bufLen) - slices[i].start;
        for(int i = 0; i < nSlices; i++) RefreshEntry(i);
        return EditStatus::Ok;
    }
    if(which == 1)
    {
        int moved = 0;
        for(int t = nSlices; t < nTable; t++)
        {
            if(tableFlags[t] & kFlagTomb) continue;
            int p = slices[t].start + delta;
            const int hi = freePos - slices[t].len;
            if(p > hi) p = hi;
            if(p < 0) p = 0;
            slices[t].start = p;
            tableFlags[t] |= kFlagOrange;
            RefreshEntry(t);
            moved++;
        }
        return moved > 0 ? EditStatus::Ok : EditStatus::BadArgs;
    }
    return EditStatus::BadArgs;
}

/* AUDIO, at block start. Field use per op:
 *
 *   op            fields                       meta (kMeta)     needs
 *   SetBoundary   i, start = pos               → slice i        grid: 0 < i < nSlices; both neighbours keep kGridMin; pos in [0, bufLen)
 *   SetEntry      t, start, len                → t              marker, not a tombstone; [0, freePos), len ≥ kRecMin; overlaps allowed
 *   AddEntry      key (−1 none), start, len,   → the new entry  a free slot (tombstones reused); key assigned at once
 *                 flags b0 orange              (else class 3, sal 0, dens 0, hit from the onset table)
 *   Split         t, start = pos               → the new (right) half; the left keeps t's; both ≥ kRecMin; keys stay on t
 *   Merge         t                            → t              the marker ending at t's start, else the previous live one; keys / loop events re-point to t
 *   Tombstone     t                            —                marker; keys → their span slice; keyless loop events dropped; the region stays
 *   SetKey        key, t                       —                any live entry
 *   SwapKeys      a, b                         —                a ≠ b; recorded owners trade too
 *   NudgeAll      a = samples, b = 0 grid / 1 markers  —        grid: linked and clamped; markers: clamped to [0, freePos)
 *   Promote       t, i                         → slice i        SetBoundary(i, marker t's start)
 *   SetKeyMap     keys[kKeys] (−1 = default)   —                BETA: every key at once; each a live entry
 *   Record        key, start, len              → the entry      PADS: key owns the span (its recording's slot reused,
 *                                                               else a free one); start ≤ freePos, end ≤ bufCap; freePos follows
 *
 * `meta` is copied verbatim when flags has kMeta (the control thread ran
 * bb::AnalyseEntry on the span first); ops marked — ignore it. The entry a
 * geometry op touches becomes orange. `gen` ≠ 0 must equal tableGen, else
 * Stale. On Ok the table generation bumps and *newIdx (AddEntry, Split)
 * is the new entry's index.
 *
 * EDIT-UNDO: Undo / Redo (no fields) walk the journal one step; every other
 * op is captured before it runs and journaled on success — as its own step,
 * or joined to the previous one with flags kJoin. */
inline EditStatus Engine::ApplyEdit(const EditReq& r, int* newIdx)
{
    if(newIdx) *newIdx = -1;
    if(!buf || nSlices < 1 || bufLen < 1) return EditStatus::NoMaterial;
    if(r.gen != 0 && r.gen != tableGen) return EditStatus::Stale;
    if(r.op == EditOp::Undo || r.op == EditOp::Redo) return UndoStep(r.op == EditOp::Redo);
    const bool quiet   = UndoQuiet(r);               /* BETA: the newest record already covers this op */
    const bool tail    = (r.flags & EditReq::kQuiet) && UndoAtEnd();   /* BETA: …else, right behind its step, it joins it */
    const bool journal = !quiet && UndoBegin(r);
    EditStatus st  = EditStatus::BadOp;
    int        who = -1;   // the entry that takes the meta and the orange
    int        idx = -1;
    switch(r.op)
    {
        case EditOp::SetBoundary: st = EdMoveBoundary(r.i, r.start); who = r.i; break;
        case EditOp::SetEntry:    st = EdSetEntry(r.t, r.start, r.len); who = r.t; break;
        case EditOp::SetKeyMap:   st = EdSetKeyMap(r.keys); break;               /* BETA */
        case EditOp::Record:      st = EdRecord(r, idx); break;                   /* PADS: meta applied inside */
        case EditOp::AddEntry:    st = EdAdd(r, idx); break;                      // meta and flags applied inside
        case EditOp::Split:       st = EdSplit(r, idx); break;                    // meta → the new half, inside
        case EditOp::Merge:       st = EdMerge(r.t); who = r.t; break;
        case EditOp::Tombstone:   st = EdTomb(r.t); break;
        case EditOp::SetKey:      st = EdSetKey(r.key, r.t); break;
        case EditOp::SwapKeys:    st = EdSwapKeys(r.a, r.b); break;
        case EditOp::NudgeAll:    st = EdNudge(r.a, r.b); break;
        case EditOp::Promote:
            if(r.t < nSlices || r.t >= nTable) st = EditStatus::NotMarker;
            else if(tableFlags[r.t] & kFlagTomb) st = EditStatus::Tomb;
            else { st = EdMoveBoundary(r.i, slices[r.t].start); who = r.i; }
            break;
        default: break;
    }
    if(st != EditStatus::Ok) return st;
    if(who >= 0 && who < nTable)
    {
        if(!(r.op == EditOp::SetEntry && (r.flags & EditReq::kKeep))) tableFlags[who] |= kFlagOrange;   /* BETA: kKeep = a machine re-span keeps its colour */
        if(r.flags & EditReq::kMeta)
        {
            stepClass[who] = r.meta.cls;
            sliceSal[who]  = r.meta.sal;
            sliceDens[who] = r.meta.dens;
            stepHit[who]   = r.meta.hit;
            stepBands[who] = 0;
            if(who >= nSlices) recMeta[who] = r.meta;
        }
    }
    if(r.op == EditOp::SetBoundary || r.op == EditOp::Promote || (r.op == EditOp::NudgeAll && r.b == 0)) SaveOverlay();
    if(newIdx) *newIdx = idx;
    BumpGen();
    if(journal) UndoCommit(!((r.flags & EditReq::kJoin) || tail));
    else if(quiet) UndoKeepLive();                  /* BETA: no record, the step it joined stays live */
    return EditStatus::Ok;
}

} // namespace bb
