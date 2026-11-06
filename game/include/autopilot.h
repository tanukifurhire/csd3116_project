// autopilot.h -- driver for drifto's frictionless car. Used by the race bots,
// the tests (drivability regression), the menu attract mode and --demo.
//
// With no tyre grip the only thing that turns the *velocity* is the drive
// force along the heading, so the driver aims the nose ahead of the travel
// direction (a drift angle proportional to how far the velocity has to
// swing) and steers the heading towards that.
#pragma once

#include <cmath>

#include "game/include/car_physics.h"
#include "game/include/track.h"

namespace game {

struct AutopilotParams
{
    float lookAhead = 11.0f;   // metres along the centreline
    float lateral = 0.0f;      // target lateral offset (+ right)
    float slipGain = 1.6f;     // desired drift angle per radian of velocity error
    float maxSlip = 1.05f;     // radians (~60 deg)
    float steerGain = 3.0f;
    float brakeLook = 26.0f;   // scan this far ahead for the tightest curve
    float cornerFactor = 0.8f; // target speed = sqrt(drive * R * factor)
    float scrubGain = 0.12f;   // extra slip (rad) per m/s over the target speed
    float maxScrub = 1.2f;
    bool digital = false;      // keyboard-like: full lock or nothing
    float nitroMinGauge = 0.0f;// > 0: burn nitro on straight-ish road when above this gauge
};

namespace detail {
// Signed angle from a to b about +Y (both horizontal), + = b is left of a.
inline float yawBetween(const dm::Vec3 &a, const dm::Vec3 &b)
{
    return std::atan2(a.z * b.x - a.x * b.z, a.x * b.x + a.z * b.z);
}
inline dm::Vec3 flat(dm::Vec3 v)
{
    v.y = 0;
    return dm::normalize(v);
}
} // namespace detail

inline net::CarInput autopilotInput(const CarSimState &s, const Track &t, const AutopilotParams &ap,
                                    uint32_t tick = 0)
{
    using detail::flat;
    using detail::yawBetween;
    // Look-ahead point on the road.
    float sa = s.sectionS + ap.lookAhead;
    uint32_t idx = s.section;
    while (idx + 1 < t.sectionCount() && sa > t.section(idx).length) { sa -= t.section(idx).length; ++idx; }
    dm::Vec3 target = t.section(idx).pointAt(sa, ap.lateral);
    dm::Vec3 toTarget = flat(target - s.position);
    dm::Vec3 heading = flat(dm::forwardOf(s.rotation));
    dm::Vec3 vel = s.speed > 3.0f ? flat(s.velocity) : heading;

    float errV = yawBetween(vel, toTarget);                    // how far the velocity must swing
    float slip = errV * ap.slipGain;

    // Speed management without brakes: too fast for the tightest curve
    // ahead => throw the nose into that curve early and scrub speed sideways.
    {
        float maxK = 0, kSign = 0;
        float sb = s.sectionS;
        uint32_t j = s.section;
        for (float ahead = 0; ahead <= ap.brakeLook; ahead += 4.0f)
        {
            float sc = sb + ahead;
            uint32_t k = j;
            while (k + 1 < t.sectionCount() && sc > t.section(k).length) { sc -= t.section(k).length; ++k; }
            float c = t.section(k).curvature();
            if (std::fabs(c) > maxK) { maxK = std::fabs(c); kSign = c > 0 ? 1.0f : -1.0f; }
        }
        if (maxK > 1e-4f)
        {
            const CarParams &p = defaultCarParams();
            float vTarget = std::sqrt(p.driveForce * ap.cornerFactor / maxK);
            float over = s.speed - vTarget;
            if (over > 0) slip += kSign * std::fmin(over * ap.scrubGain, ap.maxScrub);
        }
    }
    slip = dm::clampf(slip, -ap.maxSlip - ap.maxScrub, ap.maxSlip + ap.maxScrub);
    float desiredYaw = yawBetween(heading, vel) + slip;        // heading -> (vel rotated by slip)
    float steer = dm::clampf(-desiredYaw * ap.steerGain, -1.0f, 1.0f);
    if (ap.digital) steer = steer > 0.3f ? 1.0f : (steer < -0.3f ? -1.0f : 0.0f);

    bool nitro = false;
    if (ap.nitroMinGauge > 0 && s.nitro > ap.nitroMinGauge)
    {
        // Burn on straights: the road 30 m ahead points roughly where we go.
        float sb = s.sectionS + 30.0f;
        uint32_t j = s.section;
        while (j + 1 < t.sectionCount() && sb > t.section(j).length) { sb -= t.section(j).length; ++j; }
        float bend = std::fabs(dm::wrapAngle(t.section(j).yawAt(sb) - t.section(s.section).yawAt(s.sectionS)));
        nitro = bend < 0.35f && std::fabs(s.driftAngle) < 20.0f;
    }
    return driveInput(tick, steer, nitro);
}

inline net::CarInput autopilotInput(const CarSimState &s, const Track &t, float look = 11.0f, bool digital = false,
                                    uint32_t tick = 0)
{
    AutopilotParams ap;
    ap.lookAhead = look;
    ap.digital = digital;
    return autopilotInput(s, t, ap, tick);
}

} // namespace game
