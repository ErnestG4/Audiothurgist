/**
 * audiothurgist.cpp — M2: the Brain (clock sync + class gates).
 *
 * BreakFix engine on Alchemy Lab v2, now reading the SD card and blooming
 * reverb in the cluster zone. Boots playing the first WAV on the card (the
 * synth break if none); B1 walks the card. SD read + conditioning + class
 * analysis run off the audio thread; a double buffer swaps in the callback.
 * Record from J1/J2 is M1.6.
 *
 *   P1 Chop · P2 Tempo · P3 History · P4 Pitch · P5 Sample-select · P6 Volume
 *   B1 hold + turn a pot = record a motion loop (ParamLock) · B2 tap refill /
 *   hold flow · B3 tap reslice / hold load · B2+B3 held 2 s = Settings
 *
 * DMA law (SDK's SdCard header confirms our Pod-era finding): every buffer
 * SDMMC touches must live in SDRAM, not DTCM. BOOT_SRAM puts plain .bss in
 * DTCM, so the FILE object, IO staging, and material buffers are all placed
 * in .sdram_bss via ALCHEMY_SDRAM_BSS / BBWAV_DMA_SECTION.
 */

#include "daisy_seed.h"
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/hw/alchemy_lab_layout.h"
#include "alchemy/led/panel.h"
#include "alchemy/led/anims/level_arc.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/virtual_knob.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/param_lock.h"
#include "alchemy/surface/settings.h"
#include "alchemy/control/musical_clock.h"
#include "alchemy/control/clock_follower.h"
#include "alchemy/control/cv_edge.h"
#include "alchemy/host_link/host.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/storage/fs_extension.h"
#include "alchemy/host_link/extension.h"
#include "alchemy/host_link/frame.h"
#include "alchemy/surface/serializable.h"
#include "alchemy/host_link/component_writer.h"

#include "ff.h"
#include "Effects/reverbsc.h"

/* WAV loader's SDMMC-DMA staging goes in AXI SRAM (.axi_bss), the region the
 * SDK reserves for FatFs work buffers — SDMMC1 IDMA can reach it; DTCM (plain
 * .bss on this board) it cannot. */
#define BBWAV_DMA_SECTION ALCHEMY_SDMMC_BSS
#include "wav_loader.h"

#include "bb_engine.h"
#include "bb_onset.h"
#include "bb_condition.h"
#include "bb_testbreak.h"

using namespace alchemy;

constexpr float kSr       = 48000.f;
constexpr int   kMaxSec   = 25;
constexpr int   kBufMax   = 48000 * kMaxSec;
constexpr int   kStageMax = 96000 * kMaxSec;   /* native-rate decode staging */
constexpr int   kScratch  = 128;
constexpr int   kMaxFiles = 256;
constexpr int   kMaxOnsets = 128;

/* ── SDRAM: material double buffer, decode staging, analysis, reverb ──── */
static float ALCHEMY_SDRAM_BSS gBufA[kBufMax];
static float ALCHEMY_SDRAM_BSS gBufB[kBufMax];
static float ALCHEMY_SDRAM_BSS gStage[kStageMax];
static bb::OnsetDetector ALCHEMY_SDRAM_BSS gDet;
static daisysp::ReverbSc ALCHEMY_SDRAM_BSS gVerb;
static char ALCHEMY_SDRAM_BSS  gFiles[kMaxFiles][128];
static int                     gNumFiles = 0;

/* FatFs scan objects — AXI SRAM (DMA-reachable), file scope. */
ALCHEMY_SDMMC_BSS alignas(32) static DIR     gDir;
ALCHEMY_SDMMC_BSS alignas(32) static FILINFO gFno;

static float* gPlay = gBufA;   /* engine reads this */
static float* gLoad = gBufB;   /* loader fills this */

static bb::Engine  engine;
static AlchemyLab  hw;
static SdCard      sd;
static hostlink::FsExtension fs_ext(sd);  /* browser SD file list */

/* Clock: J3 (cv chan 0) = external clock in (24 PPQN); internal from P2 else.
 * The engine's playhead is always driven from this clock (extPhase). */
static constexpr uint8_t kClockChan = 0u;
static MusicalClock  gClock(kClockPpqnInternal, 4u);
static ClockFollower gFollower(gClock);
static CvGate        gClkEdge;   /* unipolar-gate Schmitt: V2 CV rests at 0.5, +5 V ≈ 0.75 */
static uint32_t gLastPulseUs = 0u;
static float    gP2Ref       = -1e9f;   /* tempo-knob engages on movement */
static volatile bool gNeedNativeTempo = false;  /* set on load (audio) → clock (poll) */

/* Per-class gate outs: J6/J7/J8 = kick/snare/hat (cv jacks).
 * ── musical branch: Gate Length + Gate Level (velocity from salience) ── */
static constexpr int kGateHold = 1 << 28;            /* "hold to next hit" sentinel */
static const int kGateLenSamps[4] = { (int)(0.006f * kSr), (int)(0.020f * kSr),
                                      (int)(0.050f * kSr), kGateHold };
static volatile int   gGateCd[3]     = {0, 0, 0};    /* audio arms/decrements; poll reads */
static volatile float gGateLvl[3]    = {5.f, 5.f, 5.f}; /* volts for the current gate */
static volatile bool  gGateRetrig[3] = {false, false, false}; /* Hold mode: fired while high */
static float          gSal[bb::Engine::kMaxSlices];        /* live per-slice salience 0..1 */
static float          gPendSal[bb::Engine::kMaxSlices];    /* staged with a load */
static float          gReclassSal[bb::Engine::kMaxSlices]; /* staged with a reclassify */

/* ── Web frontend state (HostLink extension 0x60–0x6F, see StateExt) ────
 * Per-slice envelope (8 min/max pairs, u8 with 128 = 0) computed at load on
 * the control thread; a 1 KB ring of the mono output decimated ×4 for the
 * scope, written by the audio thread. gMatGen bumps on every material swap so
 * the page knows when to refetch the envelope. */
static constexpr int kEnvPts = 8;
static uint8_t ALCHEMY_SDRAM_BSS gEnv[bb::Engine::kMaxSlices][kEnvPts][2];
static uint8_t ALCHEMY_SDRAM_BSS gPendEnv[bb::Engine::kMaxSlices][kEnvPts][2];
static volatile uint8_t  gMatGen  = 0;
static int8_t            gScope[1024];
static volatile uint16_t gScopeW  = 0;

/* Jack descriptors (browser labels; HYBRID/BRAIN map fills in at M3). */
Jack jk1 ("audio_l", "Audio In L",  JackSig::AudioIn);
Jack jk2 ("audio_r", "Audio In R",  JackSig::AudioIn);
Jack jk3 ("clock",   "Clock In",    JackSig::Trig);
Jack jk4 ("voct",    "V/Oct",       JackSig::Voct);
Jack jk5 ("gate",    "Gate",        JackSig::Gate);
Jack jk6 ("kick",    "Kick Gate",   JackSig::Gate);
Jack jk7 ("snare",   "Snare Gate",  JackSig::Gate);
Jack jk8 ("hat",     "Hat Gate",    JackSig::Gate);
Jack jk9 ("out_l",   "Audio Out L", JackSig::AudioOut);
Jack jk10("out_r",   "Audio Out R", JackSig::AudioOut);
static Presets     presets(hw.seed.qspi);
static ControlLoop loop(hw);
static Pager       pager(hw.buttons[kButtonB1], 2, kNumPots); /* B1 tap: Perform ↔ Brain */
/* Hold B1 + turn a pot: record that pot's motion as a loop (16 s @ 30 Hz,
 * 5.8 KB, saved with presets). Values compose into knob.Norm() automatically. */
