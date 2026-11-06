// skid_marks.h -- tyre-mark trails and tyre-smoke emission points
// (port of Car_SkidMarks: rear trails emit while IsDrifting(), front trails
// while IsFrontWheelDrift(), smoke alpha grows with drift angle).
//
// Works from a car *pose* (+ drifting flag / drift angle), so the same code
// serves the locally simulated car and interpolated remote cars.
#pragma once

#include <cstddef>
#include <deque>

#include "common/dmath.h"
#include "game/include/car_physics.h"
#include "game/include/track.h"
#include "gfx/include/renderer.h"

namespace game {

struct WheelContacts
{
    dm::Vec3 point[4];  // on the road surface under each wheel
    dm::Vec3 normal[4];
    bool valid[4] = {false, false, false, false};
};

// Contact points under the wheels for a given pose (road surface projection).
WheelContacts wheelContactsFromPose(const Track &track, uint32_t sectionHint, const dm::Vec3 &position,
                                    const dm::Quat &rotation, const CarParams &params = defaultCarParams());

// Smoke opacity for a drift angle (degrees), 0 when not drifting.
// Original: a = angle / 90 * maxSmokeAlpha; alpha = min(a * a, maxSmokeAlpha).
float smokeAlpha(float driftAngleDeg, bool drifting, float maxSmokeAlpha = 0.55f);

class SkidMarks
{
public:
    explicit SkidMarks(size_t maxPointsPerWheel = 500) : maxPoints_(maxPointsPerWheel) {}

    float width = 0.22f * kCarScale; // original 0.2 on a 2x-scale car => tyre width
    float minSpacing = 0.35f;   // metres between trail points

    // Call once per simulation tick per car.
    void update(const Track &track, uint32_t sectionHint, const dm::Vec3 &position, const dm::Quat &rotation,
                bool drifting, float driftAngleDeg, const CarParams &params = defaultCarParams());
    void clear();

    bool dirty() const { return dirty_; }
    // Builds dark translucent quads (draw with alpha ~0.6, no outline).
    void buildMesh(gfx::MeshData &out);
    size_t pointCount() const;

private:
    struct Point
    {
        dm::Vec3 pos, side; // side = half-width offset vector
        bool connected;     // joins the previous point
    };
    std::deque<Point> trails_[4];
    bool emitting_[4] = {false, false, false, false};
    size_t maxPoints_;
    bool dirty_ = false;
};

} // namespace game
