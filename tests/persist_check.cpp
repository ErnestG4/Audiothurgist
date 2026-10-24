// persist_check — Phase E (persistence) host suite: core/bb_wavwrite.h,
// core/bb_json.h, core/bb_set.h. Build + run (tests/run.sh does this under
// ASan/UBSan):
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore -Ivendor/capicola/lib
//       tests/persist_check.cpp -o /tmp/pc && /tmp/pc   (one line)
//
// 1. WAV: mono/stereo × PCM16/PCM24/float32, with cues, through a FILE*
//    sink → read back with tests/wav_read.h AND with the firmware's own
//    bbwav::Load (core/wav_loader.h, its five f_* calls shimmed over stdio),
//    plus a byte-level walk of the RIFF layout (sizes, chunk order, the
//    cue table). A memory sink with a tiny staging must produce the
//    identical byte stream (the chunking is invisible).
// 2. JSON: emitter output shape, float formatting, string escapes; parser
//    tolerance (unknown keys, nested unknown values, whitespace, 0/1 flags),
//    truncation at EVERY prefix length (must fail, never over-read — ASan
//    guards the buffer), wrong / missing version.
// 3. Set round trip: SetInfo → WAV + JSON → SetInfo, cues == slice starts,
//    and the preset backup schema.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

#include "bb_wavwrite.h"
#include "bb_json.h"
#include "bb_set.h"
#include "wav_read.h"

// ── the firmware loader over stdio: the FatFs surface it touches, from the
// shared shim (CARD: tests/fatfs_shim.h — card_check drives the card code
// through the same one) ──
#include "fatfs_shim.h"
#include "wav_loader.h"

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

static std::string gOut;   // scratch dir for the files
static std::string Path(const char* n) { return gOut + "/" + n; }

// ── sinks ──
static bool FileSink(void* ctx, const void* p, size_t n) { return fwrite(p, 1, n, (FILE*)ctx) == n; }
static bool MemSink(void* ctx, const void* p, size_t n)
{
    auto* v = (std::vector<uint8_t>*)ctx;
    v->insert(v->end(), (const uint8_t*)p, (const uint8_t*)p + n);
    return true;
}
static bool FailSink(void* ctx, const void*, size_t n)
{
    auto* left = (long*)ctx;              // fail once `left` bytes have passed
    *left -= (long)n;
    return *left >= 0;
}

static std::vector<uint8_t> ReadAll(const char* path)
{
    std::vector<uint8_t> v;
    FILE* f = fopen(path, "rb");
    if(!f) return v;
    uint8_t b[4096]; size_t n;
    while((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
    fclose(f);
    return v;
}
static uint32_t Rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t Rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }

// ── material: 1 s, deterministic, full-scale-ish with a clip case ──
static constexpr int kLen = 48000;
static float gL[kLen], gR[kLen];
static void BuildMaterial()
{
    uint32_t s = 0x9E3779B9u;
    for(int i = 0; i < kLen; i++)
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const float noise = ((int32_t)s / 2147483648.f) * 0.05f;
        gL[i] = 0.7f * sinf(6.2831853f * 220.f * i / 48000.f) + noise;
        gR[i] = 0.5f * sinf(6.2831853f * 330.f * i / 48000.f) - noise;
    }
    gL[100] = 1.5f; gL[101] = -1.5f;   // clip cases: must land on ±full scale
}
static const int kCues[5] = {0, 6000, 12000, 24000, 36000};

struct Written { std::string path; uint32_t size; int ch; bbwav::Format fmt; int nCues; };

static bool WriteFile(const char* name, bool stereo, bbwav::Format fmt, bool cues, size_t stagingBytes, Written& w)
{
    bbwav::WriteSpec s;
    s.left = gL; s.right = stereo ? gR : nullptr; s.len = kLen; s.sr = 48000;
    s.fmt = fmt; s.cues = cues ? kCues : nullptr; s.nCues = cues ? 5 : 0;
    w.path = Path(name); w.size = bbwav::FileSize(s); w.ch = stereo ? 2 : 1; w.fmt = fmt; w.nCues = s.nCues;
    FILE* f = fopen(w.path.c_str(), "wb");
    if(!f) return false;
    std::vector<uint8_t> staging(stagingBytes);
    const bool ok = bbwav::Write({FileSink, f}, s, staging.data(), staging.size());
    fclose(f);
    return ok;
}

