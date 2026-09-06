#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include "bb_rng.h"
#include "bb_grids_map.h"

namespace bb {

// Subdivision-tree pattern over a 16th grid ("more Brocot, less stochastic"):
// the bar tiles into ALIGNED power-of-2 cells — groups of notes, not single
// steps — and each cell is filled by one of three moves whose mix the knob
// controls: play in place (verbatim), repeat the previous cell (motif/roll),
// or swap with a same-drum-class cell elsewhere. Everything tiles, everything
// lands on the grid: the pieces always fit.
//
// Two GRAMMARS share this struct (Generate's `mode`):
//   0 Classic — the shipped BreakFix generator, byte-identical.
//   1 Collins — the Chop Codex upgrades (Collins 2001, "Algorithmic
//     Composition Methods for Breakbeat Science" + "Further Automatic
//     Breakbeat Cutting Methods"): position-in-phrase weighted moves, odd
//     cut figures (3+3+2 …), a fill bar with snap-back (every-4th-bar fills
//     after BBCut's SQPusher1), 1-cell stutter as a rare phrase terminator,
//     WarpCut-style geometric rolls, and — ours, not from his text — repitch
//     jitter on repeats (±1.4 %, after the detune range of his beat
//     generator; applied by the engine).
struct Pattern
{
    static constexpr int kMaxSteps = 64;

    int8_t src[kMaxSteps]; // at cell starts: source step; -1 = interior
    int8_t len[kMaxSteps]; // cell length in steps (at cell starts)
    bool   rev[kMaxSteps];
    bool   rat[kMaxSteps]; // 32nd ratchet double — real rolls at burst cores

    // Extended per-cell data (Classic derives them from rat[]; Collins sets):
    int8_t ratN[kMaxSteps];     // retriggers after the attack: 0, 1 (=rat), 2..3 = geometric series
    bool   ratAccel[kMaxSteps]; // series gaps halve (accel) or double (decel)
    bool   rep[kMaxSteps];      // cell was a repeat move — engine may repitch-jitter it
    bool   accent[kMaxSteps];   // grammar accent (Grids: map level > 192) — the gates
                                // may raise velocity for it (GATES package reads it)

    // GRAM — Options → Swaps. Per-step salience TIER (0 quiet … 2 loud, the
    // terciles of the loop's per-slice salience), set by the engine before
    // Generate; not cleared by it. With salSwaps every swap candidate must
    // share the tier of the step it replaces — Hockman & Davies 2015 weight
    // resequencing candidates by segment length precisely so "an off-beat
    // ghost note" is never pasted "in place of a salient snare". Repeats,
    // in-place cells, rests and hat ticks are not swaps and are untouched.
    // salSwaps false = the RNG stream and every output are byte-identical to
    // the shipped grammars (the extra rejection is a pure post-filter).
    uint8_t salTier[kMaxSteps] = {};
    bool    salSwaps = false;
    bool    TierOk(int cand, int pos) const
    {
        return !salSwaps || salTier[cand] == salTier[pos];
    }

    // Fill layer (Collins only): alternative cells for [fillStart, nSteps),
    // played on every 4th loop by the engine, then it snaps back to the base.
    int    fillStart = -1;

    // Language params (modes 2 Grids / 3 Euclid) — set by the engine before
    // Generate. Grids map coordinates 0..1, per-class densities 0..1,
    // rotation 0..1 (Euclid).
    float langX = 0.5f, langY = 0.5f, langDens[3] = {0.5f, 0.5f, 0.5f}, langRot = 0.f;
    int8_t fsrc[kMaxSteps];
    int8_t flen[kMaxSteps];
    bool   frev[kMaxSteps];
    int8_t fratN[kMaxSteps];
    bool   fratAccel[kMaxSteps];
    bool   frep[kMaxSteps];

    // v: 1 = big cells, verbatim … 0 = micro cells, heavy repeats/reversal
    void Generate(int nSteps, int stepsPerBeat, const uint8_t* cls, float v,
                  uint64_t cellSeed, uint64_t fillSeed, int mode = 0)
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
            ratN[i]     = 0;
            ratAccel[i] = false;
            rep[i]      = false;
            accent[i]   = false;
        }
        fillStart = -1;

