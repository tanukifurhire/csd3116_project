// chase_camera.h -- port of Car_PlayerCamera (damped follow + look-at).
//
// Original (FixedUpdate, prefab values distance 10, height 2, damping 5,
// rotationDamping 10 for a ~9.3 m car):
//   wanted = car.TransformPoint(0, height, -distance);
//   cam.pos = Lerp(cam.pos, wanted, dt * damping);
//   cam.rot = Slerp(cam.rot, LookRotation(car - cam, car.up), dt * rotationDamping);
// Here the lerps use 1 - exp(-rate * dt) so the camera behaves the same at
// any render frame rate, distances are scaled to our real-size car, and the
// follow direction leans towards the velocity so drifts stay in view.
#pragma once

#include "common/dmath.h"

namespace game {

struct ChaseCamera
{
    float distance = 6.8f;       // behind the car (+Z local is "behind" in our convention)
    float height = 2.3f;
    float damping = 5.0f;
    float rotationDamping = 10.0f;
    float lookHeight = 0.9f;     // look slightly above the car's centre
    float lookAhead = 2.0f;      // ... and a little ahead of it
    float velocityBlend = 0.35f; // 0 = pure heading (original), 1 = pure velocity direction

    dm::Vec3 position{0, 3, 8};
    dm::Vec3 target{0, 0, 0};
    dm::Vec3 up{0, 1, 0};
    bool initialised = false;

    // Jump straight to the wanted pose (spawn / respawn).
    void snap(const dm::Vec3 &carPos, const dm::Quat &carRot, const dm::Vec3 &carVel);
    void update(const dm::Vec3 &carPos, const dm::Quat &carRot, const dm::Vec3 &carVel, float dt);

private:
    void wanted(const dm::Vec3 &carPos, const dm::Quat &carRot, const dm::Vec3 &carVel, dm::Vec3 &pos,
                dm::Vec3 &look, dm::Vec3 &upDir) const;
};

} // namespace game
