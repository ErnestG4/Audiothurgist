#pragma once
// CARD — the pure pieces of the card layout, the upload receiver and the
// copy plan (docs/packages/card.md). The FatFs half is core/bb_cardfs.h; the
// shell's plumbing (the 0x67 command, the state tail, the main-loop service)
// is src/audiothurgist.cpp. Host-tested in tests/card_check.cpp.
//
//   * One folder. Everything the module reads or writes lives under
//     0:/Audiothurgist (created with this casing; FAT matches it
//     case-insensitively): breaks/*.wav are the breaks the module lists,
//     sets/ holds the sets (<name>.wav + <name>.json) and the loose files'
//     sidecars (<wav>.slices.json), presets/ the preset backups
//     (<slot>.json). Nothing outside it is read — an old card's root WAVs may
//     belong to another module, so they are only ever COPIED in, on request.
//   * List entries stay what they were: "amen.wav" (a break) and
//     "sets/amen.wav" (a set's WAV); EntryPath turns one into its card path.
//     A preset's sample name therefore means the same thing in both layouts.
//   * Upload names: 1..100 printable ASCII, none of / \ : * ? " < > |, no
//     leading space, no trailing space or dot (FAT strips those), not
//     starting with '.'; the module appends ".wav".
//   * UploadRx: the bytes arrive in order, from zero; anything out of
//     sequence resets the transfer instead of being patched in; the size is
//     checked against the buffer before a byte is copied (Kyklophoria's
//     WorldReceiver rules).
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace bb {

/* ── the layout ─────────────────────────────────────────────────────── */
constexpr const char* kCardDir    = "0:/Audiothurgist";
constexpr const char* kBreaksDir  = "0:/Audiothurgist/breaks";
constexpr const char* kSetsDir    = "0:/Audiothurgist/sets";
constexpr const char* kPresetsDir = "0:/Audiothurgist/presets";
constexpr const char* kBreaksPre  = "0:/Audiothurgist/breaks/";   // … with the separator, for joins
constexpr const char* kSetsPre    = "0:/Audiothurgist/sets/";
constexpr int    kBreakNameMax = 100;          // stem characters; ".wav" is appended
constexpr int    kEntryMax     = 128;          // a list entry incl. NUL (gFiles' stride)
constexpr int    kEntryNameMax = 120;          // longest card name the scan lists
constexpr size_t kCardPathMax  = 300;          // a copy path: 24 + 255 (FAT LFN) + ".part"

/* 0x67 reply statuses (HostLink's own 0 / 2 / 3 / 6, then the card's) */
constexpr uint8_t kStOk = 0u, kStBadArgs = 2u, kStBadState = 3u, kStTooLarge = 6u;
constexpr uint8_t kStExists = 20u, kStNoCard = 21u, kStFull = 22u;   // Kyklophoria's kStatCardExists = 20

/* the state tail's card result (byte 278): the last finished op */
enum CardResult : uint8_t
{
    kCardOk = 0, kCardNoCard = 1, kCardFull = 2, kCardWriteFail = 3, kCardBadName = 4,
    kCardNotFound = 5, kCardBusy = 6, kCardBadFile = 7, kCardExists = 8, kCardTimeout = 9,
    kCardNothing = 10,   // a copy found nothing to copy
};
/* the state tail's card state (byte 276) */
enum CardState : uint8_t { kCardIdle = 0, kCardReceiving = 1, kCardWriting = 2, kCardCopying = 3 };

namespace carddetail {
inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
inline bool SuffixCi(const char* s, const char* suf)
{
    const size_t n = strlen(s), m = strlen(suf);
    if(m > n) return false;
    for(size_t i = 0; i < m; i++) if(Lower(s[n - m + i]) != Lower(suf[i])) return false;
    return true;
}
inline bool PrefixCi(const char* s, const char* p)
{
    while(*p) { if(!*s || Lower(*s) != Lower(*p)) return false; s++; p++; }
    return true;
}
inline int CmpCi(const char* a, const char* b)
{
    for(;; a++, b++)
    {
        const int x = (unsigned char)Lower(*a), y = (unsigned char)Lower(*b);
        if(x != y || !x) return x - y;
    }
}
// bounded join of up to three parts; false (out = "") when it did not fit
inline bool Join(char* out, size_t cap, const char* a, const char* b, const char* c = "")
{
    const size_t na = strlen(a), nb = strlen(b), nc = strlen(c);
    if(cap == 0) return false;
    if(na + nb + nc + 1 > cap) { out[0] = 0; return false; }
    memcpy(out, a, na); memcpy(out + na, b, nb); memcpy(out + na + nb, c, nc + 1);
    return true;
}
} // namespace carddetail

// A hidden or system name the scan and the copy skip: ".", "..", macOS's
// "._amen.wav" AppleDouble shadows, ".Trashes".
inline bool HiddenName(const char* n) { return !n || !n[0] || n[0] == '.'; }
// "*.wav", any case, not hidden.
inline bool IsWavName(const char* n) { return !HiddenName(n) && strlen(n) > 4 && carddetail::SuffixCi(n, ".wav"); }

// An upload's stem: 1..100 printable ASCII, none of the characters FAT (or a
// path) gives meaning to, no leading space, no trailing space or dot.
inline bool BreakNameOk(const char* s)
{
    if(!s || !s[0] || s[0] == ' ' || s[0] == '.') return false;
    int n = 0;
    for(; s[n]; n++)
    {
        if(n >= kBreakNameMax) return false;
        const unsigned char c = (unsigned char)s[n];
        if(c < 0x20 || c > 0x7E) return false;
        if(strchr("/\\:*?\"<>|", c)) return false;
    }
    return s[n - 1] != ' ' && s[n - 1] != '.';
}

// A list entry → its card path: "sets/x.wav" → 0:/Audiothurgist/sets/x.wav,
// anything else → 0:/Audiothurgist/breaks/<entry>. One name, never a path:
// no separator, no drive, not hidden (so never "." or "..").
inline bool EntryPath(const char* entry, char* out, size_t cap)
{
    if(!entry || HiddenName(entry)) { if(cap) out[0] = 0; return false; }
    if(carddetail::PrefixCi(entry, "sets/"))
    {
        if(HiddenName(entry + 5) || strchr(entry + 5, '/') || strchr(entry + 5, '\\') || strchr(entry + 5, ':')) { if(cap) out[0] = 0; return false; }
        return carddetail::Join(out, cap, kSetsPre, entry + 5);
    }
    if(strchr(entry, '/') || strchr(entry, '\\') || strchr(entry, ':')) { if(cap) out[0] = 0; return false; }
    return carddetail::Join(out, cap, kBreaksPre, entry);
}
// An upload's stem → the list entry "<stem>.wav".
inline bool BreakEntry(const char* stem, char* out, size_t cap)
{
    if(!BreakNameOk(stem)) { if(cap) out[0] = 0; return false; }
    return carddetail::Join(out, cap, stem, ".wav");
}
// … → 0:/Audiothurgist/breaks/<stem>.wav and the temp name <stem>.part.
inline bool BreakPaths(const char* stem, char* wav, char* part, size_t cap)
{
    if(!BreakNameOk(stem)) { if(cap) { wav[0] = 0; part[0] = 0; } return false; }
    const bool a = carddetail::Join(wav, cap, kBreaksPre, stem, ".wav");
    const bool b = carddetail::Join(part, cap, kBreaksPre, stem, ".part");
    return a && b;
}
// … → the protocol-form lock path for SdCard::Lock ("/Audiothurgist/breaks/<stem>.wav").
inline bool BreakLockPath(const char* stem, char* out, size_t cap)
{
    if(!BreakNameOk(stem)) { if(cap) out[0] = 0; return false; }
    return carddetail::Join(out, cap, "/Audiothurgist/breaks/", stem, ".wav");
}
// A loose file's sidecar: 0:/Audiothurgist/sets/<basename of entry>.slices.json
// (the entry's directory is dropped: sets/plain.wav, a JSON-less WAV in sets/,
// gets sets/plain.wav.slices.json).
inline bool SidecarPath(const char* entry, char* out, size_t cap)
{
    if(!entry) { if(cap) out[0] = 0; return false; }
    const char* base = strrchr(entry, '/');
    base = base ? base + 1 : entry;
    if(!base[0]) { if(cap) out[0] = 0; return false; }
    return carddetail::Join(out, cap, kSetsPre, base, ".slices.json");
}

// Index of `entry` in a fixed-stride list (case-insensitive, FAT's rule), or -1.
inline int FindCardEntry(const char* files, size_t stride, int n, const char* entry)
{
    for(int i = 0; i < n; i++) if(!carddetail::CmpCi(files + (size_t)i * stride, entry)) return i;
    return -1;
}
// Append `entry` unless it is there: its index, or -1 when the list is full or
// the entry too long. Never re-sorts — every index a preset or a page holds
// stays valid until the next boot sorts the list.
inline int AddCardEntry(char* files, size_t stride, int& n, int max, const char* entry)
{
    const int i = FindCardEntry(files, stride, n, entry);
    if(i >= 0) return i;
    const size_t len = strlen(entry);
    if(n >= max || len + 1 > stride || len == 0) return -1;
    memcpy(files + (size_t)n * stride, entry, len + 1);
    return n++;
}
// Insertion sort of files[from..to) by name, case-insensitive.
inline void SortCardEntries(char* files, size_t stride, int from, int to)
{
    char tmp[kEntryMax];
    if(stride > sizeof tmp) return;
    for(int i = from + 1; i < to; i++)
        for(int j = i; j > from && carddetail::CmpCi(files + (size_t)(j - 1) * stride, files + (size_t)j * stride) > 0; j--)
        {
            memcpy(tmp, files + (size_t)(j - 1) * stride, stride);
            memcpy(files + (size_t)(j - 1) * stride, files + (size_t)j * stride, stride);
            memcpy(files + (size_t)j * stride, tmp, stride);
        }
}

// The upload's bytes are a WAV the loader could try: RIFF / WAVE, then a
// chunk walk inside the bytes that finds "fmt " (PCM or extensible, 1–2
// channels, 16 / 24 bit, ≥ 8 kHz — the loader's own rules) before "data".
// The loader still has the last word at load time; this only keeps junk off
// the card.
inline bool WavBytesOk(const uint8_t* b, uint32_t n)
{
    auto rd16 = [](const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); };
    auto rd32 = [](const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); };
    if(!b || n < 44 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return false;
    uint32_t pos = 12;
    bool fmtOk = false;
    while(pos + 8 <= n)
    {
        const uint32_t sz = rd32(b + pos + 4);
        if(!memcmp(b + pos, "fmt ", 4))
        {
            if(sz < 16 || pos + 8 + 16 > n) return false;
            const uint8_t* d = b + pos + 8;
            const uint32_t fmt = rd16(d), ch = rd16(d + 2), rate = rd32(d + 4), bits = rd16(d + 14);
            fmtOk = (fmt == 1 || fmt == 0xFFFE) && ch >= 1 && ch <= 2 && rate >= 8000 && (bits == 16 || bits == 24);
            if(!fmtOk) return false;
        }
        else if(!memcmp(b + pos, "data", 4)) return fmtOk && sz > 0;
        if(sz > n) return false;
        pos += 8 + sz + (sz & 1u);
    }
    return false;
}