        if(mode == 1)
        {
            float vc = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            if(vc > 0.96f) vc = 1.f;
            GenerateCollins(nSteps, stepsPerBeat, cls, vc, cr, fr);
            return;
        }
        if(mode == 2 || mode == 3)
        {
            float vc = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            if(vc > 0.96f) vc = 1.f;
            GenerateLang(nSteps, cls, vc, mode, fr);
            return;
        }
        if(mode == 4) // GRAM: Camp — change ringing, no draws at all
        {
            float vc = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            if(vc > 0.96f) vc = 1.f;
            GenerateCamp(nSteps, stepsPerBeat, vc, cellSeed, fillSeed);
            return;
        }

        // ── Classic (mode 0) — UNCHANGED below ─────────────────────────
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
                    if((!strict || !cls || cls[cand] == cls[pos]) && TierOk(cand, pos))
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
        // Classic post-pass: express rat[] in the extended form (no RNG use,
        // outputs above untouched) so the engine has one flatten path.
        for(int i = 0; i < kMaxSteps; i++) ratN[i] = rat[i] ? 1 : 0;
    }

    // ── Collins grammar (mode 1) ───────────────────────────────────────
    //
    // Structure = per HALF-BAR cut figures ("cut lengths should be odd and
    // ≤ half a bar": 3s and 5s resolve into an even remainder, 3+3+2 is the
    // idiom); which figure comes from the CELL seed (reslice changes the
    // cuts) and moves within cells from the FILL seed (refill changes the
    // arrangement), exactly as Classic divides the labour.
    //
    // Position in the phrase (U = 0 at loop start … 1 at end) weights every
    // move — Collins' single most audible improvement: faithful, long cells
    // early; swaps and repeats mid; rolls and 1-cell stutters only near the
    // end. The last bar (or last half of a 1-bar loop) also exists as a FILL
    // variant with boosted chaos, cross-class swaps and rolls; the engine
    // plays it every 4th loop and the next downbeat snaps back verbatim.
    struct Fig
    {
        int8_t n;
        int8_t c[8];
    };

    // ── Languages (M4): Grids / Euclid as per-step CLASS fire-flags, spoken
    // through the break's own class-matched slices. Every step is a 1-step
    // cell: firing steps play the in-place slice if its class matches, else
    // the nearest slice of the firing class; silent steps are rests. The chop
    // knob keeps a meaning — FAITHFULNESS of slice choice: full right = in
    // place/nearest, lower = same-class swaps from anywhere in the loop (fill
    // seed → B2 rerolls them). Otherwise deterministic from map/densities.
    void GenerateLang(int nSteps, const uint8_t* cls, float vc, int mode, Rng& fr)
    {
        const int bar = 16;
        uint8_t   fire[kMaxSteps];
        uint8_t   dbl[kMaxSteps];   // Grids: the map's odd (32nd) step also fires
        uint8_t   acc[kMaxSteps];   // GRAM: accent bits — Grids: firing class's map
                                    // level > 192 (pattern_generator.cc, EvaluateDrums:
                                    // `if (level > 192) accent_bits |= instrument_mask`,
                                    // tested after the density gate); Euclid: the bar
                                    // downbeat (grid step 0 of each bar) when it fires
        for(int s = 0; s < nSteps; s++)
        {
            uint8_t f = 0, d = 0, a = 0;
            if(mode == 2)
            {
                // The Grids map is ONE BAR at 32nd resolution (pichenettes:
                // kStepsPerPattern = 32, 8 steps per quarter). Our grid is
                // 16ths, so read the map's even steps for the 16th grid and
                // fold its odd steps into 32nd doubles (our ratchet). Indexing
                // it with 16th steps stretched the map over two bars and put
                // the backbeat on beat 3 (lit review, 2026-09-05).
                const uint8_t x  = (uint8_t)(langX * 255.f + 0.5f);
                const uint8_t y  = (uint8_t)(langY * 255.f + 0.5f);
                const uint8_t gs = (uint8_t)((s % bar) * 2);
                for(int c = 0; c < 3; c++)
                {
                    const uint8_t dens = (uint8_t)(langDens[c] * 255.f + 0.5f);
                    const uint8_t thr  = (uint8_t)(255 - dens);
                    const uint8_t lv   = GridsReadDrumMap(gs, (uint8_t)c, x, y);
                    if(lv > thr)
                    {
                        f |= (uint8_t)(1u << c);
                        if(lv > 192) a |= (uint8_t)(1u << c);
                    }
                    if(GridsReadDrumMap((uint8_t)(gs + 1), (uint8_t)c, x, y) > thr)
                        d |= (uint8_t)(1u << c);
                }
            }
            else
            {
                const int rot = (int)(langRot * (float)bar + 0.5f) % bar;
                const int i   = ((s % bar) - rot + bar) % bar;
                for(int c = 0; c < 3; c++)
                {
                    const int k = (int)(langDens[c] * (float)bar + 0.5f);
                    // Bjorklund-equivalent maximally-even spacing (Toussaint E(k,16))
                    if(k > 0 && ((i + 1) * k / bar) != (i * k / bar)) f |= (uint8_t)(1u << c);
                }
                if((s % bar) == 0) a = f; // the "one" of every bar carries the accent
            }
            fire[s] = f;
            dbl[s]  = d;
            acc[s]  = a;
        }
        int8_t byClass[3][kMaxSteps];
        int    nBy[3] = {0, 0, 0};
        for(int s = 0; s < nSteps; s++)
        {
            int c = cls ? (cls[s] & 3) : 3;
            if(c < 3) byClass[c][nBy[c]++] = (int8_t)s;
        }
        const float swapP = (1.f - vc) * 0.8f;
        for(int s = 0; s < nSteps; s++)
        {
            len[s] = 1;
            const uint8_t f = fire[s];
            if(!f) { src[s] = -2; continue; }              // rest
            const int c = (f & 1) ? 0 : (f & 2) ? 1 : 2;  // kick > snare > hat
            if(dbl[s] & (uint8_t)(1u << c)) ratN[s] = 1;   // map's 32nd → a double
            accent[s] = (acc[s] >> c) & 1;                 // GRAM: the firing class's accent
            int pick = -1;
            if(cls && (cls[s] & 3) == c) pick = s;         // in place
            else if(cls)                                   // nearest of that class
                for(int d = 1; d < nSteps && pick < 0; d++)
                {
                    if(s - d >= 0     && (cls[s - d] & 3) == c) { pick = s - d; break; }
                    if(s + d < nSteps && (cls[s + d] & 3) == c) { pick = s + d; break; }
                }
            if(pick < 0) pick = s;
            if(nBy[c] > 1 && fr.Uniform() < swapP)
            {
                // GRAM: with salSwaps the same-class pool is re-drawn until the
                // candidate shares the replaced step's tier (8 tries, then the
                // faithful pick stands). One draw, unconditional, when off.
                const int base = pick;
                for(int tries = 0; tries < (salSwaps ? 8 : 1); tries++)
                {
                    const int cand = byClass[c][(int)(fr.Uniform() * (float)nBy[c]) % nBy[c]];
                    if(TierOk(cand, base)) { pick = cand; break; }
                }
            }
            src[s] = (int8_t)pick;
        }
    }

    // ── Camp grammar (mode 4) — after Collins' CampCutProc (GRAM) ─────
    //
    // Change ringing. The bar is N "bells" (equal cells) and each bar plays
    // one ROW of a ringing method — a permutation of its own cells — with
    // successive bars ringing successive changes, until the bells come round.
    // Nothing is drawn: the CELL seed picks the method (plain hunt, or plain
    // bob with its "12" lead end), the direction and a mirror; the FILL seed
    // is the row index — and for Camp the engine steps fillSeed by ONE on a
    // reroll, so B2 rings the next change and flow walks the method bar after
    // bar. The knob is the number of bells: rounds (verbatim) → 4 → 8 → 16.
    // Place notation, N bells, 0-indexed places: X swaps every pair (0,1)
    // (2,3)…; "1N" holds places 0 and N-1 and swaps (1,2)(3,4)…; "12" holds
    // places 0 and 1 and swaps (2,3)(4,5)…. Plain hunt = X.1N repeated (2N
    // rows to rounds); plain bob = X.1N… with "12" as the 2N-th change,
    // (N-1)·2N rows to rounds (the plain course).
    void GenerateCamp(int nSteps, int spb, float vc, uint64_t cs, uint64_t fs)
    {
        const int barSteps = 4 * spb;
        int       N        = vc >= 1.f ? 1 : vc >= 0.5f ? 4 : vc >= 0.25f ? 8 : 16;
        if(N > barSteps) N = barSteps;
        const int  cl     = barSteps / N;
        const bool bob    = (cs >> 1) & 1;
        const bool back   = (cs >> 2) & 1;
        const bool mirror = (cs >> 3) & 1;
        const int  lead   = 2 * N;
        const int  cycle  = N < 2 ? 1 : (bob ? (N - 1) * lead : lead);
        const int  r0     = (int)(fs % (uint64_t)cycle);
        int        bar    = 0;
        for(int b0 = 0; b0 < nSteps; b0 += barSteps, bar++)
        {
            int8_t row[kMaxSteps];
            for(int i = 0; i < N; i++) row[i] = (int8_t)i;
            int k = (back ? (r0 - bar) : (r0 + bar)) % cycle;
            if(k < 0) k += cycle;
            for(int t = 1; t <= k; t++)
            {
                const int first = (t & 1) ? 0 : (bob && (t % lead) == 0) ? 2 : 1;
                for(int i = first; i + 1 < N; i += 2)
                {
                    const int8_t tmp = row[i];
                    row[i]           = row[i + 1];
                    row[i + 1]       = tmp;
                }
            }
            int to = b0 + barSteps;
            if(to > nSteps) to = nSteps;
            for(int i = 0; i < N; i++)
            {
                const int pos = b0 + i * cl;
                if(pos >= to) break;
                const int bell = mirror ? N - 1 - row[i] : row[i];
                int       s    = b0 + bell * cl;
                int       L    = cl;
                if(pos + L > to) L = to - pos;
                if(s + L > nSteps) s = pos; // a partial last bar rings in place
                src[pos] = (int8_t)s;
                len[pos] = (int8_t)L;
            }
        }
    }

    void GenerateCollins(int nSteps, int spb, const uint8_t* cls, float vc, Rng& cr, Rng& fr)
    {
        //                 v=0     .25    .5     .75    1
        const float aPlace[]  = {0.10f, 0.25f, 0.45f, 0.60f, 1.00f};
        const float aRep[]    = {0.65f, 0.55f, 0.25f, 0.03f, 0.00f};
        const float aRev[]    = {0.25f, 0.12f, 0.03f, 0.00f, 0.00f};
        const float aStrict[] = {0.20f, 0.60f, 0.90f, 1.00f, 1.00f};
        const float aCell[]   = {0.90f, 1.40f, 1.90f, 2.40f, 2.90f};
        int   zi = (int)(vc * 4.f);
        if(zi > 3) zi = 3;
        float zt = vc * 4.f - (float)zi;
        auto  lerp = [&](const float* a) { return a[zi] + zt * (a[zi + 1] - a[zi]); };
        const float pInPlace    = lerp(aPlace);
        const float pRepeat     = lerp(aRep);
        const float pReverse    = lerp(aRev);
        const float classStrict = lerp(aStrict);
        const float cellF       = lerp(aCell);
        const float repShare    = pRepeat / (1.f - pInPlace + 1e-6f);

        const int barSteps = 4 * spb;      // 16
        const int half     = barSteps / 2; // 8 — figures live here
        int       B        = nSteps / barSteps;
        if(B < 1) B = 1;
        // fill region: last bar, or the second half of a 1-bar loop; both are
        // half-bar aligned so base and fill layers never straddle a cell
        fillStart = (B >= 2) ? (B - 1) * barSteps : (nSteps >= 2 * half ? half : -1);

        // cluster field (same as Classic): burst centers from the cell seed
        const float clusterAmt = vc < 0.35f ? (0.35f - vc) / 0.35f : 0.f;
        uint8_t     centers[kMaxSteps];
        int         nCent = 0;
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

        // Cut figures per half-bar, by size pool (Collins: odd cuts ≤ half
        // a bar; 7s "reveal too much of the original").
        static const Fig kBig[]     = {{1, {8}}, {2, {4, 4}}};
        static const Fig kMid[]     = {{3, {3, 3, 2}}, {2, {5, 3}}, {2, {3, 5}},
                                       {3, {2, 3, 3}}, {3, {3, 2, 3}}, {2, {6, 2}}};
        static const Fig kSmall[]   = {{4, {2, 2, 2, 2}}, {4, {3, 3, 1, 1}}, {4, {2, 2, 3, 1}},
                                       {5, {2, 1, 2, 1, 2}}, {4, {3, 1, 3, 1}}};
        static const Fig kMicro[]   = {{8, {1, 1, 1, 1, 1, 1, 1, 1}}, {6, {2, 1, 1, 2, 1, 1}},
                                       {6, {1, 1, 2, 1, 1, 2}}, {6, {2, 2, 1, 1, 1, 1}}};
        static const Fig kStutter[] = {{3, {6, 1, 1}}, {4, {3, 3, 1, 1}},
                                       {5, {2, 2, 2, 1, 1}}, {8, {1, 1, 1, 1, 1, 1, 1, 1}}};
        static const Fig* const kPools[4] = {kBig, kMid, kSmall, kMicro};
        static const int        kPoolN[4] = {2, 6, 5, 4};

        // Generate cells over [from, to) into the given layer.
        auto genRange = [&](int from, int to, bool fillMode, int8_t* S, int8_t* L,
                            bool* R, int8_t* RN, bool* RA, bool* RP, int prevSrc) {
            for(int hb = from; hb < to; hb += half)
            {
                const float U = (float)hb / (float)nSteps; // phrase position
                // figure size: big early, smaller late, smaller in the fill
                float tEff = cellF * (1.f - 0.35f * U) * (fillMode ? 0.6f : 1.f);
                int   pool = tEff >= 2.4f ? 0 : tEff >= 1.75f ? 1 : tEff >= 1.2f ? 2 : 3;
                if(vc < 1.f)
                {
                    if(pool < 3 && cr.Uniform() < 0.35f) pool++;
                    else if(pool > 0 && cr.Uniform() < 0.15f) pool--;
                }
                const bool lastHalf = (hb + half >= nSteps);
                const bool stutter  = lastHalf && vc < 0.96f && cr.Uniform() < 0.2f;
                const Fig* fig;
                if(vc >= 1.f)
                    fig = &kBig[0];
                else if(stutter)
                    fig = &kStutter[pool];
                else
                    fig = &kPools[pool][(int)(cr.Uniform() * (float)kPoolN[pool]) % kPoolN[pool]];

                int pos = hb;
                for(int ci = 0; ci < fig->n && pos < to; ci++)
                {
                    int cl = fig->c[ci];
                    if(pos + cl > to) cl = to - pos;
                    const bool  stutterCell = stutter && (ci >= fig->n - 2);
                    const float Uc = (float)pos / (float)nSteps;
                    const float a  = clusterAmt > 0.01f ? activity(pos) : 1.f;

                    // cluster space: rests and hat ticks away from burst cores
                    const float away = clusterAmt > 0.01f ? clusterAmt * (1.f - a) : 0.f;
                    if(!stutterCell && away > 0.f && fr.Uniform() < away * 0.85f)
                    {
                        if(cl == 1 && fr.Uniform() < 0.45f)
                        {
                            int s = pos;
                            for(int t = 0; t < 6; t++)
                            {
                                int cand = (int)(fr.Uniform() * (float)nSteps);
                                if(!cls || cls[cand] == 2) { s = cand; break; }
                            }
                            S[pos] = (int8_t)s; L[pos] = 1; R[pos] = fr.Uniform() < 0.10f;
                            RN[pos] = 0; RA[pos] = false; RP[pos] = false;
                            prevSrc = s;
                        }
                        else
                        {
                            S[pos] = -2; L[pos] = (int8_t)cl;
                            R[pos] = false; RN[pos] = 0; RA[pos] = false; RP[pos] = false;
                        }
                        pos += cl;
                        continue;
                    }

                    // position-weighted move mix: faithful early, restless late
                    float placeE = pInPlace + (1.f - pInPlace) * 0.55f * (1.f - Uc);
                    if(fillMode) placeE *= 0.4f;
                    float repE = repShare + 0.25f * Uc + clusterAmt * a * 0.3f + (fillMode ? 0.15f : 0.f);
                    if(repE > 0.9f) repE = 0.9f;
                    float strictE = fillMode ? classStrict * 0.3f : classStrict;
                    float revE    = pReverse * (fillMode ? 2.f : 1.f) * (1.f + Uc);

                    int  s;
                    bool isRep = false;
                    if(stutterCell)
                    {
                        s     = prevSrc; // the stutter IS the roll
                        isRep = true;
                    }
                    else if(fr.Uniform() < placeE)
                        s = pos;
                    else if(fr.Uniform() < repE)
                    {
                        s     = prevSrc;
                        isRep = true;
                    }
                    else
                    {
                        s = pos;
                        const bool strict = fr.Uniform() < strictE;
                        const int  span   = nSteps - cl + 1;
                        for(int tries = 0; tries < 8 && span > 1; tries++)
                        {
                            int cand = (int)(fr.Uniform() * (float)span);
                            if(cand == pos) continue;
                            if((!strict || !cls || cls[cand] == cls[pos]) && TierOk(cand, pos))
                            { s = cand; break; }
                        }
                    }
                    S[pos]  = (int8_t)s;
                    L[pos]  = (int8_t)cl;
                    R[pos]  = !stutterCell && fr.Uniform() < revE;
                    RP[pos] = isRep;

                    // rolls (WarpCut: plain / even double / geometric series) on
                    // short cells — mostly near the phrase end and at burst cores
                    int8_t rn = 0;
                    bool   ra = false;
                    if(!stutterCell && cl <= 2 && vc < 0.96f)
                    {
                        float rollP = (Uc > 0.75f ? 0.45f : 0.08f) * (1.f - 0.6f * vc)
                                      + (clusterAmt > 0.01f && a > 0.5f ? 0.35f : 0.f)
                                      + (fillMode ? 0.15f : 0.f);
                        if(fr.Uniform() < rollP)
                        {
                            if(fr.Uniform() < 0.5f) rn = 1;                          // even double
                            else { rn = (int8_t)(2 + (fr.Uniform() < 0.5f ? 1 : 0)); // geometric
                                   ra = fr.Uniform() < 0.6f; }
                        }
                    }
                    RN[pos] = rn;
                    RA[pos] = ra;
                    prevSrc = s;
                    pos += cl;
                }
            }
        };

        // base layer over the whole loop
        genRange(0, nSteps, false, src, len, rev, ratN, ratAccel, rep, 0);
        // fill layer: copy base, regenerate the fill region with the boosts
        if(fillStart >= 0)
        {
            memcpy(fsrc, src, sizeof src);
            memcpy(flen, len, sizeof len);
            memcpy(frev, rev, sizeof rev);
            memcpy(fratN, ratN, sizeof ratN);
            memcpy(fratAccel, ratAccel, sizeof ratAccel);
            memcpy(frep, rep, sizeof rep);
            int pv = 0;
            for(int c = 0; c < fillStart; c++)
                if(src[c] >= 0) pv = src[c];
            genRange(fillStart, nSteps, true, fsrc, flen, frev, fratN, fratAccel, frep, pv);
        }
    }
};

} // namespace bb
