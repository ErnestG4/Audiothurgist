/**
 * audiothurgist.cpp — M2: the Brain (clock sync + class gates).
 *
 * BreakFix engine on Alchemy Lab v2, now reading the SD card and blooming
 * reverb in the cluster zone. Boots playing the first WAV on the card (the
 * synth break if none); B1 walks the card. SD read + conditioning + class
 * analysis run off the audio thread; a double buffer swaps in the callback.
 * Record from J1/J2 (or the module's own mix): the RECORD fences — a key
 * take (J5 style 7) and a loop take into the break (Options → Record).
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
#include "alchemy/led/anims/catch_pip.h"
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
#include "bb_swing.h"      /* SLICE */
#include "bb_classify.h" /* uses capicola's Analyzer (Matthew Nielsen, AGPL) — THIRD_PARTY.md */
#include "bb_gates.h"    /* GATES: pure lane law (arm / poll / volts) */
#include "bb_bars.h"
#include "bb_testbreak.h"
#include "bb_clockmath.h" /* CLOCK: ClockMap, PhaseGuard, MultZone, JitterMeter (pure, host-tested) */
#include "bb_sample.h"   /* SAMPLE: TrimRegion, AnalyseRegion (a key take, control thread) */
#include "bb_record.h"   /* RECORD: record input, kit map, take length, markers → cues, TakeRecorder */
#include "bb_sets.h"     /* SETS: set naming, engine ↔ SetInfo, the load plan (docs/packages/sample-sets.md) */
#include <new>           /* SETS: placement-new for the NOLOAD SDRAM objects */

using namespace alchemy;

constexpr float kSr       = 48000.f;
constexpr int   kMaxSec   = 25;
constexpr int   kBufMax   = 48000 * kMaxSec;
constexpr int   kStageMax = 96000 * kMaxSec;   /* native-rate decode staging */
/* RECORD: a load (or a loop take) fills at most kLoadSec of a plane, so
 * RecArm always finds kRecSec of room past the material for takes into keys */
constexpr int   kRecSec   = 8;
constexpr int   kLoadSec  = kMaxSec - kRecSec;   /* 17 s */
constexpr int   kLoadMax  = 48000 * kLoadSec;
constexpr int   kScratch  = 128;
constexpr int   kMaxFiles = 256;
constexpr int   kMaxOnsets = 128;

/* ── SDRAM: material double buffer, decode staging, analysis, reverb ──── */
static float ALCHEMY_SDRAM_BSS gBufA[kBufMax];
static float ALCHEMY_SDRAM_BSS gBufB[kBufMax];
static float ALCHEMY_SDRAM_BSS gStage[kStageMax];
/* RECORD: R planes (Options → Sampling = Stereo) and the R decode staging — SDRAM */
static float ALCHEMY_SDRAM_BSS gBufAR[kBufMax];
static float ALCHEMY_SDRAM_BSS gBufBR[kBufMax];
static float ALCHEMY_SDRAM_BSS gStageR[kStageMax];
static bb::OnsetDetector ALCHEMY_SDRAM_BSS gDet;
static daisysp::ReverbSc ALCHEMY_SDRAM_BSS gVerb;
static char ALCHEMY_SDRAM_BSS  gFiles[kMaxFiles][128];
static int                     gNumFiles = 0;

/* FatFs scan objects — AXI SRAM (DMA-reachable), file scope. */
ALCHEMY_SDMMC_BSS alignas(32) static DIR     gDir;
ALCHEMY_SDMMC_BSS alignas(32) static FILINFO gFno;

static float* gPlay = gBufA;   /* engine reads this */
static float* gLoad = gBufB;   /* loader fills this */
static float* gPlayR = gBufAR; /* RECORD: the R planes swap with their L */
static float* gLoadR = gBufBR;

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
static constexpr int kGateHold = bb::kGateHold;      /* "hold to next hit" sentinel */
static const int kGateLenSamps[4] = { (int)(0.006f * kSr), (int)(0.020f * kSr),
                                      (int)(0.050f * kSr), kGateHold };
static volatile int   gGateCd[3]     = {0, 0, 0};    /* audio arms/decrements; poll reads */
static volatile float gGateLvl[3]    = {5.f, 5.f, 5.f}; /* volts for the current gate */
static volatile bool  gGateRetrig[3] = {false, false, false}; /* re-hit while high: one poll low */
static float          gSal[bb::Engine::kMaxSlices];        /* live per-slice salience 0..1 */
static float          gPendSal[bb::Engine::kMaxSlices];    /* staged with a load */
static float          gReclassSal[bb::Engine::kMaxSlices]; /* staged with a reclassify */
/* GATES */
static uint8_t        gPendBands[bb::Engine::kMaxSlices];  /* band mask staged with a load /
                                                              re-slice → engine.stepBands */
/* GATES end */
/* PLAY */
static float          gPendDens[bb::Engine::kMaxSlices];   /* keyframe density per slice, staged with a load / re-slice */
/* PLAY */

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
/* B1 is the page button AND the lock-record button. The SDK arms a
 * recording the instant B1 is down and a pot moves > 3 %, so a page tap
 * with a hand already on a pot recorded locks by accident (field,
 * 2026-09-05). ParamLock sees B1 through this proxy: "pressed" only once
 * held 300 ms, with pot baselines taken at that moment. */
struct HoldButton : IButton
{
    IButton& b;
    float    ms;
    HoldButton(IButton& bb, float holdMs) : b(bb), ms(holdMs) {}
    bool  Pressed() const override { return b.Pressed() && b.TimeHeldMs() >= ms; }
    bool  RisingEdge() override { return false; }
    bool  FallingEdge() override { return b.FallingEdge(); }
    float TimeHeldMs() const override { return Pressed() ? b.TimeHeldMs() - ms : 0.f; }
};
static HoldButton gLockBtn(hw.buttons[kButtonB1], 300.f);
/* The motion arena (11.5 KB) lives in SDRAM, not DTCM (review M7); the
 * ParamLock object itself must stay in .bss (its ctor runs before hw.Init). */
using Locks = ParamLock<2 * kNumPots>;
static uint16_t ALCHEMY_SDRAM_BSS gLockArena[Locks::kArenaSamples];
static Locks locks(gLockBtn, pager, alchemy::LockArena{gLockArena, sizeof gLockArena});
/* bb_bars scratch (≈15 KB) — SDRAM, zeroed once at boot (NOLOAD). */
static bb::bardetail::BarScratch ALCHEMY_SDRAM_BSS gBarScratchMem;
/* Settings screen: B2+B3 held 2 s enters; brightness + the preset gesture. */
Settings           settings(hw, &pager);
void DescribeManual(alchemy::hostlink::Host& host);   /* src/audiothurgist_manual.cpp */
static hostlink::Host host(presets, "audiothurgist", "Audiothurgist",
                           "0.11.2-manual", "web");

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
static volatile int  gPendHint = 0;      /* slice count for the pending material (0 = prior) */
/* SLICE: staged with a load — the conditioning's verified whole-bar count
 * (core/bb_condition.h; 0 = none), the material's 16th swing for the grid it
 * will play on (0.5 = straight; Options → Swing), and whether the onset table
 * came from the file's cue chunk (then Options → Onsets does not replace it) */
static volatile int   gCondBars   = 0;
static volatile float gPendSwing  = 0.5f;
static bool           gPendCues   = false;
static volatile bool gReapply  = false;  /* re-slice the CURRENT material (Bars option) */
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
    "{\"kind\":\"enum\",\"labels\":[\"Hockman 3-band\",\"Legacy 120/900 (hi/lo)\",\"Two-band lo/hi\",\"Density (keyframes)\"]}";
struct ClassSel
{
    uint8_t       mode  = 0;
    volatile bool dirty = false;
};
static ClassSel      gClass;
static uint8_t       gReclassBuf[bb::Engine::kMaxSlices];
static volatile bool gReclass = false;   /* control: labels ready → audio memcpy */

/* Browser dropdown: external clock PPQN on J3 (all divide the 96-PPQN NCO). */
static const uint32_t kPpqnTable[8] = {24u, 4u, 2u, 1u, 48u, 96u, 16u, 8u};
static const char* kPpqnDisp =
    "{\"kind\":\"enum\",\"labels\":[\"24 PPQN (MIDI/DIN)\",\"4 PPQN (16ths)\",\"2 PPQN (8ths)\",\"1 PPQN (quarters)\",\"48 PPQN\",\"96 PPQN\",\"16 PPQN\",\"8 PPQN\"]}";
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
static volatile float gCallLen = 1.f;     /* staccato style: fraction of the slice */
/* SAMPLE: Voice map key playback — held calls carry their key; the gate's
 * falling edge asks the audio thread to release it (Options → Key Plays). */
static volatile bool  gCallHeld = false;
static volatile int   gCallKey  = -1;
static volatile int   gRelKey   = -1;     /* poll → audio: release this key (-1 none) */
/* SAMPLE end */
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
static const char* kGrammarDisp = /* GRAM: index 4 = Camp (change ringing) */
    "{\"kind\":\"enum\",\"labels\":[\"Classic\",\"Collins\",\"Grids\",\"Euclid\",\"Camp\",\"Play (what you play in, looped)\",\"Pads (free play, nothing written)\"]}";
struct GrammarSel
{
    uint8_t       mode  = 0;
    volatile bool dirty = false;
};
static GrammarSel gGrammar;

/* ── Options v2 (2026-09-06): one byte each. Plain structs like the above;
 * the packages that implement a behaviour read `.sel` (or its dirty flag).
 * Every recommendation that changes behaviour ships as an option (Ernest). */
struct Sel { uint8_t sel = 0; volatile bool dirty = false; };
static const char* kGateModeDisp  = "{\"kind\":\"enum\",\"labels\":[\"Every step\",\"Onsets only (once per hit)\"]}";
static Sel gGateMode = {1};    /* 6  audio reads .sel */
static const char* kBarsDisp      = "{\"kind\":\"enum\",\"labels\":[\"Auto (backbeat)\",\"1 bar\",\"2 bars\",\"4 bars\",\"8 bars (8th grid)\"]}";
static Sel gBars;        /* 7  dirty → re-slice the material */
static const char* kRestsDisp     = "{\"kind\":\"enum\",\"labels\":[\"Continue the break\",\"Gate (silence)\"]}";
static Sel gRests;       /* 8  dirty → engine.restSustain + regen */
static const char* kGateLanesDisp = "{\"kind\":\"enum\",\"labels\":[\"Dominant class\",\"Multi-label (every band over threshold)\"]}";
static Sel gGateLanes;   /* 9  GATES package */
static const char* kFadesDisp     = "{\"kind\":\"enum\",\"labels\":[\"Fixed\",\"Adaptive (keyframe density)\"]}";
static Sel gFades = {1};       /* 10 PLAYBACK package */
static const char* kTailDisp      = "{\"kind\":\"enum\",\"labels\":[\"Gate (honest)\",\"Loop forward\",\"Ping-pong\",\"Zoom (read on, BreakFix)\"]}";
static Sel gTail = {3};  /* 11 PLAYBACK package; 3 = Zoom (BreakFix's pitch rule) */
static const char* kSwapsDisp     = "{\"kind\":\"enum\",\"labels\":[\"Class only\",\"Class + salience tier\"]}";
static Sel gSwaps = {1};       /* 12 GRAMMAR package */
static const char* kDejaVuDisp    = "{\"kind\":\"enum\",\"labels\":[\"Off\",\"Some (¼ of flow bars from history)\",\"Lots (¾)\"]}";
static Sel gDejaVu = {1};      /* 13 GRAMMAR package */
static const char* kSwingDisp     = "{\"kind\":\"enum\",\"labels\":[\"Straight 16ths\",\"Auto (the material's swing)\"]}";
static Sel gSwing = {1};       /* 14 SLICING package */
static const char* kOnsetDetDisp  = "{\"kind\":\"enum\",\"labels\":[\"Spectral flux\",\"Log-filtered flux (Böck 2012)\"]}";
static Sel gOnsetDet = {1};    /* 15 SLICING package */
/* Jack maps (spec §3.3, built 2026-09-07). Brain: J3 clock · J4 v/oct · J5 gate ·
 * J6/J7/J8 kick/snare/hat out. Voice: J3 clock · J4 slice v/oct · J5 style v/oct ·
 * J6 gate in · J7 hit trigger out · J8 velocity out. Follow = Play grammar → Voice. */
static const char* kJacksDisp = "{\"kind\":\"enum\",\"labels\":[\"Follow grammar\",\"Brain (class gates out)\",\"Voice (2× v/oct + gate)\"]}";
static Sel gJacks;       /* 16 */
/* SAMPLE */
static const char* kKeyPlaysDisp = "{\"kind\":\"enum\",\"labels\":[\"One-shot (the slice)\",\"Held (read on until the gate falls)\"]}";
static Sel gKeyPlays;    /* 17 SAMPLE: Voice map keys play their slice, or read on while the gate is held */
/* SAMPLE end */
/* LOOPER (docs/packages/sample-looper.md): Play's memory. Off / 8ths = the
 * unquantized looper (events at their own sample; 8ths snaps at record time);
 * 16ths = the punch-into-steps bar exactly as before. Bytes 18..20 belong to
 * the record phase (Sampling, Record Source, Record). */
static const char* kQuantizeDisp = "{\"kind\":\"enum\",\"labels\":[\"Off (as played)\",\"16ths (punch the bar)\",\"8ths\"]}";
static Sel gQuantize;    /* 21 LOOPER: audio reads .sel */
static volatile bool gPendUndo = false;   /* B2 tap in Play → engine.looper.UndoLast() on the audio thread */
/* LOOPER end */
/* RECORD */
static const char* kSamplingDisp = "{\"kind\":\"enum\",\"labels\":[\"Mono (J1+J2 summed)\",\"Stereo (J1 → L, J2 → R)\"]}";
static Sel gSampling;    /* 18 RECORD: one plane or two (loads, takes) */
static const char* kRecSrcDisp   = "{\"kind\":\"enum\",\"labels\":[\"Inputs (J1/J2)\",\"Output (the module's own mix)\"]}";
static Sel gRecSrc;      /* 19 RECORD: what a take hears */
static const char* kRecordDisp   = "{\"kind\":\"enum\",\"labels\":[\"Off\",\"Arm (B3 tap or J6 starts a loop take)\"]}";
static Sel gRecord;      /* 20 RECORD: loop record into the break; disarms after one take */
/* RECORD end */
static volatile uint8_t gJackMapCur = 0xFFu;   /* applied map: 0 Brain, 1 Voice */
static volatile int     gHitCd  = 0;           /* Voice map: J7 hit trigger countdown (audio) */
static volatile float   gHitVel = 0.f;         /* Voice map: J8 velocity volts */

/* ── Options: ONE preset-managed component for the one-byte selectors.
 * The SDK caps managed components at kPresetMaxManaged (8); separate
 * Serializables plus pager/sample/locks/settings would blow it. Same
 * labels, same live semantics: the browser edits bytes 0..15, Deserialize
 * fans them out to the plain structs above and raises their dirty flags. */
