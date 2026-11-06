// engine_synth.cpp -- see engine_synth.h for the model.
//
// Implementation notes:
//   * Pulse-train synthesis: each firing event schedules a Pulse (delay, amplitude, shape
//     time base) in a fixed pool of 8; process() sums the active ones into one pressure
//     signal, then colours it with filters (see EngineSynth::process).
//   * Real-time safety: process() runs on the audio callback thread and uses only
//     member state (fixed arrays, no heap allocation, no locks). The mixer sets targets
//     via setTargets()/shift() from the same thread; all rapid changes pass through
//     exponential smoothers (dsp::Smooth).
//   * Filter coefficients are recomputed every 32 samples, not per sample.
//   * Everything is deterministic per seed (xorshift noise), so results are repeatable.
#include "engine_synth.h"

#include <algorithm>
#include <cmath>

namespace audio {

// Selects the per-model parameter set (cylinder amplitudes/timing offsets, exhaust
// resonances [Hz, Q, gain], intake formant, saturation drive, idle/redline rpm, pop
// probability, output level) and configures the smoothers and fixed filters.
// fs in Hz; seed decorrelates the noise streams of different cars.
void EngineSynth::init(CarModel model, float fs, uint32_t seed)
{
    model_ = model;
    fs_ = fs;
    // Knuth multiplicative hash spreads small consecutive seeds across the 32-bit state.
    rng_ = dsp::Noise(seed * 2654435761u + 12345u);
    if (model == CarModel::AE86)
    {
        // 4A-GE: firing order 1-3-4-2; slightly unequal cylinders / runners
        nCyl_ = 4;
        // cylAmp_: relative pulse amplitude; cylTime_: firing-time offset as a fraction of the firing interval.
        const float a[4] = {1.0f, 0.78f, 0.93f, 0.85f}, t[4] = {0.0f, 0.045f, -0.03f, 0.02f};
        std::copy(a, a + 4, cylAmp_);
        std::copy(t, t + 4, cylTime_);
        jitterA_ = 0.07f;
        jitterT_ = 0.008f;
        // res_ entries are {centre Hz, Q, gain}: fixed formants of the exhaust path.
        res_[0] = {150, 1.4f, 0.55f};  // tailpipe / body
        res_[1] = {420, 2.2f, 0.55f};  // 4-2-1 header
        res_[2] = {1150, 2.5f, 0.60f}; // rasp
        res_[3] = {2900, 3.0f, 0.45f}; // high rasp (scaled with load)
        bodyGain_ = 0.8f;
        intakeGain_ = 0.55f; // individual throttle bodies honk
        intakeF_ = 950;
        intakeQ_ = 1.3f;
        drive_ = 2.4f;
        idleRpm_ = 900;
        redline_ = 7600;
        popProb_ = 0.012f;
        level_ = 1.3f;
        emph_ = 0.75f;
    }
    else
    {
        // 13B: two rotors 180 deg apart, nearly identical
        nCyl_ = 2;
        const float a[4] = {1.0f, 0.97f, 1, 1}, t[4] = {0.0f, 0.008f, 0, 0};
        std::copy(a, a + 4, cylAmp_);
        std::copy(t, t + 4, cylTime_);
        jitterA_ = 0.03f;
        jitterT_ = 0.003f;
        res_[0] = {95, 1.0f, 0.50f};
        res_[1] = {330, 2.5f, 0.45f};  // the "trumpet"
        res_[2] = {760, 3.5f, 0.55f};
        res_[3] = {1700, 3.5f, 0.50f}; // buzz
        bodyGain_ = 0.7f;
        intakeGain_ = 0.12f;
        intakeF_ = 1300;
        intakeQ_ = 1.0f;
        drive_ = 2.4f;
        idleRpm_ = 900;
        redline_ = 9000;
        popProb_ = 0.06f; // rotaries pop and crackle on overrun
        level_ = 1.1f;
        emph_ = 0.8f;
    }
    // Smoother time constants (seconds): rpm/throttle/pitch-rate slewing avoids zipper noise;
    // cutS_ is configured here but not read elsewhere in this file.
    rpmS_.set(fs, 0.035f);
    thrS_.set(fs, 0.03f);
    rateS_.set(fs, 0.05f);
    cutS_.set(fs, 0.012f);
    rpmS_.v = idleRpm_;
    rateS_.v = 1;
    // 22 Hz DC blocker removes the offset of the unipolar pulse train.
    dc_.set(fs, 22.0f);
    mechLp_.set(fs, 700.0f);
    popBp_.bandpass(fs, 700.0f, 0.9f);
    updateFilters();
}

// Called from the mixer once per block with the latest game state. Values are clamped
// and become smoothing targets (the audio thread never jumps straight to them).
// rpm: engine revs/min; throttle 0..1; rate: Doppler pitch multiplier (0.5..2).
void EngineSynth::setTargets(float rpm, float throttle, bool limiter, float rate, bool closeMic)
{
    rpmT_ = std::max(300.0f, std::min(rpm, 12000.0f));
    thrT_ = std::max(0.0f, std::min(throttle, 1.0f));
    limiter_ = limiter;
    rateT_ = std::max(0.5f, std::min(rate, 2.0f));
    close_ = closeMic;
}

// Starts a 90 ms gear-change cut: throttle and pulse amplitude are reduced and pops become likelier.
void EngineSynth::shift() { shiftTimer_ = 0.09f; }

// Retunes the filters from the current (smoothed) rpm, load and Doppler rate. Run every
// 32 samples from process() rather than per sample, since the biquad coefficient maths
// (sin/cos) is comparatively costly.
void EngineSynth::updateFilters()
{
    float r = curRate_;
    float rpmN = std::min(1.2f, curRpm_ / redline_);
    for (int i = 0; i < 4; ++i)
    {
        // resonances move up a little with exhaust gas temperature / rpm
        // resonances move up a little with exhaust gas temperature / rpm
        float f = res_[i].f * (0.92f + 0.16f * rpmN) * r;
        bp_[i].bandpass(fs_, f, res_[i].q);
    }
    float load = curThr_;
    // Body low-pass cutoff opens with load (power 1.2 curve) and rpm: brighter when on throttle.
    float fc = (300.0f + 5200.0f * std::pow(load, 1.2f) * (0.35f + 0.65f * rpmN) + 250.0f * rpmN) * r;
    bodyLp_.lowpass(fs_, fc, 0.8f);
    intakeBp_.bandpass(fs_, intakeF_ * (0.8f + 0.5f * rpmN) * r, intakeQ_);
}

// Waveform of one exhaust pulse as a function of normalised time u (0 at firing, 1 = one
// firing interval). Returns 0 once the pulse has finished. AE86: difference of two
// exponentials (fast attack, slow decay) minus a Gaussian dip for the header reflection.
// RX7: linear ramp up, then a falling saw with a 6-cycle ripple for port-flow buzz.
float EngineSynth::pulseShape(float u) const
{
    if (model_ == CarModel::AE86)
    {
        if (u > 1.6f) return 0.0f;
        // sharp blow-down, fast decay, then the header's negative reflection
        float p = std::exp(-u / 0.10f) - std::exp(-u / 0.012f);
        float d = u - 0.30f;
        p -= 0.35f * std::exp(-d * d / 0.0035f);
        return p;
    }
    // rotary: port opens suddenly, long sawtooth-like exhaust phase
    if (u > 0.78f) return 0.0f;
    if (u < 0.035f) return u / 0.035f;
    float v = 1.0f - (u - 0.035f) / 0.745f;
    float p = v;
    return p * (1.0f + 0.12f * std::sin(2.0f * dsp::kPi * 6.0f * u)); // port-flow buzz
}

// Called once per firing event (phase accumulator wrap). Picks the next cylinder, derives
// the pulse amplitude from jitter, load, fuel cut, shift cut and random pops, then
// schedules the pulse into a free slot of the pulse pool with a sub-interval delay.
void EngineSynth::fireEvent()
{
    cyl_ = (cyl_ + 1) % nCyl_;
    float rpm = curRpm_;
    float idleN = std::max(0.0f, 1.0f - (rpm - idleRpm_) / 1100.0f); // 1 at idle -> 0 above ~2000
    float amp = cylAmp_[cyl_] * (1.0f + jitterA_ * (1.0f + 1.5f * (1.0f - curThr_)) * rng_.white());
    if (model_ == CarModel::RX7 && idleN > 0)
    {
        // lumpy rotary idle: uneven, sometimes skipped combustions -> "brap"
        float u = rng_.uni();
        amp *= 1.0f - idleN * 0.45f + idleN * 0.7f * u * u;
        if (rng_.uni() < 0.13f * idleN) amp *= 0.15f;
    }
    // load: overrun pulses are much weaker
    float load = 0.30f + 0.70f * curThr_;
    amp *= load;
    // fuel cut: limiter bounce (~45 ms cut / 45 ms on) and gear-shift cut
    if (limiter_ && limTimer_ < 0.045f) amp *= 0.05f;
    if (shiftTimer_ > 0) amp *= 0.25f;
    // overrun pops / shift pops: unburnt fuel lighting in the exhaust
    bool overrun = curThr_ < 0.12f && rpm > 3000;
    float popP = (overrun ? popProb_ : 0.0f) + (shiftTimer_ > 0 ? popProb_ * 3.0f : 0.0f) +
                 (limiter_ ? popProb_ * 0.5f : 0.0f);
    if (rng_.uni() < popP)
    {
        popEnv_ = 0.6f + 0.6f * rng_.uni();
        amp = std::max(amp, 1.6f);
    }

    // Time between firing events in seconds: firing rate is 2*rpm/60 Hz.
    float interval = 1.0f / std::max(1.0f, (float)(2.0 * rpm / 60.0));
    float dt = (cylTime_[cyl_] + jitterT_ * rng_.white()) * interval;
    // Per-cylinder timing offset plus jitter, converted to samples (the trailing term is always zero).
    int delay = std::max(0, (int)(dt * fs_ + 0.5f) + (int)(0.03f * fs_ * interval * 0));
    for (Pulse &p : pulses_)
        if (!p.active)
        {
            p.active = true;
            p.delay = delay;
            p.t = 0;
            p.amp = amp;
            // Stored so the pulse keeps its own normalised time base even if rpm changes mid-pulse.
            p.fire = std::max(10.0f, 2.0f * rpm / 60.0f * curRate_);
            break;
        }
}

// Renders n mono samples into out (overwritten). Real-time safe: fixed-size state only,
// no allocation or locking. Signal chain: smoothed controls -> firing phase accumulator
// -> summed exhaust pulses -> DC block + radiation pre-emphasis -> body low-pass and
// resonant band-passes -> intake/mechanical noise and pops -> tanh saturation -> level.
void EngineSynth::process(float *out, int n)
{
    const float dtS = 1.0f / fs_;
    for (int i = 0; i < n; ++i)
    {
        // Control-rate filter update every 32 samples.
        if (--filterCountdown_ <= 0)
        {
            filterCountdown_ = 32;
            updateFilters();
        }
        // lumpy idle rpm hunting for the rotary (slow random walk)
        // Two cascaded one-pole filters on white noise give a slow random wander (idle hunting).
        huntNoise_ += 0.0005f * (rng_.white() - huntNoise_ * 0.02f);
        hunt_ += 0.002f * (huntNoise_ - hunt_);
        float rpmTarget = rpmT_;
        if (model_ == CarModel::RX7 && rpmT_ < 1600) rpmTarget += hunt_ * 900.0f;
        curRpm_ = rpmS_.run(rpmTarget);
        // Throttle is lifted to 20% during the shift cut.
        float thrMul = shiftTimer_ > 0 ? 0.2f : 1.0f;
        curThr_ = thrS_.run(thrT_ * thrMul);
        curRate_ = rateS_.run(rateT_);
        if (shiftTimer_ > 0) shiftTimer_ -= dtS;
        // Limiter cycle: 90 ms period, fuel cut during the first 45 ms (see fireEvent).
        limTimer_ += dtS;
        if (limTimer_ > 0.09f) limTimer_ = 0;

        // Firing frequency in Hz; phase_ (double, to avoid drift) is the fraction of a firing interval.
        float fire = 2.0f * curRpm_ / 60.0f * curRate_;
        phase_ += fire / fs_;
        if (phase_ >= 1.0)
        {
            phase_ -= 1.0;
            fireEvent();
        }

        // Pulses still waiting on their delay count down first; each active pulse advances one sample.
        // exhaust pressure: sum of active pulses
        float x = 0;
        for (Pulse &p : pulses_)
        {
            if (!p.active) continue;
            if (p.delay > 0)
            {
                --p.delay;
                continue;
            }
            float u = p.t * p.fire / fs_;
            float v = pulseShape(u);
            if (u > 1.7f)
            {
                p.active = false;
                continue;
            }
            x += p.amp * v;
            p.t += 1.0f;
        }
        // |x| (before DC removal) acts as an envelope that modulates the intake noise.
        float env = std::fabs(x);
        x = dc_.run(x);
        // the tailpipe radiates the derivative of the flow: tilt up the spectrum
        // First-order differentiator with gain compensation.
        float xe = (x - emph_ * xPrev_) * (1.0f + emph_);
        xPrev_ = x;
        x = xe;

        float rpmN = std::min(1.2f, curRpm_ / redline_);
        // rpmN: rpm as a fraction of redline (allowed to exceed 1 slightly).
        float y = bodyLp_.run(x) * bodyGain_;
        for (int k = 0; k < 4; ++k)
        {
            float g = res_[k].g;
            if (k == 3) g *= 0.3f + 0.9f * curThr_ * rpmN; // rasp grows with load and revs
            y += g * bp_[k].run(x);
        }
        // intake / induction noise, modulated by the cylinder events
        // One noise sample is shared by the intake and mechanical paths.
        float wn = rng_.white();
        float in = intakeBp_.run(wn) * env * (0.15f + curThr_) * intakeGain_ * (close_ ? 1.5f : 1.0f);
        y += in * (0.4f + rpmN);
        // mechanical background (valvetrain / rotor whirr)
        y += mechLp_.run(wn) * 0.02f * (0.3f + rpmN);
        // overrun pop crack
        if (popEnv_ > 0.001f)
        {
            y += popBp_.run(rng_.white()) * popEnv_ * 2.2f;
            // Exponential pop decay per sample (time constant ~670 samples, about 14 ms at 48 kHz).
            popEnv_ *= 0.9985f;
        }
        // saturation: harder at load (I4 rasp)
        float drive = drive_ * (0.6f + 0.6f * curThr_);
        y = dsp::tanhf_fast(y * drive) / drive;
        // perceived loudness grows with revs and load
        float lvl = level_ * (0.45f + 0.55f * std::min(1.0f, rpmN * 1.1f)) * (0.55f + 0.45f * curThr_);
        // Final gain; the 2.0 factor is a fixed make-up gain for the mixer.
        out[i] = y * lvl * 2.0f;
    }
}

} // namespace audio
