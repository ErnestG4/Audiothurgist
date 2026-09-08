#pragma once
// SETS — the pure pieces of the card side of persistence (the FatFs code
// itself is the shell's, src/audiothurgist.cpp; the formats are bb_set.h;
// docs/packages/sample-sets.md is the package). Host-tested in
// tests/sets_check.cpp.
//
//   * Naming. A set is `sets/<name>.wav` + `sets/<name>.json`. ScanSD lists
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

// The two card paths of a set: "0:/sets/<name>.wav" and "0:/sets/<name>.json".
inline bool SetPaths(const char* name, char* wav, char* json, size_t cap)
{
    if(!SetNameOk(name)) { if(cap) { wav[0] = 0; json[0] = 0; } return false; }
    size_t a = setdetail::Cat(wav, cap, 0, "0:/sets/");
    a = setdetail::Cat(wav, cap, a, name);
    a = setdetail::Cat(wav, cap, a, ".wav");
    size_t b = setdetail::Cat(json, cap, 0, "0:/sets/");
    b = setdetail::Cat(json, cap, b, name);
    b = setdetail::Cat(json, cap, b, ".json");
    return a < cap && b < cap;
}

// The WAV's protocol-form path for SdCard::Lock ("/sets/<name>.wav"; the
// lock table compares case-insensitively).
inline bool SetLockPath(const char* name, char* out, size_t cap)
{
    if(!SetNameOk(name)) { if(cap) out[0] = 0; return false; }
    size_t at = setdetail::Cat(out, cap, 0, "/sets/");
    at = setdetail::Cat(out, cap, at, name);
    at = setdetail::Cat(out, cap, at, ".wav");
    return at < cap;
}

// "0:/presets/<slot>.json".
inline bool PresetPath(int slot, char* out, size_t cap)
{
    if(slot < 0 || slot > 999) { if(cap) out[0] = 0; return false; }
    char num[8];
    int  n = 0;
    if(slot >= 100) num[n++] = (char)('0' + slot / 100);
    if(slot >= 10) num[n++] = (char)('0' + (slot / 10) % 10);
    num[n++] = (char)('0' + slot % 10);
    num[n] = 0;
    size_t at = setdetail::Cat(out, cap, 0, "0:/presets/");
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
inline void SetFromEngine(const Engine& e, const Looper& lp, SetInfo& s, bool stereo, int sr)
{
    s.sr       = sr;
    s.stereo   = stereo;
    s.len      = e.freePos;
    s.material = e.bufLen;
    s.swing    = e.swing;
    s.nSlices  = 0;
    for(int t = 0; t < e.nTable && s.nSlices < kSetMaxSlices; t++)
    {
        SetSlice& sl = s.slices[s.nSlices++];
        sl.start = e.slices[t].start;
        sl.len   = e.slices[t].len;
        sl.key   = t < e.nSlices ? (int8_t)-1 : e.tableKey[t];
        sl.cls   = (uint8_t)(e.stepClass[t] & 3u);
        sl.sal   = setdetail::Clamp01(e.sliceSal[t]);
        sl.hit   = e.stepHit[t];
        sl.name[0] = 0;
    }
    s.nLooper = 0;
    for(int i = 0; i < lp.n && s.nLooper < kSetMaxLooper; i++)
    {
        const Looper::Event& ev = lp.ev[i];
        SetLoopEvent&        o  = s.looper[s.nLooper++];
        o.phase = (float)ev.phase;
        o.key   = ev.key;
        o.style = (uint8_t)(ev.style & Looper::kWire);
        o.vel   = ev.vel;
        o.slice = ev.slice;
    }
}

// What a set load hands SetMaterial, and the recorded entries that follow
// it. Trivially constructible on purpose (it may live in a NOLOAD section).
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
    int     nRec;                      // recorded entries, as indices into SetInfo::slices
    int     rec[Engine::kMaxTable];
};

// SetInfo → SetPlan. False (plan zeroed) when the set is malformed: no grid
// entry, a grid entry outside [0, material) or out of order, more than
// maxGrid of them, a recorded entry outside [material, len), shorter than
// recMin, keyed outside 0..keys-1 or on a key already taken. Grid entries
// are the leading key < 0 entries; everything after the first keyed entry
// must be keyed.
namespace setdetail {
inline bool PlanSetInto(const SetInfo& s, SetPlan& p, int maxGrid, int recMin, int keys)
{
    const int material = (s.material > 0 && s.material <= s.len) ? s.material : s.len;
    if(s.len <= 0 || material <= 0 || keys > 32) return false;
    int i = 0;
    for(; i < s.nSlices && s.slices[i].key < 0; i++)
    {
        const SetSlice& sl = s.slices[i];
        if(i >= maxGrid) return false;
        if(sl.start < 0 || sl.start >= material || (i > 0 && sl.start <= s.slices[i - 1].start)) return false;
        p.cls[i] = (uint8_t)(sl.cls & 3u);
        p.sal[i] = Clamp01(sl.sal);
        if(sl.hit) p.onsets[p.nOnsets++] = sl.start;
    }
    p.nGrid = i;
    if(p.nGrid < 1) return false;
    uint32_t taken = 0;
    for(; i < s.nSlices; i++)
    {
        const SetSlice& sl = s.slices[i];
        if(sl.key < 0 || sl.key >= keys) return false;
        if(taken & (1u << sl.key)) return false;
        if(sl.start < material || sl.len < recMin || sl.start + sl.len > s.len) return false;
        if(p.nRec >= Engine::kMaxTable) return false;
        taken |= 1u << sl.key;
        p.rec[p.nRec++] = i;
    }
    p.material = material;
    p.len      = s.len;
    p.swing    = (s.swing >= 0.f && s.swing <= 1.f) ? s.swing : 0.5f;
    return true;
}
} // namespace setdetail

inline bool PlanSet(const SetInfo& s, SetPlan& p, int maxGrid = Engine::kMaxSlices,
                    int recMin = Engine::kRecMin, int keys = Engine::kKeys)
{
    memset(&p, 0, sizeof p);
    if(setdetail::PlanSetInto(s, p, maxGrid, recMin, keys)) return true;
    memset(&p, 0, sizeof p);
    return false;
}

// After SetMaterial(planes, plan.material, …, plan.onsets, plan.nOnsets,
// plan.cls, plan.nGrid, …): publish the recorded entries through
// CommitSlice, in file order. `dens(start, len)` gives each entry's keyframe
// density (bb::AnalyseRegion on the plane — the JSON carries none; 0 = the
// fixed fades). Returns the number committed (all of them: PlanSet already
// checked every refusal CommitSlice knows).
template <typename DensFn>
inline int CommitSetRecordings(Engine& e, const SetInfo& s, const SetPlan& p, DensFn dens)
{
    int done = 0;
    for(int r = 0; r < p.nRec; r++)
    {
        const SetSlice& sl = s.slices[p.rec[r]];
        const float     d  = dens(sl.start, sl.len);
        if(e.CommitSlice(sl.key, sl.start, sl.len, (uint8_t)(sl.cls & 3u), setdetail::Clamp01(sl.sal), d, sl.hit) >= 0) done++;
    }
    return done;
}

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
        if(!(e.phase == e.phase) || e.key >= Engine::kKeys) continue;
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