struct Options : Serializable
{
    static constexpr size_t kBytes = 22u;   /* SAMPLE: 17 Key Plays · RECORD: 18 Sampling, 19 Record Source, 20 Record · LOOPER: 21 Quantize */
    size_t SerializedSize() const override { return kBytes; }
    void   Serialize(uint8_t* o) const override
    {
        o[0] = gClass.mode; o[1] = gClockSel.sel; o[2] = gVoiceSel.sel;
        o[3] = gGateLen.sel; o[4] = gGateLvlSel.sel; o[5] = gGrammar.mode;
        o[6] = gGateMode.sel; o[7] = gBars.sel; o[8] = gRests.sel; o[9] = gGateLanes.sel;
        o[10] = gFades.sel; o[11] = gTail.sel; o[12] = gSwaps.sel; o[13] = gDejaVu.sel;
        o[14] = gSwing.sel; o[15] = gOnsetDet.sel; o[16] = gJacks.sel;
        o[17] = gKeyPlays.sel;   /* SAMPLE */
        o[18] = gSampling.sel; o[19] = gRecSrc.sel; o[20] = gRecord.sel;   /* RECORD */
        o[21] = gQuantize.sel;                                              /* LOOPER */
    }
    static void Put(Sel& s, uint8_t v, uint8_t max)
    {
        if(v > max) v = 0;
        if(v != s.sel) { s.sel = v; s.dirty = true; }
    }
    bool Deserialize(const uint8_t* in) override
    {
        uint8_t v;
        v = in[0] > 3 ? 0 : in[0]; if(v != gClass.mode)    { gClass.mode = v;    gClass.dirty = true; }
        v = in[1] > 7 ? 0 : in[1]; if(v != gClockSel.sel)  { gClockSel.sel = v;  gClockSel.dirty = true; }
        gVoiceSel.sel   = in[2] > 1 ? 0 : in[2];
        gGateLen.sel    = in[3] > 3 ? 0 : in[3];
        gGateLvlSel.sel = in[4] > 1 ? 0 : in[4];
        v = in[5] > 6 /* Camp, Play, Pads */ ? 0 : in[5]; if(v != gGrammar.mode)  { gGrammar.mode = v;  gGrammar.dirty = true; }
        Put(gGateMode, in[6], 1);  Put(gBars, in[7], 4);     Put(gRests, in[8], 1);
        Put(gGateLanes, in[9], 1); Put(gFades, in[10], 1);   Put(gTail, in[11], 3);
        Put(gSwaps, in[12], 1);    Put(gDejaVu, in[13], 2);  Put(gSwing, in[14], 1);
        Put(gOnsetDet, in[15], 1); Put(gJacks, in[16], 2);
        Put(gKeyPlays, in[17], 1);   /* SAMPLE */
        Put(gSampling, in[18], 1); Put(gRecSrc, in[19], 1); Put(gRecord, in[20], 1);   /* RECORD */
        Put(gQuantize, in[21], 2);                                                    /* LOOPER */
        return true;
    }
    uint32_t SchemaHash() const override { return 0x4F505436u; /* 'OPT6' (22 bytes) */ }
    bool     Describe(hostlink::ComponentWriter& w) const override
    {
        using hostlink::FieldType;
        w.Label("Options");
        bool ok = w.Field("class.mode",   "Classifier",   0, FieldType::Enum, 0.f, kClassDisp,     4u);
        ok &=     w.Field("clock.ppqn",   "Clock In",     1, FieldType::Enum, 0.f, kPpqnDisp,      8u);
        ok &=     w.Field("voice.mode",   "Voice",        2, FieldType::Enum, 0.f, kVoiceDisp,     2u);
        ok &=     w.Field("gate.len",     "Gate Length",  3, FieldType::Enum, 0.f, kGateLenDisp,   4u);
        ok &=     w.Field("gate.lvl",     "Gate Level",   4, FieldType::Enum, 0.f, kGateLvlDisp,   2u);
        ok &=     w.Field("grammar.mode", "Grammar",      5, FieldType::Enum, 0.f, kGrammarDisp,   7u /* Camp, Play, Pads */);
        ok &=     w.Field("gate.mode",    "Gate Fires",   6, FieldType::Enum, 0.f, kGateModeDisp,  2u);
        ok &=     w.Field("bars",         "Bars",         7, FieldType::Enum, 0.f, kBarsDisp,      5u);
        ok &=     w.Field("rests",        "Rests",        8, FieldType::Enum, 0.f, kRestsDisp,     2u);
        ok &=     w.Field("gate.lanes",   "Gate Lanes",   9, FieldType::Enum, 0.f, kGateLanesDisp, 2u);
        ok &=     w.Field("fades",        "Fades",       10, FieldType::Enum, 0.f, kFadesDisp,     2u);
        ok &=     w.Field("tail",         "Slice Tail",  11, FieldType::Enum, 0.f, kTailDisp,      4u);
        ok &=     w.Field("swaps",        "Swaps",       12, FieldType::Enum, 0.f, kSwapsDisp,     2u);
        ok &=     w.Field("flow.dejavu",  "Flow Déjà Vu",13, FieldType::Enum, 0.f, kDejaVuDisp,    3u);
        ok &=     w.Field("swing",        "Swing",       14, FieldType::Enum, 0.f, kSwingDisp,     2u);
        ok &=     w.Field("onsets",       "Onsets",      15, FieldType::Enum, 0.f, kOnsetDetDisp,  2u);
        ok &=     w.Field("jacks",        "Jacks",       16, FieldType::Enum, 0.f, kJacksDisp,     3u);
        ok &=     w.Field("key.plays",    "Key Plays",   17, FieldType::Enum, 0.f, kKeyPlaysDisp,  2u);   /* SAMPLE */
        /* RECORD */
        ok &=     w.Field("sampling",     "Sampling",    18, FieldType::Enum, 0.f, kSamplingDisp,  2u);
        ok &=     w.Field("rec.src",      "Record Source",19, FieldType::Enum, 0.f, kRecSrcDisp,   2u);
        ok &=     w.Field("record",       "Record",      20, FieldType::Enum, 0.f, kRecordDisp,    2u);
        /* RECORD end */
        /* LOOPER */ ok &= w.Field("quantize", "Quantize", 21, FieldType::Enum, 0.f, kQuantizeDisp, 3u); /* LOOPER end */
        return ok;
    }
};
static Options gOptions;
/* ── end GRAMMAR ───────────────────────────────────────────────────────── */
/* ── RECORD ── record state. The control thread owns every gesture and the
 * analysis; the audio thread only feeds (engine.RecFeed for a take into a
 * key, gTake.Feed for a loop take into the other buffer) and meters. ──── */
static bool             gKeyArm      = false;   /* J5 style 7 stands: buttons red, keys record */
static bool             gKeyTake     = false;   /* a take into gKeyTakeKey is armed in the engine */
static int              gKeyTakeKey  = -1;
static bool             gWebTake     = false; /* the running key take was started from the browser (no gate: a second arm stops it) */
static volatile bool    gKeyTakeDone = false;   /* OnPoll → main loop: RecStop → trim → analyse → commit */
static bb::TakeRecorder gTake;                  /* the loop take, into gLoad (+ gLoadR) */
static bool             gTakeActive  = false;   /* control: a loop take runs, or waits to be serviced */
static bool             gTakeClocked = false;
static int              gTakeBars    = 0;       /* a clocked take's bar count (0 = unclocked) */
static int  ALCHEMY_SDRAM_BSS gMarkers[kMaxOnsets];   /* J6 edges during a clocked take, take-relative */
static int              gNumMarkers  = 0;
static int              gGridBars    = 1;       /* the bar count behind the current grid (control) */
static volatile bool    gPendStereo  = false;   /* the pending load / take carries an R plane */
static volatile int     gReapplyR    = -1;      /* a re-apply's plane change: -1 keep, 0 mono, 1 gPlayR */
static volatile float   gRecPeak     = 0.f;     /* audio → render: the record input's peak while a take runs */
static float            gRecMix[kScratch];      /* the mono record mix (audio thread) */
static float            gOutR[kScratch];        /* the engine's R render (audio thread) */
/* ── end RECORD ────────────────────────────────────────────────────────── */
/* ── SETS ── sets on the card (docs/packages/sample-sets.md): sets/<name>.wav
 * + sets/<name>.json, and presets/<slot>.json backups. Control thread only,
 * under SdCard::BusyGuard; every long file loop yields to the clock and, every
 * 40 ms, to the whole control loop (KeepClock), so the panel, the LEDs and
 * HostLink stay alive through a 5 MB write. DMA law: the FIL and the WAV
 * staging in .axi_bss, the JSON text in SDRAM, all 32-byte aligned. ─────── */
ALCHEMY_SDMMC_BSS alignas(32) static FIL     gSetFil;                 /* the writer's file (the loader has its own) */
ALCHEMY_SDMMC_BSS alignas(32) static uint8_t gSetStage[16384];       /* WAV staging; the writer applies the DMA pad */
alignas(32) static char    ALCHEMY_SDRAM_BSS gSetJson[bb::kSetJsonMax];          /* one f_write / f_read of the whole text */
alignas(8)  static uint8_t ALCHEMY_SDRAM_BSS gSetInfoMem[sizeof(bb::SetInfo)];   /* NOLOAD: placement-new'd at boot (NSDMI) */
alignas(8)  static uint8_t ALCHEMY_SDRAM_BSS gSetLooperMem[sizeof(bb::Looper)];
static bb::SetInfo*  gSet       = nullptr;   /* the set being saved / loaded */
static bb::Looper*   gSetLooper = nullptr;   /* looper staging, audio ↔ control, whole copies (Looper::Adopt) */
static bb::SetPlan   ALCHEMY_SDRAM_BSS gSetPlan;   /* trivially constructible; PlanSet clears it */
static volatile uint8_t gSetState      = 0;      /* 0 idle · 1 saving · 2 loading (state tail 219) */
static volatile uint8_t gSetProgress   = 0;      /* 0..255 of the current op (220) */
static volatile uint8_t gSetResult     = 0;      /* last op: 0 ok · 1 no card · 2 card full · 3 write failed · 4 bad name · 5 not found · 6 busy · 7 bad file (221) */
static volatile uint8_t gSetSeq        = 0;      /* bumps when an op finishes (222) */
static char             gSetCurName[bb::kSetMaxName] = {};   /* the set the material came from; "" = a loose file / a take (223..254) */
static char             gSetReqName[bb::kSetMaxName] = {};   /* ACTION 14 / 15's name */
static volatile uint8_t gSetReq        = 0;      /* 1 save · 2 load — serviced by the main loop */
static volatile bool    gSetSnapReq    = false;  /* control → audio: copy engine.looper into *gSetLooper */
static volatile bool    gSetPendLooper = false;  /* control → audio: adopt *gSetLooper at the apply */
static bool             gSetPendCommit = false;  /* after the apply: CommitSetRecordings */
static bool             gSetFull       = false;  /* the sink saw a short write (card full) */
static float            gSetErrT       = 0.f;    /* B-LED red flash, frames */
static uint32_t         gSetTotal = 0, gSetDone = 0;   /* the save's byte count */
static uint32_t         gSetTickMs     = 0;
static bool             gInTick        = false;  /* KeepClock re-entrancy guard */
static uint32_t         gPresetSeq[Presets::kNumSlots] = {};   /* preset backup: flash sequence last seen per slot (0 = empty) */
static uint32_t         gPresetPollMs  = 0;
static PresetName*      gPresetName    = nullptr;
/* ── end SETS ──────────────────────────────────────────────────────────── */
/* ── CLOCK ── control-thread clock state; the math is core/bb_clockmath.h ── */
static volatile float gClkFlash = 0.f;   /* B1 white flash per detected pulse */
static volatile float gClkMult  = 1.f;   /* APPLIED multiplier (telemetry mirror of gMap.mult) */
static bool           gWasExt   = false; /* ext/internal transition tracking */
/* NCO rate snapshot taken on the CONTROL thread (same thread as the PLL's
 * SetTicksPerUs, so no torn double read) and consumed by the audio thread as
 * an atomic float: the engine's varispeed rate is derived from it every block
 * so slice playback advances EXACTLY as fast as the phase does. */
/* Clock snapshot for the audio thread — published by OnPoll after every NCO
 * tick, double-buffered with a single-byte index so the ISR always reads a
 * complete record (it can preempt the writer mid-record; a seqlock would
 * deadlock the ISR). The audio thread EXTRAPOLATES from it: tick + tpu × µs
 * since the tick, so a control-thread stall (HostLink, LED render, a load)
 * no longer freezes the playhead and then jumps it — the "thinking moment"
 * that broke verbatim runs through the 160-sample continuity window. */
/* ── Time base (v0.9.6) ──────────────────────────────────────────────
 * daisy::System::GetUs() is TIM2 (200 MHz, 32-bit) divided down: it WRAPS
 * EVERY 21.5 s (CNT / 200), while everything downstream — MusicalClock,
 * ClockFollower, CvGate, our ext window and diag windows — assumes a 2^32-µs
 * modulus. Every 21.5 s the NCO leaped 50 ms (a dropped moment), the ext
 * window dropped and the follower re-locked with an alignment shift (the
 * "×2 then falls back after a few bars" report). Control thread: a 64-bit
 * tick accumulator, advanced on every poll / keep-alive (must run at least
 * every 20 s), yields microseconds that really wrap at 2^32. Audio thread:
 * raw 32-bit ticks, modular — exact for any interval under 21 s, and its
 * extrapolation is capped at 50 ms anyway. (Review 2026-09-06; the SDK's
 * clock_sync example has the same bug — report upstream.) */
static uint32_t gTicksPerUs = 200u;
static uint64_t gTicks64    = 0;
static uint32_t gLastTick   = 0;
static uint32_t NowUs()   /* control thread only */
{
    const uint32_t t = daisy::System::GetTick();
    gTicks64 += (uint32_t)(t - gLastTick);
    gLastTick = t;
    return (uint32_t)(gTicks64 / gTicksPerUs);
}
static inline uint32_t TickNow() { return daisy::System::GetTick(); }   /* any thread */

struct ClockSnap { uint64_t tick; double frac; double tpu; uint32_t tick32; bool running; bb::ClockMap map; };
static ClockSnap          gSnap[2];
static volatile uint8_t   gSnapIdx = 0;
static constexpr uint32_t kSnapMaxExtrapUs = 50000u;   /* = MusicalClock's catch-up clamp */
/* Diagnostics (GET_STATE tail; the web app shows them). */
static volatile uint16_t gDiagPollGapUs = 0;   /* worst gap between OnPoll calls, last second */
static volatile uint16_t gDiagAudioUs   = 0;   /* worst audio-callback duration, last second */
static volatile uint16_t gDiagJumps     = 0;   /* playhead forward jumps > 1.5 steps (lifetime) */
static volatile uint16_t gDiagRegens    = 0;   /* pattern regenerations (lifetime) */
static volatile uint32_t gDiagAudioCur  = 0;   /* audio: running max, rolled by OnPoll */
/* Stall forensics (v0.9.5): WHAT the control thread was doing when the
 * worst poll gap of the last second happened, how late a gate write lands
 * after the audio thread armed it, and audio callbacks over budget.
 * Phases: 1 in OnPoll · 2 SDK between polls (host poll/delay) · 3 OnFrame ·
 * 4 SDK after frame · 5 OnRender · 6 SDK after render (overlay, LED show) ·
 * 7 main-loop handlers · 8 ServiceLoad · 9 analysis (KeepClock). */
static volatile uint8_t  gDiagPhase      = 0;
static volatile uint8_t  gDiagStallPhase = 0;
static uint8_t           gDiagStallCur   = 0;
static volatile uint32_t gGateArmUs[3]   = {0u, 0u, 0u};
static volatile uint16_t gDiagGateLateUs = 0;
static uint32_t          gDiagGateLateCur = 0;
static volatile uint16_t gDiagOverruns   = 0;   /* callbacks > 450 µs, lifetime */
static double         gLastLp   = -1.;   /* audio thread: < 0 = re-arm the phase guard (new material) */
static float          gP2ExtRef = -1.f;  /* P2 while following: engage on move */
static bool           gP2ExtEng = false;
static bb::ClockMap    gMap;             /* NCO ticks → loop ticks; OnPoll writes it, audio reads the snapshot copy */
static bb::MultZone    gZone;            /* P2 → ÷4 ÷2 ×1 ×2 ×4 with hysteresis + dwell (the REQUESTED zone) */
static bb::JitterMeter gJitter;          /* per-pulse period deviation, rolled per second */
static double          gPulseTick = -1.; /* NCO position of the last external pulse; -1 = none since arming */
static volatile uint16_t gDiagJitterUs = 0, gDiagJitterRmsUs = 0;   /* last second: peak / RMS, µs */
static volatile uint8_t  gDiagClkFlags = 0;   /* b0 P2 engaged · b1 switch pending · b2 lock aligned (~2 s) · b4-7 ext transitions mod 16 */
static uint8_t           gDiagExtTrans = 0, gDiagAlignHold = 0;
/* ── end CLOCK ─────────────────────────────────────────────────────────── */

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
VirtualKnob k_tempo = VirtualKnob(P_TEMPO, "Tempo").Linear(60.f, 200.f)
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

/* ── class analysis: core/bb_classify.h (pure, host-tested) ─────────── */
static void ClassifySteps(const float* x, int len, uint8_t* cls, int mode,
                          float* sal = nullptr, int nsl = 0, uint8_t* bands = nullptr /* GATES */)
{
    bb::ClassifySteps(x, len, kSr, cls, mode, sal, nullptr, nsl, bands);
}

/* PLAY: keyframe density per slice (the Density lens, whatever classifier is
 * selected) for Options → Fades → Adaptive. Control thread, at load and at
 * a Bars re-slice; the audio thread copies it into engine.sliceDens. */
static void ComputeDensity(const float* x, int len, float* dens, int nsl = 0)
{
    static uint8_t scratch[bb::Engine::kMaxSlices];
    bb::ClassifySteps(x, len, kSr, scratch, 3, nullptr, dens, nsl);
}
/* PLAY */

/* Slice count for material: the Bars override, else the backbeat estimate
 * (core/bb_bars.h), else 0 = the engine's duration prior. Always bars × 16,
 * halved while over kMaxSlices (an 8-bar loop becomes an 8th grid). */
