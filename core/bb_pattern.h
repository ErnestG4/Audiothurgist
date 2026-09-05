#pragma once
#include <cstdint>
#include "bb_rng.h"

namespace bb {

// Subdivision-tree pattern over a 16th grid ("more Brocot, less stochastic"):
// the bar tiles into ALIGNED power-of-2 cells — groups of notes, not single
// steps — and each cell is filled by one of three moves whose mix the knob
// controls: play in place (verbatim), repeat the previous cell (motif/roll),
// or swap with a same-drum-class cell elsewhere. Everything tiles, everything
// lands on the grid: the pieces always fit.
struct Pattern
{
    static constexpr int kMaxSteps = 64;

    int8_t src[kMaxSteps]; // at cell starts: source step; -1 = interior
    int8_t len[kMaxSteps]; // cell length in steps (at cell starts)
    bool   rev[kMaxSteps];
    bool   rat[kMaxSteps]; // 32nd ratchet double — real rolls at burst cores

    // v: 1 = big cells, verbatim … 0 = micro cells, heavy repeats/reversal
    void Generate(int nSteps, int stepsPerBeat, const uint8_t* cls, float v,
                  uint64_t cellSeed, uint64_t fillSeed)
    {
        Rng cr, fr;
        cr.Seed(cellSeed);
        fr.Seed(fillSeed);
        for(int i = 0; i < kMaxSteps; i++)
        {
            src[i] = -1;
            len[i] = 0;
            rev[i] = false;
            rat[i] = false;
        }
        // staged macro-curve: five anchored zones, piecewise-lerped, so the
        // knob travels verbatim → DJ-edit → chop → rolls → micro-madness
        // instead of flipping between two regimes
        // Cell size is the most audible parameter, so it staircases FOUR
        // times across the travel — every zone door is a kind change, not a
        // density change (review finding: single-flip maxCell was the "feels
        // like 3 zones" culprit).
        //                 v=0     .25    .5     .75    1
        const float aSub[]    = {0.70f, 0.60f, 0.50f, 0.10f, 0.05f};
        const float aPlace[]  = {0.10f, 0.25f, 0.45f, 0.60f, 1.00f};
        const float aRep[]    = {0.65f, 0.55f, 0.25f, 0.03f, 0.00f};
        const float aRev[]    = {0.25f, 0.12f, 0.03f, 0.00f, 0.00f};
        const float aStrict[] = {0.20f, 0.60f, 0.90f, 1.00f, 1.00f};
        const float aCell[]   = {0.90f, 1.40f, 1.90f, 2.40f, 2.90f};
        float vc = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        if(vc > 0.96f) vc = 1.f; // hard verbatim detent inside real pot travel
        int   zi = (int)(vc * 4.f);
        if(zi > 3) zi = 3;
        float zt = vc * 4.f - (float)zi;
        auto  lerp = [&](const float* a) { return a[zi] + zt * (a[zi + 1] - a[zi]); };
        const float subdiv      = lerp(aSub);
        const float pInPlace    = lerp(aPlace);
        const float pRepeat     = lerp(aRep);
        const float pReverse    = lerp(aRev);
        const float classStrict = lerp(aStrict);
        const float cellF       = lerp(aCell);
        const int   maxCell     = cellF >= 2.5f    ? 2 * stepsPerBeat
                                  : cellF >= 1.75f ? stepsPerBeat
                                  : cellF >= 1.20f ? stepsPerBeat / 2
                                                   : 1;
        // repeat's share of total variation (the rest is swapping)
        const float repShare = pRepeat / (1.f - pInPlace + 1e-6f);

        // ARS cluster field, alive below the roll door (~9 o'clock): activity
        // bunches around per-bar burst centers (placed by the CELL seed, so
        // reslice relocates them). Near a center: micro-cells and boosted
        // repeats — roll cores. Far away: big cells and RESTS — real space
        // where the last tail rings out. Bursts and silence, not uniform buzz.
        const float clusterAmt = vc < 0.35f ? (0.35f - vc) / 0.35f : 0.f;
        uint8_t     centers[kMaxSteps];
        int         nCent    = 0;
        const int   barSteps = 4 * stepsPerBeat;
        if(clusterAmt > 0.01f)
        {
            for(int b0 = 0; b0 < nSteps; b0 += barSteps)
            {
                int nc = 1 + (cr.Uniform() < 0.5f ? 1 : 0);
                for(int j = 0; j < nc && nCent < kMaxSteps; j++)
                    centers[nCent++] =
                        (uint8_t)(b0 + (int)(cr.Uniform() * (float)barSteps));
            }
        }
        auto activity = [&](int p) -> float {
            if(nCent == 0) return 1.f;
            int best = 1 << 30;
            for(int j = 0; j < nCent; j++)
            {
                int d = p - (int)centers[j];
                if(d < 0) d = -d;
                int dw = nSteps - d;
                if(dw < d) d = dw;
                if(d < best) best = d;
            }
            float x = (float)best / 2.5f;
            return expf(-x * x);
        };

        int pos     = 0;
        int prevSrc = 0;
        while(pos < nSteps)
        {
            // (A per-class variation ladder was tried and reverted: gating
            // kick/snare rearrangement muted the bold chops — this is
            // experimental jungle, not decoration. All cells play the game;
            // the class grammar lives in the swap targets instead.)

            // Brocot walk: start big, halve while the tree says subdivide;
            // in the cluster zone, subdivision concentrates near burst cores
            float a       = clusterAmt > 0.01f ? activity(pos) : 1.f;
            float subdivE = subdiv * (clusterAmt > 0.01f ? (0.2f + 0.8f * a) : 1.f);
            int   cl      = maxCell;
            while(cl > 1 && cr.Uniform() < subdivE) cl >>= 1;
            // cells stay aligned to their own size — the tiling invariant
            int align = pos == 0 ? maxCell : (pos & -pos);
            if(align > maxCell) align = maxCell;
            if(cl > align) cl = align;
            if(pos + cl > nSteps) cl = nSteps - pos;

            // far from a burst core the space splits: some cells REST (real
            // silence), some become TICKS — single-16th flickers, hat-class
            // preferred — so the deep-left breathes without going empty
            float away = clusterAmt > 0.01f ? clusterAmt * (1.f - a) : 0.f;
            if(away > 0.f && fr.Uniform() < away * 0.85f)
            {
                if(fr.Uniform() < 0.45f)
                {
                    int s = pos;
                    for(int t = 0; t < 6; t++)
                    {
                        int cand = (int)(fr.Uniform() * (float)nSteps);
                        if(!cls || cls[cand] == 2)
                        {
                            s = cand;
                            break;
                        }
                    }
                    src[pos] = (int8_t)s;
                    len[pos] = 1;
                    rev[pos] = fr.Uniform() < 0.10f;
                    prevSrc  = s;
                    pos += 1;
                    continue;
                }
                src[pos] = -2; // explicit rest cell (distinct from interior)
                len[pos] = (int8_t)cl;
                pos += cl;
                continue;
            }
            float repShareE = repShare + clusterAmt * a * (1.f - repShare) * 0.5f;
            float varP      = 1.f - pInPlace;
            int   s;
            if(fr.Uniform() >= varP)
                s = pos;
            else if(fr.Uniform() < repShareE)
                s = prevSrc; // say it again
            else
            {
                // class-respecting swap: an aligned cell whose first step is
                // the same drum voice (kick↔kick, snare↔snare, hat↔hat)
                s = pos;
                bool strict = fr.Uniform() < classStrict;
                for(int tries = 0; tries < 8; tries++)
                {
                    int cand = ((int)(fr.Uniform() * (float)nSteps) / cl) * cl;
                    if(cand + cl > nSteps || cand == pos) continue; // no no-op swaps
                    if(!strict || !cls || cls[cand] == cls[pos])
                    {
                        s = cand;
                        break;
                    }
                }
            }
            src[pos] = (int8_t)s;
            len[pos] = (int8_t)cl;
            rev[pos] = fr.Uniform() < pReverse;
            // at burst cores, 1-step cells ratchet into 32nd doubles ~half
            // the time — the sub-step retrigger the grid alone cannot make
            rat[pos] = clusterAmt > 0.01f && a > 0.5f && cl == 1
                       && fr.Uniform() < 0.5f;
            prevSrc = s;
            pos += cl;
        }
    }
};

} // namespace bb
