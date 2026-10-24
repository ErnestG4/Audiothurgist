#pragma once
// CARD — the FatFs half (docs/packages/card.md): make the folders, list the
// breaks and sets, survey an old card, copy an old card's files in, write an
// upload. Written against the FatFs calls the shell has, like wav_loader.h:
// the firmware includes it after ff.h, tests/persist_check.cpp after its
// stdio / POSIX shim, so the same code is exercised on the host.
//
// DMA law (docs/sdk-quirks.md, alchemy/storage/sd_card.h): the caller lends
// every FIL / DIR / FILINFO and the staging buffer, all of them in .axi_bss,
// alignas(32) on the module; every transfer goes through the staging at
// bbwav::DmaPad(file offset). Nothing here is on the stack but names.
//
// Timing: control thread only. Every file loop calls bb::Yield() between
// transfers (the shell's KeepClock: the clock snapshot, and at 25 Hz the
// whole control loop), so a copy or an upload's write keeps the audio and
// the clock exactly as a set save does. The chunk is kCardChunk (8 KiB): half
// the set writer's, so one transfer is a shorter wait between yields.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "bb_card.h"
#include "bb_wavwrite.h"   // bbwav::DmaPad
#include "bb_yield.h"

namespace bb {

constexpr uint32_t kCardChunk = 8192u;

/* 0:/Audiothurgist and its three folders; FR_EXIST is success. A blank card
 * otherwise fails its first save with a generic error (f_open cannot create a
 * file in a folder that is not there) — Kyklophoria's note. */
static bool CardMkdirs()
{
    const char* d[4] = { kCardDir, kBreaksDir, kSetsDir, kPresetsDir };
    for(const char* p : d)
    {
        const FRESULT r = f_mkdir(p);
        if(r != FR_OK && r != FR_EXIST) return false;
    }
    return true;
}

/* One directory entry of pass 0 (breaks/) or 1 (sets/) → its list entry
 * ("amen.wav" / "sets/kit.wav"); false for anything the list does not show. */
static bool CardEntryOf(int pass, const FILINFO* fno, char* out, size_t stride)
{
    if((fno->fattrib & AM_DIR) || !IsWavName(fno->fname) || strlen(fno->fname) > (size_t)kEntryNameMax) return false;
    return carddetail::Join(out, stride, pass == 0 ? "" : "sets/", fno->fname);
}

/* The list: the breaks folder's WAVs sorted, then the sets folder's WAVs
 * sorted as "sets/<name>.wav". The card root is not read. Returns the count. */
static int ScanCard(DIR* dir, FILINFO* fno, char* files, size_t stride, int max)
{
    int n = 0;
    for(int pass = 0; pass < 2; pass++)
    {
        const int from = n;
        if(f_opendir(dir, pass == 0 ? kBreaksDir : kSetsDir) != FR_OK) continue;
        while(n < max && f_readdir(dir, fno) == FR_OK && fno->fname[0])
            if(CardEntryOf(pass, fno, files + (size_t)n * stride, stride)) n++;
        f_closedir(dir);
        SortCardEntries(files, stride, from, n);
    }
    return n;
}

/* A rescan that keeps every index: what the folders hold and the list does not
 * is appended (directory order; the next boot sorts it in). Returns how many
 * were added. */
static int MergeCard(DIR* dir, FILINFO* fno, char* files, size_t stride, int& n, int max)
{
    int added = 0;
    char e[kEntryMax];
    for(int pass = 0; pass < 2; pass++)
    {
        if(f_opendir(dir, pass == 0 ? kBreaksDir : kSetsDir) != FR_OK) continue;
        while(n < max && f_readdir(dir, fno) == FR_OK && fno->fname[0])
        {
            const int before = n;
            if(CardEntryOf(pass, fno, e, sizeof e) && AddCardEntry(files, stride, n, max, e) >= 0 && n > before) added++;
            Yield();
        }
        f_closedir(dir);
    }
    return added;
}

/* What the card holds, old and new (the state tail's 280..): the root's WAVs,
 * the module's breaks, the old folders, and how many copy candidates have no
 * copy in the new folder yet. Bounded by the directories' sizes; yields per
 * entry. */
static void SurveyCard(DIR* dir, FILINFO* fno, CardSurvey& s)
{
    memset(&s, 0, sizeof s);
    if(f_opendir(dir, kBreaksDir) == FR_OK)
    {
        while(f_readdir(dir, fno) == FR_OK && fno->fname[0])
            if(!(fno->fattrib & AM_DIR) && IsWavName(fno->fname) && s.breaks < 0xFFFFu) s.breaks++;
        f_closedir(dir);
    }
    for(int src = 0; src < kCopySources; src++)
    {
        if(f_opendir(dir, src == 0 ? "0:/" : CopySrcDir(src)) != FR_OK) continue;
        if(src == 1) s.oldSets = true;
        if(src == 2) s.oldPresets = true;
        while(f_readdir(dir, fno) == FR_OK && fno->fname[0])
        {
            const bool isDir = (fno->fattrib & AM_DIR) != 0;
            if(src == 0 && !isDir && IsWavName(fno->fname) && s.rootWavs < 0xFFFFu) s.rootWavs++;
            if(!CopyWants(src, fno->fname, isDir)) continue;
            char dst[kCardPathMax];   // CopyWants bounded the name: it fits
            if(!carddetail::Join(dst, sizeof dst, CopyDstDir(src), "/", fno->fname)) continue;
            if(f_stat(dst, fno) != FR_OK && s.pending < 0xFFFFu) s.pending++;
            Yield();
        }
        f_closedir(dir);
    }
}

/* One file, src → dst through `<dst>.part` then a rename, in kCardChunk
 * transfers through the staging. 1 copied, 0 skipped (dst exists), -1 failed,
 * -2 the card is full. A failure leaves no .part behind. */
static int CopyCardFile(FIL* in, FIL* out, FILINFO* fno, const char* src, const char* dst, const char* part,
                        uint8_t* stage, size_t cap, uint32_t* moved = nullptr)
{
    if(f_stat(dst, fno) == FR_OK) return 0;
    if(cap < kCardChunk + 3u) return -1;
    if(f_open(in, src, FA_READ) != FR_OK) return -1;
    if(f_open(out, part, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) { f_close(in); return -1; }
    int      r   = 1;
    uint32_t pos = 0;
    for(;;)
    {
        uint8_t* p  = stage + bbwav::DmaPad(pos);
        UINT     br = 0, bw = 0;
        if(f_read(in, p, (UINT)kCardChunk, &br) != FR_OK) { r = -1; break; }
        if(br == 0) break;
        if(f_write(out, p, br, &bw) != FR_OK) { r = -1; break; }
        if(bw != br) { r = -2; break; }   // FatFs's "disk full": a short write with FR_OK
        pos += br;
        if(moved) *moved += br;
        Yield();
    }
    f_close(in);
    if(f_close(out) != FR_OK && r == 1) r = -1;
    if(r == 1 && f_rename(part, dst) != FR_OK) r = -1;
    if(r != 1) f_unlink(part);
    return r;
}

struct CopyStats { uint16_t copied, skipped, failed; bool full; };

/* The copy: every candidate of the three sources, in order; what exists at the
 * destination is skipped. `onFile(done, total)` after each file (done counts
 * files looked at). Never moves or deletes a source. The folders must exist
 * (CardMkdirs) — they are not made here, because a mkdir inside the root while
 * the root is being walked would change what the walk sees. */
static CopyStats CopyOldCard(DIR* dir, FILINFO* fno, FIL* in, FIL* out, uint8_t* stage, size_t cap,
                             void (*onFile)(uint32_t done, uint32_t total) = nullptr)
{
    CopyStats st;
    memset(&st, 0, sizeof st);
    uint32_t total = 0, done = 0;
    for(int pass = 0; pass < 2; pass++)
        for(int src = 0; src < kCopySources && !st.full; src++)
        {
            if(f_opendir(dir, src == 0 ? "0:/" : CopySrcDir(src)) != FR_OK) continue;
            while(f_readdir(dir, fno) == FR_OK && fno->fname[0] && !st.full)
            {
                if(!CopyWants(src, fno->fname, (fno->fattrib & AM_DIR) != 0)) continue;
                if(pass == 0) { total++; continue; }
                char name[kEntryNameMax + 1], a[kCardPathMax], b[kCardPathMax], c[kCardPathMax];
                memcpy(name, fno->fname, strlen(fno->fname) + 1);
                const int r = CopyPaths(src, name, a, b, c, sizeof a) ? CopyCardFile(in, out, fno, a, b, c, stage, cap) : -1;
                if(r == 1) st.copied++;
                else if(r == 0) st.skipped++;
                else { st.failed++; if(r == -2) st.full = true; }
                done++;
                if(onFile) onFile(done, total);
                Yield();
            }
            f_closedir(dir);
        }
    return st;
}

/* An upload's bytes → `wav` through `part` and a rename, kCardChunk at a time
 * from `data` (any memory: each chunk is copied into the DMA-safe staging
 * first). With `overwrite` an existing file is replaced, else it is kept and
 * kCardExists comes back. `moved` counts bytes for the progress. */
static CardResult WriteCardFile(FIL* out, const char* wav, const char* part, const uint8_t* data, uint32_t n,
                                bool overwrite, uint8_t* stage, size_t cap, FILINFO* fno, uint32_t* moved = nullptr)
{
    if(!overwrite && f_stat(wav, fno) == FR_OK) return kCardExists;
    if(cap < kCardChunk + 3u) return kCardWriteFail;
    if(f_open(out, part, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return kCardWriteFail;
    CardResult r = kCardOk;
    for(uint32_t pos = 0; pos < n;)
    {
        const uint32_t k = n - pos < kCardChunk ? n - pos : kCardChunk;
        uint8_t*       p = stage + bbwav::DmaPad(pos);
        memcpy(p, data + pos, k);
        UINT bw = 0;
        if(f_write(out, p, (UINT)k, &bw) != FR_OK) { r = kCardWriteFail; break; }
        if(bw != k) { r = kCardFull; break; }
        pos += k;
        if(moved) *moved = pos;
        Yield();
    }
    if(f_close(out) != FR_OK && r == kCardOk) r = kCardWriteFail;
    if(r == kCardOk)
    {
        if(overwrite) f_unlink(wav);   // f_rename never replaces: the old file goes first, which is why overwrite is asked for
        if(f_rename(part, wav) != FR_OK) r = kCardWriteFail;
    }
    if(r != kCardOk) f_unlink(part);
    return r;
}

} // namespace bb