// byte-level RIFF walk: sizes and chunk order exactly as the writer promises
static void CheckLayout(const Written& w)
{
    std::vector<uint8_t> b = ReadAll(w.path.c_str());
    CHECK(b.size() == w.size, "%s: file %zu bytes, FileSize said %u", w.path.c_str(), b.size(), w.size);
    if(b.size() < 44) return;
    CHECK(!memcmp(&b[0], "RIFF", 4) && !memcmp(&b[8], "WAVE", 4), "RIFF/WAVE");
    CHECK(Rd32(&b[4]) == b.size() - 8, "RIFF size %u vs %zu", Rd32(&b[4]), b.size() - 8);
    const int bps = bbwav::BytesPerSample(w.fmt), fb = bps * w.ch;
    size_t pos = 12; int order = 0; bool sawFmt = false, sawCue = false, sawData = false, sawFact = false;
    while(pos + 8 <= b.size())
    {
        const uint32_t sz = Rd32(&b[pos + 4]);
        const uint8_t* d = &b[pos + 8];
        CHECK(pos + 8 + sz <= b.size(), "chunk %.4s overruns the file", &b[pos]);
        if(pos + 8 + sz > b.size()) break;
        if(!memcmp(&b[pos], "fmt ", 4))
        {
            sawFmt = true; CHECK(order == 0, "fmt first");
            CHECK(sz == 16, "fmt size 16");
            CHECK(Rd16(d) == (w.fmt == bbwav::kFloat32 ? 3u : 1u), "fmt tag");
            CHECK(Rd16(d + 2) == (uint32_t)w.ch, "channels");
            CHECK(Rd32(d + 4) == 48000u, "rate");
            CHECK(Rd32(d + 8) == 48000u * fb, "byte rate");
            CHECK(Rd16(d + 12) == (uint32_t)fb, "block align");
            CHECK(Rd16(d + 14) == (uint32_t)bps * 8, "bits");
        }
        else if(!memcmp(&b[pos], "fact", 4)) { sawFact = true; CHECK(sz == 4 && Rd32(d) == kLen, "fact frames"); }
        else if(!memcmp(&b[pos], "cue ", 4))
        {
            sawCue = true; CHECK(!sawData, "cue precedes data");
            CHECK(sz == 4u + 24u * w.nCues, "cue size");
            CHECK(Rd32(d) == (uint32_t)w.nCues, "cue count");
            for(int i = 0; i < w.nCues; i++)
            {
                const uint8_t* c = d + 4 + 24 * i;
                CHECK(Rd32(c) == (uint32_t)(i + 1), "cue id");
                CHECK(!memcmp(c + 8, "data", 4), "cue fccChunk");
                CHECK(Rd32(c + 20) == (uint32_t)kCues[i], "cue %d offset %u", i, Rd32(c + 20));
            }
        }
        else if(!memcmp(&b[pos], "data", 4))
        {
            sawData = true;
            CHECK(sz == (uint32_t)kLen * fb, "data size %u", sz);
            CHECK(pos + 8 + sz + (sz & 1) == b.size(), "data is the last chunk, padded");
            // spot-check the clip samples on plane 0
            if(w.fmt == bbwav::kPcm16)
            {
                CHECK((int16_t)Rd16(d + (size_t)100 * fb) == 32767, "clip +");
                CHECK((int16_t)Rd16(d + (size_t)101 * fb) == -32768, "clip -");
            }
        }
        pos += 8 + sz + (sz & 1);
        order++;
    }
    CHECK(pos == b.size(), "chunk walk ends at EOF");
    CHECK(sawFmt && sawData, "fmt + data present");
    CHECK(sawCue == (w.nCues > 0), "cue chunk presence");
    CHECK(sawFact == (w.fmt == bbwav::kFloat32), "fact only for float");
}

static float Tol(bbwav::Format f) { return f == bbwav::kPcm16 ? 1.f / 32768.f + 1e-6f : f == bbwav::kPcm24 ? 1.f / 8388608.f + 1e-6f : 0.f; }
static float Clamp1(float x) { return x > 1.f ? 1.f : x < -1.f ? -1.f : x; }

// expected mono fold of the written planes, as both readers produce it
static float Expect(int i, int ch, bbwav::Format f)
{
    float l = gL[i], r = gR[i];
    if(f != bbwav::kFloat32) { l = Clamp1(l); r = Clamp1(r); }
    // PCM readers divide by 32768 while the writer clips at 32767: the
    // clipped sample reads back as 0.99997 — within Tol.
    return ch == 2 ? 0.5f * (l + r) : l;
}

