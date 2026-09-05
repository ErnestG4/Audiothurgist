#pragma once
#include <cstdint>

namespace bb {

// xorshift64* — deterministic, allocation-free, good enough for Langevin noise.
struct Rng
{
    uint64_t s = 0x9E3779B97F4A7C15ULL;

    void Seed(uint64_t x) { s = x ? x : 0x9E3779B97F4A7C15ULL; }

    uint32_t Next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return (uint32_t)((s * 0x2545F4914F6CDD1DULL) >> 32);
    }

    float Uniform() { return (Next() >> 8) * (1.f / 16777216.f); }

    // Irwin–Hall(4), rescaled to unit variance. Bounded in ±3.46 — a feature
    // in a Langevin integrator, not a bug.
    float Gauss()
    {
        float a = Uniform() + Uniform() + Uniform() + Uniform();
        return (a - 2.f) * 1.7320508f;
    }
};

} // namespace bb