static ParamLock<2 * kNumPots> locks(hw.buttons[kButtonB1], pager);
/* Settings screen: B2+B3 held 2 s enters; brightness + the preset gesture. */
Settings           settings(hw, &pager);
void DescribeManual(alchemy::hostlink::Host& host);   /* src/audiothurgist_manual.cpp */
static hostlink::Host host(presets, "audiothurgist", "Audiothurgist",
                           "0.7.1-reviewed", "web");

/* ── shared control/audio state ─────────────────────────────────────── */
static volatile float gVol      = 0.8f;
static volatile float gVerbSend = 0.f;

static volatile bool gPendReroll  = false;
static volatile bool gPendRegen   = false;   /* same seeds, new language params */
static volatile bool gPendReslice = false;
static volatile int  gScrubDir    = 0;
static volatile bool gScrubReq    = false;

/* load handshake: control thread fills gLoad + pending meta, sets gApply;
 * audio thread swaps and calls SetMaterial (as BreakFix did at JOB_APPLY). */
static volatile bool gLoadReq   = false;   /* request a load (from B1)     */
static volatile int  gLoadIdx   = 0;       /* which file                   */
static volatile bool gApply     = false;   /* loader done → audio may swap */
static int           gPendOnsets[kMaxOnsets];
static volatile int  gPendN = 0, gPendLen = 0;
static uint8_t       gPendClass[bb::Engine::kMaxSlices];
static volatile int  gCurFile = -1;
static volatile float gLoadFlash = 0.f;

static bool  gFlowing = false;
static float gLastP3  = -1.f;
static int   gSelFile = 0;                       /* P5 highlights this file */
static const char* gLabelPtrs[kMaxFiles];        /* → gFiles[], for the browser */
static char ALCHEMY_SDRAM_BSS gSampleDisp[8192];  /* {"kind":"enum","labels":[...]} */
static constexpr int kMaxDrop = 96;               /* dropdown cap (disp size bound) */

/* Custom HostLink component: a file-name dropdown in the browser. Picking an
 * entry writes the index live; the main loop loads that file. */
struct SampleSel : Serializable
{
    uint8_t       idx   = 0;
    volatile bool dirty = false;
    size_t   SerializedSize() const override { return 1u; }
    void     Serialize(uint8_t* o) const override { o[0] = idx; }
    bool     Deserialize(const uint8_t* in) override
    {
        if(in[0] != idx) { idx = in[0]; dirty = true; }
        return true;
    }
    uint32_t SchemaHash() const override { return 0x53414D50u; /* 'SAMP' */ }
    bool     Describe(hostlink::ComponentWriter& w) const override
    {
        int n = gNumFiles < kMaxDrop ? gNumFiles : kMaxDrop;
        if(n < 1) return true;
        w.Label("Sample");
        return w.Field("sample.idx", "File", 0, hostlink::FieldType::Enum,
                       0.f, gSampleDisp, (uint16_t)n);
    }
};
static SampleSel gSample;

/* Browser dropdown: which classifier labels the steps. Switching re-labels
 * the PLAYING material live (gates + ring colors) without regenerating the
 * pattern, so the lenses can be A/B'd on the same break. */
static const char* kClassDisp =
    "{\"kind\":\"enum\",\"labels\":[\"Hockman 3-band\",\"Legacy 120/900 (hi/lo)\",\"Two-band lo/hi\"]}";
struct ClassSel
{
    uint8_t       mode  = 0;
    volatile bool dirty = false;
};
static ClassSel      gClass;
static uint8_t       gReclassBuf[bb::Engine::kMaxSlices];
static volatile bool gReclass = false;   /* control: labels ready → audio memcpy */

/* Browser dropdown: external clock PPQN on J3 (all divide the 96-PPQN NCO). */
static const uint32_t kPpqnTable[4] = {24u, 4u, 2u, 1u};
static const char* kPpqnDisp =
    "{\"kind\":\"enum\",\"labels\":[\"24 PPQN (MIDI/DIN)\",\"4 PPQN (16ths)\",\"2 PPQN (8ths)\",\"1 PPQN (quarters)\"]}";
struct ClockSel
{
    uint8_t       sel   = 0;
    volatile bool dirty = false;
};
static ClockSel gClockSel;

/* ── Voice (M3): J4 v/oct + J5 gate → the MONO keymap (spec §6.3) ──────
 * Each gate rising edge samples v/oct once, quantises with note-CENTER
 * windows (boundaries are the knife-edge bug class), and dispatches by
 * range relative to 0 V: 0 = mute toggle · 1 = gap · 2..11 = latched
 * modifier keyswitches · 12..23 reserved · 24..39 = sixteen slice keys
 * spread across the loop. Replace-don't-layer; CHOKE vs PUNCH dropdown. */
static constexpr uint8_t kVoiceVoctChan = 1u;  /* J4 */
static constexpr uint8_t kVoiceGateChan = 2u;  /* J5 */
enum : uint32_t { kModRev = 1u << 0, kModHalf = 1u << 1, kModDouble = 1u << 2,
                  kModPunch = 1u << 3, kModRat = 1u << 4 };
static uint32_t       gMods      = 0u;    /* latched keyswitch state */
static int            gLastNote  = -1;
static volatile bool  gCallReq   = false; /* control → audio handshake */
static volatile int   gCallIdx   = 0;
static volatile float gCallMul   = 1.f;
static volatile bool  gCallRev = false, gCallPunch = false, gCallRat = false;
static volatile int   gCallFlash = -1;    /* ring: called step flashes */
static volatile int   gCallFlashT = 0;
static const char* kVoiceDisp =
    "{\"kind\":\"enum\",\"labels\":[\"Choke (momentary)\",\"Punch (writes the bar)\"]}";
struct VoiceSel
{
    uint8_t sel = 0;
};
static VoiceSel gVoiceSel;

/* ── musical branch: gate shaping dropdowns (read live by audio/poll) ── */
static const char* kGateLenDisp =
    "{\"kind\":\"enum\",\"labels\":[\"6 ms trig\",\"20 ms\",\"50 ms\",\"Hold to next hit\"]}";
struct GateLenSel
{
    uint8_t sel = 0;
};
static GateLenSel gGateLen;

static const char* kGateLvlDisp =
    "{\"kind\":\"enum\",\"labels\":[\"Fixed 5 V\",\"Velocity (1.5–5 V)\"]}";
struct GateLvlSel
{
    uint8_t sel = 0;
};
static GateLvlSel gGateLvlSel;
/* ── GRAMMAR (jungle branch) ────────────────────────────────────────────
 * Browser dropdown: which pattern grammar the engine speaks. Classic is the
 * shipped BreakFix generator (byte-identical); Collins adds the Chop Codex
 * upgrades (position-weighted moves, odd cuts, fill + snap-back, stutter
 * terminator, geometric rolls, repeat repitch). Switching rerolls the fill
 * so it is audible immediately. Hardware tie-in later. */
static const char* kGrammarDisp =
    "{\"kind\":\"enum\",\"labels\":[\"Classic\",\"Collins\",\"Grids\",\"Euclid\"]}";
struct GrammarSel
{
    uint8_t       mode  = 0;
    volatile bool dirty = false;
};
static GrammarSel gGrammar;

/* ── Options: ONE preset-managed component for the six one-byte selectors.
 * The SDK caps managed components at kPresetMaxManaged (8); six separate
 * Serializables plus pager/sample/locks/settings would blow it. Same
 * labels, same live semantics: the browser edits bytes 0..5, Deserialize
 * fans them out to the plain structs above and raises their dirty flags. */
