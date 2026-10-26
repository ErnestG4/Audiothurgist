# CARD package — one folder on the card, uploads from the page, an old card copied in (2026-10-23, branch `beta-card`)

*Combust's calls of 2026-10-23: everything the module reads or writes moves
under one folder; an old card is told so and offered a copy, nothing is moved
for you; and the page uploads breaks, converting whatever the browser can
decode. Owns `core/bb_card.h` and `core/bb_cardfs.h` (new), the `/* CARD */`
lines of `src/audiothurgist.cpp`, the new paths in `core/bb_sets.h`,
`tests/card_check.cpp` and `tests/fatfs_shim.h` (new; `persist_check` now
uses the shim too), the Card panel in `web/index.html`, the CARD block of
`web/hostlink.js`, the fake card in `web/demo.js`, and selftest §7c / §9d.
Nothing here has run on hardware; the checklist at the end is Combust's.*

## What

| | before | now |
|---|---|---|
| **Breaks** | `0:/*.wav` — every WAV in the card root | `0:/Audiothurgist/breaks/*.wav` (sorted, then the sets) |
| **Sets** | `0:/sets/<name>.wav` + `.json`, sidecars `0:/sets/<wav>.slices.json` | `0:/Audiothurgist/sets/…` |
| **Preset backups** | `0:/presets/<slot>.json` | `0:/Audiothurgist/presets/<slot>.json` |
| **An old card** | — | the module lists nothing from the root; the page says so and offers **Copy them in** |
| **Getting a break onto the card** | take the card out | the page: drop or pick a file (WAV of any kind, AIFF, MP3, FLAC, OGG…), and the module writes it |

The folders are made when something is first written (an upload, a copy, a set
save, a preset backup, an autosave): `f_mkdir`, `FR_EXIST` is success
(Kyklophoria's note — a blank card's first save otherwise fails with a generic
error). Created with this casing; FAT matches it case-insensitively.

List entries keep their old form — `amen.wav` is a break, `sets/kit.wav` a
set — and `bb::EntryPath` turns one into its card path, so a preset backup's
sample name means the same thing in both layouts.

## Why

- **One folder** because the card is shared: a Lab card often carries other
  modules' files, and a root full of WAVs that are not breaks made P5's list
  long and wrong.
- **Copy, never move**: a WAV in the root may belong to another module. The
  copy runs only on request, skips anything already there (so a second run
  only fills gaps) and leaves every source where it was.
- **Uploads convert in the browser**: `decodeAudioData` already reads float
  WAV, AIFF, MP3, FLAC; the module's loader reads PCM 16/24 only. The page
  sends exactly what the module loads best: ≤ 17 s (kLoadSec — the rest of the
  plane is for takes into keys and, since PADS, samples dropped onto pads: 45 s
  planes, `docs/packages/pads.md`), 48 kHz, mono or stereo, PCM16.

## Where the bytes wait, and why there

An upload arrives **whole into SDRAM** (`gUpBuf`, 3.27 MB: 17 s × 48 kHz ×
stereo × 2 bytes + 4 KB; SDRAM 59.0 % → 63.8 %), and only when the last byte is
in does the main loop write it. So:

- each 0x67 chunk is a bounds check and a `memcpy` — no card access in the
  HostLink handler, nothing for the 1 ms control loop to wait on during the
  transfer;
- the write is **the set save's path**: `gSetState = 3` (every set-op guard
  holds; edits, autosave, set ops wait), `KeepClock` runs the whole control
  loop at 25 Hz, the writer copies 8 KiB at a time into `gSetStage` (AXI SRAM,
  DMA-safe, the DMA pad applied) and yields between transfers — half the set
  writer's 16 KiB, so each wait between yields is shorter;
- a transfer that stops half way leaves the card untouched (and drops after
  5 s of silence); the file appears under its name only by `<name>.part` +
  rename.

Writing chunk by chunk from the handler would have needed no buffer, but every
1 KB `f_write` would have stalled the control loop with no way to yield, and a
card's occasional 50–250 ms busy time would land inside a HostLink poll. The
copy streams through the same staging (a second FIL, `gCardFil`, in AXI SRAM
for the source).

## Protocol

`0x67 CARD`, capability `"card": 1` beside `"keys": 1`. Every reply starts with
a status: HostLink's 0 OK · 2 BAD_ARGS · 3 BAD_STATE · 6 TOO_LARGE, then the
card's own **20 EXISTS** (Kyklophoria's `kStatCardExists`) · **21 NO_CARD** ·
**22 CARD_FULL**.

