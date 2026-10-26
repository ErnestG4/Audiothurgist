#pragma once
// EDIT-UNDO (docs/packages/slice-edit-undo.md): the inverse-op journal —
// included at the foot of bb_engine.h after bb_edit.h, never on its own.
//
// One ring of 64 records lives wherever the shell puts it (SDRAM, NOLOAD;
// the engine only holds a pointer, set by SetUndoJournal). A record is
// what restoring one ApplyEdit needs: the entries the op touched, as they
// were (span, owner key, flags, class / salience / density / bands / hit),
// the whole key map and nTable — or, for NUDGE_ALL, every start of the
// nudged range with its orange and hit bits (the clamps make a nudge
// non-uniform, so the delta alone would not do). A record is SYMMETRIC:
// restoring it captures the same indices' current state into the same
// slot first, so an undone record is its own redo and one cursor walks the
// ring both ways (undo = cur−−, redo = cur++, a new forward op truncates
// the redo half). A step is a run of records — the first one is the head,
// the rest were posted with EditReq::kJoin (the shell's FROM_ONSETS) — and
// Undo / Redo walk a whole step.
//
// The journal is bound to the table's generation: every journaled apply
// leaves `gen` = tableGen, so a change that went past the journal (a take's
// commit, Clear, a set load, SetMaterial — which also clears explicitly)
// makes it stale, and the next Undo / Redo empties it (BadArgs) while the
// next forward op restarts it. Audio thread, no allocation, O(kMaxTable).
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bb {

struct UndoEntry               // one table entry as it was: 24 bytes
{
    int32_t start, len;
    float   sal, dens;
    int8_t  key;               // tableKey (the recorded owner)
    uint8_t idx;               // the table index
    uint8_t flags, cls, bands, hit;
    uint8_t pad[2];
};
static_assert(sizeof(UndoEntry) == 24, "UndoEntry packs to 24 bytes");

struct UndoRec
{
    static constexpr uint8_t kEntries = 0;   // u.e[0..n): the entries the op touched
    static constexpr uint8_t kStarts  = 1;   // u.nudge: the starts of [first, first + n) with their orange / hit bits
    static constexpr uint8_t kSnap    = 2;   // BETA: u.e[0..n) = every slot of [first, first + n) — the marker range, whole
    static constexpr uint8_t kKeyP    = 3;   // KEYP: u.kp = every key's settings; `first` = the key the op edited
    uint8_t op;        // the forward EditOp this record undoes (informational)
    uint8_t kind;      // kEntries / kStarts
    uint8_t head;      // 1 = the first record of a step (Undo pops back to a head)
    uint8_t n;         // entries (≤ 2) or starts (≤ kMaxTable) used
    uint8_t nTable;    // nTable as it was
    uint8_t first;     // kStarts: the first index of the range
    uint8_t pad[2];
    int8_t  keys[Engine::kKeys];   // keySlice as it was
    union
    {
        UndoEntry e[Engine::kMaxTable];   // kEntries: [0, 2) · BETA kSnap: [0, n)
        struct { uint32_t orange[3], hit[3]; int32_t start[Engine::kMaxTable]; } nudge;
        KeyParam kp[Engine::kKeys];       // KEYP: kKeyP (96 B — inside the union, the record does not grow)
    } u;
    // the bytes a record uses: the header, the keys, then what its kind fills
    size_t Bytes() const
    {
        const size_t head = offsetof(UndoRec, u);
        if(kind == kStarts) return head + sizeof(u.nudge.orange) + sizeof(u.nudge.hit) + sizeof(int32_t) * (size_t)n;
        if(kind == kSnap)   return head + sizeof(UndoEntry) * (size_t)(n > Engine::kMaxTable ? Engine::kMaxTable : n);
        if(kind == kKeyP)   return head + sizeof(u.kp);
        return head + sizeof(UndoEntry) * (size_t)(n > 2 ? 2 : n);
    }
};
/* BETA: the union grew from two entries / the nudge form (408 B) to a whole marker
 * range snapshot (96 × 24 B): 440 → 2 336 B a record, the journal 28.6 → 151.9 KB of
 * SDRAM (NOLOAD). Push copies only Bytes(), so a two-entry record still costs 80 B
 * of copying; the snapshot's bound is one memcpy of ≤ 2.3 KB at block start. */
static_assert(sizeof(UndoRec) == 8 + Engine::kKeys + 24 * Engine::kMaxTable, "UndoRec is 2 336 bytes");

struct UndoJournal
{
    static constexpr int kDepth = 64;
    UndoRec rec[kDepth];
    UndoRec scratch;           // the capture of the op in flight (committed on success)
    int     base;              // ring index of the oldest record
    int     count;             // records held, ≤ kDepth
    int     cur;               // records before the cursor: [0, cur) undo, [cur, count) redo
    uint8_t gen;               // the tableGen the journal is valid for (0 = none)
    uint8_t armed;             // scratch holds a capture

    void Clear() { base = count = cur = 0; gen = 0; armed = 0; }
    UndoRec&       Slot(int i)       { return rec[(base + i) % kDepth]; }
    const UndoRec& Slot(int i) const { return rec[(base + i) % kDepth]; }
    // steps (heads) in [from, to), counting a headless run at `from` as one
    int Steps(int from, int to) const
    {
        int s = 0;
        for(int i = from; i < to; i++) if(Slot(i).head || i == from) s++;
        return s;
    }
    int UndoSteps() const { return Steps(0, cur); }
    int RedoSteps() const { return Steps(cur, count); }
    // drop the redo half and the oldest record if the ring is full, then take the scratch
    void Push()
    {
        count = cur;
        if(count == kDepth) { base = (base + 1) % kDepth; count--; cur--; }
        UndoRec& r = Slot(count);
        memcpy(&r, &scratch, scratch.Bytes());
        count++; cur++;
        armed = 0;
    }
};

/* ── capture / restore of one entry ───────────────────────────────────── */

inline void Engine::UndoCaptureEntry(UndoEntry& e, int t) const
{
    e.idx   = (uint8_t)t;
    e.start = slices[t].start;
    e.len   = slices[t].len;
    e.key   = tableKey[t];
    e.flags = tableFlags[t];
    e.cls   = stepClass[t];
    e.sal   = sliceSal[t];
    e.dens  = sliceDens[t];
    e.bands = stepBands[t];
    e.hit   = stepHit[t] ? 1u : 0u;
    e.pad[0] = e.pad[1] = 0;
}

inline void Engine::UndoRestoreEntry(const UndoEntry& e)
{
    const int t = e.idx;
    if(t >= kMaxTable) return;
    slices[t]     = {e.start, e.len};
    tableKey[t]   = e.key;
    tableFlags[t] = e.flags;
    stepClass[t]  = e.cls;
    sliceSal[t]   = e.sal;
    sliceDens[t]  = e.dens;
    stepBands[t]  = e.bands;
    if(t >= nSlices) recMeta[t] = {e.cls, e.sal, e.dens, e.hit != 0};
    /* loopState, the gas well; the hit it derives is overridden. A marker's
     * RefreshEntry would only search the onsets for a hit thrown away below
     * (BETA: a snapshot restores ~90 of them in one block), so it is skipped. */
    if(t < nSlices) RefreshEntry(t); else loopState[t] = 0;
    stepHit[t] = e.hit != 0;         // the entry's hit may have come from AnalyseEntry, not the onset table
    if(t >= nSlices) recMeta[t].hit = stepHit[t];
}

/* ── the records ──────────────────────────────────────────────────────── */

// Capture the entries idx[0..n) (n ≤ 2), the key map and nTable into r.
inline void Engine::UndoCaptureEntries(UndoRec& r, const int* idx, int n) const
{
    r.kind = UndoRec::kEntries; r.n = 0; r.first = 0;
    r.nTable = (uint8_t)nTable;
    memcpy(r.keys, keySlice, sizeof r.keys);
    for(int j = 0; j < n && r.n < 2; j++)
    {
        if(idx[j] < 0 || idx[j] >= kMaxTable) continue;
        if(r.n > 0 && r.u.e[0].idx == idx[j]) continue;   // the same entry twice (two keys on one entry) would break the swap
        UndoCaptureEntry(r.u.e[r.n++], idx[j]);
    }
}

// Capture the starts of [first, first + n) with their orange and hit bits.
inline void Engine::UndoCaptureStarts(UndoRec& r, int first, int n) const
{
    r.kind = UndoRec::kStarts;
    if(first < 0) first = 0;
    if(n < 0) n = 0;
    if(first + n > kMaxTable) n = kMaxTable - first;
    r.first = (uint8_t)first; r.n = (uint8_t)n;
    r.nTable = (uint8_t)nTable;
    memcpy(r.keys, keySlice, sizeof r.keys);
    for(int b = 0; b < 3; b++) r.u.nudge.orange[b] = r.u.nudge.hit[b] = 0;
    for(int j = 0; j < n; j++)
    {
        const int t = first + j;
        r.u.nudge.start[j] = slices[t].start;
        if(tableFlags[t] & kFlagOrange) r.u.nudge.orange[j >> 5] |= 1u << (j & 31);
        if(stepHit[t])                  r.u.nudge.hit[j >> 5]    |= 1u << (j & 31);
    }
}

// BETA: every slot of the marker range [nSlices, kMaxTable) — live, tombstoned
// or past nTable — with the key map and nTable: what restoring any run of
// marker ops (tombstones, adds into reused or fresh slots, re-spans) and a
// key-map change needs. A bulk op's step is this one record (EditReq::kSnap),
// its later ops kQuiet.
inline void Engine::UndoCaptureSnap(UndoRec& r) const
{
    r.kind = UndoRec::kSnap;
    r.first = (uint8_t)nSlices;
    r.n = (uint8_t)(kMaxTable - nSlices);
    r.nTable = (uint8_t)nTable;
    memcpy(r.keys, keySlice, sizeof r.keys);
    for(int j = 0; j < r.n; j++) UndoCaptureEntry(r.u.e[j], nSlices + j);
}

// KEYP: every key's settings, with the key map and nTable (unchanged by the op; the swap below is then a no-op).
inline void Engine::UndoCaptureKeyP(UndoRec& r, int key) const
{
    r.kind = UndoRec::kKeyP;
    r.first = (uint8_t)(key < 0 ? 0 : key);
    r.n = 0;
    r.nTable = (uint8_t)nTable;
    memcpy(r.keys, keySlice, sizeof r.keys);
    memcpy(r.u.kp, keyp, sizeof r.u.kp);
}

// Restore r and leave the state it replaced in r (the record becomes its
// own inverse). true = a grid entry was touched (the overlay must follow).
inline bool Engine::UndoSwap(UndoRec& r)
{
    bool grid = false;
    if(r.kind == UndoRec::kKeyP)   /* KEYP: the settings trade places; gain and rate worked out again for the keys that moved */
    {
        for(int k = 0; k < kKeys; k++)
        {
            const KeyParam was = keyp[k], to = r.u.kp[k];
            if(memcmp(&was, &to, sizeof was) != 0) PutKeyParam(k, to.Valid() ? to : KeyParam{0, 0, 0});
            r.u.kp[k] = was;
        }
    }
    else if(r.kind == UndoRec::kSnap)
    {
        for(int j = 0; j < r.n && r.first + j < kMaxTable; j++)
        {
            UndoEntry now;
            UndoCaptureEntry(now, r.u.e[j].idx);
            UndoRestoreEntry(r.u.e[j]);
            r.u.e[j] = now;
        }
        if(r.first < nSlices) grid = true;   // never, by construction (first = nSlices)
    }
    else if(r.kind == UndoRec::kStarts)
    {
        const int first = r.first, n = r.n;
        if(first + n > kMaxTable) return false;
        for(int j = 0; j < n; j++)
        {
            const int t = first + j;
            const int32_t  was = slices[t].start;
            const uint32_t bit = 1u << (j & 31);
            const bool wasOrange = (tableFlags[t] & kFlagOrange) != 0, wasHit = stepHit[t];
            slices[t].start = r.u.nudge.start[j];
            if(r.u.nudge.orange[j >> 5] & bit) tableFlags[t] |= kFlagOrange; else tableFlags[t] &= (uint8_t)~kFlagOrange;
            const bool hit = (r.u.nudge.hit[j >> 5] & bit) != 0;
            r.u.nudge.start[j] = was;
            if(wasOrange) r.u.nudge.orange[j >> 5] |= bit; else r.u.nudge.orange[j >> 5] &= ~bit;
            if(wasHit)    r.u.nudge.hit[j >> 5]    |= bit; else r.u.nudge.hit[j >> 5]    &= ~bit;
            stepHit[t] = hit;   // RefreshEntry below re-derives it; put back after
        }
        if(first < nSlices)
        {
            grid = true;
            for(int i = 0; i < nSlices; i++)
                slices[i].len = (i + 1 < nSlices ? slices[i + 1].start : bufLen) - slices[i].start;
        }
        for(int j = 0; j < n; j++)
        {
            const int t = first + j;
            const bool hit = stepHit[t];
            RefreshEntry(t);
            stepHit[t] = hit;
            if(t >= nSlices) recMeta[t].hit = hit;
        }
    }
    else
    {
        for(int j = 0; j < r.n && j < 2; j++)
        {
            UndoEntry now;
            UndoCaptureEntry(now, r.u.e[j].idx);
            UndoRestoreEntry(r.u.e[j]);
            if(r.u.e[j].idx < nSlices) grid = true;
            r.u.e[j] = now;
        }
    }
    // the key map and nTable trade places too
    int8_t keys[kKeys];
    memcpy(keys, keySlice, sizeof keys);
    memcpy(keySlice, r.keys, sizeof keys);
    memcpy(r.keys, keys, sizeof keys);
    const int was = nTable;
    if(r.nTable < nTable) for(int t = r.nTable; t < nTable; t++) tableFlags[t] = 0;   // the dropped slots read as fresh
    nTable   = r.nTable;
    r.nTable = (uint8_t)was;
    return grid;
}

/* ── the hooks ApplyEdit calls ────────────────────────────────────────── */

// Before the op: capture what it will touch into the scratch record.
// false = no journal (nothing to commit later).
inline bool Engine::UndoBegin(const EditReq& r)
{
    if(!undo) return false;
    if(undo->gen != tableGen) undo->Clear();   // something went past the journal: the history restarts here
    UndoRec& s = undo->scratch;
    s.op = (uint8_t)r.op; s.head = 1; s.pad[0] = s.pad[1] = 0;
    if(r.flags & EditReq::kSnap) { UndoCaptureSnap(s); undo->armed = 1; return true; }   /* BETA: a bulk op's one record */
    int idx[2] = {-1, -1};
    int n = 0;
    switch(r.op)
    {
        case EditOp::SetBoundary:
        case EditOp::Promote:   idx[0] = r.i - 1; idx[1] = r.i; n = 2; break;
        case EditOp::SetEntry:
        case EditOp::Tombstone: idx[0] = r.t; n = 1; break;
        case EditOp::AddEntry:  idx[0] = FreeSlot(); n = 1; break;
        case EditOp::Record:    idx[0] = (r.key >= 0 && r.key < kKeys && RecordedEntry(r.key) >= 0) ? RecordedEntry(r.key) : FreeSlot(); n = 1; break;   /* PADS */
        case EditOp::Split:     idx[0] = r.t; idx[1] = FreeSlot(); n = 2; break;
        case EditOp::Merge:     idx[0] = r.t; idx[1] = MergeTarget(r.t); n = 2; break;
        case EditOp::SetKey:
        case EditOp::SetKeyMap: n = 0; break;   // the key map alone (BETA: SetKeyMap too)
        case EditOp::SetKeyParams: UndoCaptureKeyP(s, r.key); undo->armed = 1; return true;   /* KEYP */
        case EditOp::SwapKeys:
            if(r.a >= 0 && r.a < kKeys) idx[0] = keySlice[r.a];
            if(r.b >= 0 && r.b < kKeys) idx[1] = keySlice[r.b];
            n = 2; break;   // the recorded owners trade
        case EditOp::NudgeAll:
            if(r.b == 0) UndoCaptureStarts(s, 0, nSlices);
            else         UndoCaptureStarts(s, nSlices, nTable - nSlices);
            undo->armed = 1;
            return true;
        default: return false;
    }
    UndoCaptureEntries(s, idx, n);
    undo->armed = 1;
    return true;
}

// BETA: kQuiet is honoured — no record — only when the journal is live, the
// cursor is at its end (nothing undone since) and the newest record restores
// everything the op writes: any record carries the whole key map (SetKeyMap),
// a snapshot carries the whole marker range (the marker ops a bulk plan
// posts). Otherwise the op is journaled: joined to the step it follows when
// the cursor is at the end (UndoAtEnd), a step of its own when it is not.
inline bool Engine::UndoQuiet(const EditReq& r) const
{
    if(!undo || !(r.flags & EditReq::kQuiet) || (r.flags & EditReq::kSnap)) return false;
    if(undo->gen != tableGen || undo->count <= 0 || undo->cur != undo->count) return false;
    if(r.op == EditOp::SetKeyMap) return true;
    const UndoRec& last = undo->Slot(undo->count - 1);
    if(r.op == EditOp::SetKeyParams) return last.kind == UndoRec::kKeyP && last.first == r.key;   /* KEYP: a drag of one key's slider */
    if(last.kind != UndoRec::kSnap) return false;
    switch(r.op)
    {
        case EditOp::AddEntry: case EditOp::SetEntry: case EditOp::Tombstone: return r.op != EditOp::SetEntry || r.t >= nSlices;
        default: return false;
    }
}

inline void Engine::UndoKeepLive() { if(undo) undo->gen = tableGen; }
inline bool Engine::UndoAtEnd() const { return undo && undo->gen == tableGen && undo->count > 0 && undo->cur == undo->count; }

// After a successful op (tableGen already bumped): commit the capture.
inline void Engine::UndoCommit(bool head)
{
    if(!undo || !undo->armed) return;
    undo->scratch.head = head ? 1u : 0u;
    undo->Push();
    undo->gen = tableGen;
}

// The Undo / Redo op bodies: walk one step of the ring. BadArgs = nothing
// to undo / redo (or a journal something went past — emptied here).
inline EditStatus Engine::UndoStep(bool redo)
{
    if(!undo) return EditStatus::BadArgs;
    if(undo->gen != tableGen) { undo->Clear(); return EditStatus::BadArgs; }
    UndoJournal& j = *undo;
    bool grid = false;
    if(!redo)
    {
        if(j.cur <= 0) return EditStatus::BadArgs;
        bool head;
        do { j.cur--; UndoRec& r = j.Slot(j.cur); head = r.head != 0; grid |= UndoSwap(r); } while(!head && j.cur > 0);
    }
    else
    {
        if(j.cur >= j.count) return EditStatus::BadArgs;
        do { grid |= UndoSwap(j.Slot(j.cur)); j.cur++; } while(j.cur < j.count && !j.Slot(j.cur).head);
    }
    if(grid) SaveOverlay();
    BumpGen();
    j.gen = tableGen;
    return EditStatus::Ok;
}

inline void Engine::SetUndoJournal(UndoJournal* j) { undo = j; if(j) j->Clear(); }
inline void Engine::UndoClear() { if(undo) undo->Clear(); }
inline int  Engine::UndoDepth() const { return undo && undo->gen == tableGen ? undo->UndoSteps() : 0; }
inline int  Engine::RedoDepth() const { return undo && undo->gen == tableGen ? undo->RedoSteps() : 0; }

} // namespace bb