struct Options : Serializable
{
    size_t SerializedSize() const override { return 6u; }
    void   Serialize(uint8_t* o) const override
    {
        o[0] = gClass.mode; o[1] = gClockSel.sel; o[2] = gVoiceSel.sel;
        o[3] = gGateLen.sel; o[4] = gGateLvlSel.sel; o[5] = gGrammar.mode;
    }
    bool Deserialize(const uint8_t* in) override
    {
        uint8_t v;
        v = in[0] > 2 ? 0 : in[0]; if(v != gClass.mode)    { gClass.mode = v;    gClass.dirty = true; }
        v = in[1] > 3 ? 0 : in[1]; if(v != gClockSel.sel)  { gClockSel.sel = v;  gClockSel.dirty = true; }
        gVoiceSel.sel   = in[2] > 1 ? 0 : in[2];
        gGateLen.sel    = in[3] > 3 ? 0 : in[3];
        gGateLvlSel.sel = in[4] > 1 ? 0 : in[4];
        v = in[5] > 3 ? 0 : in[5]; if(v != gGrammar.mode)  { gGrammar.mode = v;  gGrammar.dirty = true; }
        return true;
    }
    uint32_t SchemaHash() const override { return 0x4F505431u; /* 'OPT1' */ }
    bool     Describe(hostlink::ComponentWriter& w) const override
    {
        using hostlink::FieldType;
        w.Label("Options");
        bool ok = w.Field("class.mode",   "Classifier",  0, FieldType::Enum, 0.f, kClassDisp,   3u);
        ok &=     w.Field("clock.ppqn",   "Clock In",    1, FieldType::Enum, 0.f, kPpqnDisp,    4u);
        ok &=     w.Field("voice.mode",   "Voice",       2, FieldType::Enum, 0.f, kVoiceDisp,   2u);
        ok &=     w.Field("gate.len",     "Gate Length", 3, FieldType::Enum, 0.f, kGateLenDisp, 4u);
        ok &=     w.Field("gate.lvl",     "Gate Level",  4, FieldType::Enum, 0.f, kGateLvlDisp, 2u);
        ok &=     w.Field("grammar.mode", "Grammar",     5, FieldType::Enum, 0.f, kGrammarDisp, 4u);
        return ok;
    }
};
static Options gOptions;
/* ── end GRAMMAR ───────────────────────────────────────────────────────── */
static volatile float gClkFlash = 0.f;   /* B1 white flash per detected pulse */
static volatile float gClkMult  = 1.f;   /* P2 while following: ÷4 ÷2 ×1 ×2 ×4 */
static bool           gWasExt   = false; /* ext/internal transition tracking */
/* NCO rate snapshot taken on the CONTROL thread (same thread as the PLL's
 * SetTicksPerUs, so no torn double read) and consumed by the audio thread as
 * an atomic float: the engine's varispeed rate is derived from it every block
 * so slice playback advances EXACTLY as fast as the phase does. */
static volatile float gTpu      = 0.f;
static double         gLastLp   = -1.;   /* phase guard memory; -1 = re-arm */
static float          gP2ExtRef = -1.f;  /* P2 while following: engage on move */
static bool           gP2ExtEng = false;
static int            gMultZone = 2;     /* 0..4 → ÷4 ÷2 ×1 ×2 ×4 (hysteresis) */

static constexpr uint8_t P_CHOP  = kPotTopLeft;
static constexpr uint8_t P_TEMPO = kPotTopRight;
static constexpr uint8_t P_HIST  = kPotMiddleLeft;
static constexpr uint8_t P_PITCH = kPotMiddleRight;
static constexpr uint8_t P_SPACE = kPotBottomLeft;
static constexpr uint8_t P_VOL   = kPotBottomRight;

/* ── declared control surface (drives the browser editor + descriptor) ─ */
static constexpr LedPanel::Rgb kChopCol {235, 120, 20};
static constexpr LedPanel::Rgb kTempoCol{ 40, 160, 190};
static constexpr LedPanel::Rgb kSpaceCol{120,  55, 190};
static constexpr LedPanel::Rgb kVolCol  {150, 225, 235};
static constexpr LedPanel::Rgb kPitchP  { 40, 200, 120};
static constexpr LedPanel::Rgb kPitchN  {235,  70,  20};
static constexpr LedPanel::Rgb kPitchC  { 60,  60,  70};

VirtualKnob k_chop  = VirtualKnob(P_CHOP,  "Chop").Linear(0.f, 1.f)
                                 .Ident("chop").Ring(Level(kChopCol));
VirtualKnob k_tempo = VirtualKnob(P_TEMPO, "Tempo").Linear(70.f, 180.f)
                                 .Unit("BPM").Ident("tempo").Ring(Level(kTempoCol));
VirtualKnob k_hist  = VirtualKnob(P_HIST,  "History").Linear(0.f, 1.f)
                                 .Ident("history").Ring(Level(LedPanel::Rgb{120, 55, 190}));
VirtualKnob k_pitch = VirtualKnob(P_PITCH, "Pitch").Linear(-12.f, 12.f)
                                 .Unit("st").Ident("pitch")
                                 .Ring(Bipolar(kPitchP, kPitchN, kPitchC));
VirtualKnob k_sample = VirtualKnob(P_SPACE, "Sample").Linear(0.f, 1.f)
                                 .Ident("sample").Ring(Level(kSpaceCol));
VirtualKnob k_vol   = VirtualKnob(P_VOL,   "Volume").Linear(0.f, 1.f)
                                 .Ident("volume").Ring(Level(kVolCol));

Page perform = Page(0).Name("Perform").Color("#eb7414")
                          .Knobs(k_chop, k_tempo, k_hist, k_pitch, k_sample, k_vol);

using Rgb = LedPanel::Rgb;
static constexpr Rgb kKick  {235,  70,  20};
static constexpr Rgb kSnare { 40, 200, 120};
static constexpr Rgb kHat   {230, 200,  40};
static constexpr Rgb kOther { 60, 120, 230};
static constexpr Rgb kRest  { 10,   6,   3};
static constexpr Rgb kPlay  {255, 255, 255};
static constexpr Rgb kDim   { 24, 100, 120};
static constexpr Rgb kDimMid{ 50, 160, 185};
static constexpr Rgb kDimHi {150, 225, 235};
static constexpr Rgb kHistLo { 70,  30, 120};
static constexpr Rgb kHistMid{120,  55, 190};
static constexpr Rgb kHistHi {190, 130, 240};

static inline Rgb ClassColor(uint8_t c)
{
    switch(c)
    {
        case 0: return kKick;
        case 1: return kSnare;
        case 2: return kHat;
        default: return kOther;
    }
}

/* ── Brain page (M4): the pattern-language console. Grids reads Map X/Y +
 * three densities (Mutable's map, verbatim); Euclid reads densities as
 * hits-per-bar + Rotation. Classic/Collins ignore these. B1 switches pages. */
VirtualKnob b_x     = VirtualKnob(kPotTopLeft,     "Map X").Linear(0.f, 1.f).Ident("lang.x").Ring(Level(kTempoCol));
VirtualKnob b_y     = VirtualKnob(kPotTopRight,    "Map Y").Linear(0.f, 1.f).Ident("lang.y").Ring(Level(kTempoCol));
VirtualKnob b_kick  = VirtualKnob(kPotMiddleLeft,  "Kick").Linear(0.f, 1.f).Ident("lang.kick").Ring(Level(kKick));
VirtualKnob b_snare = VirtualKnob(kPotMiddleRight, "Snare").Linear(0.f, 1.f).Ident("lang.snare").Ring(Level(kSnare));
VirtualKnob b_hat   = VirtualKnob(kPotBottomLeft,  "Hat").Linear(0.f, 1.f).Ident("lang.hat").Ring(Level(kHat));
VirtualKnob b_rot   = VirtualKnob(kPotBottomRight, "Rotation").Linear(0.f, 1.f).Ident("lang.rot").Ring(Level(kSpaceCol));
Page brain = Page(1).Name("Brain").Color("#3a9fe0")
                 .Knobs(b_x, b_y, b_kick, b_snare, b_hat, b_rot);

