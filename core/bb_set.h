#pragma once
// Sets and preset backups on the card — the pure half (Phase E of
// docs/sample-mode-plan.md §4). A set is `sets/<name>.wav` (the material,
// mono or stereo) + `sets/<name>.json` (the slice table, bars/tempo, the
// Options bytes and the looper's events); a preset save also writes
// `presets/<slot>.json`, a human-readable backup of every setting and the
// sample in use. Formats and the shell's FatFs to-do list:
// docs/packages/sample-persist.md.
//
// SetWriter: SetInfo + planes → WAV (through a bbwav::Sink, cue chunk =
// slice starts) and JSON (into a caller buffer, or straight to a sink).
// SetReader: JSON text → SetInfo, tolerant of unknown keys and extra
// fields, strict on the `audiothurgist` version, bounded by the buffer.
// PresetBackup: the same pair for presets/<slot>.json.
//
// No allocation, no exceptions, no STL: fixed arrays sized for the engine's
// whole table (kSetMaxSlices = Engine::kMaxTable: 64 grid + 32 recorded), the
// looper (kSetMaxLooper events) and the Options block (kSetMaxOptions bytes —
// 22 today, room for growth). The shell's side: core/bb_sets.h and
// docs/packages/sample-sets.md.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <new>
#include "bb_json.h"
#include "bb_wavwrite.h"

