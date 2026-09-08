// record_check — RECORD package (sample mode phases B + C) host suite:
// core/bb_record.h, the loader's stereo planes (core/wav_loader.h) and the
// ConditionInfo additions (core/bb_condition.h). Build + run (tests/run.sh
// does this under ASan/UBSan):
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Icore -Ivendor/capicola/lib
//       tests/record_check.cpp -o /tmp/rc && /tmp/rc /tmp   (one line)
//
// 1. The stereo shell math: the record input per Sampling, the block peak,
//    the kit-map LED, the Clear default (== the engine's own key table).
// 2. Take-length arithmetic (bars × 16 steps × samples per 16th at a tempo).
// 3. Markers → cues → the onset table (the WAV cue path, factored).
// 4. Stereo conditioning: the R plane follows ConditionBuffer's cut and gain
//    exactly (prior path and bar-cut path); the clocked take's normalise.
// 5. TakeRecorder: fixed length, cap, Stop, the R copy, the peak.
// 6. The load pipeline on a synthetic take: clocked (no trim, the markers
//    become the table, the grid is bars × 16, the native tempo is the
//    clock's) and unclocked (trimmed, cues re-based by the head cut).
// 7. bbwav::Load with two planes: L/R apart, the mono path byte-identical,
//    a mono file reports no R, and the cue walk survives more cues than
//    the caller keeps (the persist doc's loader gotcha).
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "bb_engine.h"
#include "bb_onset.h"
#include "bb_condition.h"
#include "bb_testbreak.h"
#include "bb_record.h"
#include "bb_wavwrite.h"

