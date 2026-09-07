#pragma once
// FatFS-based WAV → mono float loader, shared between the Pod shell and the
// desktop test harness (tests/wavload_test.cpp shims the f_* API over stdio —
// include this header AFTER either fatfs.h or the shim).
//
// Reads 16/24-bit PCM (plain or extensible), mono or stereo folded to mono,
// any rate ≥ 8 kHz resampled to outSr, capped at maxSec. Honors `cue ` chunk
// sample offsets (rescaled to the output rate). Returns output length in
// samples, 0 on failure with *err telling why:
//   1 open failed   2 not RIFF/WAVE   3 unsupported format   4 too short/empty
#include <cstdint>
#include <cstring>
#include "bb_yield.h"

// Placement for the FILE object and DMA staging buffer. On the Pod
// (BOOT_QSPI) plain .bss is AXI SRAM — DMA-reachable — so this is empty. On
// Alchemy Lab (BOOT_SRAM) plain .bss lands in DTCM, which SDMMC IDMA cannot
// address, so the alchemy shell defines BBWAV_DMA_SECTION to place these in
// SDRAM (the same rule the SDK's SdCard header spells out).
#ifndef BBWAV_DMA_SECTION
#define BBWAV_DMA_SECTION
#endif

namespace bbwav {

inline uint32_t rd32(const uint8_t* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint16_t rd16(const uint8_t* p)
{
    return p[0] | (p[1] << 8);
}

// static (not inline): each build includes this header in exactly one TU, and
// internal linkage keeps the DMA-section function-local statics as plain
// section members — an inline function's linkonce statics collide with plain
// globals in the same NOLOAD section (GCC "section type conflict").
static int Load(const char* name,
                float*      stage,
                int         stageMax,
                float*      out,
                int         outMax,
                float       outSr,
                int         maxSec,
                int*        cues,
                int*        nCues,
                int         maxCues,
                int*        err)
{
    BBWAV_DMA_SECTION alignas(32) static uint8_t ioBuf[16384]; // DMA-safe staging
    *err   = 0;
    *nCues = 0;
    // static, NOT stack: FIL embeds the file's 512-byte sector buffer, and the
    // stack lives in DTCM — which SDMMC IDMA cannot write. A stack FIL reads
    // silent zeros; a .bss FIL sits in AXI SRAM where DMA can reach it.
    BBWAV_DMA_SECTION alignas(32) static FIL f;
    if(f_open(&f, name, FA_READ) != FR_OK)
    {
        *err = 1;
        return 0;
    }
    UINT    br;
    uint8_t hdr[12];
    if(f_read(&f, hdr, 12, &br) != FR_OK || br != 12 || memcmp(hdr, "RIFF", 4)
       || memcmp(hdr + 8, "WAVE", 4))
    {
        f_close(&f);
        *err = 2;
        return 0;
    }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate   = 0;
    int      nStage = 0;
    uint8_t  ck[8];
    while(f_read(&f, ck, 8, &br) == FR_OK && br == 8)
    {
        uint32_t sz = rd32(ck + 4);
        if(!memcmp(ck, "fmt ", 4))
        {
            uint8_t d[40];
            UINT    want = sz < 40 ? sz : 40;
            if(f_read(&f, d, want, &br) != FR_OK) break;
            if(sz > want) f_lseek(&f, f_tell(&f) + (sz - want) + (sz & 1));
            fmt  = rd16(d);
            ch   = rd16(d + 2);
            rate = rd32(d + 4);
            bits = rd16(d + 14);
        }
        else if(!memcmp(ck, "cue ", 4))
        {
            uint8_t d[4];
            if(f_read(&f, d, 4, &br) != FR_OK) break;
            uint32_t nc     = rd32(d);
            uint32_t taken  = 0;
            for(uint32_t i = 0; i < nc; i++)
            {
                uint8_t c[24];
                if(f_read(&f, c, 24, &br) != FR_OK || br != 24) break;
                if(*nCues < maxCues)
                {
                    cues[(*nCues)++] = (int)rd32(c + 20); // dwSampleOffset
                    taken++;
                }
            }
            uint32_t consumed = 4 + 24 * (taken + 0); // still walk oversize lists
            if(consumed < sz) f_lseek(&f, f_tell(&f) + (sz - consumed) + (sz & 1));
        }
        else if(!memcmp(ck, "data", 4))
        {
            if((fmt != 1 && fmt != 0xFFFE) || (bits != 16 && bits != 24) || ch < 1
               || ch > 2 || rate < 8000)
            {
                f_close(&f);
                *err = 3;
                return 0;
            }
            const int fb     = (bits / 8) * ch;
            uint32_t  frames = sz / fb;
            uint32_t  cap    = (uint32_t)((uint64_t)maxSec * rate);
            if(frames > cap) frames = cap;
            if(frames > (uint32_t)stageMax) frames = stageMax;
            uint32_t done = 0;
            while(done < frames)
            {
                UINT     want = sizeof(ioBuf) - (sizeof(ioBuf) % fb);
                uint32_t left = (frames - done) * fb;
                if(want > left) want = left;
                if(f_read(&f, ioBuf, want, &br) != FR_OK || br == 0) break;
                bb::Yield();   /* SD read of a big file: keep the clock alive */
                uint32_t       n = br / fb;
                const uint8_t* p = ioBuf;
                for(uint32_t i = 0; i < n; i++)
                {
                    float acc = 0.f;
                    for(int c = 0; c < ch; c++)
                    {
                        int32_t v;
                        if(bits == 16)
                        {
                            v = (int16_t)rd16(p);
                            acc += v * (1.f / 32768.f);
                            p += 2;
                        }
                        else
                        {
                            v = ((p[0] << 8) | (p[1] << 16)
                                 | ((int32_t)(int8_t)p[2] << 24))
                                >> 8;
                            acc += v * (1.f / 8388608.f);
                            p += 3;
                        }
                    }
                    stage[done + i] = acc / ch;
                }
                done += n;
            }
            nStage = (int)done;
            // don't stop: cue chunks can follow data — skip the rest, keep walking
            uint32_t consumed = done * fb;
            if(consumed < sz) f_lseek(&f, f_tell(&f) + (sz - consumed) + (sz & 1));
        }
        else
        {
            f_lseek(&f, f_tell(&f) + sz + (sz & 1));
        }
    }
    f_close(&f);
    if(rate == 0 || nStage < (int)(0.25f * rate))
    {
        *err = 4;
        return 0;
    }

    float ratio  = outSr / (float)rate;
    int   outLen = (int)(nStage * ratio);
    if(outLen > outMax) outLen = outMax;
    for(int i = 0; i < outLen; i++)
    {
        float src = i / ratio;
        int   i0  = (int)src;
        if(i0 >= nStage - 1) i0 = nStage - 2;
        float fr = src - i0;
        out[i]   = stage[i0] + fr * (stage[i0 + 1] - stage[i0]);
    }
    for(int i = 0; i < *nCues; i++)
    {
        int c   = (int)(cues[i] * ratio);
        cues[i] = c < 0 ? 0 : (c >= outLen ? outLen - 1 : c);
    }
    return outLen;
}

} // namespace bbwav