/* ── class analysis over the engine's grid (mirrors its math) ───────── */
/* Classifier modes (browser dropdown "Classifier"; hardware tie-in later):
 *  0 Hockman 3-band  — kick <325 Hz, snare 325–4000 Hz, hat >4000 Hz
 *  1 Legacy 120/900  — the original split: snares mostly land in "hat", so
 *                      it behaves as hi/lo — musically its own lens (Amen!)
 *  2 Two-band lo/hi  — kick <325 Hz vs everything else, no snare class */
static void ClassifySteps(const float* x, int len, uint8_t* cls, int mode,
                          float* sal = nullptr)
{
    float dur = (float)len / kSr;
    int   bars = 1;
    while(dur / (float)bars >= 3.2f && bars < 16) bars <<= 1;
    int nsl = bars * 16;
    while(nsl > bb::Engine::kMaxSlices) nsl /= 2;
    if(nsl < 1) nsl = 1;
    int   step = len / nsl;
    float lpK = 0.f, lpM = 0.f;
    const float fK = (mode == 1) ? 120.f : 325.f;
    const float fM = (mode == 1) ? 900.f : 4000.f;
    float aK = 1.f - expf(-2.f * 3.14159f * fK / kSr);
    float aM = 1.f - expf(-2.f * 3.14159f * fM / kSr);
    static float eL[bb::Engine::kMaxSlices], eM[bb::Engine::kMaxSlices],
        eH[bb::Engine::kMaxSlices];
    for(int s = 0; s < nsl; s++)
    {
        const float* p = x + s * step;
        int n = (s == nsl - 1) ? len - s * step : step;
        double sl = 0, sm = 0, sh = 0;
        for(int i = 0; i < n; i++)
        {
            float v = p[i];
            lpK += aK * (v - lpK);
            lpM += aM * (v - lpM);
            float mid = lpM - lpK, hi = v - lpM;
            sl += lpK * lpK; sm += mid * mid; sh += hi * hi;
        }
        eL[s] = (float)(sl / n); eM[s] = (float)(sm / n); eH[s] = (float)(sh / n);
    }
    float mL = 1e-12f, mM = 1e-12f, mH = 1e-12f;
    for(int s = 0; s < nsl; s++) { mL += eL[s]; mM += eM[s]; mH += eH[s]; }
    if(sal) /* salience: loudness of the step relative to the loop's loudest */
    {
        float mx = 1e-12f;
        for(int s = 0; s < nsl; s++) { float e = sqrtf(eL[s] + eM[s] + eH[s]); if(e > mx) mx = e; }
        for(int s = 0; s < bb::Engine::kMaxSlices; s++)
            sal[s] = s < nsl ? sqrtf(eL[s] + eM[s] + eH[s]) / mx : 0.f;
    }
    mL /= nsl; mM /= nsl; mH /= nsl;
    for(int s = 0; s < nsl; s++)
    {
        float l = eL[s] / mL, m = eM[s] / mM, h = eH[s] / mH;
        if(eL[s] + eM[s] + eH[s] < 1e-7f) cls[s] = 3;
        else if(mode == 2)                  cls[s] = (l >= (m > h ? m : h)) ? 0 : 2;
        else                                cls[s] = (l >= m && l >= h) ? 0 : (m >= h ? 1 : 2);
    }
}

/* Per-slice min/max envelope over the engine's grid (same bar rule). */
static void ComputeEnvelope(const float* x, int len, uint8_t env[][kEnvPts][2])
{
    float dur = (float)len / kSr;
    int   bars = 1;
    while(dur / (float)bars >= 3.2f && bars < 16) bars <<= 1;
    int nsl = bars * 16;
    while(nsl > bb::Engine::kMaxSlices) nsl /= 2;
    if(nsl < 1) nsl = 1;
    memset(env, 128, (size_t)bb::Engine::kMaxSlices * kEnvPts * 2);
    for(int s = 0; s < nsl; s++)
    {
        const int a = (int)((long long)len * s / nsl), b = (int)((long long)len * (s + 1) / nsl);
        for(int p = 0; p < kEnvPts; p++)
        {
            const int pa = a + (b - a) * p / kEnvPts, pb = a + (b - a) * (p + 1) / kEnvPts;
            float mn = 0.f, mx = 0.f;
            for(int i = pa; i < pb; i++) { float v = x[i]; if(v < mn) mn = v; if(v > mx) mx = v; }
            int lo = (int)(mn * 127.f) + 128, hi = (int)(mx * 127.f) + 128;
            env[s][p][0] = (uint8_t)(lo < 0 ? 0 : lo > 255 ? 255 : lo);
            env[s][p][1] = (uint8_t)(hi < 0 ? 0 : hi > 255 ? 255 : hi);
        }
    }
}

/* ── audio ──────────────────────────────────────────────────────────── */
static float gMono[kScratch];

static void AudioCb(daisy::AudioHandle::InputBuffer  /*in*/,
                    daisy::AudioHandle::OutputBuffer out,
                    size_t                           size)
{
    /* new material ready: swap buffers and re-point the engine (SetMaterial
     * is audio-thread work here exactly as it was on the Pod). */
    if(gApply)
    {
        float* t = gPlay; gPlay = gLoad; gLoad = t;
        engine.SetMaterial(gPlay, gPendLen, kSr, gPendOnsets, gPendN, gPendClass);
        memcpy(gSal, gPendSal, sizeof gSal);
        memcpy(gEnv, gPendEnv, sizeof gEnv);
        gMatGen = (uint8_t)(gMatGen + 1u);
        gNeedNativeTempo = true;
        gLastLp = -1.;   /* new nSlices → new phase geometry; do not hold */
        gApply = false;
    }

    if(gPendReslice) { engine.Reslice();   gPendReslice = false; }
    if(gPendReroll)  { engine.RerollFill(); gPendReroll  = false; }
    if(gPendRegen)   { engine.RegenPattern(); gPendRegen = false; }
    if(gScrubReq)    { engine.Scrub(gScrubDir); gScrubReq = false; }
    if(gReclass)     { memcpy(engine.stepClass, gReclassBuf, sizeof gReclassBuf); memcpy(gSal, gReclassSal, sizeof gSal); gReclass = false; }
    if(gCallReq)     { engine.CallSlice(gCallIdx, gCallMul, gCallRev, gCallPunch, gCallRat); gCallReq = false; }

    int n = (int)size;
    if(n > kScratch) n = kScratch;

    /* Drive the engine playhead from the musical clock (internal or followed).
     * Position comes from the single monotonic MasterTick counter (+ FracTick),
     * NOT Bar()/BarPhase() read separately: those tore at bar boundaries under
     * the 1 ms tick and fired a spurious last-step ("skips"). One 16th = Ppqn/4
     * ticks; the loop = nSlices 16ths. gClkMult scales the rate when P2 acts
     * as a clock multiplier/divider while following. */
    engine.extPhase = true;
    {
        const float  mult  = gClkMult;
        const double tick  = ((double)gClock.MasterTick() + gClock.FracTick())
                             * (double)mult;
        const double loopT = (double)engine.nSlices * ((double)gClock.Ppqn() / 4.0);
        double lp = loopT > 0. ? fmod(tick, loopT) / loopT : 0.;

        /* RATE == PHASE ADVANCE, exactly. barSeconds feeds the varispeed rate
         * (and the ratchet countdown) in the engine; deriving it here, per
         * block, from the very ticks/µs the NCO integrates means a verbatim
         * run's voice position never drifts from the step grid. (SetBpm from
         * the control thread was integer-quantised while the PLL's rate is
         * continuous — ~20 samples/step of drift at 0.4 BPM, blowing the
         * 160-sample continuity window every ~8 steps: the ext-clock stutter.)
         * tpu == 0 while the follower acquires lock: phase is frozen and no
         * step can fire, so the previous barSeconds is simply left alone. */
        const float tpu = gTpu;
        if(tpu > 0.f && loopT > 0.)
            engine.barSeconds = (float)(loopT / ((double)tpu * 1e6 * (double)mult));

        /* Monotonic guard, half-a-step wide: a torn MasterTick/FracTick read
         * is at most ±1 tick (1/24 step) and must not fire a backward step
         * (double trigger + a bogus flow-reroll). Anything larger backward is
         * a legitimate jump (new material, multiplier zone, transport reset)
         * and must pass immediately — a 0.5-loop guard held the playhead
         * silent for up to half a loop after those. */
        static float s_lastMult = 1.f;
        if(mult != s_lastMult) { s_lastMult = mult; gLastLp = -1.; }
        const double thr = 0.5 / (double)(engine.nSlices > 0 ? engine.nSlices : 1);
        if(gLastLp >= 0. && lp < gLastLp && (gLastLp - lp) < thr) lp = gLastLp;
        gLastLp = lp;
        engine.barPhase = lp;
    }

    engine.Process(gMono, n);

    /* per-class trigger bitmask → ARM gate countdowns (sample-accurate).
     * The CV writes themselves happen in OnPoll, never here: J3–J6 are
     * MCP4728 channels and CvJack::SetVolts on them is synchronous I²C
     * (WriteAll + PulseLdac through the PCA9557). Blocking I²C inside the
     * audio ISR deadlocks the module on the first callback — the v0.3.0
     * boot hang. J7/J8 are STM DACs but stay on the same path for uniformity. */
    uint8_t gf = engine.gateFired; engine.gateFired = 0;
    const int  glen = kGateLenSamps[gGateLen.sel & 3];
    const bool vel  = gGateLvlSel.sel == 1;
    for(int c = 0; c < 3; c++)
    {
        if(gf & (1u << c))
        {
            if(gGateCd[c] > 0 && glen == kGateHold) gGateRetrig[c] = true; /* re-hit while held */
            gGateCd[c] = glen;
            int sl = engine.gateSlice[c];
            float sa = (sl >= 0 && sl < bb::Engine::kMaxSlices) ? gSal[sl] : 1.f;
            gGateLvl[c] = vel ? 1.5f + 3.5f * sa : 5.f;
        }
        else if(gGateCd[c] > 0 && glen != kGateHold) gGateCd[c] -= n;
    }

    const float v = gVol, send = gVerbSend;
    for(int i = 0; i < n; i++)
    {
        float d = gMono[i];
        float wl, wr;
        gVerb.Process(d * send, d * send, &wl, &wr);
        float l = (d + wl * 0.85f) * v;
        float r = (d + wr * 0.85f) * v;
        if(l > 1.f) l = 1.f; else if(l < -1.f) l = -1.f;
        if(r > 1.f) r = 1.f; else if(r < -1.f) r = -1.f;
        out[0][i] = l;
        out[1][i] = r;
        if((i & 3) == 0) { gScope[gScopeW & 1023u] = (int8_t)(l * 127.f); gScopeW = (uint16_t)(gScopeW + 1u); }
    }
}

