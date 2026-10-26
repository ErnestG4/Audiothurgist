#pragma once
// SETS — the pure pieces of the card side of persistence (the FatFs code
// itself is the shell's, src/audiothurgist.cpp; the formats are bb_set.h;
// docs/packages/sample-sets.md is the package). Host-tested in
// tests/sets_check.cpp.
//
//   * Naming. A set is `sets/<name>.wav` + `sets/<name>.json` under
//     0:/Audiothurgist (CARD, core/bb_card.h). ScanSD lists
//     the WAV as the entry "sets/<name>.wav" (so the plain load path still
//     works on it) and the dropdown / P5 label reads "<name> [set]". A name
//     is 1..31 of [A-Za-z0-9_-] — portable across LFN and 8.3 hosts, never a
//     path.
//   * SetFromEngine: the engine's table (grid entries, then the recorded
//     ones with their keys), classes, salience, hits, the break's length and
//     swing, and a snapshot of the looper → SetInfo. The shell adds the
//     name, bars/bpm and the Options bytes.
//   * PlanSet: SetInfo → what SetMaterial needs (grid count, material
//     length, the hit onsets, classes, salience) plus the validated recorded
//     entries; CommitSetRecordings publishes those after the swap;
//     LooperFromSet rebuilds the events (Add + Seal) for Looper::Adopt.
//   * MergeOptions: the set's option bytes over the live block.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "bb_engine.h"
#include "bb_set.h"

