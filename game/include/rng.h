// rng.h -- deterministic PRNG for everything that must match across machines
// (track layout, decorations). Never use rand()/std::random_device in game/
// for anything gameplay-relevant: every client rebuilds the identical track
// from the uint32 seed in NetClient's MatchStarted event.
#pragma once

#include <cstdint>

namespace game {

// SplitMix64: used to derive independent seeds from (seed, index) pairs.
inline uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// PCG32 (O'Neill, pcg-random.org, "XSH RR" variant). Integer-only, so the
// output is bit-identical on every compiler/platform.
class Pcg32
{
public:
    Pcg32() { seed(0, 0); }
    Pcg32(uint64_t seedValue, uint64_t stream) { seed(seedValue, stream); }

    void seed(uint64_t seedValue, uint64_t stream)
    {
        state_ = 0;
        inc_ = (stream << 1u) | 1u;
        nextU32();
        state_ += seedValue;
        nextU32();
    }

    uint32_t nextU32()
    {
        uint64_t old = state_;
        state_ = old * 6364136223846793005ull + inc_;
        uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    // Uniform integer in [lo, hiExclusive). Like Unity's Random.Range(int,int).
    int range(int lo, int hiExclusive)
    {
        if (hiExclusive <= lo) return lo;
        uint32_t span = static_cast<uint32_t>(hiExclusive - lo);
        // Lemire-style multiply-shift; tiny bias is irrelevant for spans this small.
        return lo + static_cast<int>((static_cast<uint64_t>(nextU32()) * span) >> 32);
    }

    // Uniform float in [0, 1) with 24 bits of precision (exact in float).
    float nextFloat() { return static_cast<float>(nextU32() >> 8) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) { return lo + (hi - lo) * nextFloat(); }

private:
    uint64_t state_ = 0;
    uint64_t inc_ = 1;
};

} // namespace game
