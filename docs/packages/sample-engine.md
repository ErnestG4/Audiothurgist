# SAMPLE package, phase A — the engine contract

*Branch `pkg2/engine`, 2026-09-07. Owns the SAMPLE additions to
`core/bb_engine.h`, the new `core/bb_sample.h`, and the `/* SAMPLE */`
fences in `src/audiothurgist.cpp`. Plan: `docs/sample-mode-plan.md`. The
record (B), loop-record (C), looper (D), persistence (E) and web (F)
phases build on the API below; nothing here implements a gesture, an LED,
a WAV or a JSON.*

## What it is

One material buffer, one slice table, two playback rules — the plan's §1,
made concrete:

- The **buffer** is one or two planes (`buf` = L, `bufR` = R or nullptr)
  of `bufCap` samples. The **material** (the loaded break) occupies
  `[0, bufLen)`; **recordings** are appended after it, up to `bufCap`.
  `freePos` is the end of used material — where the next take lands.
- The **table** `slices[kMaxTable]` has a *grid* part `[0, nSlices)` —
  exactly today's slices; the grammars, the clock, the bar rule, gates,
  hits, punches, tails, zoom and continuity see only this and are
  unchanged — and a *recorded* part `[nSlices, nTable)`, one entry per key
  that has been recorded into. `kMaxTable = kMaxSlices + 32 = 96`, so a
  4-bar (64-step) break still has room for every one of the 24 keys.
  Per-slice metadata (`stepClass`, `sliceSal`, `sliceDens`, `stepHit`,
  `stepBands`, loop points) is indexed by table index and sized
  `kMaxTable`.
- The **key table** `keySlice[kKeys = 24]` says which table entry each
  Voice-map note plays. `SetMaterial` fills it with the pre-SAMPLE span
  mapping (`k · nSlices / span`, `span = min(nSlices, 24)`, keys past the
  span play the last slice), then every recorded entry claims its key.
- Two **playback rules** for a call: *one-shot* (the slice, exactly as
  before — byte-identical, held by goldens) and *held* (read from the
  slice's start to the end of the material, or of the recording, until the
  key is released — the sampler rule).

Everything the audio thread touches is a plain array or scalar; nothing
allocates; the engine stays pure and host-tested.

## API

Thread column: **control** = the shell's main loop / OnPoll; **audio** =
the audio callback (the shell defers poll-thread calls through `gCallReq`
and `gRelKey`, exactly as CallSlice already was). "either" = safe from
both, but never concurrently with itself.

### Material and planes

| Call | Thread | Contract |
|---|---|---|
| `void SetMaterial(float* b, int len, float sr, const int* onsets, int nOnsets, const uint8_t* classes = nullptr, int nSlicesHint = 0, float* bR = nullptr, int cap = 0)` | audio (or before audio starts) | The old signature plus the R plane and the capacity. `cap = 0` → `bufCap = len`: no room to record (every old caller). **Same planes (same `b` and `bR` pointers) and every recorded entry lies past `len`** → the recordings survive: relocated to `[nSlices, …)`, keys re-pointed, metadata restored, `freePos` kept. Anything else is new material: the table is the grid, `freePos = len`. A take in flight (armed or pending) is cancelled either way. |
| `void Process(float* out, int n)` | audio | The mono render (unchanged, op for op). Stereo material renders its L plane. |
| `void Process(float* outL, float* outR, int n)` | audio | Both planes; mono material renders once and copies L to R. One position, one envelope and one set of loop points per voice serve both planes; the seam bridge (`clickComp`) is per plane. |
| `Voice::Render(const float* L, const float* R, float& outL, float& outR)` | audio | The per-voice reader; `R == L` or nullptr is mono. `float Render(const float* buf)` remains. |

### Key table and held playback

