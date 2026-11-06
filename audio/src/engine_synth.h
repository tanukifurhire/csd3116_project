// engine_synth.h -- procedural engine voices (internal to audio/).
//
// Firing model: a train of exhaust pressure pulses at the firing frequency
//   inline-4 four-stroke : 4 cylinders / 2 revs  -> f = 2 * rpm / 60
//   13B two-rotor rotary : 2 rotors x 1 / rev     -> f = 2 * rpm / 60
// The engines share the fundamental; the character comes from
//   * pulse shape: I4 = short sharp blow-down + header reflection (raspy,
//     lots of upper harmonics with comb notches); rotary = long saw-like
//     exhaust phase from the peripheral port (buzzy, smooth, strong low
//     harmonics, the "brap");
//   * per-cylinder variation: I4 has 4 slightly different cylinders (firing
//     order 1-3-4-2) -> energy at half / quarter orders = rasp & lope;
//     rotary's two rotors are nearly identical -> clean harmonic series;
//   * idle: rotary idle is lumpy (random weak / skipped combustions, rpm
//     hunting); the 4A-GE idles evenly;
//   * exhaust / intake resonances (fixed formants), intake noise
//     (4A-GE individual throttle bodies honk; little on the rotary),
//     load-dependent brightness and saturation, overrun pops (rotaries pop a
//     lot), rev-limiter fuel cut, shift cut.
#pragma once

#include "../include/audio.h"
#include "dsp.h"

namespace audio {

class EngineSynth
{
public:
    void init(CarModel model, float fs, uint32_t seed);
    // targets, smoothed internally. rate = doppler pitch factor.
    void setTargets(float rpm, float throttle, bool limiter, float rate, bool closeMic);
    void shift();
    void process(float *out, int n); // mono, adds nothing: overwrites out[0..n)
    CarModel model() const { return model_; }

private:
    struct Pulse
    {
        int delay = 0;      // samples until it starts
        float t = 0;        // samples since start
        float amp = 0;
        float fire = 100;   // firing frequency when fired (for the normalised shape)
        bool active = false;
    };
    struct Res
    {
        float f, q, g;
    };

    void fireEvent();
    float pulseShape(float u) const;
    void updateFilters();

    CarModel model_ = CarModel::AE86;
    float fs_ = 48000;
    dsp::Noise rng_;
    // params
    int nCyl_ = 4;
    float cylAmp_[4] = {1, 1, 1, 1}, cylTime_[4] = {0, 0, 0, 0};
    float jitterA_ = 0.05f, jitterT_ = 0.005f;
    Res res_[4]{};
    float bodyGain_ = 1, intakeGain_ = 0, intakeF_ = 900, intakeQ_ = 1.2f, drive_ = 2;
    float idleRpm_ = 900, redline_ = 7600, popProb_ = 0.02f, level_ = 1;
    float emph_ = 0.7f, xPrev_ = 0; // radiation pre-emphasis (sound ~ d/dt flow)
    // state
    dsp::Smooth rpmS_, thrS_, rateS_, cutS_;
    float rpmT_ = 900, thrT_ = 0, rateT_ = 1;
    bool limiter_ = false, close_ = false;
    double phase_ = 0;
    int cyl_ = 0;
    Pulse pulses_[8];
    float limTimer_ = 0, shiftTimer_ = 0, popEnv_ = 0;
    float huntNoise_ = 0, hunt_ = 0;
    dsp::DcBlock dc_;
    dsp::Biquad bp_[4], intakeBp_, bodyLp_, popBp_;
    dsp::OnePole mechLp_;
    int filterCountdown_ = 0;
    float curRpm_ = 900, curThr_ = 0, curRate_ = 1;
};

} // namespace audio
