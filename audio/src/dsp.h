// dsp.h -- small DSP building blocks for the audio module (internal).
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace audio {
namespace dsp {

constexpr float kPi = 3.14159265358979f;

// xorshift noise, white in [-1, 1]
struct Noise
{
    uint32_t s = 0x9E3779B9u;
    explicit Noise(uint32_t seed = 0x9E3779B9u) : s(seed ? seed : 1u) {}
    uint32_t nextU()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float white() { return (float)(nextU() >> 8) * (2.0f / 16777216.0f) - 1.0f; }
    float uni() { return (float)(nextU() >> 8) * (1.0f / 16777216.0f); }
};

// RBJ biquad, transposed direct form II
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void bandpass(float fs, float f, float q) // 0 dB peak gain
    {
        f = std::fmin(f, fs * 0.45f);
        float w = 2 * kPi * f / fs, al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al;
        b0 = al / a0;
        b1 = 0;
        b2 = -al / a0;
        a1 = -2 * c / a0;
        a2 = (1 - al) / a0;
    }
    void lowpass(float fs, float f, float q = 0.7071f)
    {
        f = std::fmin(f, fs * 0.45f);
        float w = 2 * kPi * f / fs, al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al;
        b0 = (1 - c) / 2 / a0;
        b1 = (1 - c) / a0;
        b2 = b0;
        a1 = -2 * c / a0;
        a2 = (1 - al) / a0;
    }
    void highpass(float fs, float f, float q = 0.7071f)
    {
        f = std::fmin(f, fs * 0.45f);
        float w = 2 * kPi * f / fs, al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al;
        b0 = (1 + c) / 2 / a0;
        b1 = -(1 + c) / a0;
        b2 = b0;
        a1 = -2 * c / a0;
        a2 = (1 - al) / a0;
    }
    float run(float x)
    {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// one-pole low pass (cutoff in Hz)
struct OnePole
{
    float a = 1, y = 0;
    void set(float fs, float f) { a = 1.0f - std::exp(-2 * kPi * std::fmin(f, fs * 0.49f) / fs); }
    float run(float x) { return y += a * (x - y); }
};

// DC blocker / gentle high pass
struct DcBlock
{
    float r = 0.995f, x1 = 0, y1 = 0;
    void set(float fs, float f) { r = std::exp(-2 * kPi * f / fs); }
    float run(float x)
    {
        float y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

// exponential smoother towards a target (time constant in seconds)
struct Smooth
{
    float v = 0, k = 1;
    void set(float fs, float tau) { k = tau > 0 ? 1.0f - std::exp(-1.0f / (tau * fs)) : 1.0f; }
    float run(float target) { return v += k * (target - v); }
};

inline float softclip(float x) { return x / (1.0f + std::fabs(x)) * 1.0f; }
inline float tanhf_fast(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// mono sample buffer
struct Buffer
{
    std::vector<float> data;
    float rate = 48000; // source sample rate
    bool empty() const { return data.empty(); }
};

} // namespace dsp
} // namespace audio