/* ── control frame ──────────────────────────────────────────────────── */
static void OnFrame()
{
    float pk = k_chop.Norm();
    engine.patternKnob = pk;
    gVerbSend = pk < 0.22f ? (0.22f - pk) / 0.22f * 0.75f : 0.f;

    /* Varispeed follows the musical clock: snapshot the NCO rate here (same
     * thread as the PLL writes it — no torn double) for the audio thread to
     * derive barSeconds from every block. engine.bpm is telemetry only now;
     * SetBpm's integer-quantised barSeconds must NOT be written from here. */
    gTpu       = (float)gClock.TicksPerUs();
    engine.bpm = gClock.Bpm() * gClkMult;

    engine.pitch = exp2f(k_pitch.Value() / 12.f);   /* ±12 semitones */
    gVol         = k_vol.Norm();

    /* P5 highlights a file (Selector value = index once labelled; Norm
     * fallback before the card is scanned). Loading is on B3-hold. */
    if(gNumFiles > 0)
    {
        int idx = (int)(k_sample.Value() + 0.5f);
        if(idx < 0) idx = 0;
        if(idx > gNumFiles - 1) idx = gNumFiles - 1;
        gSelFile = idx;
    }

    /* Brain page → language params; a change re-generates the pattern with
     * the SAME seeds (Grids/Euclid are deterministic from these). */
    {
        static float s_l[6] = {-1.f, -1.f, -1.f, -1.f, -1.f, -1.f};
        const float  l[6]   = { b_x.Norm(), b_y.Norm(), b_kick.Norm(),
                                b_snare.Norm(), b_hat.Norm(), b_rot.Norm() };
        bool ch = false;
        for(int i = 0; i < 6; i++) if(fabsf(l[i] - s_l[i]) > 0.01f) { s_l[i] = l[i]; ch = true; }
        engine.langX = l[0]; engine.langY = l[1];
        engine.langDens[0] = l[2]; engine.langDens[1] = l[3]; engine.langDens[2] = l[4];
        engine.langRot = l[5];
        if(ch && gGrammar.mode >= 2 && !gPendReroll) gPendRegen = true;
    }

    float p3 = k_hist.Norm();
    if(engine.histN > 1 && (p3 > gLastP3 + 0.02f || p3 < gLastP3 - 0.02f))
    {
        int cur = (engine.histHead - engine.histPos + bb::Engine::kHist)
                  % bb::Engine::kHist;
        int tgt = (int)((1.f - p3) * (float)(engine.histN - 1) + 0.5f);
        gScrubDir = cur - tgt;
        gScrubReq = true;
        gLastP3   = p3;
    }
}