static void CheckReadBack(const Written& w)
{
    // 1. tests/wav_read.h (any format)
    std::vector<float> mono; float sr = 0;
    CHECK(LoadWav(w.path.c_str(), mono, sr), "wav_read.h opens %s", w.path.c_str());
    CHECK(sr == 48000.f && (int)mono.size() == kLen, "wav_read.h: sr %g len %zu", sr, mono.size());
    float worst = 0;
    for(int i = 0; i < kLen && i < (int)mono.size(); i++) { const float e = fabsf(mono[i] - Expect(i, w.ch, w.fmt)); if(e > worst) worst = e; }
    CHECK(worst <= Tol(w.fmt), "wav_read.h: worst error %g (tol %g)", worst, Tol(w.fmt));

    // 2. the firmware's own loader (PCM only — it rejects float, by design)
    static std::vector<float> stage(kLen * 2), out(kLen * 2);
    int cues[128], nCues = -1, err = -1;
    int n = bbwav::Load(w.path.c_str(), stage.data(), (int)stage.size(), out.data(), (int)out.size(), 48000.f, 25, cues, &nCues, 128, &err);
    if(w.fmt == bbwav::kFloat32)
    {
        CHECK(n == 0 && err == 3, "bbwav::Load rejects float32 with err 3 (got n %d err %d)", n, err);
        return;
    }
    CHECK(n == kLen && err == 0, "bbwav::Load: n %d err %d", n, err);
    worst = 0;
    for(int i = 0; i < n && i < kLen; i++) { const float e = fabsf(out[i] - Expect(i, w.ch, w.fmt)); if(e > worst) worst = e; }
    CHECK(worst <= Tol(w.fmt), "bbwav::Load: worst error %g", worst);
    CHECK(nCues == w.nCues, "bbwav::Load: %d cues (want %d)", nCues, w.nCues);
    for(int i = 0; i < nCues && i < w.nCues; i++) CHECK(cues[i] == kCues[i], "cue %d = %d", i, cues[i]);
}

static void TestWav()
{
    SECTION("WAV writer: formats × channels × cues");
    struct Case { const char* name; bool stereo; bbwav::Format fmt; bool cues; } cases[] = {
        {"m16c.wav", false, bbwav::kPcm16, true},   {"s16c.wav", true, bbwav::kPcm16, true},
        {"m16.wav",  false, bbwav::kPcm16, false},  {"s24c.wav", true, bbwav::kPcm24, true},
        {"m24c.wav", false, bbwav::kPcm24, true},   {"mf32c.wav", false, bbwav::kFloat32, true},
        {"sf32.wav", true, bbwav::kFloat32, false},
    };
    for(const Case& c : cases)
    {
        Written w;
        CHECK(WriteFile(c.name, c.stereo, c.fmt, c.cues, 16384, w), "write %s", c.name);
        CheckLayout(w);
        CheckReadBack(w);
    }

    SECTION("WAV writer: staging size is invisible; sink failure is reported");
    {
        bbwav::WriteSpec s; s.left = gL; s.right = gR; s.len = 1001; s.fmt = bbwav::kPcm24; s.cues = kCues; s.nCues = 3;
        std::vector<uint8_t> big, small, odd;
        std::vector<uint8_t> st(16384);
        CHECK(bbwav::Write({MemSink, &big}, s, st.data(), st.size()), "big staging");
        CHECK(bbwav::Write({MemSink, &small}, s, st.data(), 64), "min staging");
        CHECK(bbwav::Write({MemSink, &odd}, s, st.data(), 1000), "odd staging");
        CHECK(big == small && big == odd, "byte streams identical across stagings (%zu/%zu/%zu)", big.size(), small.size(), odd.size());
        CHECK(big.size() == bbwav::FileSize(s), "size matches FileSize");
        CHECK(!bbwav::Write({MemSink, &big}, s, st.data(), 63), "staging under kMinStaging refused");
        // 3-byte mono 24-bit data with odd byte count gets a pad byte
        bbwav::WriteSpec m; m.left = gL; m.len = 1001; m.fmt = bbwav::kPcm24;
        std::vector<uint8_t> v;
        CHECK(bbwav::Write({MemSink, &v}, m, st.data(), st.size()) && (v.size() & 1) == 0 && v.size() == 44 + 3003 + 1, "odd data padded (%zu)", v.size());
        long budget = 100;   // sink dies mid-data
        CHECK(!bbwav::Write({FailSink, &budget}, s, st.data(), st.size()), "sink failure → false");
        bbwav::WriteSpec bad; bad.len = 0;
        CHECK(!bbwav::Write({MemSink, &v}, bad, st.data(), st.size()) && bbwav::FileSize(bad) == 0, "empty spec refused");
        // the DMA pad: fills start word-aligned for the sector-aligned part
        for(uint32_t off = 0; off < 2048; off++)
        {
            const uint32_t pad = bbwav::DmaPad(off);
            const uint32_t head = (512u - off % 512u) % 512u;
            if(((pad + head) & 3u) != 0) { CHECK(false, "DmaPad(%u) = %u misaligns", off, pad); break; }
        }
    }
}

