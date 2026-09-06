#pragma once
// GATES: the per-lane gate law, pure so the host suite can test it.
//
// Two threads share a lane: the AUDIO callback arms it (sample-accurate,
// from Engine::gateFired) and the CONTROL poll (~1 ms) writes the CV on
// change. Nothing here touches hardware; the shell owns the volatile
// storage and the SetVolts calls. Timing facts the law is built on:
//   - a re-hit while the lane is still high must be HEARD by an envelope
//     ("each gate hits once at the open, as drums do"): the poll drops the
//     lane for one poll period (~1 ms, more if the control thread stalls)
//     and raises it again — in every length mode, not only Hold.
//   - Hold ("to next hit") never counts down; a lane stays up until the
//     same lane fires again. Leaving Hold for a timed length must close a
//     held lane within that length, not after the 2^28-sample sentinel.
#include <cstdint>

namespace bb {

constexpr int   kGateHold = 1 << 28;   // Gate Length "Hold to next hit" sentinel
constexpr float kGateVMax = 5.f;       // fixed level and the velocity ceiling
constexpr float kGateVMin = 1.5f;      // quietest velocity (still a valid gate)

// Volts for a hit: fixed 5 V, or velocity from salience (step RMS / loudest
// step, 0..1); a grammar ACCENT lifts a velocity hit to the ceiling. The
// Density lens's per-step density is deliberately NOT folded in: it exists
// only under that lens, and velocity must not change with the Classifier.
inline float GateVolts(bool velocity, float sal, bool accent)
{
    if(!velocity || accent) return kGateVMax;
    if(sal < 0.f) sal = 0.f;
    if(sal > 1.f) sal = 1.f;
    return kGateVMin + (kGateVMax - kGateVMin) * sal;
}

// Audio side, once per block per lane. `fired` = this lane's bit of
// gateFired; `lenSamps` = the current Gate Length (or kGateHold); `n` =
// block size. References are volatile because the shell's lanes are.
inline void GateArm(volatile int& cd, volatile float& lvl, volatile bool& retrig,
                    bool fired, int lenSamps, float volts, int n)
{
    if(fired)
    {
        if(cd > 0) retrig = true;   // re-hit while high: one poll low, then up
        cd  = lenSamps;
        lvl = volts;
    }
    else if(cd > 0 && lenSamps != kGateHold)
    {
        int c = cd;
        if(c > lenSamps) c = lenSamps;   // Hold → timed: close within one length
        cd = c - n;
    }
}

// Poll side: the volts the lane wants this poll. Consumes a pending
// retrigger (the low poll); the next poll returns the level again.
inline float GatePollVolts(volatile int& cd, volatile float& lvl, volatile bool& retrig)
{
    if(retrig) { retrig = false; return 0.f; }
    return cd > 0 ? lvl : 0.f;
}

} // namespace bb