static int SliceCountFor(const float* x, int len, const int* onsets, int n, int condBars /* SLICE */,
                         int* barsOut = nullptr /* SLICE: the bar count behind the slice count */)
{
    int bars = 0;
    if(gBars.sel >= 1) bars = 1 << (gBars.sel - 1);
    else
    {
        /* SLICE: the conditioning's verified whole-bar count first (the buffer
         * was cut to it); else a confident IOI-cluster estimate whose bar
         * count for this buffer is a power of two within 6 %; else 0 = the
         * duration prior (docs/packages/slice.md) */
        bars = condBars > 0 ? condBars : 0;
        if(bars <= 0)
        {
            const float bar = bb::EstimateBarSamples(x, len, kSr, onsets, n);
            if(bar > 0.f)
            {
                const float b = (float)len / bar;
                for(int p = 1; p <= 16; p <<= 1) if(fabsf(b - (float)p) < 0.06f * (float)p) bars = p;
            }
        }
    }
    if(bars <= 0)
    {
        /* the duration prior's own bar count, for callers that need it */
        if(barsOut) { int pb = 1; const float dur = (float)len / kSr; while(dur / (float)pb >= 3.2f && pb < 16) pb <<= 1; *barsOut = pb; }
        return 0;
    }
    if(barsOut) *barsOut = bars;
    int nsl = bars * 16;
    while(nsl > bb::Engine::kMaxSlices) nsl /= 2;
    return nsl;
}

/* SLICE: the material's 16th swing against the grid it will play on (Options
 * → Swing: Auto), from core/bb_swing.h; 0.5 = straight. Only when that grid
 * IS the material's 16th grid: past 4 bars (and under the "8 bars (8th
 * grid)" override) the slices are 8ths, and straight 16th hats at 0.5 and
 * 0.75 of an 8th-step cell would read as a swung cluster (review). */
static float SwingFor(int len, const int* onsets, int n, int nslHint, int bars)
{
    if(gSwing.sel != 1) return 0.5f;
    const int nsl = nslHint >= 1 ? nslHint : bb::GridSlices(len, kSr);
    if(bars <= 0 || bars * 16 != nsl) return 0.5f;
    return bb::EstimateSwing(onsets, n, (float)len / (float)nsl).p;
}

