// sfx.cpp -- procedural impacts, nitro and tyre sounds.
//
// Two kinds of code live here:
//   * make*() -- offline generators that render a whole short sound into a dsp::Buffer.
//     They are called once from Engine::loadAssets(), never from the audio callback.
//   * Skid / NitroBurn -- streaming generators with process(out, n, amount, rate) that run
//     on the audio thread; they use only member state (no allocation, no locking).
// All sounds are synthesised from white noise (xorshift), filtered noise, decaying
// exponentials and swept sines, with a fixed seed per variant so output is repeatable.
#include "sfx.h"

#include <algorithm>
#include <cmath>

namespace audio {
namespace sfx {

namespace {

// Scales the buffer so its largest absolute sample equals `peak`.
void normalize(dsp::Buffer &b, float peak)
{
    float m = 1e-9f;
    for (float v : b.data) m = std::max(m, std::fabs(v));
    for (float &v : b.data) v *= peak / m;
}

// short fade-in/out so buffers never click
void edges(dsp::Buffer &b, float fs)
{
    int a = std::min((int)b.data.size(), (int)(0.001f * fs));
    for (int i = 0; i < a; ++i) b.data[i] *= (float)i / a;
    int r = std::min((int)b.data.size(), (int)(0.02f * fs));
    for (int i = 0; i < r; ++i) b.data[b.data.size() - 1 - i] *= (float)i / r;
}

} // namespace

// Impact sounds below are generated once at startup by summing simple layers per sample.
// `variant` changes seeds and filter frequencies so repeated hits do not sound identical.
// Bump: pitch-swept sine thud (125 -> 55 Hz) + low-passed noise click + two ringing
// band-passed noise "panel" resonances. 0.45 s.
dsp::Buffer makeBump(float fs, int variant)
{
    dsp::Buffer b;
    b.rate = fs;
    dsp::Noise rng(101 + variant * 17);
    int n = (int)(0.45f * fs);
    b.data.resize(n);
    dsp::Biquad knock, knock2, click;
    float kf = 480.0f + 90.0f * variant;
    knock.bandpass(fs, kf, 4.0f);
    knock2.bandpass(fs, kf * 2.3f, 6.0f);
    click.lowpass(fs, 1800.0f);
    double ph = 0;
    for (int i = 0; i < n; ++i)
    {
        float t = i / fs;
        // body thud: sine sweeping down
        // The 1 - exp(-t/tau) factors in the envelopes are short attack ramps that avoid a click at t = 0.
        float f = 55.0f + 70.0f * std::exp(-t / 0.03f);
        ph += 2 * dsp::kPi * f / fs;
        float thud = (float)std::sin(ph) * std::exp(-t / 0.075f) * (1.0f - std::exp(-t / 0.002f));
        float w = rng.white();
        float c = click.run(w) * std::exp(-t / 0.012f);
        float panel = knock.run(w) * std::exp(-t / 0.05f) * 3.0f + knock2.run(w) * std::exp(-t / 0.03f) * 2.0f;
        b.data[i] = thud * 1.0f + c * 0.6f + panel * 0.5f;
    }
    normalize(b, 0.9f);
    edges(b, fs);
    return b;
}

// Crash: deep swept thump, band-passed noise crunch, sparse debris grains (random
// impulses with decaying density) and five decaying sine partials for metallic ring. 1.6 s.
dsp::Buffer makeCrash(float fs, int variant)
{
    dsp::Buffer b;
    b.rate = fs;
    dsp::Noise rng(977 + variant * 31);
    int n = (int)(1.6f * fs);
    b.data.resize(n);
    dsp::Biquad crunch, grainBp, sub;
    crunch.bandpass(fs, 850.0f + 120.0f * variant, 0.8f);
    grainBp.bandpass(fs, 2600.0f, 1.5f);
    const int nm = 5;
    float mf[nm] = {430, 1037, 1611, 2390, 3480};
    float md[nm] = {0.9f, 0.7f, 0.55f, 0.4f, 0.3f};
    float ma[nm] = {0.35f, 0.28f, 0.2f, 0.14f, 0.1f};
    double mph[nm] = {0};
    for (int k = 0; k < nm; ++k) mf[k] *= 1.0f + 0.06f * rng.white() + 0.05f * variant;
    double ph = 0;
    float grain = 0;
    for (int i = 0; i < n; ++i)
    {
        float t = i / fs;
        float f = 36.0f + 50.0f * std::exp(-t / 0.05f);
        ph += 2 * dsp::kPi * f / fs;
        float thump = (float)std::sin(ph) * std::exp(-t / 0.22f) * (1.0f - std::exp(-t / 0.003f));
        float w = rng.white();
        float cr = crunch.run(w) * std::exp(-t / 0.16f) * 2.5f;
        // sparse debris grains
        // Grain trigger probability per sample = 400 grains/s at t = 0, decaying with time.
        if (rng.uni() < 400.0f / fs * std::exp(-t / 0.35f)) grain = 1.0f;
        grain *= 0.996f;
        float gr = grainBp.run(w * grain) * 3.0f;
        float ring = 0;
        for (int k = 0; k < nm; ++k)
        {
            mph[k] += 2 * dsp::kPi * mf[k] / fs;
            ring += ma[k] * (float)std::sin(mph[k]) * std::exp(-t / md[k]);
        }
        ring *= 1.0f - std::exp(-t / 0.004f);
        b.data[i] = thump * 1.2f + cr + gr * 0.6f + ring * 0.7f;
    }
    normalize(b, 0.95f);
    edges(b, fs);
    return b;
}

// Scrape: noise through two narrow band-passes (metal grind) with a randomly stepped
// amplitude modulation for judder and a sustain-then-decay envelope. 1.3 s.
dsp::Buffer makeScrape(float fs, int variant)
{
    dsp::Buffer b;
    b.rate = fs;
    dsp::Noise rng(4242 + variant * 7);
    int n = (int)(1.3f * fs);
    b.data.resize(n);
    dsp::Biquad a, c, lp;
    a.bandpass(fs, 720.0f + 60.0f * variant, 6.0f);
    c.bandpass(fs, 1850.0f + 100.0f * variant, 7.0f);
    lp.lowpass(fs, 3000.0f);
    float am = 0, amT = 0;
    for (int i = 0; i < n; ++i)
    {
        float t = i / fs;
        // New random target roughly every 12.5 ms; `am` follows it with a one-pole smoother.
        if (i % 600 == 0) amT = 0.4f + 0.6f * rng.uni(); // grinding judder
        am += 0.01f * (amT - am);
        float env = (1.0f - std::exp(-t / 0.05f)) * (t < 0.5f ? 1.0f : std::exp(-(t - 0.5f) / 0.3f));
        float w = rng.white();
        b.data[i] = (a.run(w) * 2.5f + c.run(w) * 2.0f + lp.run(w) * 0.3f) * env * am;
    }
    normalize(b, 0.8f);
    edges(b, fs);
    return b;
}

// Nitro ignition: noise whoosh whose band-pass sweeps 250 -> 3000 Hz (exponential), a
// high-passed puff and a low sine whump. 0.9 s.
dsp::Buffer makeNitroIgnite(float fs, int variant)
{
    dsp::Buffer b;
    b.rate = fs;
    dsp::Noise rng(555 + variant * 13);
    int n = (int)(0.9f * fs);
    b.data.resize(n);
    dsp::Biquad sweep, puff;
    puff.highpass(fs, 2500.0f);
    double ph = 0;
    for (int i = 0; i < n; ++i)
    {
        float t = i / fs;
        float fc = 250.0f * std::pow(12.0f, std::min(1.0f, t / 0.45f)); // 250 -> 3000 Hz
        // Coefficients are refreshed every 16 samples rather than per sample (cheaper).
        if (i % 16 == 0) sweep.bandpass(fs, fc, 1.8f);
        float w = rng.white();
        float env = (1.0f - std::exp(-t / 0.03f)) * std::exp(-t / 0.35f);
        float whoosh = sweep.run(w) * env * 3.0f;
        float p = puff.run(w) * std::exp(-t / 0.05f);
        ph += 2 * dsp::kPi * (48.0f + 30.0f * std::exp(-t / 0.05f)) / fs;
        float whump = (float)std::sin(ph) * std::exp(-t / 0.12f) * (1.0f - std::exp(-t / 0.004f));
        b.data[i] = whoosh + p * 0.5f + whump * 0.9f;
    }
    normalize(b, 0.9f);
    edges(b, fs);
    return b;
}

// ---------------------------------------------------------------------------

// ---- Real-time generators (used inside the audio callback) ----
// Skid::init: seeds noise, selects the recorded loop if available (else a procedural
// fallback is used), and starts at a random loop position so cars are out of phase.
void Skid::init(float fs, uint32_t seed, const dsp::Buffer *loop)
{
    fs_ = fs;
    rng_ = dsp::Noise(seed * 7919u + 3u);
    loop_ = (loop && !loop->empty()) ? loop : nullptr;
    amt_.set(fs, 0.04f);
    pos_ = rng_.uni() * (loop_ ? loop_->data.size() : 1);
    scrubLp_.lowpass(fs, 900.0f);
}

// Tyre squeal for n samples. amount = slip 0..1 (smoothed); rate = Doppler pitch factor.
// Uses the recorded loop resampled with linear interpolation, or three narrow noise
// band-passes as a fallback; a low-pass opens with slip and a quiet noise "scrub" is added.
void Skid::process(float *out, int n, float amount, float rate)
{
    for (int i = 0; i < n; ++i)
    {
        float a = amt_.run(std::max(0.0f, std::min(amount, 1.0f)));
        if (a < 1e-4f)
        {
            out[i] = 0;
            continue;
        }
        if (--countdown_ <= 0)
        {
            // Control-rate update every 64 samples.
            countdown_ = 64;
            // Occasional new random pitch-wobble target; fm_ glides to it.
            if (rng_.uni() < 0.05f) fmT_ = rng_.white();
            fm_ += 0.2f * (fmT_ - fm_);
            float pitch = (0.85f + 0.3f * a + 0.04f * fm_) * rate;
            if (!loop_)
            {
                bp_[0].bandpass(fs_, 980.0f * pitch, 14.0f);
                bp_[1].bandpass(fs_, 1720.0f * pitch, 16.0f);
                bp_[2].bandpass(fs_, 2650.0f * pitch, 12.0f);
            }
            // low slip = scrubby / muffled, high slip = full squeal
            loopLp_.lowpass(fs_, 1200.0f + 9000.0f * a * a, 0.7f);
        }
        // Output gain curve: slightly expanding, so light slip is quiet.
        float g = std::pow(a, 1.3f);
        float w = rng_.white();
        float scrub = scrubLp_.run(w) * 0.25f * (1.0f - 0.5f * a);
        float s;
        if (loop_)
        {
            // Looping read with linear interpolation; positions wrap at the loop length.
            size_t len = loop_->data.size();
            size_t i0 = (size_t)pos_;
            float fr = (float)(pos_ - i0);
            float v = loop_->data[i0 % len] * (1 - fr) + loop_->data[(i0 + 1) % len] * fr;
            float pitch = (0.85f + 0.3f * a + 0.04f * fm_) * rate;
            pos_ += pitch * loop_->rate / fs_;
            if (pos_ >= len) pos_ -= len;
            s = loopLp_.run(v) * 0.9f;
        }
        else
            s = loopLp_.run(bp_[0].run(w) * 1.0f + bp_[1].run(w) * 0.8f + bp_[2].run(w) * 0.5f) * 3.5f;
        out[i] = (s + scrub) * g;
    }
}

// Nitro hiss (high-passed noise), roar (low-passed noise) and a narrow whistle.
void NitroBurn::init(float fs, uint32_t seed)
{
    fs_ = fs;
    rng_ = dsp::Noise(seed * 31337u + 7u);
    hiss_.highpass(fs, 2800.0f, 0.7f);
    roar_.lowpass(fs, 320.0f, 0.9f);
    whistle_.bandpass(fs, 3300.0f, 25.0f);
    amt_.set(fs, 0.08f);
}

// Continuous nitro burn for n samples; amount = 0..1 intensity (smoothed). rate is unused.
void NitroBurn::process(float *out, int n, float amount, float rate)
{
    (void)rate;
    for (int i = 0; i < n; ++i)
    {
        float a = amt_.run(std::max(0.0f, std::min(amount, 1.0f)));
        // Free-running time for the flutter LFOs; wrapped to keep float precision.
        t_ += 1.0f / fs_;
        if (t_ > 1000.0f) t_ -= 1000.0f;
        if (a < 1e-4f)
        {
            out[i] = 0;
            continue;
        }
        float w = rng_.white();
        // Flutter: two sine LFOs (23 Hz and 37 Hz) amplitude-modulate the roar.
        float flutter = 1.0f + 0.35f * std::sin(2 * dsp::kPi * 23.0f * t_) + 0.2f * std::sin(2 * dsp::kPi * 37.0f * t_);
        float v = hiss_.run(w) * 0.45f + roar_.run(w) * 2.2f * flutter + whistle_.run(w) * 1.2f;
        out[i] = v * a;
    }
}

} // namespace sfx
} // namespace audio
