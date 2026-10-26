#pragma once
// START — the module's one device setting, "Start in" (beta-sampler,
// docs/packages/pads.md § Start in): what the module does at power-up.
//
//   * Sampler (0, the default): the module comes up in Pads (grammar 6,
//     free play, nothing written) with the transport stopped — the 24 keys
//     (J4, the page's pads, the Kit) play samples, and a beat starts only when
//     the player picks a drum grammar and presses ▶ / holds B1.
//   * As saved (1): the boot preset's grammar, running — the module as it
//     was before this setting.
//
// Where it lives, and why not in Options: the preset-managed Options block is
// 24 bytes under SchemaHash 'OPT8'; a 25th byte moves the slot hash, the SDK
// then drops every preset slot, and only options + knobs + sample + name come
// back from presets/<slot>.json — motion locks and brightness do not, and a
// slot without a backup is gone. Start in is a power-up behaviour of the
// device, not of a preset, so it lives in one small file on the card,
// 0:/Audiothurgist/startup.json:
//
//     {"audiothurgist":1,"startIn":"sampler"}      ("saved" for As saved)
//
// No file, an unreadable file, an unknown value, or no card at all → Sampler.
// The page sets it with ACTION 40 START_IN and reads it back from GET_STATE
// byte 3 bit 5 (set = As saved); the descriptor says "startIn":1.
//
// The pure half (the text both ways) and the FatFs half (read at boot, write
// on a change — tmp + rename, as the set JSON) are host-tested in
// tests/card_check.cpp over tests/fatfs_shim.h. DMA law as bb_cardfs.h: the
// caller lends the FIL and the buffer (AXI SRAM on the module); control
// thread only, under SdCard::BusyGuard.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "bb_card.h"
#include "bb_json.h"

namespace bb {

enum StartIn : uint8_t { kStartSampler = 0u, kStartSaved = 1u };
constexpr const char* kStartupPath    = "0:/Audiothurgist/startup.json";
constexpr const char* kStartupTmpPath = "0:/Audiothurgist/startup.json.tmp";
constexpr size_t      kStartupMax     = 512u;   // the file is ~40 bytes; anything past this is not ours

/* The text of the file; 0 when `cap` is too small. */
inline size_t StartupJson(uint8_t startIn, char* out, size_t cap)
{
    JsonOut j(out, cap);
    j.BeginObject();
    j.Key("audiothurgist"); j.Int(1);
    j.Key("startIn"); j.String(startIn == kStartSaved ? "saved" : "sampler");
    j.EndObject();
    j.Put('\n');
    return j.ok ? j.len : 0u;
}

/* The setting a file's text says; Sampler for anything else (no text, a
 * broken file, another version's value) — the default is the safe reading. */
inline uint8_t StartupRead(const char* text, size_t n)
{
    if(!text || !n) return kStartSampler;
    JsonIn in(text, n);
    if(!in.BeginObject()) return kStartSampler;
    char    key[24];
    uint8_t v = kStartSampler;
    while(in.NextKey(key, sizeof key))
    {
        if(!strcmp(key, "startIn"))
        {
            char s[16];
            if(!in.String(s, sizeof s)) return kStartSampler;
            v = !strcmp(s, "saved") ? kStartSaved : kStartSampler;
        }
        else in.Skip();
    }
    return in.ok ? v : kStartSampler;
}

/* Boot: read the file into `buf` (lent, kStartupMax bytes or more) and
 * return the setting; Sampler when there is no file or it cannot be read.
 * The caller has checked the card is mounted. */
static uint8_t ReadStartup(FIL* f, char* buf, size_t cap)
{
    if(cap < 2u || f_open(f, kStartupPath, FA_READ) != FR_OK) return kStartSampler;
    UINT       br = 0;   /* one read of up to cap − 1: a file that fills it is not ours */
    const bool rd = f_read(f, buf, (UINT)(cap - 1u), &br) == FR_OK && br > 0 && br < (UINT)(cap - 1u);
    f_close(f);
    return rd ? StartupRead(buf, br) : kStartSampler;
}

/* A change: write the whole file next to it and rename it over the old one,
 * so a power-off mid-write leaves the old setting, never half a file. The
 * folder is the caller's (CardMkdirs). false = not written (the old file, if
 * any, stays). */
static bool WriteStartup(FIL* f, uint8_t startIn, char* buf, size_t cap)
{
    const size_t n = StartupJson(startIn, buf, cap);
    if(!n) return false;
    if(f_open(f, kStartupTmpPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT bw = 0;
    bool ok = f_write(f, buf, (UINT)n, &bw) == FR_OK && bw == (UINT)n;
    ok = (f_close(f) == FR_OK) && ok;
    if(ok) { f_unlink(kStartupPath); ok = f_rename(kStartupTmpPath, kStartupPath) == FR_OK; }
    if(!ok) f_unlink(kStartupTmpPath);
    return ok;
}

} // namespace bb