/* Per-slice min/max envelope over the engine's grid (same bar rule). */
static void ComputeEnvelope(const float* x, int len, uint8_t env[][kEnvPts][2], int nslHint = 0)
{
    int nsl = nslHint >= 1 ? nslHint : bb::GridSlices(len, kSr);
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

static void AudioCb(daisy::AudioHandle::InputBuffer  in,   /* RECORD: J1/J2 feed the takes */
                    daisy::AudioHandle::OutputBuffer out,
                    size_t                           size)
{
    const uint32_t cbStartTick = TickNow();   /* diag: callback duration (ticks) */
    /* new material ready: swap buffers and re-point the engine (SetMaterial
     * is audio-thread work here exactly as it was on the Pod). */
    if(gApply || gReapply)
    {
        float* bR;   /* RECORD: the R plane the material takes (nullptr = mono) */
        if(gApply)
        {
            float* t = gPlay; gPlay = gLoad; gLoad = t;   /* re-apply keeps the buffer */
            t = gPlayR; gPlayR = gLoadR; gLoadR = t;       /* RECORD: the R planes swap with their L */
            bR = gPendStereo ? gPlayR : nullptr;
        }
        else { bR = gReapplyR < 0 ? engine.bufR : (gReapplyR == 1 ? gPlayR : nullptr); gReapplyR = -1; }   /* RECORD */
        /* GRAM */ memcpy(engine.sliceSal, gPendSal, sizeof gPendSal); /* before SetMaterial: its boot pattern ranks the tiers */
        engine.swing = gPendSwing;   /* SLICE: read by SetMaterial (boundaries + step timing) */
        /* SAMPLE: the plane's capacity lets recordings land past the material; a
         * re-apply (same buffer) keeps them, a load (the other buffer) starts over */
        engine.SetMaterial(gPlay, gPendLen, kSr, gPendOnsets, gPendN, gPendClass, gPendHint, bR /* RECORD */, kBufMax);
        memcpy(gSal, gPendSal, sizeof gSal);
        /* GATES */
        memcpy(engine.stepBands, gPendBands, sizeof gPendBands);   /* SAMPLE: the source's size — the engine's table is wider */
        /* GATES end */
        memcpy(engine.sliceDens, gPendDens, sizeof gPendDens);     /* PLAY */
        engine.SyncRecordedMeta();   /* SAMPLE: the copies above are grid-wide; recorded entries keep their own */
        /* SETS: a set's looper, right after SetMaterial emptied the loop (one 4 KB memcpy) */
        if(gSetPendLooper) { engine.looper.Adopt(*gSetLooper); gSetPendLooper = false; }
        memcpy(gEnv, gPendEnv, sizeof gEnv);
        gMatGen = (uint8_t)(gMatGen + 1u);
        gNeedNativeTempo = true;
        gLastLp = -1.;   /* new nSlices → new phase geometry; do not hold */
        gApply = false; gReapply = false;
    }
    engine.gateOnsetsOnly = gGateMode.sel == 1;
    /* GRAM */ engine.swapsMode = gSwaps.sel; engine.dejaVu = gDejaVu.sel; /* GRAM end */
    engine.multiLabel     = gGateLanes.sel == 1;   /* GATES */
    /* PLAY */
    engine.fadeMode = gFades.sel;   /* 0 Fixed, 1 Adaptive (keyframe density) */
    engine.tailMode = gTail.sel;    /* 0 Gate, 1 Loop forward, 2 Ping-pong   */
    /* PLAY */
    /* LOOPER */
    engine.looperQuant = gQuantize.sel;   /* 0 Off, 1 16ths (punch), 2 8ths */
    if(gPendUndo) { engine.looper.UndoLast(); gPendUndo = false; }
    /* LOOPER end */
    /* SETS: a save's looper snapshot, taken between two blocks */
    if(gSetSnapReq) { gSetLooper->Adopt(engine.looper); gSetSnapReq = false; }

    if(gPendReslice) { engine.Reslice();   gPendReslice = false; }
    if(gPendReroll)  { engine.RerollFill(); gPendReroll  = false; }
    if(gPendRegen)   { engine.RegenPattern(); gPendRegen = false; gDiagRegens = (uint16_t)(gDiagRegens + 1u); }
    if(gScrubReq)    { engine.Scrub(gScrubDir); gScrubReq = false; }
    if(gReclass)     { memcpy(engine.stepClass, gReclassBuf, sizeof gReclassBuf); memcpy(gSal, gReclassSal, sizeof gSal);
                       /* GRAM */ memcpy(engine.sliceSal, gReclassSal, sizeof gReclassSal); /* GRAM end */
                       engine.SyncRecordedMeta();   /* SAMPLE */
                       gReclass = false; }
    /* SAMPLE: a call carries its key and the held rule; a release follows the call it belongs to */
    if(gRelKey >= 0) { engine.ReleaseKey(gRelKey); gRelKey = -1; }   /* the fall preceded the rise: release first (review M3) */
    if(gCallReq)     { engine.CallSlice(gCallIdx, gCallMul, gCallRev, gCallPunch, gCallRat, gCallLen, gCallHeld, gCallKey); gCallReq = false; }
    /* SAMPLE end */

    int n = (int)size;
    if(n > kScratch) n = kScratch;

    /* CLOCK ── loop phase from the clock snapshot ─────────────────────────
     * Drive the engine playhead from the musical clock (internal or followed).
     * Position comes from the single monotonic MasterTick counter (+ FracTick),
     * NOT Bar()/BarPhase() read separately: those tore at bar boundaries under
     * the 1 ms tick and fired a spurious last-step ("skips"). The snapshot's
     * bb::ClockMap turns NCO ticks into loop ticks (one 16th = Ppqn/4; the
     * loop = nSlices 16ths) and carries the P2 multiplier: a zone change is a
     * slope change at a scheduled NCO tick, continuous by construction, and
     * the map decides from THIS block's T whether the switch has happened —
     * block-exact, no cross-thread flag, no phase jump. */
    engine.extPhase = true;
    {
        /* Extrapolate the NCO from its last published tick (see ClockSnap):
         * the control thread ticks it every ~1 ms but can stall for tens of
         * ms; without this the playhead froze and then jumped. Capped at the
         * clock's own catch-up clamp so both lose the same time past it. */
        const ClockSnap s = gSnap[gSnapIdx];
        uint32_t dtUs = (TickNow() - s.tick32) / gTicksPerUs;   /* modular ticks: wrap-safe */
        if(dtUs > kSnapMaxExtrapUs) dtUs = kSnapMaxExtrapUs;
        const double extrap = s.running ? s.tpu * (double)dtUs : 0.;
        const double T     = (double)s.tick + s.frac + extrap;
        const double mult  = s.map.MultAt(T);
        const double tick  = s.map.Loop(T);
        const double loopT = (double)engine.nSlices * ((double)gClock.Ppqn() / 4.0);
        double lp = loopT > 0. ? fmod(tick, loopT) / loopT : 0.;
        if(lp < 0.) lp += 1.;   /* a backward lock alignment can put the first loop ticks below zero */

        /* RATE == PHASE ADVANCE, exactly. barSeconds feeds the varispeed rate
         * (and the ratchet countdown) in the engine; deriving it here, per
         * block, from the very ticks/µs the NCO integrates means a verbatim
         * run's voice position never drifts from the step grid. (SetBpm from
         * the control thread was integer-quantised while the PLL's rate is
         * continuous — ~20 samples/step of drift at 0.4 BPM, blowing the
         * 160-sample continuity window every ~8 steps: the ext-clock stutter.)
         * tpu == 0 while the follower acquires lock: phase is frozen and no
         * step can fire, so the previous barSeconds is simply left alone. */
        const float tpu = (float)s.tpu;
        if(tpu > 0.f && loopT > 0.)
            engine.barSeconds = (float)(loopT / ((double)tpu * 1e6 * mult));

        /* Monotonic guard, half-a-step wide (bb::PhaseGuard): a torn
         * MasterTick/FracTick read is at most ±1 tick (1/24 step) and must not
         * fire a backward step (double trigger + a bogus flow-reroll).
         * Anything larger backward is a legitimate jump (new material,
         * transport reset, a lock alignment) and passes immediately. A
         * multiplier switch is continuous now, so it needs no re-arm; new
         * material still asks for one through gLastLp < 0. */
        static bb::PhaseGuard s_guard;
        if(gLastLp < 0.) { s_guard.Rearm(); gLastLp = 0.; }
        lp = s_guard.Apply(lp, engine.nSlices);
        gDiagJumps = s_guard.jumps;   /* > 1.5 steps forward in one block */
        engine.barPhase = lp;
    }
    /* ── end CLOCK ─────────────────────────────────────────────────────── */

    engine.Process(gMono, gOutR, n);   /* RECORD: both planes (mono material copies L to R) */

    /* per-class trigger bitmask → ARM gate countdowns (sample-accurate).
     * The CV writes themselves happen in OnPoll, never here: J3–J6 are
     * MCP4728 channels and CvJack::SetVolts on them is synchronous I²C
     * (WriteAll + PulseLdac through the PCA9557). Blocking I²C inside the
     * audio ISR deadlocks the module on the first callback — the v0.3.0
     * boot hang. J7/J8 are STM DACs but stay on the same path for uniformity. */
    /* GATES: the lane law lives in core/bb_gates.h (host-tested): a re-hit
     * while a lane is high retriggers it (one poll low) in EVERY length
     * mode; leaving Hold closes a held lane within the new length; a grammar
     * accent lifts a velocity hit to the 5 V ceiling. */
    uint8_t gf = engine.gateFired; engine.gateFired = 0;
    const int  glen = kGateLenSamps[gGateLen.sel & 3];
    const bool vel  = gGateLvlSel.sel == 1;
    for(int c = 0; c < 3; c++)
    {
        const bool fired = (gf & (1u << c)) != 0;
        float volts = 0.f;
        if(fired)
        {
            int sl = engine.gateSlice[c];
            float sa = (sl >= 0 && sl < bb::Engine::kMaxTable) ? engine.sliceSal[sl] : 1.f;   /* recorded keys too (review L10) */
            volts = bb::GateVolts(vel, sa, engine.gateAccent[c]);
        }
        bb::GateArm(gGateCd[c], gGateLvl[c], gGateRetrig[c], fired, glen, volts, n);
        if(fired) gGateArmUs[c] = cbStartTick;   /* diag: arm→write latency (ticks) */
    }
    /* GATES end */
    /* J7/J8 (snare, hat) are STM DAC channels in polling mode: SetVolts is
     * calibration math + a register write, ISR-safe. Writing them here, every
     * other block (1 ms, the old poll cadence, so the retrigger dip keeps its
     * width), removes the poll's 1–3 ms jitter and the wait behind J6's I²C
     * transaction. J6 (MCP4728, I²C) stays on the control thread. */
    {
        static uint8_t s_div = 0;
        static float   s_last[3] = {-1.f, -1.f, -1.f};
        /* Voice map: J7 = 10 ms trigger per call (ratchet doubles too), J8 = velocity of the called slice */
        if(engine.callHits)
        {
            const int li = engine.lastCallIdx;
            gHitVel = 1.5f + 3.5f * ((li >= 0 && li < bb::Engine::kMaxTable) ? engine.sliceSal[li] : 1.f);
            gHitCd  = (int)(0.010f * kSr);
            engine.callHits = 0;
        }
        else if(gHitCd > 0) gHitCd -= n;
        if((++s_div & 1u) == 0u)
        {
            if(gJackMapCur == 1u)
            {
                const float t = gHitCd > 0 ? 5.f : 0.f;
                if(t != s_last[1]) { s_last[1] = t; hw.j7.SetVolts(t); }
                const float vv = gHitCd > 0 ? gHitVel : s_last[2];
                if(vv != s_last[2]) { s_last[2] = vv; hw.j8.SetVolts(vv); }
            }
            else
                for(int c = 1; c < 3; c++)
                {
                    const float v = bb::GatePollVolts(gGateCd[c], gGateLvl[c], gGateRetrig[c]);
                    if(v != s_last[c]) { s_last[c] = v; if(c == 1) hw.j7.SetVolts(v); else hw.j8.SetVolts(v); }
                }
        }
    }

    const float v = gVol, send = gVerbSend;
    for(int i = 0; i < n; i++)
    {
        const float dl = gMono[i], dr = gOutR[i];   /* RECORD: stereo-aware mix; mono material has dr == dl */
        float wl, wr;
        gVerb.Process(dl * send, dr * send, &wl, &wr);
        float l = (dl + wl * 0.85f) * v;
        float r = (dr + wr * 0.85f) * v;
        if(l > 1.f) l = 1.f; else if(l < -1.f) l = -1.f;
        if(r > 1.f) r = 1.f; else if(r < -1.f) r = -1.f;
        out[0][i] = l;
        out[1][i] = r;
        if((i & 3) == 0) { gScope[gScopeW & 1023u] = (int8_t)(0.5f * (l + r) * 127.f); gScopeW = (uint16_t)(gScopeW + 1u); }   /* RECORD: the scope keeps the mono sum */
    }
    /* RECORD: feed the takes — the engine's (into a key, past the material)
     * and the shell's (the loop, into the other buffer); both return at once
     * when idle. Record Source: J1/J2, or what just left J9/J10 (the
     * resampling trick). Sampling: (J1+J2)/2 into L, or J1 → L, J2 → R. */
    if(engine.recArmed || gTake.running)
    {
        const bool fromOut = gRecSrc.sel == bb::kSrcOutput;
        const bb::RecPlanes rp = bb::RecordInput(gSampling.sel, fromOut ? out[0] : in[kCodecInChJ1],
                                                 fromOut ? out[1] : in[kCodecInChJ2], gRecMix, n);
        engine.RecFeed(rp.L, rp.R, n);
        gTake.Feed(rp.L, rp.R, n);
        const float pk = bb::BlockPeak(rp.L, rp.R, n);
        if(pk > gRecPeak) gRecPeak = pk;
    }
    /* RECORD end */
    const uint32_t cbUs = (TickNow() - cbStartTick) / gTicksPerUs;
    if(cbUs > gDiagAudioCur) gDiagAudioCur = cbUs;
    if(cbUs > 450u) gDiagOverruns = (uint16_t)(gDiagOverruns + 1u);
}

/* ── control frame ──────────────────────────────────────────────────── */
static void OnFrame()
{
    gDiagPhase = 3;
    float pk = k_chop.Norm();
    engine.patternKnob = pk;
    gVerbSend = pk < 0.22f ? (0.22f - pk) / 0.22f * 0.75f : 0.f;

    /* Varispeed follows the musical clock via the OnPoll snapshot (ClockSnap);
     * engine.bpm is telemetry only now — SetBpm's integer-quantised
     * barSeconds must NOT be written from here. */
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
    gDiagPhase = 4;
}

static void OnPoll(uint32_t /*t_ms*/)
{
    gDiagPhase = 1;
    if(gNeedNativeTempo)
    {
        gNeedNativeTempo = false;
        gClock.SetBpm(engine.NativeBpm());
        gP2Ref = -1e9f;   /* re-latch: P2 re-engages on movement */
    }

    /* CLOCK ── clock pipeline (spec §7): edge intake → arming → follower →
     * P2 zones → NCO tick → lock alignment → multiplier map → snapshot →
     * diagnostics. OnPoll is the only place the NCO ticks; the audio thread
     * only ever reads the published snapshot. docs/packages/clock.md. ──── */
    const uint32_t now_us = NowUs();
    gClkEdge.Tick(loop.Cv(), loop.NumCv(), now_us);
    const bool rose = gClkEdge.JustRose(kClockChan);
    static uint32_t s_prevPulseUs = 0;
    if(rose)
    {
        /* Arm on the SECOND edge within 2 s: a single spurious edge (a cable
         * going in or out, noise on an unpatched jack) must not start the
         * follower — armed, it stopped the NCO: two seconds of silence
         * (field, 2026-09-06). Once armed every pulse refreshes. */
        if(s_prevPulseUs && (now_us - s_prevPulseUs) < 2000000u) gLastPulseUs = now_us;
        s_prevPulseUs = now_us;
        gClkFlash = 1.f;
    }
    const bool ext = gLastPulseUs && (now_us - gLastPulseUs) < 2000000u;
    static float gPreExtBpm = 120.f;
    /* The NCO position before this poll's tick: the map's "now" for the
     * transitions below (a reached switch is committed first so MultAt is
     * the truth). */
    const double Tpre = (double)gClock.MasterTick() + gClock.FracTick();
    gMap.Commit(Tpre);
    if(ext && !gWasExt)
    {
        gPreExtBpm = gClock.Bpm();                     /* tempo to fall back on */
        const double tpuBefore = gClock.TicksPerUs();
        gFollower.Enable(kPpqnTable[gClockSel.sel]);   /* arms the PLL — and Stop()s
                                                          the NCO at rate 0 */
        /* Keep playing at the current tempo while the PLL acquires; lock
         * aligns the phase (after the tick, below). Start() cancels Enable()'s
         * Stop() inside the same Tick; the price is the one poll of dt that
         * ApplyDeferredTransport drops on a Start (≈1 ms, once per patch-in).
         * A stopped NCO made every patch-in a silence. */
        gClock.SetTicksPerUs(tpuBefore);
        gClock.Start();
        /* P2 reverts to ×1 and re-engages on move. Continuous: the map keeps
         * the loop where it is, only its slope is (re)set. */
        gZone.Reset(2); gP2ExtRef = -1.f; gP2ExtEng = false;
        gMap.SetMultNow(1., Tpre);
        gPulseTick = -1.; gJitter.Reset(); gDiagExtTrans = (uint8_t)(gDiagExtTrans + 1u);
    }
    if(!ext && gWasExt)
    {
        /* Clock gone: the follower had flagged the NCO as STEERED, which makes
         * SetBpm a no-op — un-steer or P2 can never take the clock back. Keep
         * the freewheeled tempo until P2 moves (re-latch), like a fresh load. */
        gFollower.Disable();
        gClock.SetSteered(false);
        if(!gClock.Running() || gClock.TicksPerUs() <= 0.)
        {
            /* Lost BEFORE lock with a stopped NCO (the v0.7.1 review finding).
             * Cannot happen now that the NCO runs through acquisition, kept
             * as the safety net: restart at the pre-clock tempo. */
            gClock.SetBpm(gPreExtBpm);
            gClock.Start();
        }
        else
        {
            /* Tempo HOLDS (checklist §1): whatever the loop was doing at ×m it
             * keeps doing. Fold the multiplier into the NCO rate and drop the
             * map to ×1 in the same poll — the loop position and its speed are
             * both continuous through the unpatch. */
            const double m = gMap.MultAt(Tpre);
            if(m != 1.) gClock.SetTicksPerUs(gClock.TicksPerUs() * m);
        }
        gMap.SetMultNow(1., Tpre);
        gP2Ref = -1e9f;
        gJitter.Reset(); gDiagExtTrans = (uint8_t)(gDiagExtTrans + 1u);
    }
    gWasExt = ext;

    if(ext)
    {
        if(rose) gFollower.OnPulse(gClkEdge.LastRiseUs(kClockChan));
        gFollower.Update(now_us);
        /* Spec §6.4: while following, P2 is a clock multiplier/divider around
         * the followed tempo — wide ×1 detent in the middle. It ENGAGES only
         * once the knob moves (an uncaught/stored P2 at boot otherwise lands
         * on ÷4 and the loop crawls). The zone map, its ±0.04 hysteresis and
         * the 100 ms dwell live in bb::MultZone (core/bb_clockmath.h). The
         * zone is a REQUEST: the map applies it at the next beat, below.
         * Note k_tempo.Norm() = stored + param-lock delta: a lock recorded on
         * P2 (B1 held ≥ 300 ms while turning) replays here — see the doc. */
        const float p = k_tempo.Norm();
        if(gP2ExtRef < 0.f) gP2ExtRef = p;
        if(!gP2ExtEng && (p > gP2ExtRef + 0.03f || p < gP2ExtRef - 0.03f))
            gP2ExtEng = true;
        if(gP2ExtEng) gZone.Update(p, daisy::System::GetNow());   /* ms, wraps at 2^32: dwell math stays wrap-safe */
    }
    else
    {
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
    {   /* Lock alignment + multiplier map (core/bb_clockmath.h). T is the NCO
         * position at now_us — which is also this poll's pulse stamp (CvGate
         * stamps a rise with the poll's now_us), so on the poll that saw a
         * pulse, T IS the pulse's NCO position. */
        const double T   = (double)gClock.MasterTick() + gClock.FracTick();
        const double tpu = gClock.TicksPerUs();
        const double ppq = (double)gClock.Ppqn();
        const double tpp = ppq / (double)gFollower.ExtPpqn();   /* NCO ticks per external pulse */
        if(ext && rose) { gPulseTick = T; gJitter.Pulse(gClkEdge.LastRiseUs(kClockChan)); }
        /* The SDK follower's DeclareLock anchors its phase reference wherever
         * the NCO happens to be at the third pulse; from then on the PI keeps
         * pulses tpp ticks apart but never puts them ON the tick grid — with
         * a running NCO the loop sat up to a whole pulse off the clock (a
         * full step at 4 PPQN, a beat at 1 PPQN). Once per lock (first lock
         * and every re-lock after a loss) shift the loop so the lock pulse
         * sits on the pulse grid, never aligning beyond a beat. */
        static bool s_wasLocked = false;
        const bool locked = ext && gFollower.Locked();
        if(locked && !s_wasLocked && gPulseTick >= 0.)
        {
            if(!gClock.Running()) gClock.Start();   /* SetAutoStart is off (main); belt and braces */
            const double m = gMap.MultAt(T);
            double A = tpp * (m > 1. ? m : 1.); if(A > ppq) A = ppq;
            gMap.Shift(bb::ClockMap::AlignDelta(gMap.Loop(gPulseTick), A, 0.4 * ppq / 4.));
            gDiagAlignHold = 2;
        }
        s_wasLocked = locked;
        /* Commit a switch the audio thread has already passed, then keep the
         * zone's multiplier requested (idempotent): it is scheduled at the
         * next pulse instant that is also a loop beat, ≥ 3 ms out. */
        gMap.Commit(T);
        if(ext) gMap.Request((double)gZone.Mult(), T, tpu * 3000., gPulseTick, gPulseTick >= 0. ? tpp : 0., ppq);
        gClkMult = (float)gMap.mult;
        gDiagClkFlags = (uint8_t)((gP2ExtEng ? 1u : 0u) | (gMap.Pending() ? 2u : 0u)
                                | (gDiagAlignHold ? 4u : 0u) | (uint8_t)((gDiagExtTrans & 15u) << 4));
    }
    {   /* Publish the clock snapshot for the audio thread (see ClockSnap). */
        const uint8_t wi = (uint8_t)(gSnapIdx ^ 1u);
        gSnap[wi].tick    = gClock.MasterTick();
        gSnap[wi].frac    = gClock.FracTick();
        gSnap[wi].tpu     = gClock.TicksPerUs();
        gSnap[wi].tick32  = gLastTick;   /* the tick NowUs() just read */
        gSnap[wi].running = gClock.Running();
        gSnap[wi].map     = gMap;
        __asm__ volatile("dmb" ::: "memory");   /* record complete before the index (review B5) */
        gSnapIdx = wi;
        /* Diagnostics: worst poll gap, worst audio callback, pulse jitter — rolled per second. */
        static uint32_t s_lastUs = 0, s_rollUs = 0, s_gapMax = 0;
        if(s_lastUs) { const uint32_t g = now_us - s_lastUs; if(g > s_gapMax) { s_gapMax = g; gDiagStallCur = gDiagPhase; } }
        s_lastUs = now_us;
        if(now_us - s_rollUs >= 1000000u)
        {
            s_rollUs       = now_us;
            gDiagPollGapUs = (uint16_t)(s_gapMax > 65535u ? 65535u : s_gapMax);
            const uint32_t a = gDiagAudioCur;
            gDiagAudioUs   = (uint16_t)(a > 65535u ? 65535u : a);
            gDiagStallPhase = gDiagStallCur; gDiagStallCur = 0;
            gDiagGateLateUs = (uint16_t)(gDiagGateLateCur > 65535u ? 65535u : gDiagGateLateCur); gDiagGateLateCur = 0;
            s_gapMax = 0; gDiagAudioCur = 0;
            uint16_t pk, rms; gJitter.Roll(pk, rms);
            gDiagJitterUs = pk; gDiagJitterRmsUs = rms;
            if(gDiagAlignHold) gDiagAlignHold--;
        }
    }
    /* ── end CLOCK ─────────────────────────────────────────────────────── */

    /* Voice map keymap: J6 gate · J4 slice v/oct · J5 style v/oct. The note IS
     * the slice (one key per slice region, up to two octaves from 0 V; one
     * semitone below 0 V toggles mute). The style note is read with the hit
     * and a CHANGE of style while the gate is held re-strikes the slice:
     * 0 plain · 1 reverse · 2 half-speed · 3 double · 4 ratchet · 5 staccato ·
     * 6 punch/choke flip. */
    /* RECORD: the record styles live on the Voice map — leaving it disarms and ends a key take */
    if(gJackMapCur != 1u)
    {
        gKeyArm = false; if(gKeyTake && !gWebTake) gKeyTakeDone = true;   /* a browser take runs on any map */
        static int s_heldOnLeave = -1;
        if(gCallHeld && gCallKey >= 0 && s_heldOnLeave != gCallKey) { gRelKey = gCallKey; s_heldOnLeave = gCallKey; }   /* a key held when the map left: release it (review L1) */
    }
    if(gJackMapCur == 1u)
    {
        static int  s_note = -99, s_style = 0, s_lastIdx = -1;
        static int  s_lastKey = -1;   /* SAMPLE: the key of the last call (0..23) */
        auto quant = [](float volts, int& last) {
            const float nf = volts * 12.f; int n = (int)floorf(nf + 0.5f);
            if(fabsf(nf - (float)n) < 0.4f) last = n; else n = last;
            return n; };
        const bool rose = gClkEdge.JustRose(3u);
        const bool held = hw.cv[3].Volts() > 2.5f;
        /* SAMPLE: the gate's falling edge releases the held key (a no-op for one-shot voices) */
        if(gClkEdge.JustFell(3u) && s_lastKey >= 0) gRelKey = s_lastKey;
        static int s_lastNote = -99, s_lastStyle = 0;
        const int n  = quant(hw.cv[kVoiceVoctChan].Volts(), s_lastNote);
        const int st = quant(hw.cv[kVoiceGateChan].Volts(), s_lastStyle);
        /* RECORD: J5 style 7 = Record (armed: every key held records into
         * that key), 8 = Clear (the key goes back to its grid slice); any
         * other style disarms. Neither calls the slice, neither re-strikes.
         * A running key take ends when the gate falls, the plane fills, or
         * the style moves off 7; the main loop finishes it (ServiceRecord). */
        const int  sty    = st < 0 ? 0 : st;
        const bool recSty = sty >= 7;
        gKeyArm = sty == 7 && !gTakeActive;
        if(gKeyTake && (engine.recFull || (!gWebTake && (gClkEdge.JustFell(3u) || sty != 7)))) gKeyTakeDone = true;   /* a browser take: only the cap or the next arm stops it */
        /* RECORD end */
        const bool restrike = held && s_lastIdx >= 0 && st != s_style && !rose && !recSty /* RECORD */;
        s_style = st;
        if(rose || restrike)
        {
            if(rose && n == -1) engine.muted = !engine.muted;
            /* RECORD */
            else if(recSty)
            {
                if(rose && n >= 0 && engine.nSlices > 0)
                {
                    const int key = n >= bb::Engine::kKeys ? bb::Engine::kKeys - 1 : n;
                    if(sty == 7)
                    {
                        if(!gKeyTake && !gKeyTakeDone && !gTakeActive && engine.RecArm(key))
                        { gKeyTake = true; gKeyTakeKey = key; gRecPeak = 0.f; }
                    }
                    else engine.ClearKey(key);   /* 8: Clear (forgets the recorded entry's key too, review L2) */
                }
            }
            /* RECORD end */
            else if(engine.nSlices > 0)
            {
                int idx = s_lastIdx;
                if(rose && n >= 0)
                {
                    /* SAMPLE: the engine's key table — the span mapping by default
                     * (k · nSlices / span, keys past the span play the last slice),
                     * a recorded key points at its take */
                    s_lastKey = n >= bb::Engine::kKeys ? bb::Engine::kKeys - 1 : n;
                    idx       = engine.KeySlice(s_lastKey);
                }
                if(idx >= 0)
                {
                    const int s = st < 0 ? 0 : (st > 6 ? 6 : st);
                    s_lastIdx  = idx;
                    gCallIdx   = idx;
                    gCallRev   = s == 1;
                    gCallMul   = s == 2 ? 0.5f : (s == 3 ? 2.f : 1.f);
                    gCallRat   = s == 4;
                    gCallLen   = s == 5 ? 0.5f : 1.f;
                    gCallPunch = gGrammar.mode == 6 ? false   /* Pads: never written */
                               : (((gGrammar.mode == 5) || gVoiceSel.sel == 1) != (s == 6));
                    gCallHeld  = gKeyPlays.sel == 1;   /* SAMPLE: Options → Key Plays = Held */
                    gCallKey   = s_lastKey;
                    gCallFlash = idx; gCallFlashT = 6;
                    gCallReq   = true;
                }
            }
        }
        s_note = n;
    }
    /* Brain map keymap: sample v/oct at each gate edge (MONO, latched mods). */
    else if(gClkEdge.JustRose(kVoiceGateChan))
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
            /* Play grammar: playing IS writing — the punch keyswitch flips to choke */
            gCallPunch = gGrammar.mode == 6 ? false
                       : gGrammar.mode == 5 ? !((gMods & kModPunch) != 0)
                                            : ((gVoiceSel.sel == 1) != ((gMods & kModPunch) != 0));
            gCallRat   = (gMods & kModRat) != 0;
            gCallLen   = 1.f;
            gCallFlash = gCallIdx; gCallFlashT = 6;
            gCallReq   = true;
        }
    }

    /* Gate CV writes — control thread (I²C-safe), on CHANGE only so J6's
     * MCP4728 isn't hit with a WriteAll+LDAC transaction every millisecond. */
    static float s_lastV[3] = {-1.f, -1.f, -1.f};
    for(int c = 0; c < (gJackMapCur == 1u ? 0 : 1); c++)   /* J6 only (I²C), Brain map only; J7/J8 are written by the audio thread */
    {
        /* GATES: consumes a pending retrigger — this poll low, next poll up */
        float v = bb::GatePollVolts(gGateCd[c], gGateLvl[c], gGateRetrig[c]);
        if(v != s_lastV[c])
        {
            if(v > 0.f && s_lastV[c] <= 0.f)   /* rising: how late after the arm? */
            {
                const uint32_t lat = (TickNow() - gGateArmUs[c]) / gTicksPerUs;
                if(lat < 2000000u && lat > gDiagGateLateCur) gDiagGateLateCur = lat;
            }
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
        /* LOOPER: in Play a tap undoes the last pass's overdub (dice means nothing there) */
        else if(gGrammar.mode == 5) { gPendUndo = true; }
        else         { gPendReroll = true; }
    }

    /* RECORD: loop record into the break (Options → Record = Arm). B3 tap
     * or the first J6 rising edge (Voice map: J6 is the gate in) starts a
     * take into the OTHER buffer from the record source. Clocked (a clock on
     * J3), the take is the Bars count of bars at the followed tempo (× P2's
     * multiplier) and stops itself on the bar; J6 edges during it are slice
     * MARKERS. Unclocked, B3 or J6 stops it. B3's tap (reslice) and hold
     * (load) are suppressed while armed; the arm drops after one take. */
    static bool s_b3_loaded = false;
    if(gRecord.sel == 1 || gTakeActive)
    {
        const bool j6  = gJackMapCur == 1u && gClkEdge.JustRose(3u);
        const bool tap = b3.FallingEdge();
        if(!gTakeActive)
        {
            if((tap || j6) && gRecord.sel == 1 && !gLoadReq && !gApply && !gReapply && !gKeyTake)
            {
                const bool clocked = gWasExt;
                const int  bars    = gBars.sel >= 1 ? 1 << (gBars.sel - 1) : (gGridBars > 0 ? gGridBars : 1);
                const int  target  = clocked ? bb::TakeSamples(bars, gClock.Bpm() * gClkMult, kSr) : 0;
                gTakeClocked = clocked && target > 0 && target <= kLoadMax;   /* longer than the plane: analysed like an unclocked take */
                gTakeBars    = gTakeClocked ? bars : 0;
                gNumMarkers  = 0;
                gRecPeak     = 0.f;
                gTake.Start(gLoad, gSampling.sel == bb::kSampStereo ? gLoadR : nullptr, kLoadMax, gTakeClocked ? target : 0);
                gTakeActive  = true;
            }
        }
        else if(gTake.running)
        {
            if(gTakeClocked) { if(j6 && gNumMarkers < kMaxOnsets) gMarkers[gNumMarkers++] = gTake.pos; }
            else if(tap || j6) gTake.Stop();
        }
        s_b3_loaded = false;
    }
    else
    {
    /* RECORD end */
    /* B3: tap = reslice · hold = load next WAV on the card. */
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
    }   /* RECORD */
    gDiagPhase = 2;
}

/* Clock keep-alive for long control-thread analyses (loads, re-slices): the
 * pure core calls bb::Yield() at ~ms granularity; we tick the NCO and
 * republish the audio thread's snapshot so its extrapolation never reaches
 * the 50 ms cap — the loop keeps time through a load (review M6). Same
 * thread as OnPoll, never re-entered from it. */
static void KeepClock()
{
    gDiagPhase = 9;
    const uint32_t now_us = NowUs();
    gClock.Tick(now_us);
    const uint8_t wi = (uint8_t)(gSnapIdx ^ 1u);
    gSnap[wi].tick    = gClock.MasterTick();
    gSnap[wi].frac    = gClock.FracTick();
    gSnap[wi].tpu     = gClock.TicksPerUs();
    gSnap[wi].tick32  = gLastTick;
    gSnap[wi].running = gClock.Running();
    gSnap[wi].map     = gMap;
    __asm__ volatile("dmb" ::: "memory");
    gSnapIdx = wi;
    /* SETS: through a set save / load the whole control loop keeps running —
     * panel, LEDs, HostLink — at 25 Hz. Never re-entered from inside itself
     * (a preset save inside Tick yields here too; a load request raised in
     * there waits for the main loop as always). */
    if(gSetState && !gInTick)
    {
        const uint32_t now = daisy::System::GetNow();
        if((uint32_t)(now - gSetTickMs) >= 40u)
        {
            gSetTickMs = now;
            gInTick    = true;
            loop.Tick();
            gInTick    = false;
        }
    }
}

/* ── render ─────────────────────────────────────────────────────────── */
static void OnRender(uint32_t /*t_ms*/)
{
    gDiagPhase = 5;
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
    /* RECORD: the three buttons red while armed (a key: J5 style 7; the
     * loop: Options → Record), full red while a take runs */
    const bool recArmed  = gKeyArm || gRecord.sel == 1 || gKeyTake || gTakeActive;
    const bool recTaking = gKeyTake || gTakeActive;
    if(recArmed)
    {
        const Rgb c = recTaking ? Rgb{255, 8, 0} : Rgb{80, 2, 0};
        hw.leds.SetButtonPair(kButtonB1, c);
        hw.leds.SetButtonPair(kButtonB2, c);
        hw.leds.SetButtonPair(kButtonB3, c);
    }
    /* RECORD end */

    /* FROZEN dials (Ernest, 2026-09-06): a pot that has not yet swept through
     * its stored value — after a page switch or a preset load — is inert
     * until it does. The SDK draws a white catch pip at the value to reach,
     * but our own overdraws hid it on P1/P3 and a single dot is easy to miss.
     * Redraw such a ring as a DIM blue-grey arc of the frozen value, with the
     * pip on top; the ring returns to its live colours the moment it catches. */
    {
        static const LedPanel::Rgb kFrozenCol = {14, 18, 40};
        for(uint8_t p = 0; p < kNumPots; p++)
        {
            if(pager.Caught(p)) continue;
            const PotState st = pager.State(pager.Page(), p);
            hw.leds.ClearRing(p);
            DrawLevelArc(hw.leds, p, kAlchemyLabArcGeometry, st.stored, {0.5f, 0.99f},
                         kFrozenCol, kFrozenCol, kFrozenCol);
            DrawCatchPip(hw.leds, p, st, kAlchemyLabArcGeometry, {0xFF, 0xFF, 0xFF});
        }
    }
    /* RECORD: the kit map on P5's ring — every key whose slot holds a
     * recording (24 keys over 16 LEDs: keys 0..7 own LEDs 0..7, keys 8..23
     * pair up on 8..15), the key being taken brighter; while a take runs,
     * the record input's level on P6's ring. Perform page only. */
    if(perfPage)
    {
        for(int k = 0; k < bb::Engine::kKeys; k++)
        {
            const bool taking = gKeyTake && k == gKeyTakeKey;
            if(taking || engine.RecordedEntry(k) >= 0)
                hw.leds.SetRingByOffset(P_SPACE, (uint8_t)bb::KeyLed(k), taking ? Rgb{255, 30, 10} : Rgb{160, 20, 8});
        }
        if(recTaking)
        {
            static float s_lvl = 0.f;
            const float pk = gRecPeak; gRecPeak = 0.f;
            s_lvl = pk > s_lvl ? pk : s_lvl * 0.85f;
            hw.leds.ClearRing(P_VOL);
            DrawLevelArc(hw.leds, P_VOL, kAlchemyLabArcGeometry, s_lvl > 1.f ? 1.f : s_lvl, {0.6f, 0.9f},
                         Rgb{20, 160, 60}, Rgb{220, 180, 30}, Rgb{255, 20, 10});
        }
    }
    /* RECORD end */
    /* SETS: a set save fills P5's ring with its progress, a load sweeps a dot
     * round it (B3 in the set colour meanwhile); a refused or failed op
     * blinks the three buttons red for a second and a half. */
    if(gSetState)
    {
        hw.leds.ClearRing(P_SPACE);
        if(gSetState == 1u)
            DrawLevelArc(hw.leds, P_SPACE, kAlchemyLabArcGeometry, (float)gSetProgress / 255.f, {0.5f, 0.99f}, kSpaceCol, kSpaceCol, kDimHi);
        else
        {
            static uint8_t s_dot = 0;
            s_dot = (uint8_t)((s_dot + 1u) % 16u);
            hw.leds.SetRingByOffset(P_SPACE, s_dot, kDimHi);
            hw.leds.SetRingByOffset(P_SPACE, (uint8_t)((s_dot + 15u) % 16u), kSpaceCol);
        }
        hw.leds.SetButtonPair(kButtonB3, kSpaceCol);
    }
    if(gSetErrT > 0.f)
    {
        gSetErrT -= 1.f;
        if((((int)gSetErrT) / 6) & 1)
        {
            const Rgb r{255, 8, 0};
            hw.leds.SetButtonPair(kButtonB1, r); hw.leds.SetButtonPair(kButtonB2, r); hw.leds.SetButtonPair(kButtonB3, r);
        }
    }
    /* SETS end */
    gDiagPhase = 6;
}

/* ── SD: scan + load (main-loop only, off the audio thread) ─────────── */
static void SortFiles(int from, int to)
{
    for(int i = from + 1; i < to; i++)
        for(int j = i; j > from && strcasecmp(gFiles[j - 1], gFiles[j]) > 0; j--)
        {
            char tmp[128];
            memcpy(tmp, gFiles[j - 1], 128);
            memcpy(gFiles[j - 1], gFiles[j], 128);
            memcpy(gFiles[j], tmp, 128);
        }
}
/* The loose WAVs of the root, sorted; then SETS: the WAVs of sets/, sorted,
 * as the entries "sets/<name>.wav" (the label shows "<name> [set]"; the
 * load path takes the JSON beside them). */
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
    SortFiles(0, gNumFiles);
    /* SETS */
    const int nRoot = gNumFiles;
    if(f_opendir(&gDir, "0:/sets") == FR_OK)
    {
        while(gNumFiles < kMaxFiles && f_readdir(&gDir, &gFno) == FR_OK && gFno.fname[0])
        {
            if(gFno.fattrib & AM_DIR) continue;
            const char* dot = strrchr(gFno.fname, '.');
            if(!dot || strcasecmp(dot, ".wav") != 0 || strlen(gFno.fname) > 120) continue;
            strcpy(gFiles[gNumFiles], "sets/");
            strcat(gFiles[gNumFiles], gFno.fname);
            gNumFiles++;
        }
        f_closedir(&gDir);
        SortFiles(nRoot, gNumFiles);
    }
    /* SETS end */
}

