// Implementation of the procedural track (see track.h for the feature menu).
// Representation: a track is a list of analytic sections, each a straight line
// or a circular arc in the XZ plane plus a height profile, joined end to end.
// A position on the road is (section, s, d): s = arc length along the section
// centreline (m), d = signed lateral offset (m, + = right of travel).
// Headings are yaw angles (rad); forward at yaw is (-sin, 0, -cos).
// Generation: features are appended lazily from one seeded RNG stream, so all
// peers building the same (seed, params) get an identical track regardless of
// how far ahead each asked. Queries are const and cheap (no allocation).
// track.cpp -- see track.h.
#include "game/include/track.h"

#include <algorithm>
#include <cmath>

namespace game {

namespace {

// Horizontal unit vectors for a yaw: forward = (-sin, 0, -cos), right = (cos, 0, -sin).
dm::Vec3 horizontalForward(float yaw) { return {-std::sin(yaw), 0.0f, -std::cos(yaw)}; }
dm::Vec3 horizontalRight(float yaw) { return {std::cos(yaw), 0.0f, -std::sin(yaw)}; }

// Rotate (x, z) about +Y by theta (right-handed: + is counter-clockwise from above).
dm::Vec3 rotateY(dm::Vec3 v, float theta)
{
    float c = std::cos(theta), s = std::sin(theta);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

// Height profile: smoothstep drop (every section starts and ends level, so
// the road is C1 in height across joints) plus a sin^2 bump for crests/dips.
float smooth01(float t) { return t * t * (3.0f - 2.0f * t); }

const float kPiF = 3.14159265358979323846f;

// Fraction 0..1 that eases in over the first `len` metres of a section
// (capped to half its length); used to blend roadside style and runoff
// width with the previous section so they have no visible step at joints.
// 0 at the start of a section, 1 once `len` metres in (smooth).
float startRamp(float s, float length, float len)
{
    float l = std::fmin(len, length * 0.5f);
    if (l <= 0) return 1.0f;
    return smooth01(dm::clampf(s / l, 0.0f, 1.0f));
}

// Remap raw uphillness u0 (-1..1, + = rising side) to the look of a style.
float styledUphill(RoadsideStyle st, float u0)
{
    switch (st)
    {
    case RoadsideStyle::Forest: return u0 * 0.35f;
    case RoadsideStyle::Cliff: return u0 < 0 ? std::fmax(-1.0f, u0 * 1.6f - 0.2f) : u0 * 0.8f;
    case RoadsideStyle::RockWall: return u0 > 0 ? std::fmin(1.0f, u0 * 1.5f + 0.25f) : u0;
    case RoadsideStyle::Runoff: return u0 * 0.3f;
    case RoadsideStyle::Count: break;
    }
    return u0;
}

} // namespace

// Human-readable feature name for logs and the debug HUD.
const char *featureName(FeatureKind k)
{
    switch (k)
    {
    case FeatureKind::Start: return "start";
    case FeatureKind::Straight: return "straight";
    case FeatureKind::Sweeper: return "sweeper";
    case FeatureKind::Corner: return "corner";
    case FeatureKind::Hairpin: return "hairpin";
    case FeatureKind::Esses: return "esses";
    case FeatureKind::Chicane: return "chicane";
    case FeatureKind::Tightening: return "tightening";
    case FeatureKind::Opening: return "opening";
    case FeatureKind::Count: break;
    }
    return "?";
}

// --------------------------------------------------------------------------
// TrackSection
// --------------------------------------------------------------------------

// Heading (rad) at arc length s: constant on lines, linear in s on arcs
// (curvature is constant). s is clamped into the section.
float TrackSection::yawAt(float s) const
{
    float sc = dm::clampf(s, 0.0f, length);
    return startYaw + curvature() * sc;
}

// Centreline height at s: linear fall of `drop`, eased with smoothstep so
// the slope is zero at both ends, plus a sin^2 crest/dip of amplitude `bump`.
float TrackSection::heightAt(float s) const
{
    if (length <= 0) return start.y;
    float t = dm::clampf(s / length, 0.0f, 1.0f);
    float b = std::sin(kPiF * t);
    return start.y - drop * smooth01(t) + bump * b * b;
}

// Analytic derivative d(height)/ds of heightAt(); zero outside (0, length).
float TrackSection::gradeAt(float s) const
{
    if (length <= 0 || s <= 0 || s >= length) return 0.0f;
    float t = s / length;
    return (-drop * 6.0f * t * (1.0f - t) + bump * kPiF * std::sin(2.0f * kPiF * t)) / length;
}

// Centreline point at s. Outside [0, length] the line is extended along the
// end tangent so queries slightly past a joint stay well defined. Arcs are
// the start point rotated about the turn centre by k*s.
dm::Vec3 TrackSection::centreAt(float s) const
{
    dm::Vec3 p;
    if (s <= 0)
        p = start + horizontalForward(startYaw) * s;
    else if (s >= length)
        p = end + horizontalForward(endYaw) * (s - length);
    else if (!isTurn())
        p = start + horizontalForward(startYaw) * s;
    else
    {
        float k = curvature();
        dm::Vec3 centre = start - horizontalRight(startYaw) * (1.0f / k);
        p = centre + rotateY(start - centre, k * s);
    }
    p.y = heightAt(s);
    return p;
}

// Frame at s: forward includes the grade, right is horizontal, normal is
// right x forward (the road surface normal).
dm::Vec3 TrackSection::forwardAt(float s) const
{
    return dm::normalize(horizontalForward(yawAt(s)) + dm::Vec3{0, gradeAt(s), 0});
}

dm::Vec3 TrackSection::rightAt(float s) const { return horizontalRight(yawAt(s)); }

// Same operations, in the same order, as normalize(cross(rightAt(s), forwardAt(s))),
// but the yaw (and its sin/cos) is evaluated once instead of twice: perf showed
// normalAt + its two yawAt/sincos chains at ~33% of a bot race.
dm::Vec3 TrackSection::normalAt(float s) const
{
    const float yaw = yawAt(s);
    const dm::Vec3 fwd = dm::normalize(horizontalForward(yaw) + dm::Vec3{0, gradeAt(s), 0});
    return dm::normalize(dm::cross(horizontalRight(yaw), fwd));
}

dm::Vec3 TrackSection::pointAt(float s, float d) const { return centreAt(s) + rightAt(s) * d; }

// Inverse of pointAt() in the horizontal plane: arc length s and lateral
// offset d of world point p relative to this section. s may fall outside
// [0, length] so the caller can move to a neighbouring section.
void TrackSection::project(const dm::Vec3 &p, float &s, float &d) const
{
    if (!isTurn())
    {
        dm::Vec3 v{p.x - start.x, 0, p.z - start.z};
        s = dm::dot(v, horizontalForward(startYaw));
        d = dm::dot(v, horizontalRight(startYaw));
        return;
    }
    float k = curvature();
    float r = std::fabs(1.0f / k);
    dm::Vec3 centre = start - horizontalRight(startYaw) * (1.0f / k);
    dm::Vec3 a{start.x - centre.x, 0, start.z - centre.z};
    dm::Vec3 v{p.x - centre.x, 0, p.z - centre.z};
    float crossY = a.z * v.x - a.x * v.z;
    float phi = std::atan2(crossY, a.x * v.x + a.z * v.z);
    float dist = std::sqrt(v.x * v.x + v.z * v.z);
    // Progress angle along the turn. atan2 wraps at +-180 deg, but a 170 deg
    // hairpin's exit passes 180 deg within a few metres, so centre the wrap
    // window on the middle of the arc instead.
    float theta = k > 0 ? phi : -phi;
    float mid = 0.5f * std::fabs(turnAngle);
    if (theta < mid - kPiF) theta += 2.0f * kPiF;
    else if (theta > mid + kPiF) theta -= 2.0f * kPiF;
    s = theta * r;
    d = k > 0 ? dist - r   // left turn: centre on the left
              : r - dist;  // right turn: centre on the right
}

// How steep the terrain rises beside the road on `side` (+1 right, -1
// left), -1..1, blended from the previous section's style over 10 m.
float TrackSection::uphillness(float s, int side) const
{
    // Up-slope = against the fall line. A road traversing the slope has a
    // rising side and a falling side; one pointing down the fall line has
    // level sides. Continuous across joints because the heading is.
    float sc = dm::clampf(s, 0.0f, length);
    dm::Vec3 up = -horizontalForward(descentYaw);
    float u0 = dm::clampf(1.4f * static_cast<float>(side) * dm::dot(rightAt(sc), up), -1.0f, 1.0f);
    float r = startRamp(sc, length, 10.0f);
    return dm::lerpf(styledUphill(prevStyle, u0), styledUphill(style, u0), r);
}

SideKind TrackSection::sideKind(float s, int side) const
{
    // Mountainside on the uphill side, guardrail everywhere else (every edge
    // is a barrier: drawn drop-off terrain isn't solid).
    return uphillness(s, side) > 0.15f ? SideKind::Wall : SideKind::Rail;
}

// Distance from the centreline to the barrier on `side`: road + shoulder +
// the (blended) runoff width.
float TrackSection::edgeAt(float s, int side, const TrackParams &p) const
{
    int i = side > 0 ? 1 : 0;
    float sc = dm::clampf(s, 0.0f, length);
    float r = startRamp(sc, length, 8.0f);
    return p.roadHalfWidth + p.shoulderWidth + dm::lerpf(prevRunoff[i], runoff[i], r);
}

// --------------------------------------------------------------------------
// Track: construction
// --------------------------------------------------------------------------

Track::Track(uint32_t seed, const TrackParams &params) : params_(params) { reset(seed); }

// Hash cell index for the self-intersection grid: 20 m square cells packed
// into one 64-bit key.
int64_t Track::cellKey(float x, float z)
{
    // Unsigned arithmetic: shifting negative values is UB before C++20.
    uint64_t cx = static_cast<uint64_t>(static_cast<int64_t>(std::floor(x / 20.0f)));
    uint64_t cz = static_cast<uint64_t>(static_cast<int64_t>(std::floor(z / 20.0f)));
    return static_cast<int64_t>((cx << 32) ^ (cz & 0xffffffffull));
}

// Restart generation from a seed: clear all state, place the start straight
// and generate the forward buffer.
void Track::reset(uint32_t seed)
{
    seed_ = seed;
    rng_.seed(splitmix64(seed), 0x7261636bull /* "rack" */);
    firstLive_ = 0;
    lastTurnSign_ = 0;
    lastKind_ = FeatureKind::Start;
    lastStraightLength_ = 0; // the start straight is added by its commit()
    sections_.clear();
    grid_.clear();
    stats_ = Stats{};

    Pose origin{{0, 0, 0}, params_.descentYaw, 0};
    std::vector<TrackSection> start{makeLine(origin, params_.startLength, 0, 0)};
    start[0].type = SectionType::Start;
    commit(start, FeatureKind::Start);
    sections_[0].style = sections_[0].prevStyle = RoadsideStyle::Forest;
    ensureGenerated(static_cast<uint32_t>(params_.forwardBufferSections));
}

// Make sure sections [0, index] exist.
void Track::ensureGenerated(uint32_t index)
{
    while (sections_.size() <= index) appendFeature();
}

// Called as the lead car progresses: extend ahead by forwardBufferSections
// and move the live window start rearBufferSections behind.
void Track::update(uint32_t currentSection)
{
    ensureGenerated(currentSection + static_cast<uint32_t>(params_.forwardBufferSections));
    uint32_t rear = static_cast<uint32_t>(params_.rearBufferSections);
    firstLive_ = currentSection > rear ? currentSection - rear : 0;
}

uint32_t Track::lastLiveSection() const { return static_cast<uint32_t>(sections_.size() - 1); }

Track::Pose Track::endPose(const TrackSection &s) { return {s.end, s.endYaw, s.startDistance + s.length}; }

// Build a straight section of `length` m from pose p with a height drop (m)
// and optional crest/dip bump (m).
TrackSection Track::makeLine(const Pose &p, float length, float drop, float bump) const
{
    TrackSection s;
    s.type = SectionType::Line;
    s.start = p.pos;
    s.startYaw = p.yaw;
    s.startDistance = p.distance;
    s.length = length;
    s.drop = drop;
    s.bump = bump;
    s.descentYaw = params_.descentYaw;
    s.endYaw = p.yaw;
    s.end = p.pos + horizontalForward(p.yaw) * length;
    s.end.y = p.pos.y - drop;
    return s;
}

// Build an arc section: positive signedAngle turns left (CCW), radius in m;
// length = radius * |angle|. The end is the start rotated about the centre.
TrackSection Track::makeArc(const Pose &p, float radius, float signedAngle, float drop) const
{
    TrackSection s;
    s.type = SectionType::Arc;
    s.start = p.pos;
    s.startYaw = p.yaw;
    s.startDistance = p.distance;
    s.radius = radius;
    s.turnAngle = signedAngle;
    s.length = radius * std::fabs(signedAngle);
    s.drop = drop;
    s.descentYaw = params_.descentYaw;
    s.endYaw = p.yaw + signedAngle;
    float k = s.curvature();
    dm::Vec3 centre = p.pos - horizontalRight(p.yaw) * (1.0f / k);
    s.end = centre + rotateY(p.pos - centre, signedAngle);
    s.end.y = p.pos.y - drop;
    return s;
}

// Weighted random index in [0, n); -1 if all weights are zero.
int Track::pickWeighted(const float *w, int n)
{
    float total = 0;
    for (int i = 0; i < n; ++i) total += w[i];
    if (total <= 0) return -1;
    float r = rng_.nextFloat() * total;
    for (int i = 0; i < n; ++i)
    {
        if (r < w[i]) return i;
        r -= w[i];
    }
    for (int i = n - 1; i >= 0; --i)
        if (w[i] > 0) return i;
    return -1;
}

// --------------------------------------------------------------------------
// Track: features
// --------------------------------------------------------------------------

// Try to lay out one feature of `kind` starting at `from`; sections go into
// `out`. Returns false when the feature does not apply (e.g. no legal turn
// direction). The caller still has to check fits(). Parameter ranges are
// artistic tuning values.
bool Track::buildFeature(FeatureKind kind, const Pose &from, std::vector<TrackSection> &out)
{
    const float band = params_.headingBand;
    const float phi0 = phi(from.yaw);
    Pose p = from;
    auto add = [&](const TrackSection &s) {
        out.push_back(s);
        p = endPose(s);
    };
    auto urand = [this](float lo, float hi) { return rng_.range(lo, hi); };
    // Pick a turn direction (+1 left, -1 right) for a turn of `angle` that
    // keeps the heading inside the band: when both fit, steer back towards
    // the fall line on steep traverses, else mostly alternate.
    auto chooseDir = [&](float angle) {
        bool left = std::fabs(phi0 + angle) <= band, right = std::fabs(phi0 - angle) <= band;
        float pref = rng_.nextFloat();
        if (left && right)
        {
            if (std::fabs(phi0) > dm::radians(50.0f)) return pref < 0.6f ? (phi0 > 0 ? -1 : 1) : (phi0 > 0 ? 1 : -1);
            if (lastTurnSign_ != 0) return pref < 0.6f ? -lastTurnSign_ : lastTurnSign_;
            return pref < 0.5f ? 1 : -1;
        }
        return left ? 1 : (right ? -1 : 0);
    };

    switch (kind)
    {
    case FeatureKind::Straight:
    {
        float len = rng_.nextFloat() < 0.15f ? urand(50.0f, 70.0f) : urand(14.0f, 40.0f);
        int prof = pickWeighted(params_.profileWeight, 5);
        float drop = 0, bump = 0;
        float a = urand(0.0f, 1.0f), b = urand(0.0f, 1.0f); // always drawn: keeps the stream stable
        if (prof == 2 && len > 30.0f) prof = 1;           // climbs are short
        if ((prof == 3 || prof == 4) && len < 22.0f) prof = 1;
        // Bumps gentle enough not to launch the car at top speed: the
        // vertical curvature 2 pi^2 h / L^2 stays under ~0.8 g / v^2.
        float maxBump = 0.0009f * len * len;
        switch (prof)
        {
        case 0: break;                                                       // flat
        case 1: drop = len * (0.03f + 0.05f * a); break;                     // downhill
        case 2: drop = -len * (0.02f + 0.03f * a); break;                    // short climb
        case 3: drop = len * 0.03f * a; bump = std::fmin(0.4f + 0.5f * b, maxBump); break;   // crest
        case 4: drop = len * 0.03f * a; bump = -std::fmin(0.4f + 0.5f * b, maxBump); break;  // dip
        default: break;
        }
        add(makeLine(p, len, drop, bump));
        return true;
    }
    case FeatureKind::Sweeper:
    {
        float angle = dm::radians(urand(30.0f, 60.0f)), r = urand(40.0f, 70.0f), g = urand(0.03f, 0.06f);
        int dir = chooseDir(angle);
        if (!dir) return false;
        add(makeArc(p, r, dir * angle, r * angle * g));
        return true;
    }
    case FeatureKind::Corner:
    {
        float angle = dm::radians(urand(70.0f, 100.0f)), r = urand(16.0f, 24.0f), g = urand(0.04f, 0.08f);
        int dir = chooseDir(angle);
        if (!dir) return false;
        add(makeArc(p, r, dir * angle, r * angle * g));
        return true;
    }
    case FeatureKind::Hairpin:
    {
        // Turn a traverse back across the slope: exit at 65-86 deg the other way.
        float exitDeg = urand(65.0f, 86.0f), r = urand(14.0f, 20.0f), g = urand(0.06f, 0.09f), link = urand(8.0f, 14.0f);
        if (std::fabs(phi0) < dm::radians(60.0f)) return false;
        float angle = dm::clampf(std::fabs(phi0) + dm::radians(exitDeg), dm::radians(150.0f), dm::radians(176.0f));
        int dir = phi0 > 0 ? -1 : 1;
        // Back-to-back hairpins get a short link straight to rotate the car.
        if (lastKind_ == FeatureKind::Hairpin) add(makeLine(p, link, link * 0.03f, 0));
        add(makeArc(p, r, dir * angle, r * angle * g));
        return true;
    }
    case FeatureKind::Esses:
    {
        float a1 = dm::radians(urand(35.0f, 60.0f)), r1 = urand(26.0f, 40.0f);
        float a2 = a1 * urand(0.8f, 1.2f), r2 = r1 * urand(0.85f, 1.15f), link = urand(0.0f, 6.0f), g = urand(0.03f, 0.06f);
        int dir = chooseDir(a1);
        if (!dir) return false;
        add(makeArc(p, r1, dir * a1, r1 * a1 * g));
        if (link > 1.0f) add(makeLine(p, link, link * g, 0));
        add(makeArc(p, r2, -dir * a2, r2 * a2 * g));
        return true;
    }
    case FeatureKind::Chicane:
    {
        float a = dm::radians(urand(25.0f, 40.0f)), r = urand(18.0f, 24.0f), link = urand(2.0f, 5.0f), g = urand(0.02f, 0.05f);
        int dir = chooseDir(a);
        if (!dir || std::fabs(phi0 - dir * a) > band) return false; // must also swing the other way
        add(makeArc(p, r, dir * a, r * a * g));
        add(makeLine(p, link, link * g, 0));
        add(makeArc(p, r, -2.0f * dir * a, 2.0f * r * a * g));
        add(makeLine(p, link, link * g, 0));
        add(makeArc(p, r, dir * a, r * a * g));
        return true;
    }
    case FeatureKind::Tightening:
    case FeatureKind::Opening:
    {
        float total = dm::radians(urand(70.0f, 130.0f)), r1 = urand(45.0f, 60.0f), g = urand(0.03f, 0.07f);
        float radii[3] = {r1, std::fmax(15.0f, r1 * 0.65f), std::fmax(15.0f, r1 * 0.42f)};
        float split[3] = {0.3f, 0.35f, 0.35f};
        int dir = chooseDir(total);
        if (!dir) return false;
        for (int i = 0; i < 3; ++i)
        {
            int j = kind == FeatureKind::Tightening ? i : 2 - i;
            float ang = total * split[j];
            add(makeArc(p, radii[j], dir * ang, radii[j] * ang * g));
        }
        return true;
    }
    case FeatureKind::Start:
    case FeatureKind::Count:
        break;
    }
    return false;
}

// Reject candidates that leave the heading band or pass within `clearance`
// of earlier road further back than `clearanceSkip` (sampled every ~3 m,
// looked up in the 3x3 neighbourhood of grid cells).
bool Track::fits(const std::vector<TrackSection> &cand) const
{
    const float band = params_.headingBand + 1e-3f;
    const float c2 = params_.clearance * params_.clearance;
    for (const TrackSection &s : cand)
    {
        if (std::fabs(phi(s.endYaw)) > band) return false;
        int n = static_cast<int>(std::ceil(s.length / 3.0f));
        for (int i = 0; i <= n; ++i)
        {
            float sa = s.length * static_cast<float>(i) / static_cast<float>(n);
            dm::Vec3 c = s.centreAt(sa);
            float dist = s.startDistance + sa;
            for (int ox = -1; ox <= 1; ++ox)
                for (int oz = -1; oz <= 1; ++oz)
                {
                    auto it = grid_.find(cellKey(c.x + ox * 20.0f, c.z + oz * 20.0f));
                    if (it == grid_.end()) continue;
                    for (const FootPoint &f : it->second)
                    {
                        if (dist - f.distance < params_.clearanceSkip) continue;
                        float dx = c.x - f.x, dz = c.z - f.z;
                        if (dx * dx + dz * dz < c2) return false;
                    }
                }
        }
    }
    return true;
}

// Accept a candidate: assign indices/styles/runoff, register its footprint in
// the spatial grid and update the generator's pacing state.
void Track::commit(std::vector<TrackSection> &cand, FeatureKind kind)
{
    // Roadside style for the whole feature (+ runoff: outside of bends, or
    // both sides of a straight).
    RoadsideStyle style = RoadsideStyle::Forest;
    if (kind != FeatureKind::Start)
        style = static_cast<RoadsideStyle>(std::max(0, pickWeighted(params_.styleWeight, 4)));
    for (size_t i = 0; i < cand.size(); ++i)
    {
        TrackSection &s = cand[i];
        s.index = static_cast<uint32_t>(sections_.size());
        s.feature = kind;
        s.featurePart = static_cast<int>(i);
        s.style = style;
        if (style == RoadsideStyle::Runoff)
        {
            if (s.isTurn()) s.runoff[s.turnAngle > 0 ? 1 : 0] = params_.runoffWidth; // outside
            else s.runoff[0] = s.runoff[1] = params_.runoffWidth * 0.7f;
        }
        if (!sections_.empty())
        {
            const TrackSection &prev = sections_.back();
            s.prevStyle = prev.style;
            s.prevRunoff[0] = prev.runoff[0];
            s.prevRunoff[1] = prev.runoff[1];
        }
        else
            s.prevStyle = style;
        // Footprint for the self-intersection check.
        int n = static_cast<int>(std::ceil(s.length / 3.0f));
        for (int k = 0; k <= n; ++k)
        {
            float sa = s.length * static_cast<float>(k) / static_cast<float>(n);
            dm::Vec3 c = s.centreAt(sa);
            grid_[cellKey(c.x, c.z)].push_back({c.x, c.z, s.startDistance + sa});
        }
        if (s.isTurn()) lastTurnSign_ = s.turnAngle > 0 ? 1 : -1;
        sections_.push_back(s);
    }
    lastKind_ = kind;
    if (kind == FeatureKind::Straight || kind == FeatureKind::Start) lastStraightLength_ += cand.back().length;
    else lastStraightLength_ = 0;
    ++stats_.features;
    ++stats_.perKind[static_cast<int>(kind)];
}

// Pick, build and commit the next feature: reweight for pacing, retry up to
// maxAttempts on failure, else fall back to a gentle bend / straight.
void Track::appendFeature()
{
    const Pose from = endPose(sections_.back());
    const float phi0 = phi(from.yaw);
    const int nk = static_cast<int>(FeatureKind::Count);
    float w[static_cast<int>(FeatureKind::Count)];
    for (int i = 0; i < nk; ++i) w[i] = params_.weight[i];
    w[static_cast<int>(FeatureKind::Start)] = 0;
    // Pacing: nothing tight straight off a long (fast) straight; hairpins
    // only turn a steep traverse back across the slope.
    bool fast = lastStraightLength_ > params_.longStraight;
    if (fast || std::fabs(phi0) < dm::radians(60.0f)) w[static_cast<int>(FeatureKind::Hairpin)] = 0;
    else w[static_cast<int>(FeatureKind::Hairpin)] *= 2.5f; // a steep traverse: switchback time
    if (fast)
    {
        w[static_cast<int>(FeatureKind::Tightening)] = 0;
        w[static_cast<int>(FeatureKind::Chicane)] = 0;
    }

    std::vector<TrackSection> cand;
    for (int attempt = 0; attempt < params_.maxAttempts; ++attempt)
    {
        int k = pickWeighted(w, nk);
        if (k < 0) break;
        cand.clear();
        if (buildFeature(static_cast<FeatureKind>(k), from, cand) && fits(cand))
        {
            commit(cand, static_cast<FeatureKind>(k));
            return;
        }
        ++stats_.rerolls;
    }
    // Fallback: a gentle bend back towards the fall line, else a straight.
    ++stats_.fallbacks;
    cand.clear();
    if (std::fabs(phi0) > dm::radians(5.0f))
    {
        float angle = std::fmin(std::fabs(phi0), dm::radians(40.0f));
        cand.push_back(makeArc(from, 60.0f, phi0 > 0 ? -angle : angle, 60.0f * angle * 0.04f));
        commit(cand, FeatureKind::Sweeper);
    }
    else
    {
        cand.push_back(makeLine(from, 20.0f, 0.8f, 0));
        commit(cand, FeatureKind::Straight);
    }
}

// --------------------------------------------------------------------------
// queries
// --------------------------------------------------------------------------

// Locate world point p on the track. Starts at the hinted section (usually
// the car's last one) and walks at most 8 sections backwards or forwards
// until s lies inside; lastMove stops it oscillating at a joint.
RoadQuery Track::query(const dm::Vec3 &p, uint32_t hint, bool withNormal) const
{
    uint32_t i = hint < sections_.size() ? hint : static_cast<uint32_t>(sections_.size() - 1);
    float s = 0, d = 0;
    int lastMove = 0;
    for (int iter = 0; iter < 8; ++iter)
    {
        sections_[i].project(p, s, d);
        if (s < 0 && i > 0 && lastMove != +1) { --i; lastMove = -1; continue; }
        if (s > sections_[i].length && i + 1 < sections_.size() && lastMove != -1) { ++i; lastMove = +1; continue; }
        break;
    }
    const TrackSection &sec = sections_[i];
    RoadQuery q;
    q.section = i;
    q.s = s;
    q.d = d;
    q.distance = sec.startDistance + s;
    q.height = sec.heightAt(s);
    if (withNormal) q.normal = sec.normalAt(s); // only the wheel rays need it
    q.edge = sec.edgeAt(s, d > 0 ? 1 : -1, params_);
    q.onRoad = std::fabs(d) <= params_.roadHalfWidth;
    q.supported = std::fabs(d) <= q.edge;
    return q;
}

} // namespace game