```
op 0 UP_BEGIN  ← u32 total, u8 flags (b0 overwrite), name (1..100 bytes, the stem)
               → status, u32 the most one upload may be (3 268 096)
op 1 UP_DATA   ← u32 total, u32 offset, bytes      (one chunk per round trip)
               → status, u32 have, u8 done (0 more · 1 complete, write queued · 2 not a WAV: dropped)
op 2 UP_ABORT  → status
op 3 COPY_OLD  → status          (the work is the main loop's; the tail tells)
op 4 RESCAN    → status          (survey again; new files appended to the list)
op 5 LIST      ← u8 first → status, u8 nFiles, u8 first, u8 n, n × {u8 len, label}
```

- **Sequencing** (`bb::UploadRx`, Kyklophoria's WorldReceiver rules): in
  order from zero; a wrong offset, a changed total or bytes past the end is
  BAD_ARGS and drops the transfer; the size is checked against the buffer at
  the begin, before a byte is copied. A begin while another upload is still
  arriving replaces it.
- **Names**: 1..100 printable ASCII, none of `/ \ : * ? " < > |`, no leading
  space or dot, no trailing space or dot; the module appends `.wav`. The page
  sanitises (`HL.breakName`: extension off, accents folded, reserved → `-`)
  and the module validates (`bb::BreakNameOk`, the same table in both test
  suites).
- **Refusals at the begin**: BAD_STATE while a set op runs or waits, a take is
  armed / running / pending, a load is requested or applying, an upload is
  being written, or a copy runs; NO_CARD; EXISTS without overwrite; CARD_FULL
  when the card's FSINFO free count says it will not fit (unknown → try).
- **Chunks**: the page sends `max_body − 24` data bytes (1000 at 1024; 488 at
  the protocol's 512) — the 9-byte header always fits.
- **The list**: the descriptor's Sample dropdown is the list at boot; an
  upload, a copy or a set save appends to the module's list **without
  re-sorting** (every index a preset or the page holds stays valid; the next
  boot sorts). LIST is how the page learns the current one.

State tail, appended after byte 275 (the page gates on length ≥ 293):

```
276 u8 cardState (0 idle · 1 receiving · 2 writing · 3 copying)   277 u8 progress 0..255
278 u8 result (0 ok · 1 no card · 2 card full · 3 write failed · 4 bad name · 5 not found
              · 6 busy · 7 bad file · 8 exists · 9 timed out · 10 nothing to copy)
279 u8 seq (bumps per finished op)
280 u8 flags (b0 mounted · b1 root WAVs · b2 old sets/ · b3 old presets/ · b4 breaks in the folder
              · b5 something not copied in yet)
281 u16 rootWavs · 283 u16 pending · 285 u16 breaks · 287 u8 lastIdx (255 none)
288 u16 copied · 290 u16 skipped (the last copy) · 292 u8 nFiles            — ends at 293
```

`setState` (219) reads **3** while an upload is written or a copy runs, so an
older page's set pill shows "busy" instead of offering a save that would be
refused. The survey (`bb::SurveyCard`) runs at boot, after a copy and on
RESCAN; `pending` counts copy candidates whose destination is missing.

**Throughput.** One chunk per round trip. On the fake (6 ms latency) the page
measured 149 KB/s in Chromium. On the module a 1 KB request is parsed at the
next 1 ms poll and the 9-byte reply leaves at the poll after
(`docs/sdk-quirks.md`: ~2 ms round trip for a small reply), so expect roughly
**250–400 KB/s** — a 17 s stereo break (3.3 MB) in ~10–15 s, plus the write
(~1–3 s at the card's speed). Estimated, not measured on hardware; the row
shows the real KB/s. The state poll interleaves (chunks go at normal priority,
so a click still goes first).

## The page

Kit → **Card — add breaks** (and Play's **Add breaks…** next to the Sample
list). Drop files on the panel (or anywhere on the page) or **Choose files…**;
each gets a row: the name it will have, what it became ("stereo, 3.00 s,
48 kHz PCM16, 563 KB" — and "the first 17 of 22.0 s" when cut), a bar,
"sending 40 % · 310 KB/s", then the module's own result. **Load it now** loads
it (ACTION 6 with `lastIdx`); EXISTS offers **Replace it**; BAD_STATE says why
and offers **Try again**. Every card job goes through one queue (`HL.serial`,
Kyklophoria's `putQueued`): two drops never interleave on the wire.

An old card (flag b5 with no breaks in the folder): a banner under the header
says it plainly — "This card keeps its breaks the old way (4 WAVs in its root,
a sets folder and a presets folder); this firmware reads only the Audiothurgist
folder and moves nothing for you" — with **Copy them in**; the panel says the
same at more length. With breaks already in the folder but old files not yet
copied, only the panel mentions it. Without `"card"` the panel, the button and
the banner stay hidden.

## Tests

- `tests/card_check.cpp` (**94**, ASan/UBSan, in `tests/run.sh`): paths
  (entries, a climb or a drive refused, the 100-char name fits), the name rule
  (good and bad tables), WavBytesOk (writer output passes; float, 8-bit, 4 ch,
  < 8 kHz, data before fmt, a lying size, every short prefix fail), UploadRx
  (sequencing, resets, sizes, progress), the copy plan and the flags; then the
  FatFs half over `tests/fatfs_shim.h` on a scratch card laid out the old way:
  **nothing listed from the root**, the survey, mkdir twice, the copy
  (byte-identical, sources kept, no `.part`, AppleDouble shadows / temp files /
  another module's folder left alone, a second run skips everything and never
  overwrites), the list sorted, the rescan that appends without moving an
  index, the upload written / EXISTS / overwritten / cut short by a full card
  with nothing left behind, the uploaded file read back by the firmware's own
  `bbwav::Load`, a yield per 8 KiB and **every write DMA-aligned** (the shim
  counts misaligned whole-sector transfers). Checked to bite: an unpadded
  staging and a missing unlink-before-rename both fail it.
- `tests/sets_check.cpp`: the set and preset paths moved.
- `web/selftest.mjs` §7c / §9d (549 → **612**): the encoder byte by byte, the
  name rule and 2000 fuzzed names through the sanitiser, the plan, the tail;
  the upload against the fake (in-order acks, every request ≤ max_body, polls
  interleaving, the file byte-identical, "Load it now"), every refusal, cancel,
  the 5 s timeout, **two uploads at once fail without the queue and both land
  through it**, the old card's copy (and that nothing moved), no card, a
  pre-card firmware; then the page via real `change` / `drop` / `click`
  gestures. The descriptor-parity check now covers `"card": 1`.
- Visual: Playwright + Chromium with the browser's real decoder (a 3 s stereo
  44.1 kHz WAV and a 22 s mono 32 kHz one), both themes, 1280 × 800 and
  360 × 740, no horizontal scroll.

Sizes (release build, the same SDK): SRAM 451 264 → 456 896 B (91.8 → 93.0 %;
the card code is built `-Os` — none of it is time-critical — and drops
`strcasecmp` / `strstr`), DTCM 111 336 → 111 488 B, SDRAM 39.57 → 42.84 MB.
`tools/sdramcheck.sh` passes; its allow-list gained `gBufAR` with the reason (a
float literal, −12.0f, that the new layout moved into that array's span).

## How Combust tests it

On a card laid out the old way (WAVs in the root, a `sets/` and a `presets/`):

- [ ] Boot: the synth break plays, P5 lists nothing; the page's banner says the
      card keeps its breaks the old way and that nothing is moved.
- [ ] **Copy them in** while the break plays. *Listen for*: no click, no
      tempo wobble, the pattern carries on; P5's ring fills with the progress.
      Then P5 lists the breaks and the sets; on the computer the root files are
      all still there and `Audiothurgist/` holds the copies.
- [ ] Press it again: "already there" for all, nothing changes.
- [ ] Drop a 44.1 kHz stereo WAV, an MP3 and a 30 s file on the page **while a
      break plays with a clock on J3**. *Listen for*: no glitch, no dropout and
      **no drift against the clock** during the transfer and during the write
      (P5's ring) — this is the one that matters. Note the KB/s each row shows.
- [ ] **Load it now** on each: the stereo one plays stereo (Sampling: Stereo),
      the 30 s one is its first 17 s.
- [ ] The same file again: "already on the card"; **Replace it** replaces it.
- [ ] Save a set during an upload's transfer: refused or waits, never both at
      once; the upload still lands.
- [ ] Pull the cable mid-upload: nothing named `<name>.wav` appears; reconnect
      and upload again.
- [ ] Boot with no card: the panel says "no card", uploads are refused plainly.
- [ ] A blank card: the first upload makes `Audiothurgist/breaks` itself.

## Known / deferred

- The descriptor's Sample dropdown in `/program` is still the boot list until
  a reboot (the SDK renders the descriptor once); the page reads LIST instead.
- No card-swap detection: a swapped card is rescanned at boot (or by RESCAN,
  which the page does not yet offer as a button).
- Uploads are breaks only; uploading a set (WAV + JSON) is not offered.