/* RECORD: the load pipeline's tail, shared by a card load and a loop take.
 * gLoad (+ gLoadR when `stereo`) holds `len` raw samples; `cues` are FILE
 * positions — a WAV's cue chunk, or a clocked take's J6 markers, the same
 * path; knownBars > 0 = a clocked take of exactly that many bars (no trim,
 * no bar cut: a silent first 16th is still the first 16th). Conditions →
 * onset table → bars → swing → classes → density → bands → envelope →
 * gApply (the audio thread swaps and calls SetMaterial). Control thread;
 * every long loop yields to the clock. */
static void PrepareMaterial(int len, int* cues, int nCues, bool stereo, int knownBars)
{
    /* SLICE: detector mode from Options → Onsets; the conditioning reports the
     * whole-bar count it cut the buffer to (0 when the prior applied) */
    gDet.mode = gOnsetDet.sel;
    bb::ConditionInfo ci;
    if(knownBars > 0) bb::NormalisePlanes(gLoad, stereo ? gLoadR : nullptr, len);
    else
    {
        len = bb::ConditionBuffer(gLoad, len, kSr, gDet, &ci, gBars.sel == 0 /* review M5 */);
        if(stereo) bb::ConditionPlane(gLoadR, ci, len);   /* the R plane takes the same cut and gain */
    }
    gCondBars = ci.bars > 0 ? ci.bars : knownBars;
    gPendCues = nCues >= 2;

    /* cue chunk positions are file positions: re-base by the head cut
     * (silence + downbeat) and drop what fell outside (review M1) —
     * bb::CuesToOnsets, the same code factored out */
    if(nCues >= 2) gPendN = bb::CuesToOnsets(cues, nCues, ci.trimmedStart, len, kSr, gPendOnsets, bb::Engine::kMaxSlices);
    else           gPendN = gDet.Detect(gLoad, len, kSr, gPendOnsets, kMaxOnsets, nullptr);

    int barsN  = 0;
    gPendHint  = SliceCountFor(gLoad, len, gPendOnsets, gPendN, gCondBars, &barsN);
    gGridBars  = barsN > 0 ? barsN : 1;
    gPendSwing = SwingFor(len, gPendOnsets, gPendN, gPendHint, barsN);   /* SLICE */
    ClassifySteps(gLoad, len, gPendClass, gClass.mode, gPendSal, gPendHint, gPendBands /* GATES */);
    ComputeDensity(gLoad, len, gPendDens, gPendHint);   /* PLAY */
    ComputeEnvelope(gLoad, len, gPendEnv, gPendHint);
    gPendLen    = len;
    gPendStereo = stereo;
    gLoadFlash  = 1.f;
    gSetCurName[0] = 0;     /* SETS: a loose file or a take, not a set */
    gApply      = true;     /* audio thread swaps + SetMaterial */
}

/* RECORD: stage a re-apply of the CURRENT material (the Bars handler's
 * recipe) — used when Options → Sampling adds or drops the R plane. */
static void StageReapply()
{
    int barsN = 0;
    const int nsl = SliceCountFor(gPlay, engine.bufLen, gPendOnsets, gPendN, gCondBars, &barsN);
    gPendSwing = SwingFor(engine.bufLen, gPendOnsets, gPendN, nsl, barsN);
    ClassifySteps(gPlay, engine.bufLen, gPendClass, gClass.mode, gPendSal, nsl, gPendBands);
    ComputeDensity(gPlay, engine.bufLen, gPendDens, nsl);
    ComputeEnvelope(gPlay, engine.bufLen, gPendEnv, nsl);
    gPendLen  = engine.bufLen;
    gPendHint = nsl;
    gReapply  = true;
}

/* RECORD: finish takes on the control thread (main loop, like ServiceLoad —
 * the trim and the analysis yield to the clock). A key take: RecStop →
 * TrimRegion → AnalyseRegion (the current classifier lens) → CommitSlice,
 * and the key plays it; a loop take: markers → cues → the load pipeline. */
static void ServiceRecord()
{
    if(gKeyTakeDone)
    {
        gKeyTakeDone = false;
        const int key = gKeyTakeKey;
        gKeyTake = false; gKeyTakeKey = -1; gWebTake = false;
        int rs = 0, rl = 0;
        if(engine.RecStop(rs, rl) > 0)
        {
            int ts = 0, tl = 0;
            if(bb::TrimRegion(gPlay, engine.bufR, rs, rl, kSr, ts, tl))
            {
                uint8_t cls = 3; float sal = 0.f, dens = 0.f; bool hit = false;
                bb::AnalyseRegion(gPlay, ts, tl, kSr, gClass.mode, cls, sal, dens, hit);
                if(engine.CommitSlice(key, ts, tl, cls, sal, dens, hit) < 0) engine.RecDiscard();
            }
            else engine.RecDiscard();
        }
        gRecPeak = 0.f;
    }
    if(gTakeActive && !gTake.running && !gApply && !gReapply)
    {
        gTakeActive = false;
        const int len = gTake.pos;
        if(gRecord.sel == 1) gRecord.sel = 0;   /* one take per arm */
        if(len >= (int)(0.25f * kSr))
        {
            /* a clocked take cut short (Record → Off mid-take) is analysed like an unclocked one */
            const int bars = (gTakeBars > 0 && len >= gTake.target) ? gTakeBars : 0;
            int cues[kMaxOnsets];
            const int nCues = bb::MarkersToCues(gMarkers, gNumMarkers, len, (int)(0.010f * kSr), cues, kMaxOnsets);
            gCurFile = -1;   /* the material is a take, not a card file */
            PrepareMaterial(len, cues, nCues, gTake.R != nullptr, bars);
        }
        gRecPeak = 0.f;
    }
}

/* ── SETS ── save / load a set, the preset backups (docs/packages/sample-sets.md).
 * Everything here runs on the control thread from the main loop, under a
 * BusyGuard, off the audio thread; the sink yields (KeepClock runs the whole
 * control loop every 40 ms while a set op is on). ──────────────────────── */
static void SetFinish(uint8_t result)
{
    gSetResult   = result;
    if(result) gSetErrT = 36.f;   /* OnRender: B1/B2/B3 blink red */
    gSetState    = 0;
    gSetProgress = 0;
    gSetSeq      = (uint8_t)(gSetSeq + 1u);
}
/* f_write as a bbwav::Sink — the whole adapter, plus progress and the yield.
 * A short write with FR_OK is FatFs's "disk full". */