// ── JSON ──
static void TestJson()
{
    SECTION("JSON emitter");
    {
        char buf[256];
        bb::JsonOut o(buf, sizeof buf);
        o.BeginObject();
        o.KeyInt("i", -42); o.KeyFloat("f", 0.25f); o.KeyFloat("g", 48000.f); o.KeyFloat("h", 1e-7f);
        o.KeyFloat("k", -3.5f); o.KeyBool("b", true); o.KeyString("s", "a\"b\\c\n\x01");
        o.Key("arr"); o.BeginArray(); o.Int(1); o.Int(2); o.BeginObject(); o.KeyInt("z", 0); o.EndObject(); o.EndArray();
        o.Key("empty"); o.BeginArray(); o.EndArray();
        o.EndObject();
        const char* want = "{\"i\":-42,\"f\":0.25,\"g\":48000,\"h\":0,\"k\":-3.5,\"b\":true,\"s\":\"a\\\"b\\\\c\\n\\u0001\",\"arr\":[1,2,{\"z\":0}],\"empty\":[]}";
        CHECK(o.ok && !strcmp(buf, want), "emitter shape:\n    %s\n    %s", buf, want);
        char tiny[16];
        bb::JsonOut t(tiny, sizeof tiny);
        t.BeginObject(); t.KeyString("name", "a long enough value"); t.EndObject();
        CHECK(!t.ok && strlen(tiny) < sizeof tiny, "overflow → ok=false, still terminated");
        bb::JsonOut z(nullptr, 0);
        CHECK(!z.ok, "zero-capacity → not ok");
    }

    SECTION("JSON parser: tolerance");
    {
        const char* text = "  {\n \"unknown\" : {\"deep\":[1,[2,{\"x\":\"}]\"}],null,true], \"q\":\"\\u00e9\"},"
                           " \"n\": -12.5e1 , \"flag\":1, \"str\":\"he said \\\"hi\\\" \\\\ \\u0041\\u00e9\", \"i\":7,\"more\":[],\"tail\":\"x\"}  ";
        bb::JsonIn in(text);
        CHECK(in.BeginObject(), "begin");
        char k[16]; float n = 0; bool flag = false; int i = 0; char s[64] = {}; int keys = 0;
        while(in.NextKey(k, sizeof k))
        {
            keys++;
            if(!strcmp(k, "n")) CHECK(in.Float(n), "n");
            else if(!strcmp(k, "flag")) CHECK(in.Bool(flag), "flag");
            else if(!strcmp(k, "i")) CHECK(in.Int(i), "i");
            else if(!strcmp(k, "str")) CHECK(in.String(s, sizeof s), "str");
            else CHECK(in.Skip(), "skip %s", k);
        }
        CHECK(in.ok && in.AtEnd(), "walked cleanly to the end");
        CHECK(keys == 7 && n == -125.f && flag && i == 7, "values: keys %d n %g flag %d i %d", keys, n, flag, i);
        CHECK(!strcmp(s, "he said \"hi\" \\ A\xc3\xa9"), "escapes decoded: '%s'", s);
        // key/string truncation is silent and safe
        bb::JsonIn t("{\"averyveryverylongkeyname\":\"averyveryverylongvalue\"}");
        char sk[8], sv[8];
        CHECK(t.BeginObject() && t.NextKey(sk, sizeof sk) && !strcmp(sk, "averyve") && t.String(sv, sizeof sv) && !strcmp(sv, "averyve") && !t.NextKey(sk, sizeof sk) && t.ok, "truncation");
        // numbers
        struct { const char* s; double v; } nums[] = {{"0", 0}, {"-0.5", -0.5}, {"12345678", 12345678}, {"1e3", 1000}, {"2.5E-2", 0.025}, {"-7e+1", -70}};
        for(auto& q : nums) { bb::JsonIn ni(q.s); double d = 1e9; CHECK(ni.Number(d) && fabs(d - q.v) < 1e-9, "number %s → %g", q.s, d); }
        const char* badNums[] = {"-", ".5", "1.", "1e", "abc", "+1", ""};
        for(const char* q : badNums) { bb::JsonIn ni(q); double d; CHECK(!ni.Number(d) && !ni.ok, "bad number '%s' rejected", q); }
        bb::JsonIn big("99999999999"); int iv; CHECK(!big.Int(iv), "int overflow rejected");
    }

    SECTION("JSON parser: malformed");
    {
        const char* bad[] = {"", "{", "{\"a\"", "{\"a\":", "{\"a\":1", "{\"a\":1,}", "{,\"a\":1}", "{\"a\" 1}", "[1,]", "{\"a\":\"unterminated}", "{\"a\":\"\\q\"}", "{\"a\":\"\\u12\"}", "{\"a\":[1,2}"};
        for(const char* q : bad)
        {
            bb::SetInfo s;
            CHECK(!bb::SetReader::Json(q, strlen(q), s), "malformed '%s' rejected", q);
        }
        // arrays: ArrayMore on a truncated list
        bb::JsonIn a("[1,2"); int v; int cnt = 0;
        CHECK(a.BeginArray(), "arr");
        while(a.ArrayMore()) { if(!a.Int(v)) break; cnt++; }
        CHECK(!a.ok && cnt == 2, "truncated array fails after 2 (ok %d cnt %d)", a.ok, cnt);
    }
}

