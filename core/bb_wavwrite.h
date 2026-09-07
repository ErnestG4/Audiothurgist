#pragma once
// RIFF/WAVE writer through an abstract sink — the mirror of wav_loader.h.
// Pure: no allocation, no exceptions, no FatFs. The host tests point the
// sink at a FILE*; the firmware points it at f_write (see
// docs/packages/sample-persist.md for the shell's plumbing).
//
// Output: PCM16 / PCM24 (fmt 1) or float32 (fmt 3, with the `fact` chunk the
// spec asks for), mono or stereo (two planes in → interleaved out), any
// rate, correct RIFF/chunk sizes, an optional `cue ` chunk carrying sample
// offsets (what bbwav::Load reads back as slice starts). Chunk order:
// fmt, [fact], [cue], data — sizes are known up front, so nothing is
// written twice and the sink never has to seek.
//
// bbwav::Load reads PCM16/PCM24 (mono or stereo, folded to mono) and the
// cue chunk; it does NOT read float32 (rejects fmt 3, err 3) — so a set the
// card must load back today is PCM16 or PCM24. Float32 is here for the day
// the loader grows a float path; tests/wav_read.h reads all three.
//
// Staging: the caller lends a byte buffer (>= kMinStaging). Every byte
// that reaches the sink is assembled in it, and each fill starts at
// staging + DmaPad(fileOffset) — the SDK's SdDmaStagingPad rule, so that
// FatFs's direct-sector DMA from the buffer is always word-aligned. On the
// firmware the buffer lives in .axi_bss, alignas(32); on the host it is any
// array. Bigger is faster (one sink call per fill); 16 KiB is what the
// loader uses.
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace bbwav {

struct Sink
{
    bool (*write)(void* ctx, const void* p, size_t n);
    void* ctx;
};

enum Format : uint8_t
{
    kPcm16   = 0,
    kPcm24   = 1,
    kFloat32 = 2,
};

struct WriteSpec
{
    const float* left   = nullptr;  // plane 0 (mono: the only plane)
    const float* right  = nullptr;  // plane 1; null = mono
    int          len    = 0;        // frames
    uint32_t     sr     = 48000u;
    Format       fmt    = kPcm16;
    const int*   cues   = nullptr;  // sample offsets (slice starts), or null
    int          nCues  = 0;
};

// Same formula as alchemy::SdDmaStagingPad (kept here so core/ stays free
// of SDK headers): the 0..3-byte offset that word-aligns the whole-sector
// part of a FatFs transfer starting at fileOffset.
constexpr uint32_t DmaPad(uint32_t fileOffset)
{
    return (4u - (((512u - (fileOffset % 512u)) % 512u) & 3u)) & 3u;
}

constexpr size_t kMinStaging = 64u;    // header (< 64 B) + 3 pad bytes fit
constexpr int    kCueEntry   = 24;     // bytes per cue point

inline int BytesPerSample(Format f) { return f == kPcm16 ? 2 : f == kPcm24 ? 3 : 4; }

// Total file size the spec will produce (for pre-allocating / free-space
// checks). 0 when the spec is invalid.
inline uint32_t FileSize(const WriteSpec& s)
{
    if(s.len <= 0 || !s.left || s.sr == 0) return 0;
    const int      ch   = s.right ? 2 : 1;
    const uint32_t data = (uint32_t)s.len * (uint32_t)(BytesPerSample(s.fmt) * ch);
    uint32_t       n    = 12u + 8u + 16u;                 // RIFF hdr + fmt
    if(s.fmt == kFloat32) n += 8u + 4u;                   // fact
    if(s.nCues > 0 && s.cues) n += 8u + 4u + (uint32_t)kCueEntry * (uint32_t)s.nCues;
    n += 8u + data + (data & 1u);                         // data (+ pad)
    return n;
}

namespace detail {

inline void Put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void Put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// Byte stream → sink, through the caller's staging with the DMA pad.
struct Out
{
    Sink     sink;
    uint8_t* stage;
    size_t   cap;
    uint32_t pos = 0;     // file offset = bytes handed to the sink
    uint8_t* cur = nullptr;
    size_t   fill = 0, room = 0;
    bool     ok = true;

    void Begin()
    {
        const uint32_t pad = DmaPad(pos);
        cur  = stage + pad;
        room = cap - pad;
        fill = 0;
    }
    bool Flush()
    {
        if(!ok) return false;
        if(fill)
        {
            if(!sink.write(sink.ctx, cur, fill)) { ok = false; return false; }
            pos += (uint32_t)fill;
        }
        Begin();
        return true;
    }
    // small writes (headers): byte-wise through the staging
    void Bytes(const void* p, size_t n)
    {
        const uint8_t* b = (const uint8_t*)p;
        while(n && ok)
        {
            if(fill == room) Flush();
            size_t k = room - fill; if(k > n) k = n;
            memcpy(cur + fill, b, k);
            fill += k; b += k; n -= k;
        }
    }
    void U32(uint32_t v) { uint8_t t[4]; Put32(t, v); Bytes(t, 4); }
    void U16(uint32_t v) { uint8_t t[2]; Put16(t, v); Bytes(t, 2); }
    void Tag(const char* t) { Bytes(t, 4); }
};

inline int32_t Clip(float x, float scale, int32_t lo, int32_t hi)
{
    float v = x * scale;
    if(!(v == v)) v = 0.f;                      // NaN → 0
    v = v < (float)lo ? (float)lo : (v > (float)hi ? (float)hi : v);
    // round half away from zero, without <cmath>
    return (int32_t)(v >= 0.f ? v + 0.5f : v - 0.5f);
}

inline void Encode(uint8_t* p, float x, Format f)
{
    if(f == kPcm16)
    {
        Put16(p, (uint32_t)Clip(x, 32768.f, -32768, 32767));
    }
    else if(f == kPcm24)
    {
        const int32_t v = Clip(x, 8388608.f, -8388608, 8388607);
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
    }
    else
    {
        uint32_t u; memcpy(&u, &x, 4);
        Put32(p, u);                            // little-endian IEEE float
    }
}

} // namespace detail

// Write the file. Returns false on a sink failure, a bad spec, or a staging
// buffer under kMinStaging. The sink is called in file order, never twice
// for the same bytes.
inline bool Write(Sink sink, const WriteSpec& s, uint8_t* staging, size_t stagingBytes)
{
    const uint32_t total = FileSize(s);
    if(total == 0 || !sink.write || !staging || stagingBytes < kMinStaging) return false;

    const int      ch    = s.right ? 2 : 1;
    const int      bps   = BytesPerSample(s.fmt);
    const int      fb    = bps * ch;
    const uint32_t data  = (uint32_t)s.len * (uint32_t)fb;
    const bool     cues  = s.nCues > 0 && s.cues;

    detail::Out o{sink, staging, stagingBytes};
    o.Begin();

    o.Tag("RIFF"); o.U32(total - 8u); o.Tag("WAVE");

    o.Tag("fmt "); o.U32(16u);
    o.U16(s.fmt == kFloat32 ? 3u : 1u);
    o.U16((uint32_t)ch);
    o.U32(s.sr);
    o.U32(s.sr * (uint32_t)fb);               // byte rate
    o.U16((uint32_t)fb);                      // block align
    o.U16((uint32_t)(bps * 8));

    if(s.fmt == kFloat32) { o.Tag("fact"); o.U32(4u); o.U32((uint32_t)s.len); }

    if(cues)
    {
        // cue point: dwName, dwPosition, fccChunk, dwChunkStart, dwBlockStart,
        // dwSampleOffset — the loader reads the last field only.
        o.Tag("cue "); o.U32(4u + (uint32_t)kCueEntry * (uint32_t)s.nCues);
        o.U32((uint32_t)s.nCues);
        for(int i = 0; i < s.nCues; i++)
        {
            const int c = s.cues[i] < 0 ? 0 : s.cues[i];
            o.U32((uint32_t)(i + 1)); o.U32((uint32_t)c); o.Tag("data");
            o.U32(0u); o.U32(0u); o.U32((uint32_t)c);
        }
    }

    o.Tag("data"); o.U32(data);
    if(!o.Flush()) return false;              // header out; frames start on a fresh fill

    int done = 0;
    while(done < s.len && o.ok)
    {
        size_t frames = (o.room - 3u) / (size_t)fb;   // keep 3 spare bytes (SDK rule 3)
        if(frames == 0) return false;                 // staging too small for one frame
        if((int)frames > s.len - done) frames = (size_t)(s.len - done);
        uint8_t* p = o.cur;
        for(size_t i = 0; i < frames; i++)
        {
            detail::Encode(p, s.left[done + (int)i], s.fmt); p += bps;
            if(ch == 2) { detail::Encode(p, s.right[done + (int)i], s.fmt); p += bps; }
        }
        o.fill = frames * (size_t)fb;
        done += (int)frames;
        o.Flush();
    }
    if(data & 1u) { const uint8_t z = 0; o.Bytes(&z, 1); }
    return o.Flush() && o.pos == total;
}

} // namespace bbwav