static bool SdSink(void* ctx, const void* p, size_t n)
{
    UINT          bw = 0;
    const FRESULT fr = f_write((FIL*)ctx, p, (UINT)n, &bw);
    if(fr != FR_OK) { if(fr == FR_DISK_ERR || fr == FR_NOT_READY) sd.OnIoError(); return false; }
    if(bw != n) { gSetFull = true; return false; }
    gSetDone += (uint32_t)n;
    if(gSetTotal)
    {
        const uint32_t pr = (uint32_t)(((uint64_t)gSetDone * 255u) / gSetTotal);
        gSetProgress = (uint8_t)(pr > 255u ? 255u : pr);
    }
    bb::Yield();
    return true;
}
static bool CardReady() { return sd.EnsureMounted(daisy::System::GetNow()); }
static bool SetDirsOk()   /* FR_EXIST is success */
{
    const FRESULT a = f_mkdir("0:/sets");
    const FRESULT b = f_mkdir("0:/presets");
    return (a == FR_OK || a == FR_EXIST) && (b == FR_OK || b == FR_EXIST);
}
/* A set saved this session joins the list at the END, so every index a host
 * or a preset already holds stays valid; the next boot sorts it in. The
 * browser's dropdown (rendered at boot) learns it at the next connect. */
static int AddSetEntry(const char* name)
{
    char entry[64];
    if(!bb::SetEntry(name, entry, sizeof entry)) return -1;
    int i = bb::FindEntry(&gFiles[0][0], sizeof gFiles[0], gNumFiles, entry);
    if(i >= 0) return i;
    if(gNumFiles >= kMaxFiles) return -1;
    strncpy(gFiles[gNumFiles], entry, 127);
    gFiles[gNumFiles][127] = 0;
    i = gNumFiles++;
    k_sample.Selector((uint8_t)(gNumFiles > 255 ? 255 : gNumFiles));
    return i;
}
/* The looper snapshot: the audio thread copies its list into *gSetLooper
 * between two blocks (Insert's memmove must not be read half-way). */
static void SnapshotLooper()
{
    gSetSnapReq = true;
    {   /* wait ≥ 2 audio blocks by the clock, not by iterations (review M5) */
        const uint32_t t0 = TickNow();
        while(gSetSnapReq && (TickNow() - t0) < 2000u * gTicksPerUs) bb::Yield();
    }
    if(gSetSnapReq) { gSetSnapReq = false; SetFinish(6); return; }   /* the audio thread never ran: refuse rather than save a stale looper */
}
/* Save the playing material as sets/<name>: the WAV first (PCM16, both
 * planes when the material is stereo, cues = the table's starts), then the
 * JSON; a failed WAV leaves nothing, a failed JSON leaves the WAV (a plain
 * break is still a valid load). Reports through gSetResult / the LEDs. */
