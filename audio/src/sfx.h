// sfx.h -- procedural one-shots and continuous effect generators (internal).
#pragma once

#include "dsp.h"

namespace audio {
namespace sfx {

// One-shot buffers rendered at init (mono, at fs). variant picks the seed.
dsp::Buffer makeBump(float fs, int variant);
dsp::Buffer makeCrash(float fs, int variant);
dsp::Buffer makeScrape(float fs, int variant);
dsp::Buffer makeNitroIgnite(float fs, int variant);

// Tyre squeal. Uses the recorded loop if one was loaded, otherwise resonant
// filtered noise. amount 0..1.
class Skid
{
public:
    void init(float fs, uint32_t seed, const dsp::Buffer *loop);
    void process(float *out, int n, float amount, float rate);

private:
    float fs_ = 48000;
    const dsp::Buffer *loop_ = nullptr;
    double pos_ = 0;
    dsp::Noise rng_;
    dsp::Biquad bp_[3], scrubLp_, loopLp_;
    dsp::Smooth amt_;
    float fm_ = 0, fmT_ = 0;
    int countdown_ = 0;
};

// Nitro burn: hiss + roar + flutter. amount 0..1.
class NitroBurn
{
public:
    void init(float fs, uint32_t seed);
    void process(float *out, int n, float amount, float rate);

private:
    float fs_ = 48000;
    dsp::Noise rng_;
    dsp::Biquad hiss_, roar_, whistle_;
    dsp::Smooth amt_;
    float t_ = 0;
};

} // namespace sfx
} // namespace audio