// ── set round trip ──
static bb::SetInfo MakeSet()
{
    bb::SetInfo s;
    strcpy(s.name, "amen \"v2\"");
    s.sr = 48000; s.stereo = true; s.len = kLen; s.material = kLen - 100; s.swing = 0.62f; s.bars = 2; s.bpm = 136.25f;   /* SETS: material + swing */
    s.nSlices = 5;
    const char* names[5] = {"kick", "snare", "hat", "", "ride/crash"};
    for(int i = 0; i < 5; i++)
    {
        s.slices[i].start = kCues[i];
        s.slices[i].len   = (i < 4 ? kCues[i + 1] : kLen) - kCues[i];
        s.slices[i].key   = (int8_t)(i == 0 ? -1 : i * 3);   /* SETS: -1 = a grid slice */
        s.slices[i].cls   = (uint8_t)(i % 4);
        s.slices[i].sal   = 0.1f * (i + 1);
        s.slices[i].hit   = (i % 2) == 0;
        s.slices[i].flags = (uint8_t)((i == 1 || i == 4) ? bb::kSetFlagOrange : 0);   /* EDIT-PERSIST */
        strcpy(s.slices[i].name, names[i]);
    }
    s.nGrid = 1; s.hasKeys = true; s.keys[3] = 2; s.keys[7] = 4; s.keys[23] = 0;   /* EDIT-PERSIST: grid count + key map */
    s.nOptions = 17;
    for(int i = 0; i < 17; i++) s.options[i] = (uint8_t)(i * 15);
    s.nLooper = 3;
    for(int i = 0; i < 3; i++) { s.looper[i] = {0.125f * (i + 1) + 0.333333f, (int8_t)(i == 2 ? -1 : i + 1), (uint8_t)(7 - i), 0.5f + 0.1f * i, (int8_t)(i == 2 ? 33 : -1)}; }   /* SETS: a keyless event carries its slice */
    return s;
}

static void CheckSame(const bb::SetInfo& a, const bb::SetInfo& b, const char* what)
{
    CHECK(!strcmp(a.name, b.name) && a.sr == b.sr && a.stereo == b.stereo && a.len == b.len && a.bars == b.bars && fabsf(a.bpm - b.bpm) < 1e-4f,
          "%s: header (name '%s' vs '%s', bpm %g vs %g)", what, a.name, b.name, a.bpm, b.bpm);
    CHECK((a.material > 0 ? a.material : a.len) == (b.material > 0 ? b.material : b.len) && fabsf(a.swing - b.swing) < 1e-5f,
          "%s: material %d vs %d, swing %g vs %g", what, a.material, b.material, a.swing, b.swing);   /* SETS */
    CHECK(a.nSlices == b.nSlices, "%s: nSlices %d vs %d", what, a.nSlices, b.nSlices);
    for(int i = 0; i < a.nSlices && i < b.nSlices; i++)
    {
        const bb::SetSlice &x = a.slices[i], &y = b.slices[i];
        CHECK(x.start == y.start && x.len == y.len && x.key == y.key && x.cls == y.cls && fabsf(x.sal - y.sal) < 1e-5f && x.hit == y.hit && x.flags == y.flags && !strcmp(x.name, y.name),
              "%s: slice %d", what, i);
    }
    CHECK(a.nGrid == b.nGrid && a.hasKeys == b.hasKeys && !memcmp(a.keys, b.keys, sizeof a.keys), "%s: grid %d vs %d, keys", what, a.nGrid, b.nGrid);   /* EDIT-PERSIST */
    CHECK(a.nOptions == b.nOptions && !memcmp(a.options, b.options, a.nOptions), "%s: options", what);
    CHECK(a.nLooper == b.nLooper, "%s: nLooper %d vs %d", what, a.nLooper, b.nLooper);
    for(int i = 0; i < a.nLooper && i < b.nLooper; i++)
    {
        const bb::SetLoopEvent &x = a.looper[i], &y = b.looper[i];
        CHECK(fabsf(x.phase - y.phase) < 1e-5f && x.key == y.key && x.style == y.style && fabsf(x.vel - y.vel) < 1e-5f && x.slice == y.slice, "%s: looper %d", what, i);   /* SETS: slice too */
    }
}

