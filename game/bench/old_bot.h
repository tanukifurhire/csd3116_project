// old_bot.h -- FROZEN copy of the pre-Reynolds race bot (race.cpp's
// BotDriver before the steering-behaviours rewrite), kept only so the bench
// can measure old vs new on the same seeds. Not used by the game.
#pragma once

#include <cmath>

#include "game/include/car_physics.h"
#include "game/include/track.h"

namespace bench {

using namespace game;

inline float oldMaxCurvatureAhead(const CarSimState &s, const Track &t, float to)
{
    float m = 0;
    for (float a = 0; a <= to; a += 4.0f)
    {
        float sc = s.sectionS + a;
        uint32_t k = s.section;
        while (k + 1 < t.sectionCount() && sc > t.section(k).length) { sc -= t.section(k).length; ++k; }
        m = std::fmax(m, std::fabs(t.section(k).curvature()));
    }
    return m;
}

struct OldBot
{
    float cornerFactor = 0.76f;
    float speedLook = 40.0f;
    float nitroMinGauge = 0.35f;
    float laneBias = 0.0f;
    int replanTicks = 5;

    float steer1 = 0, steer2 = 0;
    int ticksLeft = 0;
    float stuckTime = 0, recoverTime = 0, recoverSteer = 0;

    net::CarInput drive(const CarSimState &car, const Track &track, const CarParams &params, uint32_t tick)
    {
        const int h1 = 15, h2 = 30;
        const float dt = kFixedDt;
        if (recoverTime > 0)
        {
            recoverTime -= dt;
            return driveInput(tick, recoverSteer, false);
        }
        stuckTime = car.alive && car.speed < 3.0f ? stuckTime + dt : 0.0f;
        if (stuckTime > 0.7f)
        {
            const TrackSection &sec = track.section(car.section);
            dm::Vec3 f = dm::forwardOf(car.rotation);
            float headingYaw = std::atan2(-f.x, -f.z);
            float err = dm::wrapAngle(sec.yawAt(car.sectionS) - headingYaw);
            recoverSteer = err > 0 ? -1.0f : 1.0f;
            recoverTime = 1.2f;
            stuckTime = 0;
            ticksLeft = 0;
            return driveInput(tick, recoverSteer, false);
        }
        if (ticksLeft <= 0)
        {
            const float cand[5] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
            float best = -1e30f;
            for (float a : cand)
                for (float b : cand)
                {
                    CarSimState s = car;
                    float score = 0;
                    bool dead = false;
                    for (int k = 0; k < h1 + h2 && !dead; ++k)
                    {
                        stepCar(s, driveInput(0, k < h1 ? a : b, false), track, params);
                        if (!s.alive) { score = -1e4f + 10.0f * k; dead = true; }
                    }
                    if (!dead)
                    {
                        float edge = track.edgeOffset();
                        float over = std::fmax(0.0f, std::fabs(s.lateral) - (edge - 2.2f));
                        float k = oldMaxCurvatureAhead(s, track, speedLook);
                        float vmax = k > 1e-4f ? std::sqrt(params.driveForce * cornerFactor / k) : 1e9f;
                        float fast = std::fmax(0.0f, s.speed - vmax);
                        float travelYaw = std::atan2(-s.velocity.x, -s.velocity.z);
                        float yawErr = std::fabs(dm::wrapAngle(travelYaw - track.section(s.section).yawAt(s.sectionS)));
                        float lane = s.lateral - laneBias;
                        score = (s.distance - car.distance) - 3.0f * over * over - fast * fast - 2.0f * yawErr -
                                0.05f * lane * lane;
                    }
                    if (score > best) { best = score; steer1 = a; steer2 = b; }
                }
            ticksLeft = replanTicks;
        }
        --ticksLeft;
        float kAhead = oldMaxCurvatureAhead(car, track, 35.0f);
        float vAllowed = kAhead > 1e-4f ? std::sqrt(params.driveForce * cornerFactor / kAhead) : 1e9f;
        bool nitro = car.nitro > nitroMinGauge && std::fabs(car.driftAngle) < 30.0f && car.speed < vAllowed - 2.0f;
        return driveInput(tick, steer1, nitro);
    }
};

} // namespace bench
