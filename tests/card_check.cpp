// card_check — CARD package (docs/packages/card.md): core/bb_card.h (the
// layout, the upload names, the receiver, the copy plan) and core/bb_cardfs.h
// (the FatFs half) over tests/fatfs_shim.h — a scratch directory as the card.
// tests/run.sh builds it under ASan/UBSan:
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore -Ivendor/capicola/lib
//       tests/card_check.cpp -o /tmp/cc && /tmp/cc /tmp     (one line)
//
// 1. Paths: entries → card paths, the upload name rule, break / sidecar /
//    lock paths, the list's append and sort.
// 2. WavBytesOk: what the writer makes passes; float, 8-bit, 4 channels,
//    data before fmt, a lie in a size, truncation at every prefix fail.
// 3. UploadRx: in order from zero, out of sequence resets, the size checked
//    first, a restart from zero, the progress byte.
// 4. The copy plan and the tail's flags (pure).
// 5. The FatFs half on an old card: nothing listed from the root, the survey,
//    the folders made twice, the copy (byte-identical, the sources kept, no
//    .part left, a second copy skips everything), the list afterwards, an
//    upload written / refused / overwritten / cut short by a full card, the
//    uploaded file through the firmware's own loader, the yield and the DMA
//    alignment of every write.
// 6. beta-sampler: Start in — the file's text both ways (every broken or
//    foreign reading is Sampler), and the FatFs half: no file, written,
//    read back, rewritten, no .tmp left, a stale .tmp, a file too big, no
//    folder, no card (the old setting kept).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>

#include "fatfs_shim.h"
#include "wav_loader.h"
#include "bb_card.h"
#include "bb_cardfs.h"
#include "bb_wavwrite.h"
#include "bb_startup.h"   // beta-sampler: Start in (core/bb_startup.h)

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

