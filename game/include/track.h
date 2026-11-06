// track.h -- seeded, endless mountain-descent track.
//
// Origins: drifto-old's TrackController chained prefab sections (straight /
// downhill straight / 170 deg hairpins strictly alternating left-right) at
// the previous section's end transform. This generator keeps that idea --
// sections attached end to end, generated ahead of the leading car from a
// single deterministic PCG32 stream -- but chooses from a weighted menu of
// FEATURES, each built from exact line / circular-arc sections:
//
//   Straight   14-40 m (sometimes 50-70 m runs), flat / downhill / short
//              uphill / crest / dip elevation profile
//   Sweeper    fast bend, R 40-70 m, 30-60 deg
//   Corner     ~90 deg (70-100), R 16-24 m
//   Hairpin    150-176 deg, R 14-20 m (the original's switchback)
//   Esses      two opposite bends, R 26-40 m, 35-60 deg each
//   Chicane    left-right-left (or mirrored), R 18-24 m
//   Tightening decreasing radius: three arcs, R1 -> 0.65 R1 -> 0.42 R1
//   Opening    increasing radius (the reverse)
//
// Never self-intersecting: (1) the heading relative to the downhill
// direction ("fall line", descentYaw) stays within +-headingBand (88 deg),
// so the road always makes progress down the mountain -- hairpins are only
// offered when they turn a traverse back across the slope; (2) every new
// feature is checked against a spatial hash of all earlier road: no point
// may come within `clearance` (18 m) horizontally of road more than
// `clearanceSkip` (35 m) of arc length back. A failing candidate is
// re-rolled (same RNG stream => still deterministic); after maxAttempts a
// gentle sweeper back towards the fall line (or a short straight) is used.
//
// Everything is analytic: a section's centreline is a line or an arc plus a
// C1 height profile (smoothstep drop + a sin^2 crest/dip bump), so physics
// queries are exact and cheap. Rendering geometry comes from the same
// functions (track_mesh.h).
//
// Determinism: the layout depends only on (seed, TrackParams); sections are
// always generated sequentially, however far ahead a client asks.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "common/dmath.h"
#include "game/include/rng.h"

namespace game {

// Original prefab dimensions are multiplied by this (12 m road -> 9 m).
constexpr float kWorldScale = 0.75f;

enum class SectionType : uint8_t
{
    Start, // the hand-placed startSection of the original scene
    Line,
    Arc,
};

enum class FeatureKind : uint8_t
{
    Start,
    Straight,
    Sweeper,
    Corner,
    Hairpin,
    Esses,
    Chicane,
    Tightening,
    Opening,
    Count
};
const char *featureName(FeatureKind k);

// Roadside look (and runoff width); the barrier itself is always there.
enum class RoadsideStyle : uint8_t
{
    Forest,   // gentle slopes, guardrails, dense trees
    Cliff,    // steep drop-off on the downhill side
    RockWall, // tall cut bank on the uphill side
    Runoff,   // wide gravel runoff (outside of bends / both sides of straights)
    Count
};

enum class SideKind : uint8_t
{
    Open, // (unused: every edge has a barrier -- kept for compatibility)
    Wall, // rising mountainside (cut bank) -- solid
    Rail, // guardrail -- solid
};

struct TrackParams
{
    // Road cross-section. Original road is 1 unit * 12 = 12 m wide.
    float roadHalfWidth = 6.0f * kWorldScale; // 4.5 m
    float shoulderWidth = 1.0f;               // drivable kerb/gravel beyond the lines
    float kerbWidth = 0.6f;                   // painted kerb (bends)
    float runoffWidth = 3.0f;                 // extra drivable gravel (Runoff style)
    float startLength = 48.0f;                // the grid straight

    // Feature weights (relative). 0 disables a kind.
    float weight[static_cast<int>(FeatureKind::Count)] = {0, 16, 22, 14, 14, 12, 6, 9, 7};
    // Straight elevation profiles: flat, downhill, uphill, crest, dip.
    float profileWeight[5] = {20, 45, 15, 10, 10};
    // Roadside styles: forest, cliff, rock wall, runoff.
    float styleWeight[4] = {35, 25, 25, 15};

    float descentYaw = 0.0f;        // fall line (the start straight points down it)
    float headingBand = dm::radians(88.0f);
    float clearance = 18.0f;        // min horizontal distance to older road
    float clearanceSkip = 35.0f;    // ...ignoring the last this-many metres of arc
    int maxAttempts = 12;
    float longStraight = 30.0f;     // no hairpin / tightening / chicane right after a longer straight

    int forwardBufferSections = 14; // generated ahead of the current section
    int rearBufferSections = 2;
};

struct TrackSection
{
    uint32_t index = 0;
    SectionType type = SectionType::Line;
    FeatureKind feature = FeatureKind::Straight;
    RoadsideStyle style = RoadsideStyle::Forest;
    RoadsideStyle prevStyle = RoadsideStyle::Forest; // blended in over the first metres
    int featurePart = 0;      // index of this section within its feature
    dm::Vec3 start;           // centreline start (road surface)
    float startYaw = 0;       // dm convention: 0 faces -Z, + turns left
    float length = 0;         // centreline arc length
    float turnAngle = 0;      // signed total heading change, + = left
    float radius = 0;         // arcs only
    float drop = 0;           // metres lost over the section (< 0 = uphill)
    float bump = 0;           // sin^2 crest (+) / dip (-) height, metres
    float runoff[2] = {0, 0}; // extra drivable width: [0] left, [1] right
    float prevRunoff[2] = {0, 0}; // previous section's, blended in over the first metres
    float descentYaw = 0;     // fall line (copied from the params)
    float startDistance = 0;  // cumulative centreline distance at s = 0
    dm::Vec3 end;
    float endYaw = 0;

