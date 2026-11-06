// gearbox.h -- COSMETIC gearbox for the tachometer (display only).
//
// The car model has no engine or gears (drifto's drive force is constant);
// this just derives a believable gear + rpm from speed so the arcade
// tachometer sweeps up and drops ~2000 rpm on each upshift. It never feeds
// back into the simulation, so it can't affect determinism; the client keeps
// one per car for the tacho and the engine sound.
//
// Gear g reaches the redline at vRedline[g]. Consecutive gears differ by
// ratio r = (shiftUp - drop) / shiftUp, so an upshift at shiftUpRpm lands
// exactly `drop` rpm lower. Fifth is geared long so that cruising at the
// car's top speed sits at topSpeedRpm (~7200), below the shift point and the
// redline: a pinned throttle hums steadily instead of banging off a limiter.
// A sixth only engages on nitro. Wheelspin, or the over-speed left after a
// nitro burn, is eased into a soft ceiling just under the redline.
#pragma once

#include <cmath>

namespace game {

class Gearbox
{
public:
    float idleRpm = 900.0f;
    float redlineRpm = 8000.0f;
    float shiftUpRpm = 7600.0f;
    float upshiftDrop = 2000.0f;
    float shiftDownRpm = 3600.0f;
    float shiftHold = 0.25f;      // seconds between shifts (no hunting)
    float wheelspinBoost = 0.15f; // extra rpm fraction at a big drift angle
    float topSpeedRpm = 7200.0f;  // 5th-gear rpm at the car's top speed
    float softCeiling = 7600.0f;  // above this the rpm eases toward the redline

    // topGearSpeed: the car's top speed (m/s); 5th turns topSpeedRpm there.
    explicit Gearbox(float topGearSpeed = 22.0f) { configure(topGearSpeed); }

    void configure(float topGearSpeed)
    {
        float r = (shiftUpRpm - upshiftDrop) / shiftUpRpm; // 0.737
        // vRedline[5] = v5 (redline in 5th, above top speed);
        // vRedline[g] = v5 * r^(5-g). The nitro 6th is a little taller
        // (v5 / r^1.25) so the nitro top speed (~1.45x) also cruises near
        // topSpeedRpm rather than up against the redline.
        float v5 = topGearSpeed * redlineRpm / topSpeedRpm;
        for (int g = 1; g <= 5; ++g) vRedline_[g] = v5 * std::pow(r, static_cast<float>(5 - g));
        vRedline_[6] = v5 / std::pow(r, 1.25f);
    }
    void reset()
    {
        gear_ = 1;
        rpm_ = idleRpm;
        hold_ = 0;
        limiter_ = false;
    }

    // speed in m/s; nitro = burning; driftAngleDeg for wheelspin.
    void update(float speed, bool nitro, float driftAngleDeg, float dt)
    {
        hold_ = hold_ > dt ? hold_ - dt : 0.0f;
        int maxGear = nitro ? 6 : 5;
        if (gear_ > maxGear && hold_ == 0.0f) { gear_ = maxGear; hold_ = shiftHold; } // nitro over: back to 5th
        float target = rpmIn(gear_, speed);
        if (hold_ == 0.0f)
        {
            // Shift when the *needle* gets there, and drop it by `upshiftDrop`
            // (then it settles on the new gear's rpm).
            if (rpm_ >= shiftUpRpm && target >= shiftUpRpm && gear_ < maxGear)
            {
                ++gear_;
                hold_ = shiftHold;
                rpm_ -= upshiftDrop;
            }
            else if (gear_ > 1 && rpmIn(gear_, speed) < shiftDownRpm && rpmIn(gear_ - 1, speed) < shiftUpRpm - 400.0f)
            {
                --gear_;
                hold_ = shiftHold;
            }
            target = rpmIn(gear_, speed);
        }
        float spin = std::fmin(std::fabs(driftAngleDeg) / 45.0f, 1.0f) * wheelspinBoost;
        target *= 1.0f + spin;
        // Soft ceiling: above softCeiling the rpm approaches the redline
        // smoothly and never reaches it -- no limiter cut, no bounce.
        if (target > softCeiling)
        {
            float room = redlineRpm * 0.985f - softCeiling;
            target = softCeiling + room * (1.0f - std::exp(-(target - softCeiling) / room));
        }
        // Needle deep in the red zone (HUD lamp only; never a fuel cut).
        limiter_ = target >= redlineRpm * 0.96f;
        // Needle: quick to rise, a little slower to fall.
        float k = 1.0f - std::exp(-(target > rpm_ ? 25.0f : 10.0f) * dt);
        rpm_ += (target - rpm_) * k;
        if (rpm_ < idleRpm) rpm_ = idleRpm;
    }

    int gear() const { return gear_; }
    float rpm() const { return rpm_; }
    bool revLimiter() const { return limiter_; }
    float redlineSpeed(int g) const { return vRedline_[g < 1 ? 1 : (g > 6 ? 6 : g)]; }

private:
    float rpmIn(int g, float speed) const
    {
        float r = redlineRpm * speed / vRedline_[g];
        return r < idleRpm ? idleRpm : r;
    }

    float vRedline_[7] = {0, 0, 0, 0, 0, 0, 0};
    int gear_ = 1;
    float rpm_ = 900.0f;
    float hold_ = 0;
    bool limiter_ = false;
};

} // namespace game
