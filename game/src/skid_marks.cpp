// Skid marks and tyre smoke support. Wheel contact points are recomputed from
// the car pose (not stored in the sim), so remote cars get marks too. Each
// wheel owns a polyline of Points (position, half-width side vector,
// connected flag) with a bounded length; buildMesh() turns them into a flat
// quad strip lying on the road. Rebuilding is only needed when dirty_ is set.
// Fed by WorldView::onSimTick once per fixed tick; purely visual, the
// simulation never reads it.
// skid_marks.cpp
#include "game/include/skid_marks.h"

#include <cmath>

namespace game {

// Project each wheel mount down to the road. A wheel counts as in contact
// only when the road is within suspension reach below it. Points are lifted
// 2 cm to avoid z-fighting. Wheel order: front pair then rear pair.
WheelContacts wheelContactsFromPose(const Track &track, uint32_t sectionHint, const dm::Vec3 &position,
                                    const dm::Quat &rotation, const CarParams &params)
{
    WheelContacts c;
    for (int i = 0; i < 4; ++i)
    {
        dm::Vec3 mount = position + dm::rotate(rotation, params.wheelLocal[i]);
        RoadQuery q = track.query(mount, sectionHint);
        // Only when the road is actually just under the wheel.
        if (!q.supported || mount.y - q.height > params.maxDistance + 0.1f || mount.y - q.height < -0.5f) continue;
        c.point[i] = {mount.x, q.height + 0.02f, mount.z};
        c.normal[i] = q.normal;
        c.valid[i] = true;
    }
    return c;
}

// Smoke opacity from the drift angle (deg): quadratic in angle, capped.
float smokeAlpha(float driftAngleDeg, bool drifting, float maxSmokeAlpha)
{
    if (!drifting) return 0.0f;
    // Original: a = angle / 90 * maxSmokeAlpha (0.2); alpha = min(a*a, max) --
    // nearly invisible at our scale, so a is boosted 1.6 / 0.2 = 8x.
    float a = std::fabs(driftAngleDeg) / 90.0f * 1.6f;
    float alpha = a * a;
    return alpha < maxSmokeAlpha ? alpha : maxSmokeAlpha;
}

// Append points for wheels that are skidding this frame. Rear wheels mark
// whenever the car drifts; front wheels only past the visual steering angle.
// A point is added only after the wheel moved minSpacing; `connected`
// links it to the previous one so the strip breaks where contact was lost.
void SkidMarks::update(const Track &track, uint32_t sectionHint, const dm::Vec3 &position, const dm::Quat &rotation,
                       bool drifting, float driftAngleDeg, const CarParams &params)
{
    WheelContacts wc = wheelContactsFromPose(track, sectionHint, position, rotation, params);
    bool frontDrift = drifting && std::fabs(driftAngleDeg) > params.maxVisualSteeringAngle;
    dm::Vec3 right = dm::rightOf(rotation);
    for (int i = 0; i < 4; ++i)
    {
        bool emit = wc.valid[i] && (i >= WheelRL ? drifting : frontDrift);
        if (!emit) { emitting_[i] = false; continue; }
        std::deque<Point> &t = trails_[i];
        // side is the right vector projected into the road plane, scaled to half width.
        dm::Vec3 side = dm::normalize(right - wc.normal[i] * dm::dot(right, wc.normal[i])) * (width * 0.5f);
        if (emitting_[i] && !t.empty() && dm::length(wc.point[i] - t.back().pos) < minSpacing) continue;
        t.push_back({wc.point[i], side, emitting_[i] && !t.empty()});
        if (t.size() > maxPoints_) t.pop_front();
        emitting_[i] = true;
        dirty_ = true;
    }
}

// Drop every trail.
void SkidMarks::clear()
{
    for (auto &t : trails_) t.clear();
    for (bool &e : emitting_) e = false;
    dirty_ = true;
}

// Total stored points across all wheels.
size_t SkidMarks::pointCount() const
{
    size_t n = 0;
    for (const auto &t : trails_) n += t.size();
    return n;
}

// Rebuild the vertex data: a quad between consecutive connected points of
// each trail, wound counter-clockwise seen from above.
void SkidMarks::buildMesh(gfx::MeshData &m)
{
    m = gfx::MeshData{};
    const dm::Vec3 color{0.05f, 0.05f, 0.05f};
    for (const auto &t : trails_)
        for (size_t i = 1; i < t.size(); ++i)
        {
            if (!t[i].connected) continue;
            const Point &a = t[i - 1], &b = t[i];
            uint32_t base = static_cast<uint32_t>(m.positions.size());
            m.positions.insert(m.positions.end(), {a.pos - a.side, a.pos + a.side, b.pos - b.side, b.pos + b.side});
            dm::Vec3 n{0, 1, 0};
            m.normals.insert(m.normals.end(), {n, n, n, n});
            m.colors.insert(m.colors.end(), {color, color, color, color});
            // CCW seen from above regardless of travel direction.
            dm::Vec3 fn = dm::cross(a.side * 2.0f, b.pos - a.pos - a.side);
            if (fn.y >= 0)
                m.indices.insert(m.indices.end(), {base, base + 1, base + 3, base, base + 3, base + 2});
            else
                m.indices.insert(m.indices.end(), {base, base + 3, base + 1, base, base + 2, base + 3});
        }
    dirty_ = false;
}

} // namespace game