    bool isTurn() const { return turnAngle != 0.0f; }
    float curvature() const { return length > 0 ? turnAngle / length : 0; }
    // -1 = left, +1 = right: the side the centre of the bend is on.
    int insideSide() const { return turnAngle > 0 ? -1 : 1; }

    // Centreline at arc length s (linear extrapolation outside [0, length]).
    dm::Vec3 centreAt(float s) const;
    float yawAt(float s) const;
    float heightAt(float s) const;
    float gradeAt(float s) const;                  // dy/ds
    dm::Vec3 forwardAt(float s) const;             // unit tangent incl. slope
    dm::Vec3 rightAt(float s) const;               // horizontal unit right
    dm::Vec3 normalAt(float s) const;              // road surface normal
    dm::Vec3 pointAt(float s, float d) const;      // d = lateral offset, + right

    // Horizontal projection of p onto this section: arc length s (may be
    // outside [0, length]) and signed lateral offset d (+ right).
    void project(const dm::Vec3 &p, float &s, float &d) const;

    // Mountainside shape on one side (-1 left, +1 right) at s:
    // +1 = rising wall, -1 = drop-off, 0 = level. From the fall line and the
    // road heading (continuous across joints), shaped by the style.
    float uphillness(float s, int side) const;
    SideKind sideKind(float s, int side) const;
    // Drivable half-width on a side at s (lines + shoulder + runoff; the
    // runoff ramps in/out over each section's ends so it's continuous).
    float edgeAt(float s, int side, const TrackParams &p) const;
};

// Result of a road query at a world point.
struct RoadQuery
{
    uint32_t section = 0;
    float s = 0;         // arc length within `section`
    float d = 0;         // lateral offset, + right
    float distance = 0;  // total centreline distance from the track start
    float height = 0;    // road surface height (flat cross-section)
    float edge = 0;      // drivable half-width on the point's side
    dm::Vec3 normal{0, 1, 0};
    bool onRoad = false;    // between the edge lines
    bool supported = false; // road, shoulder or runoff under the point
};

class Track
{
public:
    explicit Track(uint32_t seed = 1, const TrackParams &params = TrackParams());

    void reset(uint32_t seed);
    uint32_t seed() const { return seed_; }
    const TrackParams &params() const { return params_; }

    // Sequentially generate sections up to and including `index`.
    void ensureGenerated(uint32_t index);
    // Generate forwardBufferSections ahead of the current section; the
    // "rear buffer" only moves firstLiveSection (sections are never freed --
    // they're ~150 bytes -- so queries anywhere on the field stay exact).
    void update(uint32_t currentSection);
    uint32_t firstLiveSection() const { return firstLive_; }
    uint32_t lastLiveSection() const;

    uint32_t sectionCount() const { return static_cast<uint32_t>(sections_.size()); }
    const TrackSection &section(uint32_t i) const { return sections_[i < sections_.size() ? i : sections_.size() - 1]; }

    // Locate p on the road, starting the search at section `hint` and
    // walking to neighbours while p is past either end.
    // withNormal = false skips the surface normal (q.normal stays {0,1,0}):
    // the wall-corner and position queries in stepCar never read it.
    RoadQuery query(const dm::Vec3 &p, uint32_t hint, bool withNormal = true) const;

    dm::Vec3 pointAt(uint32_t section, float s, float d) const { return this->section(section).pointAt(s, d); }
    // Lines + shoulder (no runoff): the narrowest drivable half-width.
    float edgeOffset() const { return params_.roadHalfWidth + params_.shoulderWidth; }
    float edgeAt(uint32_t section, float s, int side) const { return this->section(section).edgeAt(s, side, params_); }

    // Generator statistics (tests / tuning).
    struct Stats
    {
        uint32_t features = 0, rerolls = 0, fallbacks = 0;
        uint32_t perKind[static_cast<int>(FeatureKind::Count)] = {};
    };
    const Stats &stats() const { return stats_; }

private:
    struct Pose
    {
        dm::Vec3 pos;
        float yaw;
        float distance;
    };
    struct FootPoint
    {
        float x, z, distance;
    };

    void appendFeature();
    bool buildFeature(FeatureKind kind, const Pose &from, std::vector<TrackSection> &out);
    bool fits(const std::vector<TrackSection> &cand) const;
    void commit(std::vector<TrackSection> &cand, FeatureKind kind);
    TrackSection makeLine(const Pose &p, float length, float drop, float bump) const;
    TrackSection makeArc(const Pose &p, float radius, float signedAngle, float drop) const;
    static Pose endPose(const TrackSection &s);
    float phi(float yaw) const { return dm::wrapAngle(yaw - params_.descentYaw); }
    int pickWeighted(const float *w, int n);
    static int64_t cellKey(float x, float z);

    TrackParams params_;
    uint32_t seed_ = 1;
    Pcg32 rng_;
    uint32_t firstLive_ = 0;
    int lastTurnSign_ = 0;          // +1 left, -1 right, 0 none yet
    FeatureKind lastKind_ = FeatureKind::Start;
    float lastStraightLength_ = 0;
    std::vector<TrackSection> sections_;
    std::unordered_map<int64_t, std::vector<FootPoint>> grid_;
    Stats stats_;
};

} // namespace game