static void OnPoll(uint32_t /*t_ms*/)
{
    if(gNeedNativeTempo)
    {
        gNeedNativeTempo = false;
        gClock.SetBpm(engine.NativeBpm());
        gP2Ref = -1e9f;   /* re-latch: P2 re-engages on movement */
    }

    /* Clock pipeline (spec §7): edge intake → follower → NCO tick. */
    const uint32_t now_us = daisy::System::GetUs();
    gClkEdge.Tick(loop.Cv(), loop.NumCv(), now_us);
    const bool rose = gClkEdge.JustRose(kClockChan);
    if(rose) { gLastPulseUs = now_us; gClkFlash = 1.f; }
    const bool ext = gLastPulseUs && (now_us - gLastPulseUs) < 2000000u;
    static float gPreExtBpm = 120.f;
    if(ext && !gWasExt)
    {
        gPreExtBpm = gClock.Bpm();                     /* tempo to fall back on */
        gFollower.Enable(kPpqnTable[gClockSel.sel]);   /* arm the PLL (stops the
                                                          NCO until 3 pulses lock) */
        gP2ExtRef = -1.f; gP2ExtEng = false; gMultZone = 2; gClkMult = 1.f;
    }
    if(!ext && gWasExt)
    {
        /* Clock gone: the follower had flagged the NCO as STEERED, which makes
         * SetBpm a no-op — un-steer or P2 can never take the clock back. Keep
         * the freewheeled tempo until P2 moves (re-latch), like a fresh load. */
        gFollower.Disable();
        gClock.SetSteered(false);
        /* Lost BEFORE lock (cable bounce, master stopped within the first
         * three pulses): Enable() had stopped the NCO at rate 0 and only a
         * lock restarts it — Disable() does not. Restart at the pre-clock
         * tempo or the module stays silent until reboot (review finding). */
        if(!gClock.Running() || gClock.TicksPerUs() <= 0.)
        {
            gClock.SetBpm(gPreExtBpm);
            gClock.Start();
        }
        gP2Ref   = -1e9f;
        gClkMult = 1.f;
    }
    gWasExt = ext;

    if(ext)
    {
        if(rose) gFollower.OnPulse(gClkEdge.LastRiseUs(kClockChan));
        gFollower.Update(now_us);
        /* Spec §6.4: while following, P2 is a clock multiplier/divider around
         * the followed tempo — wide ×1 detent in the middle. It ENGAGES only
         * once the knob moves (an uncaught/stored P2 at boot otherwise lands
         * on ÷4 and the loop crawls), and zones carry ±0.02 hysteresis so pot
         * noise on an edge can't flip the multiplier back and forth. */
        const float p = k_tempo.Norm();
        if(gP2ExtRef < 0.f) gP2ExtRef = p;
        if(!gP2ExtEng && (p > gP2ExtRef + 0.03f || p < gP2ExtRef - 0.03f))
            gP2ExtEng = true;
        if(gP2ExtEng)
        {
            static const float kB[4]  = {0.15f, 0.35f, 0.65f, 0.85f};
            static const float kM[5]  = {0.25f, 0.5f, 1.f, 2.f, 4.f};
            int z = gMultZone;
            while(z < 4 && p > kB[z] + 0.02f)     z++;
            while(z > 0 && p < kB[z - 1] - 0.02f) z--;
            gMultZone = z;
            gClkMult  = kM[z];
        }
    }
    else
    {
        gClkMult = 1.f;
        /* Internal tempo from P2, engaging only once the knob moves (so a
         * fresh load keeps playing at its native tempo). Native detent. */
        float pv = k_tempo.Value();
        if(gP2Ref < -1e8f) gP2Ref = pv;
        if(pv > gP2Ref + 1.f || pv < gP2Ref - 1.f)
        {
            float nb = engine.NativeBpm();
            float b  = (pv > nb - 2.f && pv < nb + 2.f) ? nb : (float)((int)(pv + 0.5f));
            gClock.SetBpm(b);
        }
    }
    gClock.Tick(now_us);

    /* Voice keymap: sample v/oct at each gate edge (MONO, latched mods). */
    if(gClkEdge.JustRose(kVoiceGateChan))
    {
        const float nf = hw.cv[kVoiceVoctChan].Volts() * 12.f;
        int n = (int)floorf(nf + 0.5f);
        if(fabsf(nf - (float)n) < 0.4f) gLastNote = n;   /* inside the note-center window */
        else n = gLastNote;                                /* on a boundary: keep last */
        if(n == 0)                       engine.muted = !engine.muted;
        else if(n >= 2 && n <= 11)       gMods ^= (1u << (n - 2));
        else if(n >= 24 && n <= 39 && engine.nSlices > 0)
        {
            const int k = n - 24;
            gCallIdx   = (k * engine.nSlices) / 16;      /* 16 keys span the loop */
            gCallMul   = ((gMods & kModHalf) ? 0.5f : 1.f) * ((gMods & kModDouble) ? 2.f : 1.f);
            gCallRev   = (gMods & kModRev) != 0;
            gCallPunch = ((gVoiceSel.sel == 1) != ((gMods & kModPunch) != 0));
            gCallRat   = (gMods & kModRat) != 0;
            gCallFlash = gCallIdx; gCallFlashT = 6;
            gCallReq   = true;
        }
    }

    /* Gate CV writes — control thread (I²C-safe), on CHANGE only so J6's
     * MCP4728 isn't hit with a WriteAll+LDAC transaction every millisecond. */
    static float s_lastV[3] = {-1.f, -1.f, -1.f};
    for(int c = 0; c < 3; c++)
    {
        float v = gGateCd[c] > 0 ? gGateLvl[c] : 0.f;
        if(gGateRetrig[c]) { v = 0.f; gGateRetrig[c] = false; } /* 1 ms low, then back up */
        if(v != s_lastV[c])
        {
            s_lastV[c] = v;
            if(c == 0)      hw.j6.SetVolts(v);
            else if(c == 1) hw.j7.SetVolts(v);
            else            hw.j8.SetVolts(v);
        }
    }

    IButton& b2 = hw.buttons[kButtonB2];
    IButton& b3 = hw.buttons[kButtonB3];
    /* B1 belongs to the Pager + ParamLock. B2+B3 together is the Settings
     * chord: while both are down (or Settings is open) none of the single-
     * button gestures may fire, and the releases that end the chord must
     * not read as taps. */
    static bool s_chord       = false;
    static bool s_wasSettings = false;
    if(b2.Pressed() && b3.Pressed()) s_chord = true;
    if(!b2.Pressed() && !b3.Pressed()) s_chord = false;
    /* Settings leaves on the RISING edge of B2 or B3 (single button, so no
     * chord) and IsActive() is already false by the time we run — without
     * this latch the exit tap reads as dice / reslice / a hold-load. Stay
     * quiet until both buttons are up again. */
    if(settings.IsActive()) s_wasSettings = true;
    if(s_wasSettings && !b2.Pressed() && !b3.Pressed()) s_wasSettings = false;
    if(s_chord || settings.IsActive() || s_wasSettings)
    {
        if(gFlowing) { gFlowing = false; engine.flowRegen = false; }
        return;
    }

    /* B2: tap = refill (dice), hold = flow. */
    if(b2.Pressed() && !gFlowing && b2.TimeHeldMs() > 300.f)
    {
        gFlowing         = true;
        engine.flowRegen = true;
    }
    if(b2.FallingEdge())
    {
        if(gFlowing) { gFlowing = false; engine.flowRegen = false; }
        else         { gPendReroll = true; }
    }

    /* B3: tap = reslice · hold = load next WAV on the card. */
    static bool s_b3_loaded = false;
    if(b3.Pressed() && b3.TimeHeldMs() > 400.f && !s_b3_loaded
       && gNumFiles > 0 && !gLoadReq && !gApply)
    {
        gLoadIdx    = gSelFile;
        gLoadReq    = true;
        s_b3_loaded = true;
    }
    if(b3.FallingEdge())
    {
        if(!s_b3_loaded) gPendReslice = true;
        s_b3_loaded = false;
    }
}

/* ── render ─────────────────────────────────────────────────────────── */
static void OnRender(uint32_t /*t_ms*/)
{
    if(settings.IsActive()) return;   /* the hook fires inside Settings too */
    const bool perfPage = pager.Page() == 0;   /* Brain page keeps its own arcs */
    int ns  = engine.nSlices > 0 ? engine.nSlices : 1;
    int cur = (int)(engine.barPhase * (double)ns);
    if(cur >= ns) cur = ns - 1;
    int base = (cur / 16) * 16;
    if(perfPage) for(int i = 0; i < 16; i++)
    {
        int s = base + i;
        Rgb c;
        if(s >= ns) c = kRest;
        else if(gCallFlashT > 0 && s == gCallFlash) c = Rgb{120, 160, 255}; /* voice call */
        else if(s == cur) c = kPlay;
        else { int8_t src = engine.stepSrc[s]; c = (src < 0) ? kRest : ClassColor(engine.stepClass[src]); }
        if(engine.muted) { c.r >>= 2; c.g >>= 2; c.b >>= 2; }   /* muted: dim the bar */
        hw.leds.SetRingByOffset(P_CHOP, i, c);
    }

    if(gCallFlashT > 0) gCallFlashT--;
    float hist_lvl = 1.f;
    if(engine.histN > 1)
    {
        int depth = (engine.histHead - engine.histPos + bb::Engine::kHist)
                    % bb::Engine::kHist;
        hist_lvl = (float)(engine.histN - 1 - depth) / (float)(engine.histN - 1);
    }
    if(perfPage) DrawLevelArc(hw.leds, P_HIST, kAlchemyLabArcGeometry, hist_lvl,
                 {0.5f, 0.99f}, kHistLo, kHistMid, kHistHi);
    /* P2/P4/P5/P6 arcs are drawn by the framework's per-knob Ring styles
     * (PerfRenderer, before this hook); we only overdraw P1 (bar) + P3. */

    /* B1: clock indicator — white flash per detected pulse, green base while
     * the follower is locked, amber pulse on sample load. */
    gLoadFlash *= 0.90f;
    gClkFlash  *= 0.80f;
    {
        const float lf = gLoadFlash, cf = gClkFlash;
        const bool  locked = gWasExt && gFollower.Locked();
        auto c8 = [](float v) { return (uint8_t)(v > 255.f ? 255.f : (v < 0.f ? 0.f : v)); };
        hw.leds.SetButtonPair(kButtonB1,
            Rgb{c8((locked ? 6.f : 20.f) + 200.f * lf + 220.f * cf),
                c8((locked ? 120.f : 12.f) + 120.f * lf + 220.f * cf),
                c8(4.f + 220.f * cf)});
    }
    hw.leds.SetButtonPair(kButtonB2, gFlowing ? Rgb{20, 160, 60} : Rgb{6, 10, 6});
    hw.leds.SetButtonPair(kButtonB3, Rgb{12, 6, 2});
}