/* ── the upload receiver ────────────────────────────────────────────── */
// The caller lends the buffer (SDRAM on the module) and its capacity. POD:
// no initialisers (the module keeps one in plain .bss, the tests on the
// stack) — Reset() before use.
struct UploadRx
{
    uint8_t* buf;
    uint32_t cap, total, have;
    bool     open;

    void Reset() { total = 0; have = 0; open = false; }
    // a new transfer of `total` bytes: kStOk, kStBadArgs (0), kStTooLarge
    uint8_t Begin(uint32_t t)
    {
        Reset();
        if(t == 0) return kStBadArgs;
        if(!buf || t > cap) return kStTooLarge;
        total = t; open = true;
        return kStOk;
    }
    // bytes at `off` of a transfer of `t`: kStOk; kStBadState when none is
    // open; kStBadArgs (and the transfer dropped) when the total does not
    // match, the offset is not the next byte, or the bytes run past the end.
    uint8_t Take(uint32_t t, uint32_t off, const uint8_t* data, uint32_t len)
    {
        if(!open) return kStBadState;
        if(t != total || off != have || (uint64_t)off + len > total || (len && !data)) { Reset(); return kStBadArgs; }
        if(len) memcpy(buf + off, data, len);
        have = off + len;
        return kStOk;
    }
    bool     Done() const { return open && total > 0 && have == total; }
    uint8_t  Progress() const { return total ? (uint8_t)(((uint64_t)have * 255u) / total) : 0u; }
};

