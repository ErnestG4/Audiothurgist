#pragma once
#include <cmath>
#include "bb_rng.h"

namespace bb {

// Coulomb gas on a ring (overdamped Dyson Brownian motion, circular ensemble).
//
// N points on [0, 2π) with pairwise log repulsion scaled by beta, plus
// Langevin noise. beta > 0 sweeps Poisson → GOE → GUE → rigid/Euclidean.
// beta < 0 is the CLUSTER zone: same integrator with attractive coupling.
// That is a musical device, NOT an RMT ensemble — a Hawkes process is the
// principled model for the super-Poissonian side and may replace it later.
//
// Update() is control-rate (call at ~100–200 Hz). O(N²), N ≤ 32: trivial.
struct BetaGas
{
    static constexpr int   kMaxN   = 64; // matches Engine::kMaxSlices — a gas
                                         // smaller than the slice count mutes
                                         // part of the break at every β
    static constexpr float kPi     = 3.14159265f;
    static constexpr float kTwoPi  = 6.28318531f;

    float theta[kMaxN]; // point angles, [0, 2π)
    float force[kMaxN];
    int   n          = 16;
    float beta       = 2.f;   // clamp externally to [-1.5, ~80]
    bool  frozen     = false; // capture: hold the current realisation
    float noiseSigma = 0.7f;  // rad/√s — sets how fast patterns drift at β=0
    Rng   rng;

    // Parent-layer coupling: attractor wells at the material's own onset
    // phases (the bop). At high wellG points snap to the break's actual
    // groove — the musical "rigid" limit for uneven real-world material,
    // where the pure β→∞ even lattice would quantize it robotically.
    const float* wells    = nullptr;
    int          nWells   = 0;
    float        wellG    = 0.f; // lock strength (relaxation rate, knob units)
    float        wellTimeScale = 1.f; // set from ring duration: lock tightness
                                      // must be constant in TIME, and angle
                                      // per millisecond shrinks on long rings
    float        noiseMul = 1.f; // cooling: →0 as the lock tightens, so the
                                 // full-right groove is tight (ms), not ±20 ms
    int clusterBunches = 1; // cluster seeds: one bunch per musical bar, so a
                            // multi-bar ring bursts rhythmically instead of
                            // once per file with seconds of dead air

    void Init(int n_, uint64_t seed)
    {
        rng.Seed(seed);
        SetN(n_);
        for(int i = 0; i < kMaxN; i++)
            theta[i] = rng.Uniform() * kTwoPi;
    }

    // Points beyond a previous smaller n keep their old (still valid) angles.
    void SetN(int n_)
    {
        if(n_ < 1) n_ = 1;
        if(n_ > kMaxN) n_ = kMaxN;
        n = n_;
    }

    // Fresh realisation at the current knob character. Draw an APPROXIMATE
    // stationary sample directly — locked: at wells with thermal jitter;
    // cluster: one bunch; repulsed: jittered lattice; else uniform — then a
    // feather-light polish anneal. (A long hot anneal at n=64 was 16k pair
    // evaluations in one audio block: a 339%-of-deadline overrun per reroll.)
    void Reroll()
    {
        float geff = wellG * wellTimeScale;
        if(geff > 8.f && nWells > 0)
        {
            float sd = noiseSigma * noiseMul / sqrtf(2.f * geff);
            for(int i = 0; i < n; i++)
                theta[i] = Wrap(wells[(i * nWells) / n] + sd * rng.Gauss());
        }
        else if(beta < -0.05f)
        {
            int   nb  = clusterBunches < 1 ? 1 : clusterBunches;
            float rot = rng.Uniform() * kTwoPi;
            float w   = 1.f / (1.f + 2.f * -beta);
            for(int i = 0; i < n; i++)
            {
                float c = rot + (float)((i * nb) / n) * (kTwoPi / (float)nb);
                theta[i] = Wrap(c + w * rng.Gauss());
            }
        }
        else if(beta > 0.05f)
        {
            float rot = rng.Uniform() * kTwoPi;
            float sp  = kTwoPi / (float)n;
            float cv  = 1.f / sqrtf(1.f + 1.7f * beta); // ≈ spacing CV(β)
            float sj  = cv * sp * 0.707f;
            for(int i = 0; i < n; i++)
                theta[i] = Wrap(rot + i * sp + sj * rng.Gauss());
        }
        else
        {
            for(int i = 0; i < n; i++) theta[i] = rng.Uniform() * kTwoPi;
        }
        for(int i = n; i < kMaxN; i++) theta[i] = rng.Uniform() * kTwoPi;
        annealRemaining = 12; // polish only — one Step per Update call
    }

    int annealRemaining = 0;

    void Update(float dt)
    {
        if(annealRemaining > 0)
        {
            annealRemaining--;
            Step(0.012f);
            return;
        }
        if(frozen) return;
        Step(dt);
    }

  private:
    void Step(float dt)
    {
        if(n < 2)
        {
            theta[0] = Wrap(theta[0] + noiseSigma * noiseMul * sqrtf(dt) * rng.Gauss());
            return;
        }
        if(dt > 0.05f) dt = 0.05f;

        for(int i = 0; i < n; i++) force[i] = 0.f;

        for(int i = 1; i < n; i++)
            for(int j = 0; j < i; j++)
            {
                float d = theta[i] - theta[j];
                if(d > kPi) d -= kTwoPi;
                else if(d < -kPi) d += kTwoPi;
                float ad = fabsf(d);
                if(ad < 1e-3f) d = (d >= 0.f) ? 1e-3f : -1e-3f;
                // 0.5·cot(d/2) ≈ 1/d − d/12 : exact near 0, ~11% off at d=π.
                float f = 1.f / d - d * (1.f / 12.f);
                if(f > 60.f) f = 60.f;
                else if(f < -60.f) f = -60.f;
                force[i] += f;
                force[j] -= f;
            }

        const float scale = beta * dt / (float)n;
        const float sn    = noiseSigma * noiseMul * sqrtf(dt);
        // wells as EXACT exponential relaxation, not an explicit spring force:
        // unconditionally stable, so long rings can be pulled arbitrarily
        // stiff (an explicit spring at the needed stiffness would oscillate)
        const bool  pull  = wellG > 0.f && nWells > 0;
        const float relax = pull ? expf(-wellG * wellTimeScale * dt) : 1.f;
        for(int i = 0; i < n; i++)
        {
            float step = force[i] * scale;
            if(step > 0.15f) step = 0.15f;
            else if(step < -0.15f) step = -0.15f;
            float t = theta[i] + step + sn * rng.Gauss();
            if(pull)
            {
                // assigned wells, not nearest: each point owns one onset
                // (spread across the list), so full lock covers every slice
                // instead of piling points into the loudest neighbourhoods
                float w = wells[(i * nWells) / n];
                float d = t - w;
                if(d > kPi) d -= kTwoPi;
                else if(d < -kPi) d += kTwoPi;
                t = w + d * relax;
            }
            theta[i] = Wrap(t);
        }
    }

  public:
    static float Wrap(float t)
    {
        t -= kTwoPi * floorf(t * (1.f / kTwoPi));
        return t;
    }
};

} // namespace bb