static void SaveSet(const char* name)
{
    if(gSetState) { SetFinish(6); return; }
    if(!bb::SetNameOk(name)) { SetFinish(4); return; }
    if(gKeyTake || gTakeActive || engine.recArmed || engine.recPending || gApply || gReapply || gLoadReq || engine.bufLen <= 0 || !engine.buf)
    { SetFinish(6); return; }
    if(!CardReady()) { SetFinish(1); return; }
    SdCard::BusyGuard busy(sd);
    gSetState = 1; gSetProgress = 0; gSetFull = false;
    if(!SetDirsOk()) { SetFinish(3); return; }
    SnapshotLooper();
    bb::SetInfo& s = *gSet;
    new(&s) bb::SetInfo();
    bb::SetFromEngine(engine, *gSetLooper, s, engine.bufR != nullptr, (int)kSr);
    strncpy(s.name, name, sizeof s.name - 1);
    s.bars     = gGridBars;
    s.bpm      = engine.NativeBpm();
    s.nOptions = (int)Options::kBytes;
    gOptions.Serialize(s.options);
    if(s.len <= 0 || s.nSlices <= 0) { SetFinish(6); return; }
    /* room: the WAV's exact size + the JSON's ceiling against FatFs's cached free count (unknown → try) */
    bbwav::WriteSpec spec;
    spec.left = engine.buf; spec.right = s.stereo ? engine.bufR : nullptr; spec.len = s.len; spec.sr = (uint32_t)kSr;
    spec.cues = nullptr; spec.nCues = s.nSlices;   /* size estimate only (review L8) */
    const uint32_t need = bbwav::FileSize(spec) + (uint32_t)bb::kSetJsonMax;
    uint32_t totalKib = 0, freeKib = 0; bool freeValid = false;
    if(sd.VolumeKib(totalKib, freeKib, freeValid) && freeValid && (uint64_t)freeKib * 1024u < (uint64_t)need + 65536u)
    { SetFinish(2); return; }
    char wav[64], json[64], lock[64];
    bb::SetPaths(name, wav, json, sizeof wav);
    bb::SetLockPath(name, lock, sizeof lock);
    sd.Lock(lock);
    gSetTotal = need - (uint32_t)bb::kSetJsonMax; gSetDone = 0;
    f_unlink(json);   /* a failed or interrupted save leaves at most a WAV, never a mismatched pair (review M2) */
    bool wavOk = f_open(&gSetFil, wav, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    if(wavOk)
    {
        wavOk = bb::SetWriter::Wav(bbwav::Sink{&SdSink, &gSetFil}, s, engine.buf, engine.bufR, gSetStage, sizeof gSetStage);
        wavOk = (f_close(&gSetFil) == FR_OK) && wavOk;
        if(!wavOk) f_unlink(wav);
    }
    bool jsonOk = false;
    if(wavOk)
    {
        gSetTotal = 0;
        jsonOk = f_open(&gSetFil, json, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
        if(jsonOk)
        {
            jsonOk = bb::SetWriter::Json(bbwav::Sink{&SdSink, &gSetFil}, s, gSetJson, sizeof gSetJson);
            jsonOk = (f_close(&gSetFil) == FR_OK) && jsonOk;
            if(!jsonOk) f_unlink(json);
        }
    }
    sd.Unlock(lock);
    if(!wavOk || !jsonOk) { SetFinish(gSetFull ? 2 : 3); return; }
    gCurFile = AddSetEntry(name);
    strncpy(gSetCurName, name, sizeof gSetCurName - 1); gSetCurName[sizeof gSetCurName - 1] = 0;
    SetFinish(0);
}
/* A set from the card into the load buffer: the WAV whole (no 17 s cap — a
 * set carries its recordings past the break, and the loader's DMA path is
 * the same), then the JSON: options (through Options::Deserialize, the
 * blob-write path, so the dirty flags fire — minus the re-slice ones, the
 * set's table IS that re-slice), the grid as SetMaterial rebuilds it from
 * its count, swing and hit onsets, classes and salience from the file, the
 * recorded entries after the swap (ServiceSets), the looper adopted by the
 * audio thread at the swap. No conditioning: a set was saved conditioned and
 * its positions are the buffer's. Returns 1 staged, 0 no JSON beside the
 * WAV (the caller takes the plain path), -1 failed and reported. */
static int LoadSet(const char* name, int fileIdx)
{
    char wav[64], json[64];
    if(!bb::SetPaths(name, wav, json, sizeof wav)) { SetFinish(4); return -1; }
    if(!CardReady()) { SetFinish(1); return -1; }
    SdCard::BusyGuard busy(sd);
    if(f_stat(wav, &gFno) != FR_OK) { SetFinish(5); return -1; }
    if(f_stat(json, &gFno) != FR_OK) return 0;
    gSetState = 2; gSetProgress = 0;
    if(f_open(&gSetFil, json, FA_READ) != FR_OK) { SetFinish(7); return -1; }
    const uint32_t sz = (uint32_t)f_size(&gSetFil);
    UINT       br = 0;
    const bool rd = sz > 0 && sz < sizeof gSetJson && f_read(&gSetFil, gSetJson, (UINT)sz, &br) == FR_OK && br == sz;
    f_close(&gSetFil);
    bb::Yield();
    if(!rd || !bb::SetReader::Json(gSetJson, sz, *gSet) || gSet->sr != (int)kSr) { SetFinish(7); return -1; }
    /* options first: the set's grid was cut under them */
    uint8_t opt[Options::kBytes];
    gOptions.Serialize(opt);
    bb::MergeOptions(*gSet, opt, opt, (int)Options::kBytes);
    opt[20] = 0;   /* Record: a load never arms a take */
    gOptions.Deserialize(opt);
    gBars.dirty = false; gSwing.dirty = false; gOnsetDet.dirty = false; gSampling.dirty = false; gClass.dirty = false;
    const bool wantR = gSampling.sel == bb::kSampStereo && gSet->stereo;
    bool gotR = false;
    int  cues[kMaxOnsets], nCues = 0, err = 0;
    const int len = bbwav::Load(wav, gStage, kStageMax, gLoad, kBufMax, kSr, kMaxSec, cues, &nCues, kMaxOnsets, &err,
                                wantR ? gLoadR : nullptr, wantR ? gStageR : nullptr, &gotR);
    if(len <= 0 || len < gSet->len) { SetFinish(7); return -1; }   /* unreadable, or cut short of what the JSON describes */
    if(!bb::PlanSet(*gSet, gSetPlan)) { SetFinish(7); return -1; }
    const bb::SetPlan& p = gSetPlan;
    /* stage what PrepareMaterial stages, minus the conditioning */
    const int nsl = p.nGrid;
    gDet.mode  = gOnsetDet.sel;
    gCondBars  = gSet->bars;
    gPendCues  = true;   /* Options → Onsets does not replace the set's hits */
    gPendN     = p.nOnsets;
    memcpy(gPendOnsets, p.onsets, sizeof(int) * (size_t)p.nOnsets);
    gGridBars  = gSet->bars > 0 ? gSet->bars : (nsl / 16 > 0 ? nsl / 16 : 1);
    gPendSwing = p.swing;
    ClassifySteps(gLoad, p.material, gPendClass, gClass.mode, gPendSal, nsl, gPendBands);   /* the band masks are not in the file */
    for(int i = 0; i < nsl; i++) { gPendClass[i] = p.cls[i]; gPendSal[i] = p.sal[i]; }       /* the file's classes and salience win */
    ComputeDensity(gLoad, p.material, gPendDens, nsl);
    ComputeEnvelope(gLoad, p.material, gPendEnv, nsl);
    gPendLen    = p.material;
    gPendHint   = nsl;
    gPendStereo = gotR;
    gSetPendLooper = false;
    const int nEv = bb::LooperFromSet(*gSet, *gSetLooper);
    gSetLooper->divisions = nsl;
    gSetPendLooper = nEv > 0;          /* the audio thread adopts it right after SetMaterial (which empties the loop) */
    gSetPendCommit = p.nRec > 0;       /* CommitSlice needs the new planes: after the swap */
    gCurFile = fileIdx;
    strncpy(gSetCurName, name, sizeof gSetCurName - 1); gSetCurName[sizeof gSetCurName - 1] = 0;
    gLoadFlash = 1.f;
    gApply     = true;
    SetFinish(0);
    return 1;
}
/* presets/<slot>.json holds the SLOT's bytes (not the live state — a host
 * may import into a slot other than the playing one), found through the
 * managed order: pager · locks · sample · options · settings · name. */
static bool ReadSlotComponent(uint8_t slot, const Serializable* which, uint8_t* dst, size_t cap)
{
    uint32_t off = 0;
    for(uint8_t i = 0; i < presets.NumManaged(); i++)
    {
        const Serializable* c = presets.ManagedAt(i);
        if(!c) return false;
        const size_t n = c->SerializedSize();
        if(c == which)
        {
            if(n > cap || n > 0xFFFFu) return false;
            return presets.ReadSlotBytes(slot, off, dst, (uint16_t)n, nullptr, nullptr) == (int32_t)n;
        }
        off += (uint32_t)n;
    }
    return false;
}
static bool WritePresetBackup(uint8_t slot)
{
    if(!CardReady()) return false;
    SdCard::BusyGuard busy(sd);
    if(!SetDirsOk()) return false;
    static bb::PresetInfo p;   /* ~250 B: keep it off the stack */
    new(&p) bb::PresetInfo();
    p.slot = slot;
    uint8_t b[Pager::kMaxPages * kNumPots * sizeof(float)];
    if(ReadSlotComponent(slot, &pager, b, sizeof b))
    {
        p.nPages = pager.NumPages() < bb::kSetMaxPages ? pager.NumPages() : bb::kSetMaxPages;
        for(int pg = 0; pg < p.nPages; pg++)
            for(int k = 0; k < bb::kSetPots && k < kNumPots; k++)
                memcpy(&p.pager[pg][k], b + ((size_t)pg * kNumPots + (size_t)k) * sizeof(float), sizeof(float));
    }
    if(ReadSlotComponent(slot, &gOptions, b, sizeof b)) { p.nOptions = (int)Options::kBytes; memcpy(p.options, b, Options::kBytes); }
    if(gNumFiles > 0 && ReadSlotComponent(slot, &gSample, b, sizeof b) && b[0] < gNumFiles)
    { strncpy(p.sample, gFiles[b[0]], sizeof p.sample - 1); }
    if(gPresetName && ReadSlotComponent(slot, gPresetName, b, sizeof b))
    { memcpy(p.name, b, PresetName::kCapacity < sizeof p.name ? PresetName::kCapacity : sizeof p.name); p.name[sizeof p.name - 1] = 0; }
    char path[40];
    if(!bb::PresetPath(slot, path, sizeof path)) return false;
    gSetTotal = 0;
    bool ok = f_open(&gSetFil, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    if(ok)
    {
        ok = bb::PresetBackup::Write(bbwav::Sink{&SdSink, &gSetFil}, p, gSetJson, sizeof gSetJson);
        ok = (f_close(&gSetFil) == FR_OK) && ok;
        if(!ok) f_unlink(path);
    }
    return ok;
}
/* Every preset save — the Settings gesture, a host PRESET_SAVE, an import —
 * moves the slot's flash sequence; the poll (4 Hz, two header reads per
 * slot) sees it and writes the backup. A slot erased loses its backup too,
 * or the next boot would resurrect it. */
static void ServicePresetBackup()
{
    const uint32_t now = daisy::System::GetNow();
    if((uint32_t)(now - gPresetPollMs) < 250u) return;
    gPresetPollMs = now;
    for(uint8_t s = 0; s < Presets::kNumSlots; s++)
    {
        const Presets::SlotInfo in  = presets.InfoFor(s);
        const uint32_t          seq = in.valid ? in.seq + 1u : 0u;
        if(seq == gPresetSeq[s]) continue;
        const bool wasValid = gPresetSeq[s] != 0u;
        gPresetSeq[s] = seq;
        if(seq) { if(in.schema_match) WritePresetBackup(s); }
        else if(wasValid)
        {
            char path[40];
            if(bb::PresetPath(s, path, sizeof path) && CardReady()) { SdCard::BusyGuard busy(sd); f_unlink(path); }
        }
    }
}
static void PrimePresetSeq()
{
    for(uint8_t s = 0; s < Presets::kNumSlots; s++)
    {
        const Presets::SlotInfo in = presets.InfoFor(s);
        gPresetSeq[s] = in.valid ? in.seq + 1u : 0u;
    }
}
/* Boot: a slot the flash no longer honours — empty, or its schema hash moved
 * with the managed set ("presets re-init on OPT bumps") — whose backup is on
 * the card comes back: options + pager + sample + name → live → Save(slot),
 * valid again under the new schema. Runs before StartAudio; the caller
 * re-loads slot 0 afterwards so the live state is the boot preset's. */
static void ApplyPresetBackup(const bb::PresetInfo& p)
{
    uint8_t opt[Options::kBytes];
    gOptions.Serialize(opt);
    for(int i = 0; i < p.nOptions && i < (int)Options::kBytes; i++) opt[i] = p.options[i];
    opt[20] = 0;   /* Record: never armed by a restore */
    gOptions.Deserialize(opt);
    if(p.nPages > 0)
    {
        uint8_t pb[Pager::kMaxPages * kNumPots * sizeof(float)];
        pager.Serialize(pb);
        for(int pg = 0; pg < p.nPages && pg < pager.NumPages(); pg++)
            for(int k = 0; k < bb::kSetPots && k < kNumPots; k++)
                memcpy(pb + ((size_t)pg * kNumPots + (size_t)k) * sizeof(float), &p.pager[pg][k], sizeof(float));
        pager.Deserialize(pb);
    }
    if(gNumFiles > 0 && p.sample[0])
    {
        const int i = bb::FindEntry(&gFiles[0][0], sizeof gFiles[0], gNumFiles, p.sample);
        if(i >= 0 && i < 256) { const uint8_t b = (uint8_t)i; gSample.Deserialize(&b); }
    }
    if(gPresetName) gPresetName->Set(p.name);
}
static int RestorePresetBackups()
{
    if(!CardReady()) return 0;
    int restored = 0;
    for(int s = (int)Presets::kNumSlots - 1; s >= 0; s--)
    {
        if(presets.HasValid((uint8_t)s)) continue;
        char path[40];
        if(!bb::PresetPath(s, path, sizeof path)) continue;
        SdCard::BusyGuard busy(sd);
        if(f_open(&gSetFil, path, FA_READ) != FR_OK) continue;
        const uint32_t sz = (uint32_t)f_size(&gSetFil);
        UINT       br = 0;
        const bool rd = sz > 0 && sz < sizeof gSetJson && f_read(&gSetFil, gSetJson, (UINT)sz, &br) == FR_OK && br == sz;
        f_close(&gSetFil);
        static bb::PresetInfo p;
        if(!rd || !bb::PresetBackup::Read(gSetJson, sz, p)) continue;
        ApplyPresetBackup(p);
        if(presets.Save((uint8_t)s)) restored++;
    }
    return restored;
}
/* Main loop: the recordings of a loaded set once the audio thread has the
 * planes, ACTION 14 / 15, the preset backups. */
static void ServiceSets()
{
    if(gSetPendCommit && !gApply)
    {
        gSetPendCommit = false;
        bb::CommitSetRecordings(engine, *gSet, gSetPlan, [](int start, int len) {
            uint8_t cls = 3; float sal = 0.f, dens = 0.f; bool hit = false;
            bb::AnalyseRegion(engine.buf, start, len, kSr, gClass.mode, cls, sal, dens, hit);
            return dens; });
    }
    if(gSetReq && !gLoadReq && !gApply && !gReapply && !gTakeActive && !gSetState)
    {
        const uint8_t req = gSetReq;
        gSetReq = 0;
        if(req == 1u) SaveSet(gSetReqName);
        else { gLoadIdx = -1; gLoadReq = true; }   /* by name: ServiceLoad resolves it */
    }
    ServicePresetBackup();
}
/* ── end SETS ──────────────────────────────────────────────────────────── */

/* Runs between control ticks; does the heavy SD read → condition → classify
 * into gLoad, then hands off to the audio thread via gApply. */
static void ServiceLoad()
{
    gDiagPhase = 8;
    if(!gLoadReq || gApply || gReapply || gTakeActive /* RECORD: the loop take owns gLoad */) return;   /* review L1 */
    gLoadReq = false;

    /* SETS: the entry — from the list, or ACTION 15's name (gLoadIdx = -1) */
    char entry[128];
    int  idx = gLoadIdx;
    if(idx >= 0 && idx < gNumFiles) { strncpy(entry, gFiles[idx], 127); entry[127] = 0; }
    else if(idx < 0 && bb::SetEntry(gSetReqName, entry, sizeof entry)) idx = bb::FindEntry(&gFiles[0][0], sizeof gFiles[0], gNumFiles, entry);
    else return;
    char stem[bb::kSetMaxName];
    if(bb::SetStem(entry, stem, sizeof stem))
    {
        const int r = LoadSet(stem, idx);
        if(r != 0) return;   /* staged, or failed and reported; 0 = a WAV without a JSON: the plain path */
    }
    /* SETS end */

    char path[140];
    path[0] = '0'; path[1] = ':'; path[2] = '/';
    strncpy(path + 3, entry, 136);
    path[139] = 0;

    /* RECORD: at most kLoadSec (17 s) of the 25 s plane, so a take into a key
     * always finds room; Sampling = Stereo keeps a stereo file's planes apart */
    const bool wantR = gSampling.sel == bb::kSampStereo;
    bool gotR = false;
    int cues[kMaxOnsets], nCues = 0, err = 0;
    int len = bbwav::Load(path, gStage, kStageMax, gLoad, kLoadMax, kSr,
                          kLoadSec, cues, &nCues, kMaxOnsets, &err,
                          wantR ? gLoadR : nullptr, wantR ? gStageR : nullptr, &gotR);
    if(len <= 0) { if(gLoadIdx < 0) SetFinish(7); return; }   /* leave current material playing on failure; SETS: a by-name miss is reported */

    gCurFile = idx;
    PrepareMaterial(len, cues, nCues, gotR, 0);   /* RECORD: the shared tail */
}

/* ── StateExt: the web frontend's window into the instrument ────────────
 * HostLink extension claiming 0x60–0x6F. All handlers run on the control
 * thread; engine arrays are read as-is (a torn read is one frame of pixels).
 *   0x60 GET_STATE  → u8 status, u8 nSlices, u8 curStep, u8 flags
 *                     (b0 muted b1 ext b2 locked b3 flowing b4 brainPage),
 *                     u8 matGen, u8 grammar, u8 classifier, u8 curFile,
 *                     u8 chop(0..255), u8 histN, u8 histDepth, u8 callFlash,
 *                     f32 bpm, f32 mult, i8 stepSrc[64], u8 stepClass[64],
 *                     u16 pollGapUs, u16 audioUs, u16 jumps, u16 regens (diag),
 *                     u16 lockMask, f32 nativeBpm, u8 gridBars, u8 barsOpt (v0.8.0)
 *                     u16 jitterUs, u8 multZone, u8 clkFlags, u16 jitterRmsUs (CLOCK, 164..169)
 *                     … u8 jackMap (175), u8 looperEvents (176, LOOPER)
 *   0x63 ACTION ops 7 clear locks · 8 page(u8 0/1)
 *   0x61 GET_ENV    ← u8 first, u8 count(≤30) → u8 status, u8 first, u8 n,
 *                     then n × (8 × {u8 min, u8 max})
 *   0x62 SCOPE      → u8 status, u16 n(256), i8 samples[256] (×4 decimated)
 *   0x63 ACTION     ← u8 op [,args]: 0 dice · 1 reslice · 2 mute toggle ·
 *                     3 punch(u8 step, u8 slice) · 4 flow(u8 on) ·
 *                     5 scrub(u8 depth) · 6 load(u8 fileIdx)      → u8 status */
/* WEB ── sample-mode shell state the browser reads and the flags it raises.
 * Phase F (web) defines these so the branch compiles alone; the RECORD and
 * LOOPER phases own the real ones — de-duplicate on merge (same names).
 *   gRecState  0 idle · 1 armed (waiting for the gate) · 2 taking · 3 pending
 *   gRecKey    key of the take in flight, 255 none
 *   gInLevel   peak |input| of the last audio block, 0..255
 * While gRecState stays 0 the state tail derives the record state from the
 * engine (recArmed / recPending / recKey), so the page shows engine-driven
 * takes even before RECORD lands.
 *   0x64 GET_WAVE   ← u32 start, u16 n (≤480), u8 plane (0 L · 1 R · 2 mono sum)
 *                     [, u8 step (1..128: each output byte = the largest-magnitude
 *                     sample of `step` source samples)]
 *                   → u8 status, u32 start, u16 n, u8 step, i8 x[n]   (BAD_ARGS past freePos)
 *   0x65 GET_TABLE  ← [u8 first, u8 count] → u8 status, u8 nSlices, u8 nTable, then per
 *                     entry {u32 start, u32 len, i8 key, u8 cls, u8 sal, u8 hit, u8 dens}
 *                     (13 bytes; ≤39 per reply — chunk with first/count; count from length)
 *   0x66 GET_HITS   ← [u8 first] → u8 status, u8 nTotal, u8 first, u32 pos[…] (≤127 per reply)
 *   0x63 ACTION     9 rec arm(u8 key) · 10 rec clear(u8 key) · 11 key plays(u8 0/1) ·
 *                   12 looper undo · 13 looper clear ·
 *                   SETS: 14 save set(name bytes) · 15 load set(name bytes)
 *   state tail      176 u8 looperEvents (LOOPER) · 177 u8 recState · 178 u8 recKey ·
 *                   179 u32 freePos · 183 u32 bufCap · 187 u8 stereo · 188 u8 inLevel ·
 *                   189 u8 nTable · 190 u32 bufLen · 194 i8 keySlice[24] · 218 u8 keyPlays ·
 *                   SETS: 219 u8 setState · 220 u8 setProgress · 221 u8 setResult ·
 *                   222 u8 setSeq · 223 char setName[32] (ends at 255) */
/* WEB → RECORD: ACTION 9 / 10. -1 = nothing requested; the consumer clears
 * the flag after acting on it (the same path as the panel's record gestures). */
static volatile int8_t  gWebRecArm   = -1;    /* WEB → RECORD: arm a take into this key */
static volatile int8_t  gWebRecClear = -1;    /* WEB → RECORD: clear this key's recording */
/* WEB → LOOPER: ACTION 12 / 13. The consumer clears the flag after acting. */
static volatile bool    gWebLooperUndo  = false;   /* WEB → LOOPER */
static volatile bool    gWebLooperClear = false;   /* WEB → LOOPER */
/* WEB */

struct StateExt : hostlink::IHostlinkExtension
{
    uint8_t FirstCmd() const override { return 0x60u; }
    uint8_t LastCmd()  const override { return 0x6Fu; }
    const char* DescriptorRootJson() const override
    {
        /* WEB: wave/table/hits (0x64..0x66) advertised so the page can feature-detect the Kit tab. */
        /* SETS: "sets":1 = ACTION 14 / 15 and the 219.. state tail. */
        return "\"audiothurgist\":{\"ext\":1,\"state\":96,\"env\":97,\"scope\":98,\"action\":99,\"wave\":100,\"table\":101,\"hits\":102,\"sets\":1}";
    }
    /* WEB: one float → i8 (±127), saturating. */
    static int8_t ToI8(float v)
    {
        float s = v * 127.f;
        if(s > 127.f) s = 127.f; else if(s < -127.f) s = -127.f;
        return (int8_t)(s >= 0.f ? s + 0.5f : s - 0.5f);
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
                /* diagnostics tail (v0.7.3): u16 pollGapUs, u16 audioUs, u16 jumps, u16 regens */
                w.U16(gDiagPollGapUs); w.U16(gDiagAudioUs); w.U16(gDiagJumps); w.U16(gDiagRegens);
                /* v0.8.0 tail: u16 lockMask (bit page*6+pot), f32 nativeBpm, u8 bars(grid), u8 barsOpt */
                uint16_t lm = 0;
                for(uint8_t pg = 0; pg < 2; pg++)
                    for(uint8_t p = 0; p < kNumPots; p++)
                        if(locks.IsActiveAtPage(pg, p)) lm |= (uint16_t)(1u << (pg * kNumPots + p));
                w.U16(lm);
                float nb = engine.NativeBpm(); memcpy(&u, &nb, 4); w.U32(u);
                w.U8((uint8_t)(engine.nSlices / 16)); w.U8(gBars.sel);
                /* CLOCK tail (164..169): u16 jitterUs = peak per-pulse period deviation over the
                 * last second; u8 multZone = the REQUESTED P2 zone 0..4 (÷4 ÷2 ×1 ×2 ×4 — f32 mult
                 * above is the applied one); u8 clkFlags b0 P2 engaged · b1 switch pending ·
                 * b2 lock aligned (~2 s) · b4-7 ext transitions mod 16; u16 jitterRmsUs. */
                w.U16(gDiagJitterUs); w.U8((uint8_t)gZone.zone); w.U8(gDiagClkFlags); w.U16(gDiagJitterRmsUs);
                /* v0.9.5 forensics: u8 stallPhase, u16 gateLateUs, u16 overruns (170..174) */
                w.U8(gDiagStallPhase); w.U16(gDiagGateLateUs); w.U16(gDiagOverruns);
                w.U8(gJackMapCur == 1u ? 1u : 0u);   /* 175: jack map (0 Brain, 1 Voice) */
                /* WEB: sample-mode tail (176..218), fed by RECORD's state and LOOPER's count. */
                w.U8((uint8_t)(engine.looper.n > 255 ? 255 : engine.looper.n));   /* 176: loop events (LOOPER) */
                {
                    uint8_t rs = (gKeyTake || gTakeActive) ? 2u
                               : (gKeyArm || gRecord.sel == 1) ? 1u
                               : engine.recPending ? 3u : 0u;
                    uint8_t rk = gKeyTakeKey >= 0 ? (uint8_t)gKeyTakeKey : (engine.recKey < 0 ? 255u : (uint8_t)engine.recKey);
                    w.U8(rs); w.U8(rk);                                        /* 177, 178 */
                }
                w.U32((uint32_t)engine.freePos); w.U32((uint32_t)engine.bufCap);   /* 179, 183 */
                { float pk = gRecPeak; if(pk < 0.f) pk = 0.f; if(pk > 1.f) pk = 1.f; w.U8(engine.bufR ? 1u : 0u); w.U8((uint8_t)(pk * 255.f)); }   /* 187, 188 */
                w.U8((uint8_t)engine.nTable); w.U32((uint32_t)engine.bufLen); /* 189, 190 */
                w.Bytes((const uint8_t*)engine.keySlice, (size_t)bb::Engine::kKeys);   /* 194..217 */
                w.U8(gKeyPlays.sel);                                          /* 218: Options → Key Plays */
                /* SETS tail (219..254): u8 setState (0 idle · 1 saving · 2 loading) · u8 progress 0..255 ·
                 * u8 result of the last op (0 ok · 1 no card · 2 card full · 3 write failed · 4 bad name ·
                 * 5 not found · 6 busy · 7 bad file) · u8 seq (bumps per finished op) · char name[32] of
                 * the set the material came from (NUL-padded; empty = a loose file or a take). */
                w.U8(gSetState); w.U8(gSetProgress); w.U8(gSetResult); w.U8(gSetSeq);
                w.Bytes((const uint8_t*)gSetCurName, sizeof gSetCurName);
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
                            engine.PunchStep(st, f.body[2], false);
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
                    case 7: locks.Clear(); break;              /* clear every motion lock */
                    case 8:
                    {
                        float ph[kNumPots];
                        for(int i = 0; i < kNumPots; i++) ph[i] = hw.pots[i].Value();
                        pager.GoToPage(f.len >= 2 && f.body[1] ? 1u : 0u, ph);
                        break;
                    }
                    /* WEB: sample-mode ops. 9/10 and 12/13 raise the flags RECORD / LOOPER
                     * consume on their gesture paths; 11 is the Key Plays option itself. */
                    case 9:   /* WEB → RECORD: arm a take into key */
                        if(f.len < 2 || f.body[1] >= bb::Engine::kKeys) { w.U8(2u); return; }
                        /* a second arm from the browser STOPS a browser take (review H1) */
                        if(!(gKeyTake && gWebTake) && (engine.recArmed || engine.recPending || gKeyTake || gTakeActive)) { w.U8(3u); return; }   /* BAD_STATE */
                        gWebRecArm = (int8_t)f.body[1];
                        break;
                    case 10:  /* WEB → RECORD: clear key's recording */
                        if(f.len < 2 || f.body[1] >= bb::Engine::kKeys) { w.U8(2u); return; }
                        if(engine.RecordedEntry(f.body[1]) < 0) { w.U8(3u); return; }                    /* BAD_STATE */
                        gWebRecClear = (int8_t)f.body[1];
                        break;
                    case 11:  /* Key Plays: 0 one-shot · 1 held (Options byte 17, same as a blob write) */
                        if(f.len < 2 || f.body[1] > 1u) { w.U8(2u); return; }
                        if(f.body[1] != gKeyPlays.sel) { gKeyPlays.sel = f.body[1]; gKeyPlays.dirty = true; }
                        break;
                    case 12: gWebLooperUndo  = true; break;   /* WEB → LOOPER */
                    case 13: gWebLooperClear = true; break;   /* WEB → LOOPER */
                    /* SETS: 14 save set(name bytes) · 15 load set(name bytes) — the name is the
                     * rest of the body (1..31 of [A-Za-z0-9_-]); the main loop does the work,
                     * the state tail (219..) reports it. BAD_STATE while an op runs or a load waits. */
                    case 14:
                    case 15:
                    {
                        const size_t n = (size_t)f.len - 1u;
                        if(n < 1u || n > (size_t)bb::kSetNameMax) { w.U8(2u); return; }
                        char nm[bb::kSetMaxName];
                        memcpy(nm, f.body + 1, n); nm[n] = 0;
                        if(!bb::SetNameOk(nm)) { w.U8(2u); return; }
                        if(gSetState || gSetReq || gLoadReq || gApply || gReapply || gTakeActive || (op == 14u && (gKeyTake || engine.recArmed || engine.recPending)))
                        { w.U8(3u); return; }
                        memcpy(gSetReqName, nm, n + 1u);
                        gSetReq = op == 14u ? 1u : 2u;
                        break;
                    }
                    default: w.U8(2u); return;   /* BAD_ARGS */
                }
                w.U8(0u);
                return;
            }
            /* WEB: 0x64 GET_WAVE — the material buffer, windowed and optionally decimated.
             * Bounded by freePos (recordings live past bufLen); a take in flight is not
             * exposed until it is committed. ≤ 480 × 128 source samples per call (~1 ms). */
            case 0x64u:
            {
                if(f.len < 7) { w.U8(2u); return; }   /* BAD_ARGS */
                uint32_t start = (uint32_t)f.body[0] | ((uint32_t)f.body[1] << 8) | ((uint32_t)f.body[2] << 16) | ((uint32_t)f.body[3] << 24);
                uint32_t n     = (uint32_t)f.body[4] | ((uint32_t)f.body[5] << 8);
                const uint8_t plane = f.body[6];
                uint32_t step = f.len >= 8 ? f.body[7] : 1u;
                if(step == 0u) step = 1u;
                if(step > 128u) step = 128u;
                if(n > 480u) n = 480u;
                const float* L = engine.buf; const float* R = engine.bufR ? engine.bufR : engine.buf;
                const uint32_t lim = (uint32_t)(engine.freePos > 0 ? engine.freePos : 0);
                if(!L || plane > 2u || start >= lim) { w.U8(2u); return; }   /* BAD_ARGS */
                if(start + n * step > lim) n = (lim - start) / step;         /* whole steps only */
                if(n == 0u) { w.U8(2u); return; }
                w.U8(0u); w.U32(start); w.U16((uint16_t)n); w.U8((uint8_t)step);
                const float* P = plane == 1u ? R : L;
                for(uint32_t i = 0; i < n; i++)
                {
                    const uint32_t a = start + i * step;
                    float best = 0.f, mag = -1.f;
                    for(uint32_t k = 0; k < step; k++)
                    {
                        float v = plane == 2u ? 0.5f * (L[a + k] + R[a + k]) : P[a + k];
                        float m = v < 0.f ? -v : v;
                        if(m > mag) { mag = m; best = v; }
                    }
                    w.U8((uint8_t)ToI8(best));
                }
                return;
            }
            /* WEB: 0x65 GET_TABLE — every table entry, grid then recorded, 13 bytes each. */
            case 0x65u:
            {
                const int nt = engine.nTable;
                int first = f.len >= 1 ? f.body[0] : 0;
                int count = f.len >= 2 ? f.body[1] : 39;
                if(first > nt) { w.U8(2u); return; }   /* BAD_ARGS (first == nt → empty reply) */
                if(count > 39) count = 39;
                if(first + count > nt) count = nt - first;
                w.U8(0u); w.U8((uint8_t)engine.nSlices); w.U8((uint8_t)nt);
                for(int t = first; t < first + count; t++)
                {
                    w.U32((uint32_t)engine.slices[t].start); w.U32((uint32_t)engine.slices[t].len);
                    w.U8((uint8_t)(t >= engine.nSlices ? engine.tableKey[t] : -1));
                    w.U8(engine.stepClass[t] & 3u);
                    float sal = engine.sliceSal[t];  if(sal < 0.f) sal = 0.f; if(sal > 1.f) sal = 1.f;
                    w.U8((uint8_t)(sal * 255.f + 0.5f));
                    w.U8(engine.stepHit[t] ? 1u : 0u);
                    /* sliceDens is keyframes per sample; the adaptive fade spans 0.02 (sparse) .. 0.2 (dense) */
                    float d = engine.sliceDens[t] * 5.f; if(d < 0.f) d = 0.f; if(d > 1.f) d = 1.f;
                    w.U8((uint8_t)(d * 255.f + 0.5f));
                }
                return;
            }
            /* WEB: 0x66 GET_HITS — the shell's onset table for the current material
             * (gPendOnsets / gPendN: refilled by every load and re-slice, read after apply). */
            case 0x66u:
            {
                int n = gPendN; if(n < 0) n = 0; if(n > kMaxOnsets) n = kMaxOnsets;
                int first = f.len >= 1 ? f.body[0] : 0;
                if(first > n) { w.U8(2u); return; }   /* BAD_ARGS */
                int count = n - first; if(count > 127) count = 127;
                w.U8(0u); w.U8((uint8_t)n); w.U8((uint8_t)first);
                for(int i = first; i < first + count; i++) w.U32((uint32_t)(gPendOnsets[i] < 0 ? 0 : gPendOnsets[i]));
                return;
            }
            default: w.U8(1u); return;   /* UNSUPPORTED */
        }
    }
};
static StateExt gStateExt;