namespace bb {

constexpr int         kSetNameMax = kSetMaxName - 1;   // 31 chars
constexpr const char* kSetDirName = "sets";
constexpr const char* kPresetDirName = "presets";

namespace setdetail {

inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
inline bool PrefixCi(const char* s, const char* p)
{
    while(*p) { if(!*s || Lower(*s) != Lower(*p)) return false; s++; p++; }
    return true;
}
inline bool SuffixCi(const char* s, const char* suf)
{
    const size_t n = strlen(s), m = strlen(suf);
    if(m > n) return false;
    return PrefixCi(s + (n - m), suf);
}
// bounded append; returns the new length, or cap when it did not fit (dst
// is then NUL-terminated but useless — callers treat >= cap as failure)
inline size_t Cat(char* dst, size_t cap, size_t at, const char* src)
{
    if(cap == 0) return 0;
    while(*src && at + 1 < cap) dst[at++] = *src++;
    dst[at] = 0;
    return *src ? cap : at;
}
inline bool EqCi(const char* a, const char* b)
{
    while(*a && *b) { if(Lower(*a) != Lower(*b)) return false; a++; b++; }
    return *a == *b;
}
inline float Clamp01(float v) { return !(v == v) ? 0.f : (v < 0.f ? 0.f : (v > 1.f ? 1.f : v)); }

} // namespace setdetail

// A set name: 1..31 of [A-Za-z0-9_-].
inline bool SetNameOk(const char* n)
{
    if(!n || !n[0]) return false;
    for(int i = 0; n[i]; i++)
    {
        if(i >= kSetNameMax) return false;
        const char c = n[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if(!ok) return false;
    }
    return true;
}

// "sets/<file>.wav" — the entry ScanSD stores for a set's WAV.
inline bool IsSetEntry(const char* entry)
{
    return entry && setdetail::PrefixCi(entry, "sets/") && setdetail::SuffixCi(entry, ".wav") && strlen(entry) > 9;
}

// "sets/amen.wav" → "amen". False when not a set entry, or when the stem is
// not a valid set name (a foreign file dropped into sets/ by hand).
inline bool SetStem(const char* entry, char* name, size_t cap)
{
    if(cap) name[0] = 0;
    if(!IsSetEntry(entry) || cap == 0) return false;
    const char*  s = entry + 5;
    const size_t n = strlen(s) - 4;
    if(n + 1 > cap || n > (size_t)kSetNameMax) return false;
    memcpy(name, s, n);
    name[n] = 0;
    if(SetNameOk(name)) return true;
    name[0] = 0;
    return false;
}

// name → "sets/<name>.wav" (the list entry).
inline bool SetEntry(const char* name, char* out, size_t cap)
{
    if(!SetNameOk(name)) { if(cap) out[0] = 0; return false; }
    size_t at = setdetail::Cat(out, cap, 0, "sets/");
    at = setdetail::Cat(out, cap, at, name);
    at = setdetail::Cat(out, cap, at, ".wav");
    return at < cap;
}

// The label a list shows: "<name> [set]" for a set entry, the entry itself
// for anything else.
inline void SetLabel(const char* entry, char* out, size_t cap)
{
    char stem[kSetMaxName];
    if(SetStem(entry, stem, sizeof stem))
    {
        size_t at = setdetail::Cat(out, cap, 0, stem);
        setdetail::Cat(out, cap, at, " [set]");
    }
    else setdetail::Cat(out, cap, 0, entry ? entry : "");
}

// The two card paths of a set: "0:/Audiothurgist/sets/<name>.wav" and
// "0:/Audiothurgist/sets/<name>.json" (CARD: everything under one folder,
// core/bb_card.h).
inline bool SetPaths(const char* name, char* wav, char* json, size_t cap)
{
    if(!SetNameOk(name)) { if(cap) { wav[0] = 0; json[0] = 0; } return false; }
    size_t a = setdetail::Cat(wav, cap, 0, "0:/Audiothurgist/sets/");
    a = setdetail::Cat(wav, cap, a, name);
    a = setdetail::Cat(wav, cap, a, ".wav");
    size_t b = setdetail::Cat(json, cap, 0, "0:/Audiothurgist/sets/");
    b = setdetail::Cat(json, cap, b, name);
    b = setdetail::Cat(json, cap, b, ".json");
    return a < cap && b < cap;
}

// The WAV's protocol-form path for SdCard::Lock ("/Audiothurgist/sets/<name>.wav"; the
// lock table compares case-insensitively).
inline bool SetLockPath(const char* name, char* out, size_t cap)
{
    if(!SetNameOk(name)) { if(cap) out[0] = 0; return false; }
    size_t at = setdetail::Cat(out, cap, 0, "/Audiothurgist/sets/");
    at = setdetail::Cat(out, cap, at, name);
    at = setdetail::Cat(out, cap, at, ".wav");
    return at < cap;
}

// "0:/Audiothurgist/presets/<slot>.json".
inline bool PresetPath(int slot, char* out, size_t cap)
{
    if(slot < 0 || slot > 999) { if(cap) out[0] = 0; return false; }
    char num[8];
    int  n = 0;
    if(slot >= 100) num[n++] = (char)('0' + slot / 100);
    if(slot >= 10) num[n++] = (char)('0' + (slot / 10) % 10);
    num[n++] = (char)('0' + slot % 10);
    num[n] = 0;
    size_t at = setdetail::Cat(out, cap, 0, "0:/Audiothurgist/presets/");
    at = setdetail::Cat(out, cap, at, num);
    at = setdetail::Cat(out, cap, at, ".json");
    return at < cap;
}

// Index of `entry` in a fixed-stride list of C strings (case-insensitive,
// FAT's rule), or -1.
inline int FindEntry(const char* files, size_t stride, int n, const char* entry)
{
    if(!files || !entry || !entry[0]) return -1;
    for(int i = 0; i < n; i++)
        if(setdetail::EqCi(files + (size_t)i * stride, entry)) return i;
    return -1;
}

/* ── engine ↔ set ──────────────────────────────────────────────────────── */

// The engine's table, keys and metadata, the break's length and swing, and
// a looper snapshot (Looper::Adopt of the live one, taken on the audio
// thread) → SetInfo. name / bars / bpm / options stay the caller's.
//
// EDIT-PERSIST: the table is written COMPACTED — tombstones are skipped, and
// every index that refers to the table (the key map, a keyless looper
// event's `slice`) is re-indexed to the file's order; a keyless event whose
// entry is gone is dropped. `maxLen` (0 = freePos) bounds what the file may
// describe: an entry past it is skipped like a tombstone (keys on it → the
// default), and `len` is that bound — the set-JSON autosave passes the WAV's
// length (a take since the save is not in the WAV), the loose-file sidecar
// passes bufLen (the break only). Per-entry `flags` = the orange bit;
// `grid` = nSlices; `keys` = keySlice through the map.
inline void SetFromEngine(const Engine& e, const Looper& lp, SetInfo& s, bool stereo, int sr, int maxLen = 0)
{
    const int lim = (maxLen > 0 && maxLen < e.freePos) ? maxLen : e.freePos;
    s.sr       = sr;
    s.stereo   = stereo;
    s.len      = lim;
    s.material = e.bufLen < lim ? e.bufLen : lim;
    s.swing    = e.swing;
    s.nGrid    = e.nSlices;
    s.nSlices  = 0;
    int8_t map[Engine::kMaxTable];
    for(int t = 0; t < Engine::kMaxTable; t++) map[t] = -1;
    for(int t = 0; t < e.nTable && s.nSlices < kSetMaxSlices; t++)
    {
        if(t >= e.nSlices)
        {
            const bool live = !(e.tableFlags[t] & Engine::kFlagTomb) && e.slices[t].len > 0 && e.slices[t].start >= 0
                              && e.slices[t].start <= lim && e.slices[t].len <= lim - e.slices[t].start;
            if(!live) continue;
        }
        map[t] = (int8_t)s.nSlices;
        SetSlice& sl = s.slices[s.nSlices++];
        sl.start = e.slices[t].start;
        sl.len   = e.slices[t].len;
        sl.key   = t < e.nSlices ? (int8_t)-1 : e.tableKey[t];
        sl.cls   = (uint8_t)(e.stepClass[t] & 3u);
        sl.sal   = setdetail::Clamp01(e.sliceSal[t]);
        sl.hit   = e.stepHit[t];
        sl.flags = (uint8_t)(e.tableFlags[t] & Engine::kFlagOrange);
        sl.name[0] = 0;
    }
    s.hasKeys = true;
    for(int k = 0; k < kSetKeys && k < Engine::kKeys; k++)
    {
        const int ks = e.KeySlice(k);
        s.keys[k] = ks >= 0 && ks < Engine::kMaxTable ? map[ks] : (int8_t)-1;
    }
    for(int k = 0; k < kSetKeys && k < Engine::kKeys; k++)   /* KEYP */
    {
        s.keyp[k].gain  = e.keyp[k].gain;
        s.keyp[k].plays = e.keyp[k].plays;
        s.keyp[k].cents = e.keyp[k].cents;
    }
    s.nLooper = 0;
    for(int i = 0; i < lp.n && s.nLooper < kSetMaxLooper; i++)
    {
        const Looper::Event& ev = lp.ev[i];
        const int8_t         sl = (ev.slice >= 0 && ev.slice < Engine::kMaxTable) ? map[ev.slice] : (int8_t)-1;
        if(ev.key < 0 && ev.slice >= 0 && sl < 0) continue;   /* a keyless event on an entry the file does not hold */
        SetLoopEvent& o = s.looper[s.nLooper++];
        o.phase = (float)ev.phase;
        o.key   = ev.key;
        o.style = (uint8_t)(ev.style & Looper::kWire);
        o.vel   = ev.vel;
        o.slice = sl;
    }
}

// What a set load hands SetMaterial, and the entries that follow it.
// Trivially constructible on purpose (it may live in a NOLOAD section).
struct SetPlan
{
    int     nGrid;                     // grid entries = SetMaterial's nSlicesHint
    int     material;                  // the break's length = SetMaterial's len
    int     len;                       // frames in the planes (material + recordings)
    float   swing;                     // Engine::swing before SetMaterial
    int     nOnsets;                   // starts of the grid slices flagged `hit` → stepHit
    int     onsets[Engine::kMaxSlices];
    uint8_t cls[Engine::kMaxSlices];   // → SetMaterial's classes
    float   sal[Engine::kMaxSlices];   // → Engine::sliceSal before SetMaterial
    int     nRec;                      // markers after the grid (recordings and in-break spans), as indices into SetInfo::slices
    int     rec[Engine::kMaxTable];
    /* EDIT-PERSIST */
    int     starts[Engine::kMaxSlices];    // the file's grid starts → Engine::SetGridBoundaries after SetMaterial
    uint8_t gridFlags[Engine::kMaxSlices]; // the grid's orange bits, for the same call
    bool    hasKeys;                       // the file carried a key map
    int8_t  keys[Engine::kKeys];           // FILE indices (into SetInfo::slices), −1 = the default
};

// SetInfo → SetPlan. False (plan zeroed) when the set is malformed: no grid
// entry, more than maxGrid of them, a grid that does not tile [0, material)
// — starts[0] ≠ 0, not ascending by ≥ gridMin, or the last one < gridMin
// before the material's end — a marker outside [0, len) or shorter than
// recMin, a marker keyed outside −1..keys-1 or on a key already taken, or a
// key-map index outside −1..nSlices-1.
//
// The grid is the file's `grid` count when it carries one; else (an old
// file) the leading key −1 entries that start inside the material. What
// follows are markers: recordings past the material, and — EDIT-PERSIST —
// spans of the break itself, keyed or keyless, in any order.
namespace setdetail {
inline bool PlanSetInto(const SetInfo& s, SetPlan& p, int maxGrid, int recMin, int keys, int gridMin)
{
    const int material = (s.material > 0 && s.material <= s.len) ? s.material : s.len;
    if(s.len <= 0 || material <= 0 || keys > 32 || keys > Engine::kKeys) return false;
    int nGrid = s.nGrid;
    if(nGrid <= 0)
    {
        nGrid = 0;
        while(nGrid < s.nSlices && s.slices[nGrid].key < 0 && s.slices[nGrid].start >= 0 && s.slices[nGrid].start < material) nGrid++;
    }
    if(nGrid < 1 || nGrid > maxGrid || nGrid > s.nSlices || nGrid > Engine::kMaxSlices) return false;
    for(int i = 0; i < nGrid; i++)
    {
        const SetSlice& sl = s.slices[i];
        if(sl.start < 0 || sl.start >= material) return false;
        if(i == 0 ? sl.start != 0 : sl.start - s.slices[i - 1].start < gridMin) return false;
        p.starts[i]    = sl.start;
        p.gridFlags[i] = (uint8_t)(sl.flags & kSetFlagOrange);
        p.cls[i]       = (uint8_t)(sl.cls & 3u);
        p.sal[i]       = Clamp01(sl.sal);
        if(sl.hit) p.onsets[p.nOnsets++] = sl.start;
    }
    if(material - s.slices[nGrid - 1].start < gridMin) return false;
    p.nGrid = nGrid;
    uint32_t taken = 0;
    for(int i = nGrid; i < s.nSlices; i++)
    {
        const SetSlice& sl = s.slices[i];
        if(sl.key >= keys) return false;
        if(sl.key >= 0)
        {
            if(taken & (1u << sl.key)) return false;
            taken |= 1u << sl.key;
        }
        if(sl.start < 0 || sl.len < recMin || sl.start > s.len || sl.len > s.len - sl.start) return false;   /* overflow-safe (review M1) */
        if(nGrid + p.nRec >= Engine::kMaxTable) return false;
        p.rec[p.nRec++] = i;
    }
    p.hasKeys = s.hasKeys;
    for(int k = 0; k < Engine::kKeys; k++)
    {
        const int v = k < kSetKeys ? s.keys[k] : -1;
        if(v >= s.nSlices) return false;
        p.keys[k] = (int8_t)(v < 0 ? -1 : v);
    }
    p.material = material;
    p.len      = s.len;
    p.swing    = (s.swing >= 0.f && s.swing <= 1.f) ? s.swing : 0.5f;
    return true;
}
} // namespace setdetail

inline bool PlanSet(const SetInfo& s, SetPlan& p, int maxGrid = Engine::kMaxSlices,
                    int recMin = Engine::kRecMin, int keys = Engine::kKeys, int gridMin = Engine::kGridMin)
{
    memset(&p, 0, sizeof p);
    if(setdetail::PlanSetInto(s, p, maxGrid, recMin, keys, gridMin)) return true;
    memset(&p, 0, sizeof p);
    return false;
}

// After SetMaterial(planes, plan.material, …, plan.onsets, plan.nOnsets,
// plan.cls, plan.nGrid, …) — and, on the audio thread, SetGridBoundaries
// (plan.starts, plan.nGrid, plan.gridFlags): publish the markers through
// CommitEntry, in file order, with their keys and flags. `dens(start, len)`
// gives each entry's keyframe density (bb::AnalyseRegion on the plane — the
// JSON carries none; 0 = the fixed fades). Returns the number committed
// (all of them: PlanSet already checked every refusal CommitEntry knows).
//
// EDIT-PERSIST: `liveKeys` (optional, Engine::kKeys wide) receives the
// file's key map translated to LIVE indices for Engine::SetKeyTable: a key
// on grid slice i → i when the engine's grid has the file's count (else the
// default, −1); a key on marker j → the index CommitEntry gave it (−1 when
// it was refused); −1 stays −1.
template <typename DensFn>
inline int CommitSetRecordings(Engine& e, const SetInfo& s, const SetPlan& p, DensFn dens, int8_t* liveKeys = nullptr)
{
    int    done = 0;
    int8_t live[Engine::kMaxTable];
    for(int t = 0; t < Engine::kMaxTable; t++) live[t] = -1;
    for(int r = 0; r < p.nRec; r++)
    {
        const SetSlice& sl = s.slices[p.rec[r]];
        const float     d  = dens(sl.start, sl.len);
        const int       t  = e.CommitEntry(sl.key, sl.start, sl.len, (uint8_t)(sl.cls & 3u), setdetail::Clamp01(sl.sal), d, sl.hit, sl.flags);
        if(t >= 0) { done++; if(p.rec[r] < Engine::kMaxTable) live[p.rec[r]] = (int8_t)t; }
    }
    if(liveKeys)
    {
        const bool gridHolds = e.nSlices == p.nGrid;
        for(int k = 0; k < Engine::kKeys; k++)
        {
            const int f = p.keys[k];
            if(f < 0)             liveKeys[k] = -1;
            else if(f < p.nGrid)  liveKeys[k] = (int8_t)(gridHolds ? f : -1);
            else                  liveKeys[k] = f < Engine::kMaxTable ? live[f] : (int8_t)-1;
        }
    }
    return done;
}

// KEYP: the set's key settings → the engine's form (an old file's: every key neutral), for Engine::SetKeyParams
// after the load (with the key table).
inline void KeyParamsFromSet(const SetInfo& s, KeyParam* out)
{
    for(int k = 0; k < Engine::kKeys; k++)
    {
        const SetKeyParam p = k < kSetKeys ? s.keyp[k] : SetKeyParam();
        out[k] = {p.gain, p.plays, p.cents};
    }
}
static_assert(kSetGainMin == KeyParam::kGainMin && kSetGainMax == KeyParam::kGainMax && kSetCentsMax == KeyParam::kCentsMax
              && kSetPlaysMax == KeyParam::kLoop && kSetKeys == Engine::kKeys, "the set's key settings are the engine's");

// The set's events into a (cleared) looper: Add, then Seal, so the first
// live overdub after the load is its own undo unit. Returns the count added
// (events with an impossible key or a NaN phase are dropped).
inline int LooperFromSet(const SetInfo& s, Looper& l)
{
    l.Clear();
    int n = 0;
    for(int i = 0; i < s.nLooper; i++)
    {
        const SetLoopEvent& e = s.looper[i];
        if(!(e.phase == e.phase) || !(e.phase > -1e6f && e.phase < 1e6f) || e.key >= Engine::kKeys) continue;   /* NaN and ±inf (review L6) */
        if(l.Add((double)e.phase, e.key, e.slice, e.style, e.vel)) n++;
    }
    l.Seal();
    return n;
}

// The set's option bytes over the live block: the set's value where it has
// one, the live value past its count (a set from an older schema keeps the
// newer options as they are). out[n] may alias live.
inline void MergeOptions(const SetInfo& s, const uint8_t* live, uint8_t* out, int n)
{
    for(int i = 0; i < n; i++)
        out[i] = i < s.nOptions ? s.options[i] : live[i];
}

} // namespace bb