/* ── SD: scan + load (main-loop only, off the audio thread) ─────────── */
static void ScanSD()
{
    gNumFiles = 0;
    if(f_opendir(&gDir, "0:/") != FR_OK) return;
    while(gNumFiles < kMaxFiles && f_readdir(&gDir, &gFno) == FR_OK && gFno.fname[0])
    {
        if(gFno.fattrib & AM_DIR) continue;
        const char* dot = strrchr(gFno.fname, '.');
        if(!dot || strcasecmp(dot, ".wav") != 0) continue;
        strncpy(gFiles[gNumFiles], gFno.fname, 127);
        gFiles[gNumFiles][127] = 0;
        gNumFiles++;
    }
    f_closedir(&gDir);
    for(int i = 1; i < gNumFiles; i++)
        for(int j = i; j > 0 && strcasecmp(gFiles[j - 1], gFiles[j]) > 0; j--)
        {
            char tmp[128];
            memcpy(tmp, gFiles[j - 1], 128);
            memcpy(gFiles[j - 1], gFiles[j], 128);
            memcpy(gFiles[j], tmp, 128);
        }
}

/* Runs between control ticks; does the heavy SD read → condition → classify
 * into gLoad, then hands off to the audio thread via gApply. */
static void ServiceLoad()
{
    if(!gLoadReq || gApply) return;
    gLoadReq = false;

    char path[140];
    path[0] = '0'; path[1] = ':'; path[2] = '/';
    strncpy(path + 3, gFiles[gLoadIdx], 136);
    path[139] = 0;

    int cues[kMaxOnsets], nCues = 0, err = 0;
    int len = bbwav::Load(path, gStage, kStageMax, gLoad, kBufMax, kSr,
                          kMaxSec, cues, &nCues, kMaxOnsets, &err);
    if(len <= 0) return;   /* leave current material playing on failure */

    len = bb::ConditionBuffer(gLoad, len, kSr, gDet);

    if(nCues >= 2)
    {
        for(int i = 1; i < nCues; i++)
            for(int j = i; j > 0 && cues[j - 1] > cues[j]; j--)
            { int t = cues[j - 1]; cues[j - 1] = cues[j]; cues[j] = t; }
        gPendN = 0;
        if(cues[0] > (int)(0.02f * kSr)) gPendOnsets[gPendN++] = 0;
        for(int i = 0; i < nCues && gPendN < bb::Engine::kMaxSlices; i++)
            gPendOnsets[gPendN++] = cues[i];
    }
    else
    {
        gPendN = gDet.Detect(gLoad, len, kSr, gPendOnsets, kMaxOnsets, nullptr);
    }

    ClassifySteps(gLoad, len, gPendClass, gClass.mode, gPendSal);
    ComputeEnvelope(gLoad, len, gPendEnv);
    gPendLen   = len;
    gCurFile   = gLoadIdx;
    gLoadFlash = 1.f;
    gApply     = true;     /* audio thread swaps + SetMaterial */
}

/* ── StateExt: the web frontend's window into the instrument ────────────
 * HostLink extension claiming 0x60–0x6F. All handlers run on the control
 * thread; engine arrays are read as-is (a torn read is one frame of pixels).
 *   0x60 GET_STATE  → u8 status, u8 nSlices, u8 curStep, u8 flags
 *                     (b0 muted b1 ext b2 locked b3 flowing b4 brainPage),
 *                     u8 matGen, u8 grammar, u8 classifier, u8 curFile,
 *                     u8 chop(0..255), u8 histN, u8 histDepth, u8 callFlash,
 *                     f32 bpm, f32 mult, i8 stepSrc[64], u8 stepClass[64]
 *   0x61 GET_ENV    ← u8 first, u8 count(≤30) → u8 status, u8 first, u8 n,
 *                     then n × (8 × {u8 min, u8 max})
 *   0x62 SCOPE      → u8 status, u16 n(256), i8 samples[256] (×4 decimated)
 *   0x63 ACTION     ← u8 op [,args]: 0 dice · 1 reslice · 2 mute toggle ·
 *                     3 punch(u8 step, u8 slice) · 4 flow(u8 on) ·
 *                     5 scrub(u8 depth) · 6 load(u8 fileIdx)      → u8 status */
struct StateExt : hostlink::IHostlinkExtension
{
    uint8_t FirstCmd() const override { return 0x60u; }
    uint8_t LastCmd()  const override { return 0x6Fu; }
    const char* DescriptorRootJson() const override
    {
        return "\"audiothurgist\":{\"ext\":1,\"state\":96,\"env\":97,\"scope\":98,\"action\":99}";
    }
    void Handle(const hostlink::ParsedFrame& f, hostlink::FrameWriter& w, uint32_t) override
    {
        switch(f.type)
        {
            case 0x60u:
            {
                w.U8(0u);
                const int ns = engine.nSlices;
                int cur = (int)(engine.barPhase * (double)(ns > 0 ? ns : 1)); if(cur >= ns) cur = ns > 0 ? ns - 1 : 0;
                uint8_t flags = (engine.muted ? 1u : 0u) | (gWasExt ? 2u : 0u)
                              | ((gWasExt && gFollower.Locked()) ? 4u : 0u)
                              | (gFlowing ? 8u : 0u) | (pager.Page() ? 16u : 0u);
                w.U8((uint8_t)ns); w.U8((uint8_t)cur); w.U8(flags); w.U8(gMatGen);
                w.U8(gGrammar.mode); w.U8(gClass.mode); w.U8((uint8_t)(gCurFile < 0 ? 255 : gCurFile));
                w.U8((uint8_t)(k_chop.Norm() * 255.f));
                int depth = engine.histN > 0 ? (engine.histHead - engine.histPos + bb::Engine::kHist) % bb::Engine::kHist : 0;
                w.U8((uint8_t)engine.histN); w.U8((uint8_t)depth);
                w.U8((uint8_t)(gCallFlashT > 0 ? (gCallFlash & 0x7F) : 255));
                float bpm = engine.bpm, mult = gClkMult; uint32_t u;
                memcpy(&u, &bpm, 4); w.U32(u); memcpy(&u, &mult, 4); w.U32(u);
                w.Bytes((const uint8_t*)engine.stepSrc, bb::Engine::kMaxSlices);
                w.Bytes(engine.stepClass, bb::Engine::kMaxSlices);
                return;
            }
            case 0x61u:
            {
                const uint8_t first = f.len >= 1 ? f.body[0] : 0u;
                uint8_t n = f.len >= 2 ? f.body[1] : 30u;
                if(first >= bb::Engine::kMaxSlices) { w.U8(2u); return; }   /* BAD_ARGS */
                if(n > 30u) n = 30u;
                if(first + n > bb::Engine::kMaxSlices) n = (uint8_t)(bb::Engine::kMaxSlices - first);
                w.U8(0u); w.U8(first); w.U8(n);
                w.Bytes(&gEnv[first][0][0], (size_t)n * kEnvPts * 2u);
                return;
            }
            case 0x62u:
            {
                uint8_t buf[256];
                const uint16_t wpos = gScopeW;
                for(int i = 0; i < 256; i++) buf[i] = (uint8_t)gScope[(wpos - 256 + i) & 1023u];
                w.U8(0u); w.U16(256u); w.Bytes(buf, 256u);
                return;
            }
            case 0x63u:
            {
                if(f.len < 1) { w.U8(2u); return; }   /* BAD_ARGS */
                const uint8_t op = f.body[0];
                switch(op)
                {
                    case 0: gPendReroll = true; break;
                    case 1: gPendReslice = true; break;
                    case 2: engine.muted = !engine.muted; break;
                    case 3:
                        if(f.len >= 3 && f.body[1] < engine.nSlices && f.body[2] < engine.nSlices)
                        {
                            const int st = f.body[1];
                            engine.stepSrc[st] = (int8_t)f.body[2]; engine.stepRev[st] = false;
                            engine.stepRatN[st] = 0; engine.stepRatAccel[st] = false; engine.stepJit[st] = 0;
                            gCallFlash = st; gCallFlashT = 6;
                        }
                        break;
                    case 4: gFlowing = f.len >= 2 && f.body[1]; engine.flowRegen = gFlowing; break;
                    case 5:
                        if(f.len >= 2 && engine.histN > 1)
                        {
                            int cur = (engine.histHead - engine.histPos + bb::Engine::kHist) % bb::Engine::kHist;
                            int tgt = f.body[1]; if(tgt > engine.histN - 1) tgt = engine.histN - 1;
                            gScrubDir = cur - tgt; gScrubReq = true;
                        }
                        break;
                    case 6:
                        if(f.len >= 2 && f.body[1] < gNumFiles && !gLoadReq && !gApply) { gLoadIdx = f.body[1]; gLoadReq = true; }
                        break;
                    default: w.U8(2u); return;   /* BAD_ARGS */
                }
                w.U8(0u);
                return;
            }
            default: w.U8(1u); return;   /* UNSUPPORTED */
        }
    }
};
static StateExt gStateExt;