static bool MemSink(void* ctx, const void* p, size_t n)
{
    auto* v = (std::vector<uint8_t>*)ctx;
    v->insert(v->end(), (const uint8_t*)p, (const uint8_t*)p + n);
    return true;
}
// A WAV as the page's encoder makes it (bbwav::Write, PCM16), `frames` long.
static std::vector<uint8_t> MakeWav(int frames, bool stereo, bbwav::Format fmt = bbwav::kPcm16, uint32_t sr = 48000u)
{
    std::vector<float> l((size_t)frames), r((size_t)frames);
    for(int i = 0; i < frames; i++) { l[(size_t)i] = 0.5f * sinf(0.01f * (float)i); r[(size_t)i] = -l[(size_t)i]; }
    bbwav::WriteSpec s;
    s.left = l.data(); s.right = stereo ? r.data() : nullptr; s.len = frames; s.sr = sr; s.fmt = fmt;
    std::vector<uint8_t> out;
    static uint8_t staging[4096];
    bbwav::Write({MemSink, &out}, s, staging, sizeof staging);
    return out;
}
static std::vector<uint8_t> ReadAll(const std::string& p)
{
    std::vector<uint8_t> v;
    FILE* f = fopen(p.c_str(), "rb");
    if(!f) return v;
    uint8_t b[4096]; size_t n;
    while((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
    fclose(f);
    return v;
}
static void Put(const std::string& rel, const std::vector<uint8_t>& bytes)
{
    FILE* f = fopen((gShimCard + "/" + rel).c_str(), "wb");
    if(!f) return;
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
}
static void Put(const std::string& rel, const char* text) { Put(rel, std::vector<uint8_t>(text, text + strlen(text))); }
static bool Exists(const std::string& rel) { return ShimExists(gShimCard + "/" + rel); }
static int  gYields = 0;
static void CountYield() { gYields++; }

static void Paths()
{
    SECTION("paths");
    char p[160];
    CHECK(bb::EntryPath("amen.wav", p, sizeof p) && !strcmp(p, "0:/Audiothurgist/breaks/amen.wav"), "break entry → %s", p);
    CHECK(bb::EntryPath("sets/kit.wav", p, sizeof p) && !strcmp(p, "0:/Audiothurgist/sets/kit.wav"), "set entry → %s", p);
    CHECK(bb::EntryPath("SETS/Kit.WAV", p, sizeof p) && !strcmp(p, "0:/Audiothurgist/sets/Kit.WAV"), "set entry, any case → %s", p);
    CHECK(!bb::EntryPath("a/b.wav", p, sizeof p) && p[0] == 0 && !bb::EntryPath("../x.wav", p, sizeof p) && !bb::EntryPath("sets/../x.wav", p, sizeof p) && !bb::EntryPath("sets/..", p, sizeof p) && !bb::EntryPath("sets/.x.wav", p, sizeof p) && !bb::EntryPath("..", p, sizeof p)
          && !bb::EntryPath("sets/a/b.wav", p, sizeof p) && !bb::EntryPath("", p, sizeof p) && !bb::EntryPath("c:x.wav", p, sizeof p) && !bb::EntryPath(".hid.wav", p, sizeof p),
          "an entry that is a path, a climb, empty or hidden is refused");
    CHECK(!bb::EntryPath("amen.wav", p, 20) && p[0] == 0, "a short buffer is refused, not truncated");

    /* the upload name: what the page's sanitiser makes always passes; the rule itself */
    const std::string long100(100, 'a');
    const char* good[] = {"amen", "Amen Break 170", "a", "think (live) #2", "x-y_z.v2", "~tilde", "100%", long100.c_str()};
    bool allGood = true;
    for(const char* g : good) allGood = allGood && bb::BreakNameOk(g);
    CHECK(allGood, "good names pass");
    const std::string long101(101, 'a');
    const char* bad[] = {"", " lead", "trail ", "dot.", ".hidden", "a/b", "a\\b", "c:d", "star*", "q?", "quote\"", "lt<", "gt>", "pipe|", "tab\t", "del\x7f", "é", long101.c_str()};
    int badPassed = 0;
    for(const char* b : bad) if(bb::BreakNameOk(b)) { badPassed++; printf("    passed: [%s]\n", b); }
    CHECK(badPassed == 0, "%d bad names passed", badPassed);
    CHECK(!bb::BreakNameOk(nullptr), "null refused");

    char w[160], t[160];
    CHECK(bb::BreakPaths("amen 170", w, t, sizeof w) && !strcmp(w, "0:/Audiothurgist/breaks/amen 170.wav") && !strcmp(t, "0:/Audiothurgist/breaks/amen 170.part"), "break paths: %s %s", w, t);
    CHECK(!bb::BreakPaths("a/b", w, t, sizeof w) && w[0] == 0 && t[0] == 0, "break paths of a bad name");
    { char w2[160], t2[160]; const std::string n(100, 'z'); CHECK(bb::BreakPaths(n.c_str(), w2, t2, sizeof w2) && strlen(w2) == 24 + 100 + 4, "the longest name fits a 160-byte path (%zu)", strlen(w2)); }
    CHECK(bb::BreakLockPath("amen", w, sizeof w) && !strcmp(w, "/Audiothurgist/breaks/amen.wav"), "lock path %s", w);
    CHECK(bb::BreakEntry("amen", w, sizeof w) && !strcmp(w, "amen.wav") && !bb::BreakEntry("a:b", w, sizeof w), "entry %s", w);
    { const std::string n(100, 'q'); char e[bb::kEntryMax]; CHECK(bb::BreakEntry(n.c_str(), e, sizeof e) && strlen(e) == 104, "the longest upload's entry fits the list's 128"); }
    CHECK(bb::SidecarPath("amen.wav", w, sizeof w) && !strcmp(w, "0:/Audiothurgist/sets/amen.wav.slices.json"), "sidecar %s", w);
    CHECK(bb::SidecarPath("sets/plain.wav", w, sizeof w) && !strcmp(w, "0:/Audiothurgist/sets/plain.wav.slices.json"), "sidecar of a JSON-less set WAV %s", w);
    CHECK(!bb::SidecarPath("sets/", w, sizeof w) && !bb::SidecarPath("amen.wav", w, 30), "sidecar edges");

    static char files[4][128] = {};
    int n = 0;
    CHECK(bb::AddCardEntry(&files[0][0], 128, n, 4, "b.wav") == 0 && bb::AddCardEntry(&files[0][0], 128, n, 4, "a.wav") == 1 && n == 2, "append in arrival order");
    CHECK(bb::AddCardEntry(&files[0][0], 128, n, 4, "B.WAV") == 0 && n == 2, "a name already there (any case) keeps its index");
    bb::AddCardEntry(&files[0][0], 128, n, 4, "c.wav"); bb::AddCardEntry(&files[0][0], 128, n, 4, "sets/d.wav");
    CHECK(bb::AddCardEntry(&files[0][0], 128, n, 4, "e.wav") == -1 && n == 4, "a full list refuses");
    CHECK(bb::AddCardEntry(&files[0][0], 128, n, 5, std::string(128, 'x').c_str()) == -1, "an entry longer than the stride refuses");
    bb::SortCardEntries(&files[0][0], 128, 0, 3);
    CHECK(!strcmp(files[0], "a.wav") && !strcmp(files[1], "b.wav") && !strcmp(files[2], "c.wav") && !strcmp(files[3], "sets/d.wav"), "sort a range, case-insensitive");
    CHECK(bb::IsWavName("A.WAV") && !bb::IsWavName("._a.wav") && !bb::IsWavName(".wav") && !bb::IsWavName("a.wave") && !bb::IsWavName("wav"), "IsWavName");
}

static void WavBytes()
{
    SECTION("WavBytesOk");
    const auto m = MakeWav(1000, false), s = MakeWav(1000, true), p24 = MakeWav(500, true, bbwav::kPcm24), fl = MakeWav(500, false, bbwav::kFloat32);
    CHECK(bb::WavBytesOk(m.data(), (uint32_t)m.size()) && bb::WavBytesOk(s.data(), (uint32_t)s.size()) && bb::WavBytesOk(p24.data(), (uint32_t)p24.size()), "PCM16 mono / stereo and PCM24 pass");
    CHECK(!bb::WavBytesOk(fl.data(), (uint32_t)fl.size()), "float32 refused (the loader cannot read it)");
    const auto low = MakeWav(500, false, bbwav::kPcm16, 7999), ok8 = MakeWav(500, false, bbwav::kPcm16, 8000);
    CHECK(!bb::WavBytesOk(low.data(), (uint32_t)low.size()) && bb::WavBytesOk(ok8.data(), (uint32_t)ok8.size()), "below 8 kHz refused, 8 kHz passes");
    int prefixPass = 0;
    for(size_t k = 0; k < 44; k++) if(bb::WavBytesOk(s.data(), (uint32_t)k)) prefixPass++;
    CHECK(prefixPass == 0, "every prefix shorter than the header fails (%d passed)", prefixPass);
    auto c = s; c[22] = 4; CHECK(!bb::WavBytesOk(c.data(), (uint32_t)c.size()), "four channels refused");
    c = s; c[34] = 8;      CHECK(!bb::WavBytesOk(c.data(), (uint32_t)c.size()), "8-bit refused");
    c = s; memcpy(&c[0], "RIFX", 4); CHECK(!bb::WavBytesOk(c.data(), (uint32_t)c.size()), "not RIFF");
    c = s; memcpy(&c[12], "data", 4); CHECK(!bb::WavBytesOk(c.data(), (uint32_t)c.size()), "data before fmt");
    c = s; c[16] = 0xFF; c[17] = 0xFF; c[18] = 0xFF; c[19] = 0x7F; CHECK(!bb::WavBytesOk(c.data(), (uint32_t)c.size()), "a chunk size past the end");
    std::vector<uint8_t> junk(4000, 0x5A); CHECK(!bb::WavBytesOk(junk.data(), (uint32_t)junk.size()) && !bb::WavBytesOk(nullptr, 100), "junk / null");
}

static void Receiver()
{
    SECTION("UploadRx");
    static uint8_t mem[4096];
    bb::UploadRx rx; rx.buf = mem; rx.cap = sizeof mem; rx.Reset();
    std::vector<uint8_t> src(3000); for(size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(i * 7 + 3);
    CHECK(rx.Take(3000, 0, src.data(), 10) == bb::kStBadState, "a chunk with nothing open is BAD_STATE");
    CHECK(rx.Begin(0) == bb::kStBadArgs && !rx.open, "a zero total is BAD_ARGS");
    CHECK(rx.Begin(4097) == bb::kStTooLarge && !rx.open, "past the buffer is TOO_LARGE, before a byte is copied");
    CHECK(rx.Begin(3000) == bb::kStOk && rx.open && rx.have == 0 && rx.Progress() == 0, "begin");
    CHECK(rx.Take(3000, 0, src.data(), 1000) == bb::kStOk && rx.have == 1000 && rx.Progress() == 85, "first chunk (progress %u)", rx.Progress());
    CHECK(rx.Take(3000, 1000, src.data() + 1000, 0) == bb::kStOk && rx.have == 1000, "an empty chunk is harmless");
    CHECK(rx.Take(3000, 1000, src.data() + 1000, 1000) == bb::kStOk && !rx.Done(), "second chunk");
    { bb::UploadRx r2 = rx; CHECK(r2.Take(3000, 1000, src.data() + 1000, 1000) == bb::kStBadArgs && !r2.open && r2.have == 0, "a repeated chunk resets"); }
    { bb::UploadRx r2 = rx; CHECK(r2.Take(3000, 2500, src.data(), 500) == bb::kStBadArgs && !r2.open, "a gap resets"); }
    { bb::UploadRx r2 = rx; CHECK(r2.Take(2999, 2000, src.data(), 999) == bb::kStBadArgs && !r2.open, "a changed total resets"); }
    { bb::UploadRx r2 = rx; CHECK(r2.Take(3000, 2000, src.data(), 1001) == bb::kStBadArgs && !r2.open, "running past the total resets"); }
    { bb::UploadRx r2 = rx; CHECK(r2.Take(3000, 2000, nullptr, 10) == bb::kStBadArgs, "null data with a length resets"); }
    CHECK(rx.Take(3000, 2000, src.data() + 2000, 1000) == bb::kStOk && rx.Done() && rx.Progress() == 255, "last chunk: done");
    CHECK(!memcmp(mem, src.data(), 3000), "the bytes are the bytes");
    CHECK(rx.Take(3000, 3000, src.data(), 1) == bb::kStBadArgs && !rx.Done(), "a byte past a finished transfer resets it");
    CHECK(rx.Begin(100) == bb::kStOk && rx.have == 0 && rx.total == 100, "a new begin starts over");
    bb::UploadRx none; none.buf = nullptr; none.cap = 0; none.Reset();
    CHECK(none.Begin(10) == bb::kStTooLarge, "no buffer: TOO_LARGE");
}

static void Plan()
{
    SECTION("copy plan");
    CHECK(bb::CopyWants(0, "amen.wav", false) && bb::CopyWants(0, "AMEN.WAV", false) && !bb::CopyWants(0, "notes.txt", false) && !bb::CopyWants(0, "x.wav", true)
          && !bb::CopyWants(0, "._amen.wav", false), "root: WAVs only, no folder, no AppleDouble shadow");
    CHECK(bb::CopyWants(1, "kit.json", false) && bb::CopyWants(1, "kit.wav", false) && bb::CopyWants(1, "a.wav.slices.json", false) && !bb::CopyWants(1, "kit.json.tmp", false)
          && !bb::CopyWants(1, "kit.part", false) && !bb::CopyWants(1, ".DS_Store", false), "sets: everything but temp and hidden files");
    CHECK(bb::CopyWants(2, "0.json", false) && !bb::CopyWants(3, "0.json", false) && !bb::CopyWants(-1, "a.wav", false), "presets; no fourth source");
    CHECK(!bb::CopyWants(0, (std::string(121, 'a') + ".wav").c_str(), false) && bb::CopyWants(0, (std::string(116, 'a') + ".wav").c_str(), false), "a name too long for the list stays put");
    char a[bb::kCardPathMax], b[bb::kCardPathMax], c[bb::kCardPathMax];
    CHECK(bb::CopyPaths(0, "amen.wav", a, b, c, sizeof a) && !strcmp(a, "0:/amen.wav") && !strcmp(b, "0:/Audiothurgist/breaks/amen.wav") && !strcmp(c, "0:/Audiothurgist/breaks/amen.wav.part"), "root paths %s %s %s", a, b, c);
    CHECK(bb::CopyPaths(1, "kit.json", a, b, c, sizeof a) && !strcmp(a, "0:/sets/kit.json") && !strcmp(b, "0:/Audiothurgist/sets/kit.json"), "sets paths %s %s", a, b);
    CHECK(bb::CopyPaths(2, "3.json", a, b, c, sizeof a) && !strcmp(a, "0:/presets/3.json") && !strcmp(b, "0:/Audiothurgist/presets/3.json"), "presets paths %s %s", a, b);
    CHECK(!bb::CopyPaths(0, "amen.wav", a, b, c, 30), "a short buffer refuses");
    bb::CardSurvey s; memset(&s, 0, sizeof s);
    CHECK(bb::CardFlags(s, false) == 0 && bb::CardFlags(s, true) == 1, "flags: unmounted 0, an empty mounted card 1");
    s.rootWavs = 3; s.oldSets = true; s.oldPresets = true; s.pending = 5;
    CHECK(bb::CardFlags(s, true) == (1 | 2 | 4 | 8 | 32), "flags: an old card");
    s.breaks = 2; s.pending = 0;
    CHECK(bb::CardFlags(s, true) == (1 | 2 | 4 | 8 | 16), "flags: copied in");
}

static void FatFs(const std::string& scratch)
{
    SECTION("FatFs half on an old card");
    gShimCard = scratch + "/card_check-card";
    (void)!system(("rm -rf '" + gShimCard + "' && mkdir -p '" + gShimCard + "/sets' '" + gShimCard + "/presets' '" + gShimCard + "/Other Module'").c_str());
    const auto amen = MakeWav(48000, false), think = MakeWav(24000, true);
    Put("amen.wav", amen); Put("Think.WAV", think); Put("._amen.wav", "AppleDouble"); Put("notes.txt", "not a break");
    Put("Other Module/x.wav", amen);   // another module's folder: not ours
    Put("sets/kit.wav", think); Put("sets/kit.json", "{\"audiothurgist\":1}"); Put("sets/amen.wav.slices.json", "{\"audiothurgist\":1}"); Put("sets/half.json.tmp", "{");
    Put("presets/0.json", "{\"audiothurgist\":1,\"slot\":0}");

    static DIR dir; static FILINFO fno; static FIL fa, fb;
    alignas(32) static uint8_t stage[bb::kCardChunk + 32];
    static char files[16][128];
    bb::gYield = CountYield;

    CHECK(bb::ScanCard(&dir, &fno, &files[0][0], 128, 16) == 0, "an old card lists nothing — the root is not read");
    bb::CardSurvey s;
    bb::SurveyCard(&dir, &fno, s);
    CHECK(s.rootWavs == 2 && s.breaks == 0 && s.oldSets && s.oldPresets && s.pending == 2 + 3 + 1, "survey: 2 root WAVs, old sets / presets, 6 to copy (got %u %u %d %d %u)", s.rootWavs, s.breaks, s.oldSets, s.oldPresets, s.pending);
    CHECK(bb::CardFlags(s, true) == (1 | 2 | 4 | 8 | 32), "…the tail's flags say so");

    CHECK(bb::CardMkdirs() && bb::CardMkdirs(), "the folders, made twice (FR_EXIST is success)");
    CHECK(Exists("Audiothurgist/breaks") && Exists("Audiothurgist/sets") && Exists("Audiothurgist/presets"), "…with this casing");
    { const std::string keep = gShimCard; gShimCard = scratch + "/nowhere/card"; CHECK(!bb::CardMkdirs(), "no card volume: the mkdir fails, reported"); gShimCard = keep; }

    static uint32_t lastDone = 0, lastTotal = 0;
    gYields = 0; gShimWrites = 0; gShimMisaligned = 0;
    bb::CopyStats st = bb::CopyOldCard(&dir, &fno, &fa, &fb, stage, sizeof stage, [](uint32_t d, uint32_t t) { lastDone = d; lastTotal = t; });
    CHECK(st.copied == 6 && st.skipped == 0 && st.failed == 0 && !st.full, "copy: 6 copied (%u %u %u)", st.copied, st.skipped, st.failed);
    CHECK(lastDone == 6 && lastTotal == 6, "progress per file, total counted first (%u / %u)", lastDone, lastTotal);
    CHECK(ReadAll(gShimCard + "/Audiothurgist/breaks/amen.wav") == amen && ReadAll(gShimCard + "/Audiothurgist/breaks/Think.WAV") == think
          && ReadAll(gShimCard + "/Audiothurgist/sets/kit.wav") == think && Exists("Audiothurgist/sets/kit.json") && Exists("Audiothurgist/sets/amen.wav.slices.json")
          && Exists("Audiothurgist/presets/0.json"), "byte-identical copies in the right folders");
    CHECK(!Exists("Audiothurgist/breaks/._amen.wav") && !Exists("Audiothurgist/breaks/notes.txt") && !Exists("Audiothurgist/sets/half.json.tmp") && !Exists("Audiothurgist/breaks/x.wav"),
          "not copied: the shadow, the text, the temp file, another module's folder");
    CHECK(Exists("amen.wav") && Exists("Think.WAV") && Exists("sets/kit.wav") && Exists("presets/0.json"), "copied, never moved: every source is still there");
    CHECK(!Exists("Audiothurgist/breaks/amen.wav.part") && !Exists("Audiothurgist/sets/kit.wav.part"), "no .part left behind");
    CHECK(gYields >= 6 + 6 && gShimMisaligned == 0 && gShimWrites > 0, "yields per chunk and per file (%d), every write DMA-aligned (%d of %d off)", gYields, gShimMisaligned, gShimWrites);

    st = bb::CopyOldCard(&dir, &fno, &fa, &fb, stage, sizeof stage);
    CHECK(st.copied == 0 && st.skipped == 6, "a second copy skips everything (%u %u)", st.copied, st.skipped);
    Put("Audiothurgist/breaks/amen.wav", "edited in place");   // a file already there is never overwritten by a copy
    st = bb::CopyOldCard(&dir, &fno, &fa, &fb, stage, sizeof stage);
    CHECK(st.copied == 0 && ReadAll(gShimCard + "/Audiothurgist/breaks/amen.wav").size() == 15, "…and never overwrites one");
    Put("Audiothurgist/breaks/amen.wav", amen);

    const int n = bb::ScanCard(&dir, &fno, &files[0][0], 128, 16);
    CHECK(n == 3 && !strcmp(files[0], "amen.wav") && !strcmp(files[1], "Think.WAV") && !strcmp(files[2], "sets/kit.wav"), "the list: breaks sorted, then sets (%d: %s %s %s)", n, files[0], files[1], files[2]);
    bb::SurveyCard(&dir, &fno, s);
    CHECK(s.breaks == 2 && s.pending == 0 && s.rootWavs == 2 && bb::CardFlags(s, true) == (1 | 2 | 4 | 8 | 16), "survey after: nothing pending");
    CHECK(bb::ScanCard(&dir, &fno, &files[0][0], 128, 2) == 2, "the list stops at its capacity");
    /* the rescan that keeps every index: a new break joins at the end, the old order stays */
    {
        int m = bb::ScanCard(&dir, &fno, &files[0][0], 128, 16);
        Put("Audiothurgist/breaks/Aardvark.wav", amen);
        const int added = bb::MergeCard(&dir, &fno, &files[0][0], 128, m, 16);
        CHECK(added == 1 && m == 4 && !strcmp(files[0], "amen.wav") && !strcmp(files[2], "sets/kit.wav") && !strcmp(files[3], "Aardvark.wav"), "merge: appended, nothing moved (%d %d %s)", added, m, files[3]);
        CHECK(bb::MergeCard(&dir, &fno, &files[0][0], 128, m, 16) == 0 && m == 4, "merge again: nothing new");
        remove((gShimCard + "/Audiothurgist/breaks/Aardvark.wav").c_str());
    }

    /* the upload's write */
    char wav[160], part[160];
    const auto up = MakeWav(17 * 48000 / 10, true);   // 1.7 s stereo
    bb::BreakPaths("Upload One", wav, part, sizeof wav);
    uint32_t moved = 0; gYields = 0; gShimMisaligned = 0; gShimWrites = 0;
    CHECK(bb::WriteCardFile(&fa, wav, part, up.data(), (uint32_t)up.size(), false, stage, sizeof stage, &fno, &moved) == bb::kCardOk, "upload written");
    CHECK(ReadAll(gShimCard + "/Audiothurgist/breaks/Upload One.wav") == up && !Exists("Audiothurgist/breaks/Upload One.part") && moved == up.size(), "…byte for byte, no .part, progress to the end");
    CHECK(gYields >= (int)(up.size() / bb::kCardChunk) && gShimMisaligned == 0, "…yielding per 8 KiB (%d yields), aligned", gYields);
    const auto other = MakeWav(4800, false);
    CHECK(bb::WriteCardFile(&fa, wav, part, other.data(), (uint32_t)other.size(), false, stage, sizeof stage, &fno) == bb::kCardExists
          && ReadAll(gShimCard + "/Audiothurgist/breaks/Upload One.wav") == up, "the same name without overwrite: EXISTS, the file kept");
    CHECK(bb::WriteCardFile(&fa, wav, part, other.data(), (uint32_t)other.size(), true, stage, sizeof stage, &fno) == bb::kCardOk
          && ReadAll(gShimCard + "/Audiothurgist/breaks/Upload One.wav") == other, "with overwrite: replaced");
    bb::BreakPaths("Too Big", wav, part, sizeof wav);
    gShimFree = 20000;
    CHECK(bb::WriteCardFile(&fa, wav, part, up.data(), (uint32_t)up.size(), false, stage, sizeof stage, &fno) == bb::kCardFull, "a full card: CARD_FULL");
    CHECK(!Exists("Audiothurgist/breaks/Too Big.wav") && !Exists("Audiothurgist/breaks/Too Big.part"), "…and nothing left on it");
    gShimFree = 5000;
    Put("half.wav", up);
    st = bb::CopyOldCard(&dir, &fno, &fa, &fb, stage, sizeof stage);
    CHECK(st.full && st.copied == 0 && !Exists("Audiothurgist/breaks/half.wav") && !Exists("Audiothurgist/breaks/half.wav.part"), "a copy that fills the card stops, leaves no half file");
    gShimFree = -1;
    { const std::string keep = gShimCard; gShimCard = scratch + "/nowhere"; bb::BreakPaths("x", wav, part, sizeof wav);
      CHECK(bb::WriteCardFile(&fa, wav, part, up.data(), 100, false, stage, sizeof stage, &fno) == bb::kCardWriteFail, "no folder: WRITE_FAILED"); gShimCard = keep; }
    { alignas(32) static uint8_t tiny[64]; bb::BreakPaths("y", wav, part, sizeof wav);
      CHECK(bb::WriteCardFile(&fa, wav, part, up.data(), 100, false, tiny, sizeof tiny, &fno) == bb::kCardWriteFail, "a staging smaller than a chunk is refused"); }

    /* the uploaded file, listed, through the firmware's own loader */
    bb::BreakPaths("Upload One", wav, part, sizeof wav);
    bb::WriteCardFile(&fa, wav, part, up.data(), (uint32_t)up.size(), true, stage, sizeof stage, &fno);
    static int  idxFiles = 0;
    idxFiles = bb::ScanCard(&dir, &fno, &files[0][0], 128, 16);
    const int at = bb::FindCardEntry(&files[0][0], 128, idxFiles, "Upload One.wav");
    CHECK(at >= 0, "the upload is in the list (%d entries)", idxFiles);
    char path[160];
    CHECK(at >= 0 && bb::EntryPath(files[at], path, sizeof path), "its card path %s", path);
    static float stg[96000], outL[96000], stgR[96000], outR[96000];
    int cues[8], nCues = 0, err = 0; bool gotR = false;
    const int len = bbwav::Load(path, stg, 96000, outL, 96000, 48000.f, 17, cues, &nCues, 8, &err, outR, stgR, &gotR);
    CHECK(len == 17 * 48000 / 10 && gotR && err == 0, "the loader reads it: %d frames, stereo %d, err %d", len, gotR, err);
    bb::gYield = nullptr;
}

/* beta-sampler: Start in (core/bb_startup.h) — Audiothurgist/startup.json */
static void Startup(const std::string& scratch)
{
    SECTION("Start in (startup.json)");
    char b[bb::kStartupMax];
    size_t n = bb::StartupJson(bb::kStartSaved, b, sizeof b);
    CHECK(n > 0 && !strcmp(b, "{\"audiothurgist\":1,\"startIn\":\"saved\"}\n"), "As saved's text: %s", b);
    CHECK(bb::StartupRead(b, n) == bb::kStartSaved, "…reads back As saved");
    n = bb::StartupJson(bb::kStartSampler, b, sizeof b);
    CHECK(n > 0 && strstr(b, "\"sampler\"") && bb::StartupRead(b, n) == bb::kStartSampler, "Sampler's text and its reading: %s", b);
    CHECK(bb::StartupJson(bb::kStartSaved, b, 20) == 0, "a buffer too small: nothing (0)");
    CHECK(bb::StartupJson(7, b, sizeof b) > 0 && bb::StartupRead(b, strlen(b)) == bb::kStartSampler, "any other value is written as Sampler");
    {   /* every strict prefix of As saved's file is broken JSON → Sampler (the default), never a guess */
        bb::StartupJson(bb::kStartSaved, b, sizeof b); const size_t m = strlen(b) - 1;   /* without the newline */
        int saved = 0; for(size_t k = 0; k < m; k++) if(bb::StartupRead(b, k) == bb::kStartSaved) saved++;
        CHECK(saved == 0, "every truncation reads Sampler (%d did not)", saved);
    }
    const char* other[] = { "", "{}", "[]", "{\"startIn\":1}", "{\"startIn\":\"Saved\"}", "{\"startIn\":\"later\"}", "not json", "{\"startIn\":\"saved\"" };
    int bad = 0; for(const char* t : other) if(bb::StartupRead(t, strlen(t)) != bb::kStartSampler) bad++;
    CHECK(bad == 0, "empty, foreign, a number, another casing, an unknown word, unclosed: all Sampler (%d not)", bad);
    const char* fut = "{\"audiothurgist\":2,\"brightness\":[1,2],\"startIn\":\"saved\",\"later\":{\"x\":1}}";
    CHECK(bb::StartupRead(fut, strlen(fut)) == bb::kStartSaved, "a later file with more keys still reads (unknown keys skipped)");
    CHECK(bb::StartupRead(nullptr, 5) == bb::kStartSampler, "no text: Sampler");

    gShimCard = scratch + "/startup-card";
    (void)!system(("rm -rf '" + gShimCard + "' && mkdir -p '" + gShimCard + "'").c_str());
    static FIL f;
    alignas(32) static char buf[bb::kStartupMax];
    CHECK(bb::ReadStartup(&f, buf, sizeof buf) == bb::kStartSampler, "a card without the file: Sampler");
    CHECK(!bb::WriteStartup(&f, bb::kStartSaved, buf, sizeof buf) && !Exists("Audiothurgist/startup.json") && !Exists("Audiothurgist/startup.json.tmp"), "no folder yet: not written, nothing left behind");
    CHECK(bb::CardMkdirs(), "the folders made");
    CHECK(bb::WriteStartup(&f, bb::kStartSaved, buf, sizeof buf) && Exists("Audiothurgist/startup.json") && !Exists("Audiothurgist/startup.json.tmp"), "As saved written (tmp + rename, no tmp left)");
    { const auto t = ReadAll(gShimCard + "/Audiothurgist/startup.json"); CHECK(std::string(t.begin(), t.end()) == "{\"audiothurgist\":1,\"startIn\":\"saved\"}\n", "…the file says so"); }
    CHECK(bb::ReadStartup(&f, buf, sizeof buf) == bb::kStartSaved, "…and reads back As saved");
    Put("Audiothurgist/startup.json.tmp", "{half");   /* a power-off mid-write last time */
    CHECK(bb::WriteStartup(&f, bb::kStartSampler, buf, sizeof buf) && bb::ReadStartup(&f, buf, sizeof buf) == bb::kStartSampler && !Exists("Audiothurgist/startup.json.tmp"), "rewritten as Sampler over a stale tmp");
    { std::string big(bb::kStartupMax + 10, ' '); big = "{\"startIn\":\"saved\"}" + big; Put("Audiothurgist/startup.json", big.c_str());
      CHECK(bb::ReadStartup(&f, buf, sizeof buf) == bb::kStartSampler, "a file that fills the buffer is not ours: Sampler"); }
    Put("Audiothurgist/startup.json", "{\"audiothurgist\":1,\"startIn\":\"saved\"}");
    CHECK(bb::ReadStartup(&f, buf, sizeof buf) == bb::kStartSaved, "written by hand, no newline: read");
    { const std::string keep = gShimCard; gShimCard = scratch + "/nowhere/card";
      CHECK(bb::ReadStartup(&f, buf, sizeof buf) == bb::kStartSampler && !bb::WriteStartup(&f, bb::kStartSaved, buf, sizeof buf), "no card volume: Sampler, the write refused"); gShimCard = keep; }
    CHECK(bb::ReadStartup(&f, buf, sizeof buf) == bb::kStartSaved, "…and the card's setting untouched by the failed write");
}

int main(int argc, char** argv)
{
    const std::string scratch = argc > 1 ? argv[1] : "/tmp";
    Paths();
    WavBytes();
    Receiver();
    Plan();
    FatFs(scratch);
    Startup(scratch);
    printf("\ncard_check: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