/* ── the copy plan (an old card's files → the new folder) ───────────── */
// Three sources, in order: the root's *.wav → breaks/, the old sets/ → sets/,
// the old presets/ → presets/. Never a directory, never a hidden name, never a
// half-written temp file (.tmp / .part); a name too long for the list is left
// where it is. What already exists at the destination is skipped (decided by
// the FatFs half with f_stat) — so a second copy only fills the gaps.
constexpr int kCopySources = 3;
inline const char* CopySrcDir(int s) { return s == 0 ? "0:" : s == 1 ? "0:/sets" : "0:/presets"; }
inline const char* CopyDstDir(int s) { return s == 0 ? kBreaksDir : s == 1 ? kSetsDir : kPresetsDir; }
inline bool CopyWants(int s, const char* name, bool isDir)
{
    if(isDir || HiddenName(name) || strlen(name) > (size_t)kEntryNameMax) return false;
    if(s == 0) return IsWavName(name);
    if(s < 0 || s >= kCopySources) return false;
    return !carddetail::SuffixCi(name, ".tmp") && !carddetail::SuffixCi(name, ".part");
}
// src, dst and dst's temp name for `name` from source s.
inline bool CopyPaths(int s, const char* name, char* src, char* dst, char* part, size_t cap)
{
    if(s < 0 || s >= kCopySources) return false;
    return carddetail::Join(src, cap, CopySrcDir(s), "/", name) && carddetail::Join(dst, cap, CopyDstDir(s), "/", name)
        && carddetail::Join(part, cap, CopyDstDir(s), "/", name) && carddetail::Join(part + strlen(part), cap - strlen(part), ".part", "");
}

/* ── the survey (state tail 280..): what the card holds, old and new ── */
struct CardSurvey
{
    uint16_t rootWavs;   // *.wav on the root
    uint16_t pending;    // copy candidates whose destination is missing
    uint16_t breaks;     // *.wav in 0:/Audiothurgist/breaks
    bool     oldSets, oldPresets;   // the old 0:/sets, 0:/presets folders exist
};
// tail byte 280: b0 mounted · b1 root WAVs · b2 old sets/ · b3 old presets/ ·
// b4 the module's breaks folder has WAVs · b5 something is not copied in yet
inline uint8_t CardFlags(const CardSurvey& s, bool mounted)
{
    if(!mounted) return 0u;
    return (uint8_t)(1u | (s.rootWavs ? 2u : 0u) | (s.oldSets ? 4u : 0u) | (s.oldPresets ? 8u : 0u)
                     | (s.breaks ? 16u : 0u) | (s.pending ? 32u : 0u));
}

} // namespace bb
