// Third-person chase camera. wanted() computes the ideal camera pose from
// the car; update() (called once per rendered frame with the frame dt in
// seconds) moves the actual camera towards it with frame-rate independent
// exponential smoothing, so the camera lags slightly behind in turns.
// chase_camera.cpp
#include "game/include/chase_camera.h"

#include <cmath>

namespace game {

// Ideal camera pose: behind the car along a direction blending the car's
// heading with its travel direction, raised by `height`, looking `lookAhead`
// ahead. Outputs position, look-at target and up vector.
void ChaseCamera::wanted(const dm::Vec3 &carPos, const dm::Quat &carRot, const dm::Vec3 &carVel, dm::Vec3 &pos,
                         dm::Vec3 &look, dm::Vec3 &upDir) const
{
    dm::Vec3 carUp = dm::upOf(carRot);
    dm::Vec3 heading = dm::forwardOf(carRot);
    heading.y = 0;
    heading = dm::normalize(heading);
    if (dm::lengthSq(heading) < 1e-6f) heading = {0, 0, -1};

    // Blend heading with the travel direction (only when actually moving).
    dm::Vec3 vel{carVel.x, 0, carVel.z};
    float sp = dm::length(vel);
    dm::Vec3 dir = heading;
    if (sp > 2.0f)
    {
        float w = velocityBlend * dm::clampf((sp - 2.0f) / 6.0f, 0.0f, 1.0f);
        dir = dm::normalize(heading * (1.0f - w) + (vel / sp) * w);
    }
    pos = carPos - dir * distance + dm::Vec3{0, height, 0};
    look = carPos + dir * lookAhead + dm::Vec3{0, lookHeight, 0};
    // Mostly world-up; a hint of the car's roll like LookRotation(.., car.up).
    upDir = dm::normalize(dm::Vec3{0, 1, 0} * 0.85f + carUp * 0.15f);
}

// Place the camera exactly at its ideal pose (no smoothing), e.g. on
// respawn or first frame.
void ChaseCamera::snap(const dm::Vec3 &carPos, const dm::Quat &carRot, const dm::Vec3 &carVel)
{
    wanted(carPos, carRot, carVel, position, target, up);
    initialised = true;
}

// Smooth towards the ideal pose. 1 - exp(-k*dt) gives the same response
// regardless of frame rate. Position and look-at use different rates.
void ChaseCamera::update(const dm::Vec3 &carPos, const dm::Quat &carRot, const dm::Vec3 &carVel, float dt)
{
    if (!initialised) { snap(carPos, carRot, carVel); return; }
    dm::Vec3 wp, wl, wu;
    wanted(carPos, carRot, carVel, wp, wl, wu);
    float kp = 1.0f - std::exp(-damping * dt);
    float kr = 1.0f - std::exp(-rotationDamping * dt);
    position = dm::lerp(position, wp, kp);
    target = dm::lerp(target, wl, kr);
    up = dm::normalize(dm::lerp(up, wu, kr));
    // Never let the camera fall too far behind (e.g. after a long frame hitch).
    dm::Vec3 off = position - carPos;
    float maxDist = (distance + height) * 2.5f;
    if (dm::length(off) > maxDist) position = carPos + dm::normalize(off) * maxDist;
}

} // namespace game