static void TestSet()
{
    SECTION("Set JSON round trip");
    static char json[bb::kSetJsonMax];
    const bb::SetInfo s = MakeSet();
    size_t n = 0;
    CHECK(bb::SetWriter::Json(s, json, sizeof json, &n) && n > 0 && n == strlen(json), "emit (%zu bytes)", n);
    printf("  %.*s\n", (int)(n > 300 ? 300 : n), json);
    bb::SetInfo r;
    CHECK(bb::SetReader::Json(json, n, r), "parse");
    CheckSame(s, r, "roundtrip");

    // truncated at every prefix: must fail and never over-read (ASan). The
    // emitter ends with '}\n', so the last prefix that is NOT a whole
    // document is n-2 long.
    int okPrefixes = 0;
    for(size_t k = 0; k + 1 < n; k++)
    {
        std::vector<char> pre(json, json + k);   // exact-size heap copy: any over-read trips ASan
        bb::SetInfo t;
        if(bb::SetReader::Json(pre.data(), k, t)) okPrefixes++;
    }
    CHECK(okPrefixes == 0, "%d truncated prefixes parsed as valid", okPrefixes);

    // wrong / missing version
    {
        std::string v2 = json; v2.replace(v2.find("\"audiothurgist\":1"), 17, "\"audiothurgist\":2");
        bb::SetInfo t;
        CHECK(!bb::SetReader::Json(v2.data(), v2.size(), t), "version 2 rejected");
        std::string nov = json; nov.replace(nov.find("\"audiothurgist\":1,"), 18, "");
        CHECK(!bb::SetReader::Json(nov.data(), nov.size(), t), "missing version rejected");
        std::string late = json; late.replace(late.find("\"audiothurgist\":1,"), 18, "");
        late.insert(late.rfind('}'), ",\"audiothurgist\":1");
        CHECK(bb::SetReader::Json(late.data(), late.size(), t), "version anywhere in the object is fine");
    }
    // forward compatibility: unknown keys at every level, extra slice fields, pretty-printing
    {
        const char* fut =
            "{\n  \"audiothurgist\": 1,\n  \"generator\": {\"app\": \"web\", \"v\": [1, 2, 3]},\n  \"name\": \"future\",\n"
            "  \"sr\": 48000, \"stereo\": true, \"len\": 96000, \"bars\": 4, \"bpm\": 170,\n"
            "  \"slices\": [ {\"start\": 0, \"len\": 100, \"key\": 1, \"cls\": 2, \"sal\": 0.5, \"hit\": true, \"name\": \"k\", \"color\": \"#ff0\", \"tags\": [\"a\"]},\n"
            "               {\"start\": 100, \"len\": 200, \"mood\": {\"x\": null}} ],\n"
            "  \"options\": [1, 2, 3, 300, -4],\n  \"looper\": [ {\"phase\": 0.5, \"key\": 2, \"style\": 3, \"vel\": 1, \"swing\": 0.1} ],\n"
            "  \"notes\": \"anything\"\n}\n";
        bb::SetInfo t;
        CHECK(bb::SetReader::Json(fut, strlen(fut), t), "future file parses");
        CHECK(!strcmp(t.name, "future") && t.stereo && t.len == 96000 && t.bars == 4 && t.bpm == 170.f, "future header");
        CHECK(t.nSlices == 2 && t.slices[0].hit && t.slices[0].key == 1 && t.slices[1].start == 100 && t.slices[1].len == 200 && t.slices[1].name[0] == 0, "future slices");
        CHECK(t.nOptions == 5 && t.options[3] == 255 && t.options[4] == 0, "options clamped: %d %d", t.options[3], t.options[4]);
        CHECK(t.nLooper == 1 && t.looper[0].style == 3 && t.looper[0].vel == 1.f, "future looper");
    }
    /* EDIT-PERSIST: an old file (no grid / keys / flags) reads with the defaults; bogus values clamp */
    {
        std::string old = json;
        auto strip = [&](const char* key, char endc) {
            for(;;) { const size_t a = old.find(key); if(a == std::string::npos) return; size_t b = old.find(endc, a); if(b == std::string::npos) return; b++; if(b < old.size() && old[b] == ',') b++; old.erase(a, b - a); }
        };
        strip("\"grid\":", ','); strip("\"keys\":[", ']'); strip("\"flags\":", ',');
        CHECK(old.find("\"grid\"") == std::string::npos && old.find("\"keys\"") == std::string::npos && old.find("\"flags\"") == std::string::npos, "old-format text");
        bb::SetInfo t;
        CHECK(bb::SetReader::Json(old.data(), old.size(), t) && t.nGrid == 0 && !t.hasKeys && t.slices[1].flags == 0 && t.slices[4].flags == 0 && t.nSlices == 5, "old JSON: grid 0 (inferred later), no keys, flags 0");
        bool def = true; for(int k = 0; k < bb::kSetKeys; k++) def = def && t.keys[k] == -1;
        CHECK(def, "…every key at the default");
        const char* odd = "{\"audiothurgist\":1,\"sr\":48000,\"len\":1000,\"grid\":-3,\"slices\":[{\"start\":0,\"len\":500,\"flags\":3},{\"start\":500,\"len\":500,\"flags\":2}],"
                          "\"keys\":[1,-5,200,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,7,8,9,10]}";
        CHECK(bb::SetReader::Json(odd, strlen(odd), t) && t.nGrid == 0 && t.slices[0].flags == 1 && t.slices[1].flags == 0 && t.hasKeys && t.keys[0] == 1 && t.keys[1] == -1 && t.keys[2] == -1 && t.keys[23] == 1,
              "clamps: a negative grid → 0, flags masked to the orange bit, keys outside −1..127 → −1, 28 keys → 24");
        const char* big = "{\"audiothurgist\":1,\"sr\":48000,\"len\":1000,\"grid\":9,\"slices\":[{\"start\":0,\"len\":1000}]}";
        CHECK(bb::SetReader::Json(big, strlen(big), t) && t.nGrid == 0, "a grid count past the entries → 0 (inferred)");
        const char* few = "{\"audiothurgist\":1,\"sr\":48000,\"len\":1000,\"keys\":[5,6]}";
        CHECK(bb::SetReader::Json(few, strlen(few), t) && t.hasKeys && t.keys[0] == 5 && t.keys[1] == 6 && t.keys[2] == -1, "a short key list: the rest default");
    }
    /* EDIT-PERSIST: the Options schema pin — 24 bytes (OPT8: 22 Tap Allowance, 23 Autosave); the shell static_asserts Options::kBytes against it */
    {
        CHECK(bb::kSetOptionsToday == 24, "kSetOptionsToday = %d (OPT8)", bb::kSetOptionsToday);
        bb::SetInfo o; o.sr = 48000; o.len = 10; o.nOptions = bb::kSetOptionsToday;
        for(int i = 0; i < bb::kSetOptionsToday; i++) o.options[i] = (uint8_t)(i * 9);
        char ob[4096]; size_t on = 0;
        CHECK(bb::SetWriter::Json(o, ob, sizeof ob, &on) && strstr(ob, "\"options\":[0,9,18,") && strstr(ob, ",198,207]"), "24 option bytes emitted");
        bb::SetInfo t;
        CHECK(bb::SetReader::Json(ob, on, t) && t.nOptions == 24 && t.options[22] == 198 && t.options[23] == 207, "…and read back: byte 23 (Autosave) = 207");
        bb::PresetInfo p; p.nOptions = bb::kSetOptionsToday; for(int i = 0; i < 24; i++) p.options[i] = (uint8_t)(23 - i);
        char pb[1024]; size_t pn = 0; bb::PresetInfo q;
        CHECK(bb::PresetBackup::Write(p, pb, sizeof pb, &pn) && bb::PresetBackup::Read(pb, pn, q) && q.nOptions == 24 && q.options[23] == 0 && q.options[22] == 1, "a preset backup carries all 24");
    }
    // capacity: 100 slices in the file → 64 kept, rest consumed, still valid
    {
        std::string many = "{\"audiothurgist\":1,\"sr\":48000,\"slices\":[";
        for(int i = 0; i < 100; i++) { char b[64]; snprintf(b, sizeof b, "%s{\"start\":%d,\"len\":10}", i ? "," : "", i * 10); many += b; }
        many += "],\"options\":[";
        for(int i = 0; i < 40; i++) { char b[16]; snprintf(b, sizeof b, "%s%d", i ? "," : "", i); many += b; }
        many += "],\"len\":1000}";
        bb::SetInfo t;
        CHECK(bb::SetReader::Json(many.data(), many.size(), t), "oversize lists parse");
        CHECK(t.nSlices == bb::kSetMaxSlices && t.slices[63].start == 630 && t.nOptions == bb::kSetMaxOptions && t.len == 1000, "capped: %d slices, %d options", t.nSlices, t.nOptions);
    }
    // emitter capacity
    {
        char small[128];
        CHECK(!bb::SetWriter::Json(s, small, sizeof small, &n) && n == 0, "small buffer → false, len 0");
        // a full set fits kSetJsonMax
        bb::SetInfo full = s;
        full.nSlices = bb::kSetMaxSlices; full.nLooper = bb::kSetMaxLooper; full.nOptions = bb::kSetMaxOptions;
        for(int i = 0; i < full.nSlices; i++) { full.slices[i] = s.slices[i % 5]; strcpy(full.slices[i].name, "a thirty-one character name...."); full.slices[i].start = 1000000 + i; full.slices[i].len = 1000000; full.slices[i].sal = 0.123456f; }
        for(int i = 0; i < full.nLooper; i++) { full.looper[i] = {123.456789f, 23, 127, 0.987654f, 95}; }   /* SETS: int8 keys and slices */
        CHECK(bb::SetWriter::Json(full, json, sizeof json, &n), "worst-case set fits kSetJsonMax (%zu of %zu)", n, sizeof json);
        printf("  worst-case set JSON: %zu bytes\n", n);
        bb::SetInfo t;
        CHECK(bb::SetReader::Json(json, n, t), "worst-case parses");
        CheckSame(full, t, "worst-case");
    }

    SECTION("Set: WAV + JSON via sinks, cues == slice starts");
    {
        static uint8_t staging[16384];
        std::vector<uint8_t> wav, js;
        CHECK(bb::SetWriter::Wav({MemSink, &wav}, s, gL, gR, staging, sizeof staging), "set wav");
        CHECK(bb::SetWriter::Json({MemSink, &js}, s, json, sizeof json), "set json via sink");
        FILE* f = fopen(Path("set.wav").c_str(), "wb"); fwrite(wav.data(), 1, wav.size(), f); fclose(f);
        f = fopen(Path("set.json").c_str(), "wb"); fwrite(js.data(), 1, js.size(), f); fclose(f);
        std::vector<uint8_t> jr = ReadAll(Path("set.json").c_str());
        bb::SetInfo t;
        CHECK(bb::SetReader::Json((const char*)jr.data(), jr.size(), t), "set json from file");
        CheckSame(s, t, "set");
        static std::vector<float> stage(kLen * 2), out(kLen * 2);
        int cues[128], nCues = 0, err = 0;
        const int n2 = bbwav::Load(Path("set.wav").c_str(), stage.data(), (int)stage.size(), out.data(), (int)out.size(), 48000.f, 25, cues, &nCues, 128, &err);
        CHECK(n2 == kLen && err == 0 && nCues == t.nSlices, "loader: n %d err %d cues %d", n2, err, nCues);
        for(int i = 0; i < nCues && i < t.nSlices; i++) CHECK(cues[i] == t.slices[i].start, "cue %d == slice start", i);
        // a mono set writes one channel even if a right plane is offered
        bb::SetInfo mono = s; mono.stereo = false;
        std::vector<uint8_t> mw;
        CHECK(bb::SetWriter::Wav({MemSink, &mw}, mono, gL, gR, staging, sizeof staging) && Rd16(&mw[22]) == 1, "mono set → 1 channel");
        // slices past the buffer end are not written as cues
        bb::SetInfo oob = s; oob.slices[4].start = kLen + 5;
        std::vector<uint8_t> ow;
        CHECK(bb::SetWriter::Wav({MemSink, &ow}, oob, gL, gR, staging, sizeof staging) && Rd32(&ow[12 + 8 + 16 + 8]) == 4u, "out-of-range slice dropped from cues");
    }

    SECTION("Preset backup round trip");
    {
        bb::PresetInfo p;
        p.slot = 3; strcpy(p.name, "late night"); p.nOptions = 17;
        for(int i = 0; i < 17; i++) p.options[i] = (uint8_t)(200 - i);
        p.nPages = 2;
        for(int g = 0; g < 2; g++) for(int k = 0; k < 6; k++) p.pager[g][k] = 0.1f * (g * 6 + k);
        strcpy(p.sample, "AMEN_170.WAV");
        char buf[1024]; size_t pn = 0;
        CHECK(bb::PresetBackup::Write(p, buf, sizeof buf, &pn), "emit preset");
        printf("  %s", buf);
        bb::PresetInfo q;
        CHECK(bb::PresetBackup::Read(buf, pn, q), "parse preset");
        bool same = q.slot == 3 && !strcmp(q.name, p.name) && q.nOptions == 17 && !memcmp(q.options, p.options, 17) && q.nPages == 2 && !strcmp(q.sample, p.sample);
        for(int g = 0; g < 2; g++) for(int k = 0; k < 6; k++) same = same && fabsf(q.pager[g][k] - p.pager[g][k]) < 1e-5f;
        CHECK(same, "preset fields survive");
        int okp = 0;
        for(size_t k = 0; k + 1 < pn; k++) { std::vector<char> pre(buf, buf + k); bb::PresetInfo t; if(bb::PresetBackup::Read(pre.data(), k, t)) okp++; }
        CHECK(okp == 0, "%d truncated preset prefixes accepted", okp);
        const char* fut = "{\"audiothurgist\":1,\"slot\":9,\"pager\":[[1,2,3,4,5,6,7],[0.5]],\"extra\":{\"a\":[]},\"sample\":\"x.wav\",\"options\":[]}";
        CHECK(bb::PresetBackup::Read(fut, strlen(fut), q) && q.slot == 9 && q.nPages == 2 && q.pager[0][5] == 6.f && q.pager[1][0] == 0.5f && q.pager[1][1] == 0.f && q.nOptions == 0, "future preset");
        const char* v0 = "{\"audiothurgist\":0,\"slot\":1}";
        CHECK(!bb::PresetBackup::Read(v0, strlen(v0), q), "preset version 0 rejected");
    }
}

int main(int argc, char** argv)
{
    gOut = argc > 1 ? argv[1] : "/tmp";
    BuildMaterial();
    TestWav();
    TestJson();
    TestSet();
    printf("\npersist_check: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