int main()
{
    hw.Init();
    hw.j6.EnableCvOutput();   /* kick gate  */
    hw.j7.EnableCvOutput();   /* snare gate */
    hw.j8.EnableCvOutput();   /* hat gate   */
    sd.Init();
    sd.EnsureMounted(0);

    gVerb.Init(kSr);
    gVerb.SetFeedback(0.87f);
    gVerb.SetLpFreq(9000.f);

    engine.Init(0xA0D107);

    /* Boot material: first WAV on the card, else the synth break. */
    ScanSD();
    if(gNumFiles > 0)
    {
        uint8_t n = gNumFiles > 255 ? 255 : (uint8_t)gNumFiles;
        k_sample.Selector(n);

        /* Build the browser dropdown's enum-labels JSON from the card. */
        int drop = gNumFiles < kMaxDrop ? gNumFiles : kMaxDrop;
        char* w = gSampleDisp;
        char* end = gSampleDisp + sizeof(gSampleDisp) - 4;
        const char* head = "{\"kind\":\"enum\",\"labels\":[";
        while(*head && w < end) *w++ = *head++;
        for(int i = 0; i < drop && w < end - 140; i++)
        {
            if(i) *w++ = ',';
            *w++ = '"';
            for(const char* p = gFiles[i]; *p && w < end - 2; p++)
            {
                if(*p == '"' || *p == '\\') *w++ = '\\';
                *w++ = *p;
            }
            *w++ = '"';
        }
        *w++ = ']'; *w++ = '}'; *w = 0;
    }
    int  len = 0;
    bool loaded = false;
    if(gNumFiles > 0)
    {
        char path[140];
        path[0] = '0'; path[1] = ':'; path[2] = '/';
        strncpy(path + 3, gFiles[0], 136); path[139] = 0;
        int cues[kMaxOnsets], nCues = 0, err = 0;
        len = bbwav::Load(path, gStage, kStageMax, gPlay, kBufMax, kSr,
                          kMaxSec, cues, &nCues, kMaxOnsets, &err);
        if(len > 0)
        {
            len = bb::ConditionBuffer(gPlay, len, kSr, gDet);
            int no = (nCues >= 2) ? 0 : gDet.Detect(gPlay, len, kSr, gPendOnsets, kMaxOnsets, nullptr);
            if(nCues >= 2)
            {
                for(int i = 0; i < nCues && no < bb::Engine::kMaxSlices; i++)
                    gPendOnsets[no++] = cues[i];
            }
            static uint8_t cls[bb::Engine::kMaxSlices];
            ClassifySteps(gPlay, len, cls, gClass.mode, gSal);
            engine.SetMaterial(gPlay, len, kSr, gPendOnsets, no, cls);
            ComputeEnvelope(gPlay, len, gEnv);
            gCurFile = 0;
            loaded   = true;
        }
    }
    if(!loaded)
    {
        len = bb::MakeTestBreak(gPlay, kBufMax, kSr);
        static uint8_t cls[bb::Engine::kMaxSlices];
        int onsets[kMaxOnsets];
        int no = gDet.Detect(gPlay, len, kSr, onsets, kMaxOnsets, nullptr);
        ClassifySteps(gPlay, len, cls, gClass.mode, gSal);
        engine.SetMaterial(gPlay, len, kSr, onsets, no, cls);
        ComputeEnvelope(gPlay, len, gEnv);
    }

    gClock.SetBpm(engine.NativeBpm());
    gClock.Start();
    gFollower.SetLossPolicy(ClockLossPolicy::Freewheel);
    gFollower.SetAutoStart(true);
    /* NOT gFollower.Enable() here: Enable() does SetSteered(true) + Stop() +
     * SetTicksPerUs(0) while it waits for lock, and SetBpm is a no-op while
     * steered — arming at boot froze unpatched playback after one step
     * (v0.3.3/0.3.4). The PLL is armed on the internal→ext transition in
     * OnPoll, exactly when a clock actually appears. */
    gClkEdge.Init(kNumCvInputs);   /* edge detector needs Init (channels) — never called in v0.3.x */

    host.Extend(fs_ext);
    host.Extend(gStateExt);          /* web frontend: state / envelope / scope / actions */
    DescribeManual(host);            /* jacks + help text + the manual (Attach) */

    settings.UseBrightness();
    settings.UsePresets(presets);
    settings.Page(0).Name("Setup");
    presets.Manage(pager);
    presets.Manage(locks);
    if(gNumFiles > 0) presets.Manage(gSample);
    presets.Manage(gOptions);        /* classifier, clock PPQN, voice, gates, grammar */
    presets.Manage(settings);
    presets.UseNames();              /* renameable preset names, from the browser */

    loop.Use(pager).Use(locks).Use(settings).Use(perform).Use(brain).Use(host)
        .OnPoll(OnPoll).OnFrame(OnFrame).OnRender(OnRender);

    presets.Init();
    presets.BootLoad();   /* HostLink starts here: descriptor + panel USB up */

    hw.StartAudio(AudioCb);

    for(;;)
    {
        loop.Tick();
        if(gSample.dirty && !gLoadReq && !gApply)
        {
            gSample.dirty = false;
            gLoadIdx = gSample.idx < gNumFiles ? gSample.idx : 0;
            gLoadReq = true;
        }
        if(gClass.dirty && !gReclass && !gApply && engine.bufLen > 0)
        {
            gClass.dirty = false;
            ClassifySteps(gPlay, engine.bufLen, gReclassBuf, gClass.mode, gReclassSal);
            gReclass = true;
        }
        if(gClockSel.dirty)
        {
            gClockSel.dirty = false;
            gFollower.SetExtPpqn(kPpqnTable[gClockSel.sel]);
        }
        /* GRAMMAR (jungle branch): mode → engine, reroll so it's audible now */
        if(gGrammar.dirty && !gPendReroll)
        {
            gGrammar.dirty     = false;
            engine.patternMode = gGrammar.mode;
            gPendReroll        = true;
        }
        ServiceLoad();
    }
}