| Call | Thread | Contract |
|---|---|---|
| `int KeySlice(int key) const` | either | The table entry key 0..23 plays (clamped both ways; always valid while `nTable > 0`). |
| `bool SetKeySlice(int key, int slice)` | control | Point a key at any table entry. false = out of range. Reset by `SetMaterial`. |
| `void CallSlice(int idx, float rateMul, bool rev, bool punch, bool ratchet, float lenMul = 1.f, bool held = false, int key = -1)` | audio | The old call plus the rule. `held = false`: today's behaviour, byte-identical. `held = true`: the voice reads from the slice's start to **`ReadEnd(idx)`** — the material's end (`bufLen`) for a grid slice, its own end for a recorded one — with no zoom, no tail, no staccato; tagged `Voice::held`, `Voice::key = key`. Reversed and held reads back to the material's (or recording's) start. `idx` is clamped to `nTable - 1`, so a recorded entry can be called and punched. A ratcheted held call's double is held too. |
| `void ReleaseKey(int key)` | audio | Every held voice tagged `key` fades out with the normal choke geometry (5 ms fixed, or the slice's adaptive release; a reversed voice flips forward first, value-continuous). One-shot voices are untouched; an unknown key is a no-op. |
| `int ReadEnd(int idx) const` | either | A voice's read limit for table entry `idx` (above). No fade ever reads past it — a choke at the last grid slice must not play the first recording's attack. |

A held voice belongs to its key: the sequencer's continuity branch never
adopts it, and a new hit only chokes it in the last 10 ms before its end,
exactly as any voice. Several held voices may sound at once (one per key,
or one per press).

### Recording into a key

State machine: **Idle → Armed → Pending → Idle**. `RecArm` refuses while
Armed or Pending; `RecStop` moves Armed → Pending (or → Idle when nothing
was taken); `CommitSlice` and `RecDiscard` move Pending → Idle.
`SetMaterial` cancels anything in flight.

| Call | Thread | Contract |
|---|---|---|
| `bool RecArm(int key)` | control | Arm a take into key 0..23 at `freePos`. **false** = a take is armed or pending, fewer than `kRecMin` (480 samples, 10 ms) remain before `bufCap`, or the table is full and the key has no entry to replace (unreachable through 24 keys with `kMaxTable = 96`). The audio thread starts appending on its next `RecFeed`. |
| `void RecFeed(const float* in, const float* inR, int n)` / `void RecFeed(const float* in, int n)` | audio | Append `n` input samples at the write head while armed; O(n), one bounds clamp, no per-sample branch. `inR` feeds the R plane (nullptr: the L input is copied to both planes); ignored on mono material. Stops itself at `bufCap` (`recFull = true`, `recArmed = false`). Call it every block whatever the state — it returns at once when idle. |
| `int RecStop(int& start, int& len)` | control | Stop the take (the audio thread appends nothing after this returns) and hand back its raw region `[start, start + len)`; returns `len` (0 = nothing taken, state → Idle). The region now belongs to the control thread. |
| `void RecDiscard()` | control | Drop a pending take; its memory is free again. |
| `int CommitSlice(int key, int start, int len, uint8_t cls, float sal, float dens, bool hit)` | control | Publish `[start, start + len)` as key's slice: a new table entry, or the key's existing recorded entry replaced (its old memory is orphaned until the set is saved and reloaded); `stepClass/sliceSal/sliceDens/stepHit` set, `stepBands = 0` (dominant class), loop points re-searched, `keySlice[key]` → the entry, `freePos` advanced past the region. Returns the table index or -1 (bad range, `len < kRecMin`, table full). Publication order — region, entry, count, key — leaves the table consistent at every step for the audio thread. |
| `void SyncRecordedMeta()` | either | Re-assert the recorded entries' metadata after a grid-wide copy into `stepClass/sliceSal/sliceDens/stepBands` (the shell's `kMaxSlices`-wide memcpys would overwrite entries `[nSlices, 64)`). Idempotent; `SetMaterial` calls it; the shell calls it after its copies (three places, fenced). |
| `int RecordedEntry(int key) const` | either | The table index of key's recording, or -1. |

Readable state: `recArmed` (volatile), `recPending`, `recFull` (volatile:
the take hit the cap), `recKey`, `recPos` (volatile: the write head — an
input meter or a "seconds left" display), `freePos`, `bufCap`, `nTable`,
`tableKey[t]` (owner key of entry `t ≥ nSlices`).

Between `RecStop` and `CommitSlice` the shell conditions the region **in
place** with `core/bb_sample.h` (control thread, `bb::Yield()` inside):

| Helper | Contract |
|---|---|
| `bool bb::TrimRegion(float* x, float* xR, int start, int len, float sr, int& outStart, int& outLen)` | Trim silence at −40 dB re the region's own peak (breaths of ~0.7 ms before, 5 ms after, as `ConditionBuffer`), normalise to 0.95 with one gain for both planes. Nothing moves: the result lies inside the input; the leading silence stays orphaned (a few ms). false = silent or shorter than `kRecMin` → `RecDiscard`. |
| `void bb::AnalyseRegion(const float* x, int start, int len, float sr, int mode, uint8_t& cls, float& sal, float& dens, bool& hit, float refRms = 0.f)` | The recorded twin of the per-step analysis: `cls` = dominant band judged **absolutely** (Hockman 325/4000 for modes 0 and 3 — the Density lens needs a population to rank —, legacy 120/900 for 1, lo/hi for 2; silence → 3); `sal` = RMS / `refRms` clamped to 1 (`refRms ≤ 0` → 1.0: a normalised take is a full hit); `dens` = keyframe density over the region; `hit` = the first 10 ms reach a quarter of the peak. First guesses for ears, like the lens ratios. |

The canonical sequence, per key:

```
if(engine.RecArm(key)) { … while the gate is held the audio thread calls
                          engine.RecFeed(inL, inR, n) every block … }
int rs, rl; if(engine.RecStop(rs, rl) > 0)
{
    int ts, tl;
    if(bb::TrimRegion(gPlay, gPlayR, rs, rl, kSr, ts, tl))
    {
        uint8_t cls; float sal, dens; bool hit;
        bb::AnalyseRegion(gPlay, ts, tl, kSr, gClass.mode, cls, sal, dens, hit);
        engine.CommitSlice(key, ts, tl, cls, sal, dens, hit);
    }
    else engine.RecDiscard();
}
```

## Invariants

1. `0 ≤ bufLen ≤ freePos ≤ bufCap`; every table entry lies in
   `[0, freePos)`; grid entries in `[0, bufLen)`.
2. `nSlices ≤ nTable ≤ kMaxTable`; entries `[nSlices, nTable)` each own a
   distinct key (`tableKey`), and `keySlice[tableKey[t]] == t`.
3. `keySlice[k]` is a valid table index for every `k` while `nTable > 0`.
4. Grid behaviour with no recordings is **byte-identical** to the
   pre-SAMPLE engine: the four PLAY goldens and the SAMPLE golden
   (`786e423d477968a2`: calls, chokes, a steal, adaptive fades, zoom, a
   ratchet) hold for `Process(out, n)`, for `Process(L, R, n)` on mono
   material, and for stereo material whose R plane duplicates L.
5. A voice never reads past its `bound` (`Voice::bound = ReadEnd(idx)` at
   trigger); choke and release fades clamp to it.
6. The audio thread writes only `[recPos, bufCap)` while `recArmed`; the
   control thread writes a region only between `RecStop` and
   `CommitSlice`/`RecDiscard`, when no table entry points into it and the
   take is stopped.
7. `SetMaterial` on the same planes keeps recordings past the new
   material (Options → Bars / Swing / Onsets / Classifier re-applies keep
   the kit); a load into the other buffer starts over. Phase E's loader
   should reload a set's table through `CommitSlice` after `SetMaterial`.

## Shell hooks landed (`/* SAMPLE */` fences in `src/audiothurgist.cpp`)

- **Options byte 17 "Key Plays"**: One-shot / Held (`gKeyPlays`, `Sel`,
  clamp 1). `kBytes = 18`, schema `OPT3 → OPT4`, `Describe` field 17
  (`key.plays`). Presets saved under OPT3 re-init.
- **Voice map**: `idx = engine.KeySlice(key)` (`key = min(note, 23)`)
  replaces the inline span mapping; the call carries `gCallHeld`
  (`Key Plays == Held`) and `gCallKey`; `gClkEdge.JustFell(3u)` (J6, the
  SDK's `CvGate` has it) posts `gRelKey`, which AudioCb turns into
  `engine.ReleaseKey` right after any pending call.
- **AudioCb / boot**: `SetMaterial(…, nullptr, kBufMax)`; the grid-wide
  `memcpy`s copy the **source's** size (the engine's arrays are wider) and
  are followed by `engine.SyncRecordedMeta()`.
- **Not hooked** (phase B): `RecFeed` from the audio input, the record
  gestures, the LEDs, stereo planes in the shell (`gBufA/gBufB` are still
  one mono plane each of `kBufMax` — the loader may fill it entirely, in
  which case `RecArm` fails: phase B sizes the planes, per the plan's §1).

## Tests (`tests/engine_check.cpp`, SAMPLE sections; `tests/run.sh`)

Key table default and override on 16 and 64 slices; a held call reading on
past its slice (one-shot stays silent there), tagged, released with a
bounded max |Δ| (no click), the wrong key a no-op, reversed held reading
back to the start, continuity leaving a held voice alone; record: arm →
feed 12000 samples of 1 kHz → stop → trim → analyse (mid band, hit,
density) → commit → the key plays it, freePos advanced, table grew, a
second take into the same key replaces it, a different key appends, a take
stops itself at the cap, a plane with no room refuses `RecArm`,
`CommitSlice` range refusals, `SyncRecordedMeta`, recordings following a
re-slice to 32 and back, new material dropping them; stereo: R renders
its own plane, `Process(L, R)` on mono material gives `R == L ==
Process(out)`, stereo recording lands in both planes (mono feed copied),
and the golden on all three render paths.
