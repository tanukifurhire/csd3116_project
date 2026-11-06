// gearbox.h -- COSMETIC gearbox for the tachometer (display only).
//
// The car model has no engine or gears (drifto's drive force is constant);
// this just derives a believable gear + rpm from speed so the arcade
// tachometer sweeps up, drops ~2000 rpm on each upshift and bounces off the
// limiter. It never feeds back into the simulation, so it can't affect
// determinism; the client keeps one for whichever car the camera follows.
//
// Gear g reaches the redline at vRedline[g]. Consecutive gears differ by
// ratio r = (shiftUp - drop) / shiftUp, so an upshift at shiftUpRpm lands
// exactly `drop` rpm lower. Five gears top out just above the car's top
// speed (so the limiter bounces when pinned); a sixth only engages on nitro.
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

    // topGearSpeed: speed (m/s) at which 5th hits the redline -- the car's
    // top speed, so the limiter bounces when it's pinned.
    explicit Gearbox(float topGearSpeed = 22.0f) { configure(topGearSpeed); }

    void configure(float topGearSpeed)
    {
        float r = (shiftUpRpm - upshiftDrop) / shiftUpRpm; // 0.737
        // vRedline[5] = top; vRedline[g] = top * r^(5-g); 6th = top / r.
        for (int g = 1; g <= 6; ++g) vRedline_[g] = topGearSpeed * std::pow(r, static_cast<float>(5 - g));
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
        // Pinned in the top available gear: bounce off the limiter.
        limiter_ = target >= redlineRpm * 0.985f && (gear_ == maxGear || nitro);
        if (target > redlineRpm) target = redlineRpm;
        if (limiter_)
        {
            phase_ += dt;
            target = redlineRpm - (std::fmod(phase_, 0.1f) < 0.05f ? 0.0f : 350.0f);
        }
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
    float phase_ = 0;
    bool limiter_ = false;
};

} // namespace game