namespace bb {

constexpr int kSetSchema      = 1;     // "audiothurgist": 1
constexpr int kSetMaxSlices   = 96;    // = Engine::kMaxTable (64 grid + 32 recorded) /* SETS */
constexpr int kSetMaxLooper   = 256;
constexpr int kSetMaxOptions  = 32;
constexpr int kSetMaxName     = 32;    // set / slice / preset names (incl. NUL)
constexpr int kSetMaxFile     = 64;    // preset backup's sample filename
constexpr int kSetMaxPages    = 4;     // pager pages backed up
constexpr int kSetPots        = 6;     // pots per page
// JSON buffer the shell should lend: a full 96-entry / 256-event set emits
// under 28 KB (persist_check prints the figure); the reader takes any length.
constexpr size_t kSetJsonMax  = 32768;   /* SETS: was 24 KiB for 64 slices */

struct SetSlice
{
    int     start = 0;      // frames into the buffer
    int     len   = 0;      // frames
    int8_t  key   = -1;     // Voice key 0..23 of a RECORDED entry; -1 = a grid slice /* SETS */
    uint8_t cls   = 0;      // class: 0 kick 1 snare 2 hat 3 other
    float   sal   = 0.f;    // salience 0..1
    bool    hit   = false;  // begins with a detected onset
    char    name[kSetMaxName] = {};
};

struct SetLoopEvent
{
    float   phase = 0.f;    // position in the loop, 0..1 of the bar (unquantized)
    int8_t  key   = -1;     // Voice key 0..23, or -1: a keyless call (Brain map, web) /* SETS */
    uint8_t style = 0;      // bb::Looper style bits (kWire)
    float   vel   = 0.f;
    int8_t  slice = -1;     // table entry called — the replay target of a keyless event /* SETS */
};

struct SetInfo
{
    char    name[kSetMaxName] = {};
    int     sr       = 48000;
    bool    stereo   = false;
    int     len      = 0;                   // frames per plane in the WAV (material + recordings = Engine::freePos)
    int     material = 0;                   // the break's length (Engine::bufLen); 0 = len /* SETS */
    float   swing    = 0.5f;                // the grid's 16th swing (Engine::swing; 0.5 straight) /* SETS */
    int     bars     = 0;
    float   bpm      = 0.f;
    int     nSlices  = 0;
    SetSlice slices[kSetMaxSlices];
    int     nOptions = 0;
    uint8_t options[kSetMaxOptions] = {};
    int     nLooper  = 0;
    SetLoopEvent looper[kSetMaxLooper];
};

struct PresetInfo
{
    int     slot = 0;
    char    name[kSetMaxName] = {};
    int     nOptions = 0;
    uint8_t options[kSetMaxOptions] = {};
    int     nPages = 0;
    float   pager[kSetMaxPages][kSetPots] = {};
    char    sample[kSetMaxFile] = {};       // filename of the material in use
};

namespace detail {

inline void CopyStr(char* dst, size_t cap, const char* src)
{
    size_t n = 0;
    while(src && src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

inline void OptionsOut(JsonOut& o, const uint8_t* opt, int n)
{
    o.Key("options"); o.BeginArray();
    for(int i = 0; i < n && i < kSetMaxOptions; i++) o.Int(opt[i]);
    o.EndArray();
}

// "options":[…] → bytes; values outside 0..255 are clamped, extras
// consumed and dropped.
inline bool OptionsIn(JsonIn& in, uint8_t* opt, int& n)
{
    if(!in.BeginArray()) return false;
    n = 0;
    while(in.ArrayMore())
    {
        int v; if(!in.Int(v)) return false;
        if(n < kSetMaxOptions) opt[n++] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    return in.ok;
}

// A whole JSON buffer through a sink (the JSON file's single f_write).
inline bool SinkText(bbwav::Sink sink, const char* text, size_t n)
{
    return sink.write && sink.write(sink.ctx, text, n);
}

} // namespace detail

struct SetWriter
{
    // sets/<name>.wav — planes L (and R when stereo), cues = slice starts.
    // fmt: kPcm16 (the format bbwav::Load reads back; default), kPcm24 or
    // kFloat32 (host-readable only until the loader learns floats).
    static bool Wav(bbwav::Sink sink, const SetInfo& s, const float* left, const float* right,
                    uint8_t* staging, size_t stagingBytes, bbwav::Format fmt = bbwav::kPcm16)
    {
        int cues[kSetMaxSlices];
        int nc = 0;
        for(int i = 0; i < s.nSlices && i < kSetMaxSlices; i++)
            if(s.slices[i].start >= 0 && s.slices[i].start < s.len) cues[nc++] = s.slices[i].start;
        bbwav::WriteSpec w;
        w.left  = left;
        w.right = s.stereo ? right : nullptr;
        w.len   = s.len;
        w.sr    = (uint32_t)(s.sr > 0 ? s.sr : 48000);
        w.fmt   = fmt;
        w.cues  = nc ? cues : nullptr;
        w.nCues = nc;
        return bbwav::Write(sink, w, staging, stagingBytes);
    }

    // sets/<name>.json into buf[cap]; *outLen = bytes (without the NUL).
    // False (and buf unusable) when cap is too small — lend kSetJsonMax.
    static bool Json(const SetInfo& s, char* buf, size_t cap, size_t* outLen = nullptr)
    {
        JsonOut o(buf, cap);
        o.BeginObject();
        o.KeyInt("audiothurgist", kSetSchema);
        o.KeyString("name", s.name);
        o.KeyInt("sr", s.sr);
        o.KeyInt("stereo", s.stereo ? 1 : 0);
        o.KeyInt("len", s.len);
        o.KeyInt("material", s.material > 0 ? s.material : s.len);   /* SETS */
        o.KeyFloat("swing", s.swing);                                  /* SETS */
        o.KeyInt("bars", s.bars);
        o.KeyFloat("bpm", s.bpm);
        o.Key("slices"); o.BeginArray();
        for(int i = 0; i < s.nSlices && i < kSetMaxSlices; i++)
        {
            const SetSlice& sl = s.slices[i];
            o.BeginObject();
            o.KeyInt("start", sl.start);
            o.KeyInt("len", sl.len);
            o.KeyInt("key", sl.key);
            o.KeyInt("cls", sl.cls);
            o.KeyFloat("sal", sl.sal);
            o.KeyInt("hit", sl.hit ? 1 : 0);
            o.KeyString("name", sl.name);
            o.EndObject();
        }
        o.EndArray();
        detail::OptionsOut(o, s.options, s.nOptions);
        o.Key("looper"); o.BeginArray();
        for(int i = 0; i < s.nLooper && i < kSetMaxLooper; i++)
        {
            const SetLoopEvent& e = s.looper[i];
            o.BeginObject();
            o.KeyFloat("phase", e.phase);
            o.KeyInt("key", e.key);
            o.KeyInt("style", e.style);
            o.KeyFloat("vel", e.vel);
            o.KeyInt("slice", e.slice);   /* SETS */
            o.EndObject();
        }
        o.EndArray();
        o.EndObject();
        o.Put('\n');
        if(outLen) *outLen = o.ok ? o.len : 0;
        return o.ok;
    }

    // Emit into `scratch` (kSetJsonMax bytes, DMA-placed on the firmware)
    // and hand it to the sink in one write.
    static bool Json(bbwav::Sink sink, const SetInfo& s, char* scratch, size_t cap)
    {
        size_t n = 0;
        return Json(s, scratch, cap, &n) && detail::SinkText(sink, scratch, n);
    }
};

struct SetReader
{
    // Parse sets/<name>.json (text[n], need not be NUL-terminated) into
    // `out` (reset first). False on malformed / truncated input or a schema
    // version other than kSetSchema. Unknown keys anywhere are skipped;
    // slices, options and looper events past the fixed capacity are read
    // and dropped (nSlices etc. report what was kept).
    static bool Json(const char* text, size_t n, SetInfo& out)
    {
        new(&out) SetInfo();   /* SETS: reset in place — a SetInfo temporary is ~9 KB of stack (DTCM on the module) */
        JsonIn in(text, n);
        if(!in.BeginObject()) return false;
        bool sawVersion = false;
        int  version    = 0;
        char key[24];
        while(in.NextKey(key, sizeof key))
        {
            if(!strcmp(key, "audiothurgist")) { if(!in.Int(version)) return false; sawVersion = true; }
            else if(!strcmp(key, "name"))   { if(!in.String(out.name, sizeof out.name)) return false; }
            else if(!strcmp(key, "sr"))     { if(!in.Int(out.sr)) return false; }
            else if(!strcmp(key, "stereo")) { bool b; if(!in.Bool(b)) return false; out.stereo = b; }
            else if(!strcmp(key, "len"))    { if(!in.Int(out.len)) return false; }
            else if(!strcmp(key, "material")) { if(!in.Int(out.material)) return false; }   /* SETS */
            else if(!strcmp(key, "swing"))  { if(!in.Float(out.swing)) return false; }      /* SETS */
            else if(!strcmp(key, "bars"))   { if(!in.Int(out.bars)) return false; }
            else if(!strcmp(key, "bpm"))    { if(!in.Float(out.bpm)) return false; }
            else if(!strcmp(key, "slices")) { if(!Slices(in, out)) return false; }
            else if(!strcmp(key, "options")){ if(!detail::OptionsIn(in, out.options, out.nOptions)) return false; }
            else if(!strcmp(key, "looper")) { if(!Looper(in, out)) return false; }
            else if(!in.Skip()) return false;
        }
        if(!in.ok) return false;
        if(!sawVersion || version != kSetSchema) return false;
        if(out.sr <= 0 || out.len < 0) return false;
        if(out.material <= 0 || out.material > out.len) out.material = out.len;   /* SETS: absent / bogus → the whole file */
        return true;
    }

  private:
    static bool Slices(JsonIn& in, SetInfo& out)
    {
        if(!in.BeginArray()) return false;
        while(in.ArrayMore())
        {
            SetSlice sl;
            if(!in.BeginObject()) return false;
            char key[16];
            while(in.NextKey(key, sizeof key))
            {
                int v;
                if(!strcmp(key, "start"))     { if(!in.Int(sl.start)) return false; }
                else if(!strcmp(key, "len")) { if(!in.Int(sl.len)) return false; }
                else if(!strcmp(key, "key")) { if(!in.Int(v)) return false; sl.key = (int8_t)(v < -1 || v > 127 ? -1 : v); }   /* SETS */
                else if(!strcmp(key, "cls")) { if(!in.Int(v)) return false; sl.cls = (uint8_t)v; }
                else if(!strcmp(key, "sal")) { if(!in.Float(sl.sal)) return false; }
                else if(!strcmp(key, "hit")) { bool b; if(!in.Bool(b)) return false; sl.hit = b; }
                else if(!strcmp(key, "name")){ if(!in.String(sl.name, sizeof sl.name)) return false; }
                else if(!in.Skip()) return false;
            }
            if(!in.ok) return false;
            if(out.nSlices < kSetMaxSlices) out.slices[out.nSlices++] = sl;
        }
        return in.ok;
    }
    static bool Looper(JsonIn& in, SetInfo& out)
    {
        if(!in.BeginArray()) return false;
        while(in.ArrayMore())
        {
            SetLoopEvent e;
            if(!in.BeginObject()) return false;
            char key[16];
            while(in.NextKey(key, sizeof key))
            {
                int v;
                if(!strcmp(key, "phase"))       { if(!in.Float(e.phase)) return false; }
                else if(!strcmp(key, "key"))   { if(!in.Int(v)) return false; e.key = (int8_t)(v < -1 || v > 127 ? -1 : v); }     /* SETS */
                else if(!strcmp(key, "style")) { if(!in.Int(v)) return false; e.style = (uint8_t)v; }
                else if(!strcmp(key, "vel"))   { if(!in.Float(e.vel)) return false; }
                else if(!strcmp(key, "slice")) { if(!in.Int(v)) return false; e.slice = (int8_t)(v < -1 || v > 127 ? -1 : v); }   /* SETS */
                else if(!in.Skip()) return false;
            }
            if(!in.ok) return false;
            if(out.nLooper < kSetMaxLooper) out.looper[out.nLooper++] = e;
        }
        return in.ok;
    }
};

struct PresetBackup
{
    // presets/<slot>.json into buf[cap].
    static bool Write(const PresetInfo& p, char* buf, size_t cap, size_t* outLen = nullptr)
    {
        JsonOut o(buf, cap);
        o.BeginObject();
        o.KeyInt("audiothurgist", kSetSchema);
        o.KeyInt("slot", p.slot);
        o.KeyString("name", p.name);
        detail::OptionsOut(o, p.options, p.nOptions);
        o.Key("pager"); o.BeginArray();
        for(int g = 0; g < p.nPages && g < kSetMaxPages; g++)
        {
            o.BeginArray();
            for(int k = 0; k < kSetPots; k++) o.Float(p.pager[g][k]);
            o.EndArray();
        }
        o.EndArray();
        o.KeyString("sample", p.sample);
        o.EndObject();
        o.Put('\n');
        if(outLen) *outLen = o.ok ? o.len : 0;
        return o.ok;
    }
    static bool Write(bbwav::Sink sink, const PresetInfo& p, char* scratch, size_t cap)
    {
        size_t n = 0;
        return Write(p, scratch, cap, &n) && detail::SinkText(sink, scratch, n);
    }

    // Same tolerance rules as SetReader::Json.
    static bool Read(const char* text, size_t n, PresetInfo& out)
    {
        new(&out) PresetInfo();
        JsonIn in(text, n);
        if(!in.BeginObject()) return false;
        bool sawVersion = false;
        int  version    = 0;
        char key[24];
        while(in.NextKey(key, sizeof key))
        {
            if(!strcmp(key, "audiothurgist")) { if(!in.Int(version)) return false; sawVersion = true; }
            else if(!strcmp(key, "slot"))    { if(!in.Int(out.slot)) return false; }
            else if(!strcmp(key, "name"))    { if(!in.String(out.name, sizeof out.name)) return false; }
            else if(!strcmp(key, "options")) { if(!detail::OptionsIn(in, out.options, out.nOptions)) return false; }
            else if(!strcmp(key, "sample"))  { if(!in.String(out.sample, sizeof out.sample)) return false; }
            else if(!strcmp(key, "pager"))
            {
                if(!in.BeginArray()) return false;
                while(in.ArrayMore())
                {
                    float row[kSetPots] = {};
                    int   k = 0;
                    if(!in.BeginArray()) return false;
                    while(in.ArrayMore())
                    {
                        float f; if(!in.Float(f)) return false;
                        if(k < kSetPots) row[k] = f;
                        k++;
                    }
                    if(!in.ok) return false;
                    if(out.nPages < kSetMaxPages) memcpy(out.pager[out.nPages++], row, sizeof row);
                }
                if(!in.ok) return false;
            }
            else if(!in.Skip()) return false;
        }
        if(!in.ok) return false;
        return sawVersion && version == kSetSchema;
    }
};

} // namespace bb