int main()
{
    /* 480 MHz (Luke approved 2026-09-07; local SDK patch until Hermetic's point
     * release carries the same option). TIM2 then ticks at 240 MHz — our time
     * base reads the tick rate at boot, so nothing else changes. */
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kEngineBlockSamples, true);
    gTicksPerUs = daisy::System::GetTickFreq() / 1000000u;
    if(gTicksPerUs == 0u) gTicksPerUs = 1u;
    gLastTick   = daisy::System::GetTick();
    locks.Init();                                   /* SDRAM arena is NOLOAD */
    memset(&gBarScratchMem, 0, sizeof gBarScratchMem);
    bb::bardetail::gBarScratch = &gBarScratchMem;
    hw.j6.EnableCvOutput();   /* kick gate  */
    hw.j7.EnableCvOutput();   /* snare gate */
    hw.j8.EnableCvOutput();   /* hat gate   */
    sd.Init();
    sd.EnsureMounted(0);
    /* SETS: NOLOAD placements — the writer's FIL zeroed as the SDK does its
     * FATFS; the SDRAM objects constructed in place (their NSDMI would
     * otherwise run before SDRAM is up). */
    memset(&gSetFil, 0, sizeof gSetFil);
    gSet       = new(gSetInfoMem) bb::SetInfo();
    gSetLooper = new(gSetLooperMem) bb::Looper();

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
            char lab[128];
            bb::SetLabel(gFiles[i], lab, sizeof lab);   /* SETS: "sets/amen.wav" shows as "amen [set]" */
            for(const char* p = lab; *p && w < end - 2; p++)
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
        len = bbwav::Load(path, gStage, kStageMax, gPlay, kLoadMax, kSr,
                          kLoadSec, cues, &nCues, kMaxOnsets, &err);   /* RECORD: mono at boot (presets apply Sampling after), 17 s cap */
        if(len > 0)
        {
            bb::ConditionInfo ci;                                  /* SLICE */
            gDet.mode = gOnsetDet.sel;                             /* review M4: SDRAM object, no NSDMI */
            len = bb::ConditionBuffer(gPlay, len, kSr, gDet, &ci, gBars.sel == 0);
            gCondBars = ci.bars;
            int no = (nCues >= 2) ? 0 : gDet.Detect(gPlay, len, kSr, gPendOnsets, kMaxOnsets, nullptr);
            if(nCues >= 2)
            {
                for(int i = 0; i < nCues && no < bb::Engine::kMaxSlices; i++)
                {
                    const int q = cues[i] - ci.trimmedStart;   /* review M1 */
                    if(q >= 0 && q < len) gPendOnsets[no++] = q;
                }
                if(no == 0) gPendOnsets[no++] = 0;
            }
            gPendCues = nCues >= 2;
            gPendN    = no;
            /* SLICE: the boot load takes the same bar rule as a later load
             * (v0.8.0 sliced it by the prior alone) */
            int barsN = 0;
            const int nsl = SliceCountFor(gPlay, len, gPendOnsets, no, gCondBars, &barsN);
            gGridBars = barsN > 0 ? barsN : 1;   /* RECORD */
            static uint8_t cls[bb::Engine::kMaxSlices];
            ClassifySteps(gPlay, len, cls, gClass.mode, gSal, nsl, engine.stepBands /* GATES: audio not started */);
            memcpy(engine.sliceSal, gSal, sizeof gSal);   /* GRAM tiers at boot (review M3); SAMPLE: the source's size */
            engine.swing = SwingFor(len, gPendOnsets, no, nsl, barsN);   /* SLICE: boot honours Swing */
            engine.SetMaterial(gPlay, len, kSr, gPendOnsets, no, cls, nsl, nullptr, kBufMax /* SAMPLE */);
            ComputeDensity(gPlay, len, engine.sliceDens, engine.nSlices);   /* PLAY: audio not started yet */
            ComputeEnvelope(gPlay, len, gEnv, nsl);
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
        memcpy(gPendOnsets, onsets, sizeof(int) * (size_t)no);   /* SLICE: a later Bars/Swing/Onsets
        gPendN = no; gPendCues = false;                              re-apply needs the table (review) */
        ClassifySteps(gPlay, len, cls, gClass.mode, gSal, 0, engine.stepBands /* GATES */);
        memcpy(engine.sliceSal, gSal, sizeof gSal);   /* review M3; SAMPLE: the source's size */
        engine.SetMaterial(gPlay, len, kSr, onsets, no, cls, 0, nullptr, kBufMax /* SAMPLE */);
        ComputeDensity(gPlay, len, engine.sliceDens, engine.nSlices);   /* PLAY: audio not started yet */
        ComputeEnvelope(gPlay, len, gEnv);
    }

    gClock.SetBpm(engine.NativeBpm());
    gClock.Start();
    bb::gYield = KeepClock;   /* loads no longer freeze the playhead */
    gFollower.SetLossPolicy(ClockLossPolicy::Freewheel);
    /* CLOCK: no auto-start. The NCO is kept running through acquisition (see
     * OnPoll), and DeclareLock's Start() on an already-running NCO is not a
     * no-op: ApplyDeferredTransport clears have_last_us_, so the next Tick
     * drops one poll of dt — a ~1 ms phase slip at every lock and re-lock.
     * OnPoll starts the NCO itself in the (theoretical) stopped case. */
    gFollower.SetAutoStart(false);
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
    gPresetName = &presets.UseNames();   /* renameable preset names, from the browser; SETS: into the backups */

    loop.Use(pager).Use(locks).Use(settings).Use(perform).Use(brain).Use(host)
        .OnPoll(OnPoll).OnFrame(OnFrame).OnRender(OnRender);

    presets.SetYield([](void*) { KeepClock(); }, nullptr);   /* QSPI erase/program: keep the clock alive */
    presets.Init();
    presets.BootLoad();   /* HostLink starts here: descriptor + panel USB up */
    /* SETS: slots the flash no longer honours (a schema bump, an erase) come
     * back from presets/<slot>.json; slot 0 is re-loaded so the live state is
     * the boot preset's whatever was restored. */
    if(RestorePresetBackups() > 0 && presets.HasValid(0)) presets.Load(0);
    PrimePresetSeq();

    hw.StartAudio(AudioCb);

    for(;;)
    {
        gDiagPhase = 2;
        loop.Tick();
        gDiagPhase = 7;
        if(gSample.dirty && !gLoadReq && !gApply)
        {
            gSample.dirty = false;
            gLoadIdx = gSample.idx < gNumFiles ? gSample.idx : 0;
            gLoadReq = true;
        }
        if(gClass.dirty && !gReclass && !gApply && !gReapply && engine.bufLen > 0)
        {
            gClass.dirty = false;
            ClassifySteps(gPlay, engine.bufLen, gReclassBuf, gClass.mode, gReclassSal, engine.nSlices);
            gReclass = true;
        }
        /* Bars override / auto re-estimate: re-slice the CURRENT material in
         * place (classes + envelope recomputed for the new grid first). */
        if(gBars.dirty && !gApply && !gReapply && !gLoadReq && !gReclass && engine.bufLen > 0)
        {
            gBars.dirty = false;
            int barsN = 0;
            const int nsl = SliceCountFor(gPlay, engine.bufLen, gPendOnsets, gPendN, gCondBars, &barsN);
            gPendSwing = SwingFor(engine.bufLen, gPendOnsets, gPendN, nsl, barsN);   /* SLICE */
            ClassifySteps(gPlay, engine.bufLen, gPendClass, gClass.mode, gPendSal, nsl, gPendBands /* GATES */);
            ComputeDensity(gPlay, engine.bufLen, gPendDens, nsl);   /* PLAY */
            ComputeEnvelope(gPlay, engine.bufLen, gPendEnv, nsl);
            gPendLen  = engine.bufLen;
            gPendHint = nsl;
            gReapply  = true;
        }
        /* SLICE: Options → Onsets re-detects the table (unless it came from
         * the file's cues), Options → Swing re-measures; both re-apply the
         * current material with the new boundaries and step timing. */
        if((gSwing.dirty || gOnsetDet.dirty) && !gApply && !gReapply && !gLoadReq && !gReclass && engine.bufLen > 0)
        {
            if(gOnsetDet.dirty)
            {
                gOnsetDet.dirty = false;
                gDet.mode       = gOnsetDet.sel;
                if(!gPendCues) gPendN = gDet.Detect(gPlay, engine.bufLen, kSr, gPendOnsets, kMaxOnsets, nullptr);
            }
            gSwing.dirty  = false;
            int barsN = 0;
            const int nsl = SliceCountFor(gPlay, engine.bufLen, gPendOnsets, gPendN, gCondBars, &barsN);
            gPendSwing    = SwingFor(engine.bufLen, gPendOnsets, gPendN, nsl, barsN);
            ClassifySteps(gPlay, engine.bufLen, gPendClass, gClass.mode, gPendSal, nsl, gPendBands /* GATES */);
            ComputeDensity(gPlay, engine.bufLen, gPendDens, nsl);   /* PLAY */
            ComputeEnvelope(gPlay, engine.bufLen, gPendEnv, nsl);
            gPendLen  = engine.bufLen;
            gPendHint = nsl;
            gReapply  = true;
        }
        {   /* Jack map: option, or follow the grammar (Play → Voice). J6 flips
             * between MCP4728 output and ADC input on the control thread (I²C). */
            const uint8_t want = gJacks.sel == 0 ? (gGrammar.mode >= 5 ? 1u : 0u) : (uint8_t)(gJacks.sel - 1u);
            if(want != gJackMapCur)
            {
                if(want == 1u) hw.j6.DisableCvOutput(); else { hw.j6.EnableCvOutput(); hw.j6.SetVolts(0.f); }
                gJackMapCur = want;
            }
        }
        {   /* WEB → RECORD / LOOPER: browser actions, on the control thread */
            if(gWebRecClear >= 0)
            {
                const int k = gWebRecClear; gWebRecClear = -1;
                if(k < bb::Engine::kKeys && !(gKeyTake && gKeyTakeKey == k)) engine.ClearKey(k);
            }
            if(gWebRecArm >= 0)
            {
                const int k = gWebRecArm; gWebRecArm = -1;
                if(gKeyTake && gWebTake) { gKeyTakeDone = true; }   /* a browser take stops on the next arm */
                else if(k < bb::Engine::kKeys && !gKeyTake && !gKeyTakeDone && !gTakeActive && engine.RecArm(k))
                { gKeyTake = true; gKeyTakeKey = k; gRecPeak = 0.f; gWebTake = true; }
            }
            if(gWebLooperUndo)  { gWebLooperUndo = false; gPendUndo = true; }
            if(gWebLooperClear) { gWebLooperClear = false; if(gGrammar.mode == 5) gPendReslice = true; }
        }
        if(gRests.dirty && !gPendReroll)
        {
            gRests.dirty       = false;
            engine.restSustain = (gGrammar.mode == 2 || gGrammar.mode == 3) && gRests.sel == 0;
            gPendRegen         = true;
        }
        /* GRAM: Swaps re-ranks the tiers and regenerates (same seeds) so the
         * change is audible now; Déjà Vu applies at the next flow bar. */
        if(gSwaps.dirty && !gPendReroll) { gSwaps.dirty = false; gPendRegen = true; }
        if(gDejaVu.dirty) gDejaVu.dirty = false;
        /* GRAM end */
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
            engine.restSustain = (gGrammar.mode == 2 || gGrammar.mode == 3) && gRests.sel == 0;   /* Grids/Euclid rests continue the break */
            gPendReroll        = true;
        }
        /* RECORD: Options → Sampling re-applies the material with (or
         * without) its R plane now when no key holds a recording — the
         * engine keeps recordings only on the same planes — else it applies
         * at the next load or loop take. Record Source is read live by the
         * audio thread; Record → Off stops an unclocked take. */
        if(gSampling.dirty && !gApply && !gReapply && !gLoadReq && !gReclass && !gTakeActive && !gKeyTake && engine.bufLen > 0)
        {
            gSampling.dirty = false;
            const bool wantR = gSampling.sel == bb::kSampStereo, haveR = engine.bufR != nullptr;
            if(wantR != haveR && engine.nTable == engine.nSlices)
            {
                if(wantR) for(int i = 0; i < engine.bufLen; i++) { gPlayR[i] = gPlay[i]; if((i & 4095) == 4095) bb::Yield(); }
                gReapplyR = wantR ? 1 : 0;
                StageReapply();
            }
        }
        if(gRecSrc.dirty) gRecSrc.dirty = false;
        if(gRecord.dirty) { gRecord.dirty = false; if(gRecord.sel == 0 && gTakeActive && gTake.running) gTake.Stop(); }
        ServiceRecord();
        /* RECORD end */
        ServiceSets();   /* SETS: a loaded set's recordings, ACTION 14 / 15, the preset backups */
        ServiceLoad();
    }
}