// ── the firmware loader over stdio (as persist_check does) ──
struct FIL { FILE* fp; };
typedef unsigned UINT;
enum FRESULT { FR_OK = 0, FR_DISK_ERR = 1 };
#define FA_READ 1
static FRESULT f_open(FIL* f, const char* n, int) { f->fp = fopen(n, "rb"); return f->fp ? FR_OK : FR_DISK_ERR; }
static FRESULT f_read(FIL* f, void* b, UINT n, UINT* br) { *br = (UINT)fread(b, 1, n, f->fp); return FR_OK; }
static FRESULT f_lseek(FIL* f, uint32_t o) { return fseek(f->fp, (long)o, SEEK_SET) == 0 ? FR_OK : FR_DISK_ERR; }
static uint32_t f_tell(FIL* f) { return (uint32_t)ftell(f->fp); }
static FRESULT f_close(FIL* f) { fclose(f->fp); return FR_OK; }
#include "wav_loader.h"

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...) do { if(cond) { gPass++; } else { gFail++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)
#define SECTION(name) printf("\n== %s ==\n", name)

static constexpr float kSr = 48000.f;
static bb::OnsetDetector gDet;

static bool FileSink(void* ctx, const void* p, size_t n) { return fwrite(p, 1, n, (FILE*)ctx) == n; }

int main(int argc, char** argv)
{
    const std::string out = argc > 1 ? argv[1] : "/tmp";

    SECTION("record input: Sampling = Mono sums J1+J2 into L, Stereo keeps the planes apart");
    {
        float a[24], b[24], mix[24];
        for(int i = 0; i < 24; i++) { a[i] = 0.5f; b[i] = -0.25f + 0.01f * (float)i; }
        bb::RecPlanes m = bb::RecordInput(bb::kSampMono, a, b, mix, 24);
        bool ok = m.L == mix && m.R == nullptr;
        for(int i = 0; i < 24 && ok; i++) if(fabsf(mix[i] - 0.5f * (a[i] + b[i])) > 1e-7f) ok = false;
        CHECK(ok, "Mono: L = (a + b) / 2 in the mix buffer, no R");
        bb::RecPlanes s = bb::RecordInput(bb::kSampStereo, a, b, mix, 24);
        CHECK(s.L == a && s.R == b, "Stereo: L = a, R = b, no copy");
        float c[8] = {0.1f, -0.7f, 0.2f, 0.f, 0.f, 0.f, 0.f, 0.3f}, d[8] = {0.f, 0.f, 0.f, 0.f, 0.9f, 0.f, 0.f, 0.f};
        CHECK(bb::BlockPeak(c, nullptr, 8) == 0.7f && bb::BlockPeak(c, d, 8) == 0.9f, "block peak over one or two planes");
    }

    SECTION("kit map: 24 keys over 16 LEDs — keys 0..7 own LEDs 0..7, keys 8..23 pair on 8..15");
    {
        int wrong = 0;
        for(int k = 0; k < 8; k++) if(bb::KeyLed(k) != k) wrong++;
        for(int k = 8; k < 24; k++) if(bb::KeyLed(k) != 8 + (k - 8) / 2) wrong++;
        CHECK(wrong == 0 && bb::KeyLed(8) == 8 && bb::KeyLed(9) == 8 && bb::KeyLed(22) == 15 && bb::KeyLed(23) == 15, "%d wrong", wrong);
        CHECK(bb::KeyLed(-3) == 0 && bb::KeyLed(99) == 15, "clamped both ways");
        int used[16] = {0}; for(int k = 0; k < 24; k++) used[bb::KeyLed(k)]++;
        bool every = true; for(int i = 0; i < 16; i++) if(used[i] < 1 || used[i] > 2) every = false;
        CHECK(every, "every LED carries one or two keys");
    }

    SECTION("Clear's default == the engine's own key table (SpanSlice vs KeySlice)");
    {
        static float m16[48000 * 2], m64[48000 * 9];
        for(int i = 0; i < 48000 * 2; i++) m16[i] = 0.1f * sinf(0.02f * (float)i);
        for(int i = 0; i < 48000 * 9; i++) m64[i] = 0.1f * sinf(0.01f * (float)i);
        bb::Engine e; e.Init(1);
        e.SetMaterial(m16, 48000 * 2, kSr, nullptr, 0, nullptr);
        int wrong = 0; for(int k = 0; k < 24; k++) if(bb::SpanSlice(k, e.nSlices) != e.KeySlice(k)) wrong++;
        CHECK(e.nSlices == 16 && wrong == 0, "16 slices: %d wrong", wrong);
        e.SetMaterial(m64, 48000 * 9, kSr, nullptr, 0, nullptr);
        wrong = 0; for(int k = 0; k < 24; k++) if(bb::SpanSlice(k, e.nSlices) != e.KeySlice(k)) wrong++;
        CHECK(e.nSlices == 64 && wrong == 0, "64 slices: %d wrong", wrong);
        // Clear after a take puts the key back where it was
        static float big[48000 * 3]; memcpy(big, m16, sizeof m16);
        e.SetMaterial(big, 48000 * 2, kSr, nullptr, 0, nullptr, 0, nullptr, 48000 * 3);
        CHECK(e.RecArm(5), "arm key 5");
        float in[24]; for(int j = 0; j < 24; j++) in[j] = 0.4f;
        for(int i = 0; i < 4800; i += 24) e.RecFeed(in, 24);
        int rs, rl; e.RecStop(rs, rl);
        CHECK(e.CommitSlice(5, rs, rl, 0, 1.f, 0.f, true) == 16 && e.KeySlice(5) == 16, "key 5 → the recording");
        CHECK(e.SetKeySlice(5, bb::SpanSlice(5, e.nSlices)) && e.KeySlice(5) == 5 && e.RecordedEntry(5) == 16,
              "Clear: key 5 plays slice 5 again; the region stays in the buffer (entry 16 kept)");
    }

    SECTION("take length: bars × 16 × (15 / bpm) s at the sample rate");
    {
        CHECK(bb::TakeSamples(1, 120.f, kSr) == 96000, "1 bar at 120 BPM = 96000 samples (got %d)", bb::TakeSamples(1, 120.f, kSr));
        CHECK(bb::TakeSamples(2, 120.f, kSr) == 192000, "2 bars = 192000");
        CHECK(bb::TakeSamples(4, 170.f, kSr) == 271059, "4 bars at 170 BPM = 271059 (got %d)", bb::TakeSamples(4, 170.f, kSr));
        CHECK(bb::TakeSamples(1, 60.f, kSr) == 192000 && bb::TakeSamples(8, 60.f, kSr) == 8 * 192000, "60 BPM: 4 s per bar");
        CHECK(bb::TakeSamples(0, 120.f, kSr) == 0 && bb::TakeSamples(2, 0.f, kSr) == 0 && bb::TakeSamples(2, -1.f, kSr) == 0, "no bars or no tempo = no auto-stop");
        // the loop the clock will then play: N bars at the take's tempo IS the tempo
        static float take[192000];
        for(int i = 0; i < 192000; i++) take[i] = 0.3f * sinf(0.05f * (float)i);
        bb::Engine e; e.Init(1);
        e.SetMaterial(take, bb::TakeSamples(2, 120.f, kSr), kSr, nullptr, 0, nullptr, 32);
        CHECK(fabsf(e.NativeBpm() - 120.f) < 0.05f, "a 2-bar take at 120 sliced to 32 reads 120 BPM native (%.2f)", e.NativeBpm());
    }

    SECTION("markers → cues → onset table (the WAV cue path, factored)");
    {
        int m[8] = {24000, 24010, 48000, -5, 96000, 200000, 72000, 72100};
        int cues[8];
        const int n = bb::MarkersToCues(m, 8, 192000, 480, cues, 8);
        CHECK(n == 4 && cues[0] == 24000 && cues[1] == 48000 && cues[2] == 96000 && cues[3] == 72000,
              "double edges dropped (10 ms: 24010, 72100), out-of-range dropped, order kept (n %d)", n);
        int few[2]; CHECK(bb::MarkersToCues(m, 8, 192000, 480, few, 2) == 2 && few[1] == 48000, "capped at maxCues");
        // CuesToOnsets: re-base, drop, sort, open with 0 unless the first cue is within 20 ms
        {
            // {5000,100,3000,900000,4000} − 200 → keep in [0,10000) → {4800,2800,3800} → sorted; 2800 > 20 ms → open with 0
            int c2[5] = {5000, 100, 3000, 900000, 4000};
            int on[8];
            const int no = bb::CuesToOnsets(c2, 5, 200, 10000, kSr, on, 8);
            CHECK(no == 4 && on[0] == 0 && on[1] == 2800 && on[2] == 3800 && on[3] == 4800, "re-based, dropped, sorted, opened with 0 (n %d)", no);
        }
        {
            int c3[3] = {5000, 3000, 4000};
            int o3[8];
            const int n3 = bb::CuesToOnsets(c3, 3, 200, 10000, kSr, o3, 8);
            CHECK(n3 == 4 && o3[0] == 0 && o3[1] == 2800 && o3[2] == 3800 && o3[3] == 4800, "re-based, sorted, opened with 0 (n %d: %d %d %d %d)", n3, o3[0], o3[1], o3[2], o3[3]);
            CHECK(c3[0] == 2800 && c3[1] == 3800 && c3[2] == 4800, "the cue list is rewritten in place (re-based, sorted)");
        }
        {
            int c4[3] = {700, 5000, 9000};   // 700 - 200 = 500 samples ≈ 10 ms: within 20 ms of the start → no extra 0
            int o4[8];
            const int n4 = bb::CuesToOnsets(c4, 3, 200, 10000, kSr, o4, 8);
            CHECK(n4 == 3 && o4[0] == 500 && o4[1] == 4800 && o4[2] == 8800, "a first cue within 20 ms IS the start (n %d, first %d)", n4, o4[0]);
        }
        {
            int c5[3] = {50, 60, 70};   // everything before the head cut
            int o5[8];
            const int n5 = bb::CuesToOnsets(c5, 3, 200, 10000, kSr, o5, 8);
            CHECK(n5 == 1 && o5[0] == 0, "nothing survives the cut: the table is {0}");
            int c6[4] = {1000, 2000, 3000, 4000};
            int o6[2];
            CHECK(bb::CuesToOnsets(c6, 4, 0, 10000, kSr, o6, 2) == 2 && o6[0] == 0 && o6[1] == 1000, "capped at maxOnsets");
        }
    }

    SECTION("stereo conditioning: the R plane follows ConditionBuffer's cut and gain exactly");
    {
        // prior path: a tone with 0.5 s of silence ahead of it, no bar structure
        const int head = 24000, body = 48000 * 2, len = head + body;
        static float L[48000 * 3], R[48000 * 3];
        for(int i = 0; i < len; i++) L[i] = i < head ? 0.f : 0.4f * sinf(0.03f * (float)(i - head));
        memcpy(R, L, sizeof(float) * (size_t)len);
        bb::ConditionInfo ci;
        const int n = bb::ConditionBuffer(L, len, kSr, gDet, &ci, true);
        CHECK(ci.trimmedStart >= head - 32 && ci.trimmedStart <= head - 30 && n <= body + 240 + 32,
              "the prior path reports the head cut too (trimmedStart %d ≈ %d, len %d)", ci.trimmedStart, head - 32, n);
        CHECK(fabsf(ci.gain - 0.95f / 0.4f) < 1e-4f, "…and the gain (%.4f)", ci.gain);
        bb::ConditionPlane(R, ci, n);
        bool same = true; for(int i = 0; i < n; i++) if(R[i] != L[i]) { same = false; break; }
        CHECK(same, "R == L after the same cut and gain, bit for bit");
        // the bar-cut path: the synth break with silence ahead — whichever branch the estimator takes, R must follow
        static float BL[48000 * 6], BR[48000 * 6];
        const int bh = 12000;
        const int bl = bb::MakeTestBreak(BL + bh, 48000 * 6 - bh, kSr);
        for(int i = 0; i < bh; i++) BL[i] = 0.f;
        for(int i = 0; i < bh + bl; i++) BL[i] *= 0.5f;   // headroom, so the gain is not 1
        memcpy(BR, BL, sizeof(float) * (size_t)(bh + bl));
        bb::ConditionInfo cb;
        const int nb = bb::ConditionBuffer(BL, bh + bl, kSr, gDet, &cb, true);
        bb::ConditionPlane(BR, cb, nb);
        same = true; for(int i = 0; i < nb; i++) if(BR[i] != BL[i]) { same = false; break; }
        CHECK(same && cb.trimmedStart >= bh - 32 && cb.gain > 1.f, "the break: R follows (trimmedStart %d, bars %d, gain %.3f, len %d)", cb.trimmedStart, cb.bars, cb.gain, nb);
        // NormalisePlanes: one gain for both, peak → 0.95 wherever it is
        float a[100], b[100];
        for(int i = 0; i < 100; i++) { a[i] = 0.1f; b[i] = 0.f; }
        b[40] = -0.5f;
        const float g = bb::NormalisePlanes(a, b, 100);
        CHECK(fabsf(g - 1.9f) < 1e-5f && fabsf(b[40] + 0.95f) < 1e-5f && fabsf(a[0] - 0.19f) < 1e-5f, "normalised across both planes (g %.3f)", g);
        float z[10] = {0}; CHECK(bb::NormalisePlanes(z, nullptr, 10) == 1.f, "silence: left alone");
    }

    SECTION("TakeRecorder: fixed length, cap, Stop, the R copy, the peak");
    {
        static float L[10000], R[10000];
        bb::TakeRecorder t;
        float in[24], inR[24];
        for(int j = 0; j < 24; j++) { in[j] = 0.25f; inR[j] = -0.5f; }
        t.Start(L, R, 10000, 100);   // target 100: stops mid-block, exactly there
        int blocks = 0;
        while(t.running && blocks < 100) { t.Feed(in, inR, 24); blocks++; }
        CHECK(!t.running && t.pos == 100 && blocks == 5, "stops itself at the target (pos %d after %d blocks)", t.pos, blocks);
        CHECK(L[99] == 0.25f && R[99] == -0.5f && L[100] == 0.f, "…without writing past it; R took the R input");
        t.Feed(in, inR, 24);
        CHECK(t.pos == 100, "a feed after the stop is ignored");
        CHECK(fabsf(t.peak - 0.25f) < 1e-7f, "peak = the L input's (%.3f)", t.peak);
        memset(L, 0, sizeof L); memset(R, 0, sizeof R);
        t.Start(L, R, 10000, 0);     // no target: runs to the cap
        while(t.running) t.Feed(in, nullptr, 24);
        CHECK(t.pos == 10000 && R[9999] == 0.25f, "no target: the cap stops it; a mono feed is copied to R");
        t.Start(L, nullptr, 10000, 5000);
        for(int i = 0; i < 10; i++) t.Feed(in, nullptr, 24);
        CHECK(t.Stop() == 240 && !t.running && t.pos == 240, "Stop returns the length taken");
        t.Start(L, nullptr, 10000, 20000);
        CHECK(t.target == 10000, "a target past the cap is the cap");
    }

    SECTION("the load pipeline on a synthetic take");
    {
        // a clocked 2-bar take at 120 BPM: 32 steps of 6000 samples, a tone per
        // step (silent first 16th — the downbeat rest must stay where it is),
        // J6 markers on steps 4, 8 and 12 (with ~1 ms of poll jitter)
        const int bars = 2, len = bb::TakeSamples(bars, 120.f, kSr), step = len / 32;
        static float L[192000], R[192000];
        for(int s = 0; s < 32; s++)
            for(int i = 0; i < step; i++)
            {
                const float v = s == 0 ? 0.f : 0.3f * sinf(6.2831853f * (200.f + 50.f * (float)s) * (float)i / kSr);
                L[s * step + i] = v; R[s * step + i] = -v;
            }
        int markers[3] = {4 * step + 40, 8 * step + 12, 12 * step + 55};
        // clocked: normalise (no trim), markers → cues → onsets, the grid is bars × 16
        const float g = bb::NormalisePlanes(L, R, len);
        CHECK(fabsf(g - 0.95f / 0.3f) < 1e-3f && L[0] == 0.f && fabsf(R[step + 100] + L[step + 100]) < 1e-6f, "normalised, both planes, nothing moved");
        int cues[8];
        const int nc = bb::MarkersToCues(markers, 3, len, 480, cues, 8);
        int onsets[8];
        const int no = bb::CuesToOnsets(cues, nc, 0 /* no head cut */, len, kSr, onsets, bb::Engine::kMaxSlices);
        CHECK(nc == 3 && no == 4 && onsets[0] == 0 && onsets[1] == markers[0], "the table is 0 + the three markers");
        bb::Engine e; e.Init(7);
        e.SetMaterial(L, len, kSr, onsets, no, nullptr, bars * 16, R, 192000);
        CHECK(e.nSlices == 32 && fabsf(e.NativeBpm() - 120.f) < 0.05f, "32 slices, native 120 BPM (%.2f)", e.NativeBpm());
        int hits = 0; for(int s = 0; s < 32; s++) if(e.stepHit[s]) hits++;
        CHECK(hits == 4 && e.stepHit[0] && e.stepHit[4] && e.stepHit[8] && e.stepHit[12], "the markers are the hits (%d): steps 0, 4, 8, 12", hits);
        // unclocked: the same take through ConditionBuffer — the silent first
        // 16th goes, the markers re-base by the head cut and still land
        static float UL[192000], UR[192000];
        memcpy(UL, L, sizeof L); memcpy(UR, R, sizeof R);
        bb::ConditionInfo ci;
        const int ulen = bb::ConditionBuffer(UL, len, kSr, gDet, &ci, true);
        bb::ConditionPlane(UR, ci, ulen);
        int uc[3]; memcpy(uc, markers, sizeof uc);
        int un[8];
        const int uno = bb::CuesToOnsets(uc, 3, ci.trimmedStart, ulen, kSr, un, bb::Engine::kMaxSlices);
        CHECK(ci.trimmedStart >= step - 40 && ci.trimmedStart <= step, "the head cut is the silent 16th (%d of %d)", ci.trimmedStart, step);
        CHECK(uno == 4 && un[1] == markers[0] - ci.trimmedStart, "markers re-based by the cut (%d → %d)", markers[0], un[1]);
        bool same = true; for(int i = 0; i < ulen; i++) if(UR[i] != -UL[i] && fabsf(UR[i] + UL[i]) > 1e-6f) { same = false; break; }
        CHECK(same, "R still mirrors L after the cut");
    }

    SECTION("bbwav::Load: two planes apart, the mono path unchanged, a mono file reports no R, oversize cue lists");
    {
        const int len = 24000;
        static float wl[len], wr[len];
        for(int i = 0; i < len; i++) { wl[i] = 0.5f * sinf(0.01f * (float)i); wr[i] = -0.25f * sinf(0.037f * (float)i); }
        const int cuesIn[5] = {100, 2000, 5000, 9000, 15000};
        auto write = [&](const char* name, bool stereo, int nCues) -> std::string {
            const std::string path = out + "/" + name;
            FILE* fp = fopen(path.c_str(), "wb");
            bbwav::WriteSpec sp; sp.left = wl; sp.right = stereo ? wr : nullptr; sp.len = len; sp.sr = 48000; sp.fmt = bbwav::kPcm16;
            sp.cues = cuesIn; sp.nCues = nCues;
            static uint8_t staging[16384];
            const bool ok = bbwav::Write(bbwav::Sink{&FileSink, fp}, sp, staging, sizeof staging);
            fclose(fp);
            CHECK(ok, "wrote %s", name);
            return path;
        };
        const std::string st = write("rc_stereo.wav", true, 5), mo = write("rc_mono.wav", false, 5);
        static std::vector<float> stage(len * 2), stageR(len * 2), a(len * 2), b(len * 2), c(len * 2);
        int cues[8], nCues = 0, err = 0; bool gotR = true;
        // 1. the old call: folds to mono
        const int n1 = bbwav::Load(st.c_str(), stage.data(), len * 2, a.data(), len * 2, kSr, 25, cues, &nCues, 8, &err);
        CHECK(n1 == len && err == 0 && nCues == 5, "mono fold: n %d err %d cues %d", n1, err, nCues);
        // 2. two planes
        const int n2 = bbwav::Load(st.c_str(), stage.data(), len * 2, b.data(), len * 2, kSr, 25, cues, &nCues, 8, &err, c.data(), stageR.data(), &gotR);
        CHECK(n2 == len && err == 0 && gotR && nCues == 5, "planes: n %d err %d gotR %d", n2, err, (int)gotR);
        float worstL = 0.f, worstR = 0.f, worstFold = 0.f;
        for(int i = 0; i < len; i++)
        {
            const float el = fabsf(b[i] - wl[i]), er = fabsf(c[i] - wr[i]), ef = fabsf(a[i] - 0.5f * (b[i] + c[i]));
            if(el > worstL) worstL = el; if(er > worstR) worstR = er; if(ef > worstFold) worstFold = ef;
        }
        CHECK(worstL < 1.f / 32768.f * 1.01f && worstR < 1.f / 32768.f * 1.01f, "L and R within one LSB (%.2e / %.2e)", worstL, worstR);
        CHECK(worstFold < 1e-6f, "the mono fold == (L + R) / 2 of the planes (%.2e)", worstFold);
        // 3. the planes asked for on a mono file: L only, gotR false, byte-identical to the plain call
        static std::vector<float> d(len * 2), e2(len * 2);
        const int n3 = bbwav::Load(mo.c_str(), stage.data(), len * 2, d.data(), len * 2, kSr, 25, cues, &nCues, 8, &err);
        gotR = true;
        const int n4 = bbwav::Load(mo.c_str(), stage.data(), len * 2, e2.data(), len * 2, kSr, 25, cues, &nCues, 8, &err, c.data(), stageR.data(), &gotR);
        CHECK(n3 == len && n4 == len && !gotR && memcmp(d.data(), e2.data(), sizeof(float) * (size_t)len) == 0, "a mono file: no R, the same samples");
        // 4. more cues than the caller keeps: the data still loads (the walk used to skip only the kept entries' bytes)
        const int n5 = bbwav::Load(st.c_str(), stage.data(), len * 2, a.data(), len * 2, kSr, 25, cues, &nCues, 2, &err);
        CHECK(n5 == len && err == 0 && nCues == 2 && cues[0] == 100 && cues[1] == 2000, "maxCues 2 of 5: n %d err %d cues %d", n5, err, nCues);
        remove(st.c_str()); remove(mo.c_str());
    }

    printf("\nrecord_check: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
