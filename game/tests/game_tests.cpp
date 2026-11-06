// game_tests.cpp -- headless tests for game/ (no GL, no DDS, no window).
// Build: make game_tests      Run: ./game_tests [filter]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

#include "game/bench/old_bot.h"
#include "game/include/autopilot.h"
#include "game/include/car_physics.h"
#include "game/include/chase_camera.h"
#include "game/include/gearbox.h"
#include "game/include/multiplayer_session.h"
#include "game/include/race.h"
#include "game/include/rng.h"
#include "game/include/skid_marks.h"
#include "game/include/track.h"
#include "game/include/track_mesh.h"
#include "game/tests/fake_transport.h"

using namespace game;

// ---------------------------------------------------------------------------
// tiny test harness
// ---------------------------------------------------------------------------
namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_current;

#define CHECK(cond)                                                                             \
    do {                                                                                        \
        ++g_checks;                                                                             \
        if (!(cond)) {                                                                          \
            ++g_failures;                                                                       \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
        }                                                                                       \
    } while (0)
#define CHECK_MSG(cond, ...)                                                                    \
    do {                                                                                        \
        ++g_checks;                                                                             \
        if (!(cond)) {                                                                          \
            ++g_failures;                                                                       \
            std::printf("    FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);                   \
            std::printf(__VA_ARGS__);                                                           \
            std::printf("\n");                                                                  \
        }                                                                                       \
    } while (0)

struct TestCase { const char *name; std::function<void()> fn; };
std::vector<TestCase> &registry() { static std::vector<TestCase> r; return r; }
struct Registrar { Registrar(const char *n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
#define TEST(name) static void name(); static Registrar reg_##name(#name, name); static void name()

bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }
bool sameBits(const dm::Vec3 &a, const dm::Vec3 &b) { return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.z, b.z); }

bool sectionsIdentical(const TrackSection &a, const TrackSection &b)
{
    return a.index == b.index && a.type == b.type && a.feature == b.feature && a.style == b.style &&
           a.featurePart == b.featurePart && sameBits(a.start, b.start) && sameBits(a.startYaw, b.startYaw) &&
           sameBits(a.length, b.length) && sameBits(a.turnAngle, b.turnAngle) && sameBits(a.radius, b.radius) &&
           sameBits(a.drop, b.drop) && sameBits(a.bump, b.bump) && sameBits(a.runoff[0], b.runoff[0]) &&
           sameBits(a.runoff[1], b.runoff[1]) && sameBits(a.startDistance, b.startDistance) && sameBits(a.end, b.end) &&
           sameBits(a.endYaw, b.endYaw);
}

// A flat, plain, endless straight down the fall line (physics tests).
TrackParams allStraights()
{
    TrackParams p;
    for (float &w : p.weight) w = 0;
    p.weight[static_cast<int>(FeatureKind::Straight)] = 1;
    for (float &w : p.profileWeight) w = 0;
    p.profileWeight[0] = 1; // flat
    for (float &w : p.styleWeight) w = 0;
    p.styleWeight[0] = 1;   // forest: guardrails both sides, no runoff
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// track generation
// ---------------------------------------------------------------------------

TEST(track_same_seed_is_identical)
{
    Track a(424242), b(424242);
    a.ensureGenerated(1500);
    // Generate b in a different pattern (as a client streaming ahead would).
    for (uint32_t i = 0; i <= 1500; i += 7) b.update(i);
    b.ensureGenerated(1500);
    bool all = true;
    for (uint32_t i = 0; i <= 1500; ++i) all = all && sectionsIdentical(a.section(i), b.section(i));
    CHECK(all);
    for (uint32_t i : {0u, 5u, 17u, 120u, 777u})
    {
        SectionGeometry ga = buildSectionGeometry(a, i), gb = buildSectionGeometry(b, i);
        bool same = ga.road.positions.size() == gb.road.positions.size() &&
                    ga.terrain.positions.size() == gb.terrain.positions.size() && ga.trees.size() == gb.trees.size() &&
                    std::memcmp(ga.road.positions.data(), gb.road.positions.data(),
                                ga.road.positions.size() * sizeof(dm::Vec3)) == 0 &&
                    std::memcmp(ga.terrain.positions.data(), gb.terrain.positions.data(),
                                ga.terrain.positions.size() * sizeof(dm::Vec3)) == 0;
        for (size_t k = 0; same && k < ga.trees.size(); ++k)
            same = ga.trees[k].model == gb.trees[k].model && sameBits(ga.trees[k].position, gb.trees[k].position);
        CHECK(same);
    }
    Track c(1);
    c.reset(424242);
    c.ensureGenerated(1500);
    bool sameAfterReset = true;
    for (uint32_t i = 0; i <= 1500; ++i) sameAfterReset = sameAfterReset && sectionsIdentical(a.section(i), c.section(i));
    CHECK(sameAfterReset);
}

TEST(track_different_seeds_differ)
{
    int differing = 0;
    for (uint32_t seed = 1; seed < 20; ++seed)
    {
        Track a(seed), b(seed + 1000);
        a.ensureGenerated(60);
        b.ensureGenerated(60);
        bool diff = false;
        for (uint32_t i = 1; i <= 60; ++i)
            diff = diff || a.section(i).feature != b.section(i).feature || !sameBits(a.section(i).turnAngle, b.section(i).turnAngle);
        differing += diff;
    }
    CHECK_MSG(differing == 19, "%d/19 seed pairs differ", differing);
}

TEST(track_every_feature_style_and_profile_appears)
{
    const int nk = static_cast<int>(FeatureKind::Count);
    uint32_t perKind[static_cast<int>(FeatureKind::Count)] = {};
    uint32_t perStyle[4] = {}, climbs = 0, crests = 0, dips = 0, runoffSections = 0, features = 0, rerolls = 0, fallbacks = 0;
    double metres[static_cast<int>(FeatureKind::Count)] = {};
    for (uint32_t seed : {5u, 55u, 555u, 5555u, 55555u})
    {
        Track t(seed);
        t.ensureGenerated(3000);
        for (int k = 0; k < nk; ++k) perKind[k] += t.stats().perKind[k];
        features += t.stats().features;
        rerolls += t.stats().rerolls;
        fallbacks += t.stats().fallbacks;
        for (uint32_t i = 1; i < t.sectionCount(); ++i)
        {
            const TrackSection &s = t.section(i);
            ++perStyle[static_cast<int>(s.style)];
            metres[static_cast<int>(s.feature)] += s.length;
            climbs += s.drop < 0;
            crests += s.bump > 0;
            dips += s.bump < 0;
            runoffSections += s.runoff[0] > 0 || s.runoff[1] > 0;
        }
    }
    std::printf("    %u features (%u re-rolls, %u fallbacks):", features, rerolls, fallbacks);
    for (int k = 1; k < nk; ++k)
        std::printf(" %s %.1f%%", featureName(static_cast<FeatureKind>(k)), 100.0 * perKind[k] / features);
    std::printf("\n    styles forest/cliff/rock/runoff sections: %u/%u/%u/%u; climbs %u, crests %u, dips %u\n", perStyle[0],
                perStyle[1], perStyle[2], perStyle[3], climbs, crests, dips);
    for (int k = 1; k < nk; ++k) CHECK_MSG(perKind[k] > 50, "%s appears %u times", featureName(static_cast<FeatureKind>(k)), perKind[k]);
    for (uint32_t c : perStyle) CHECK(c > 100);
    CHECK(climbs > 50 && crests > 30 && dips > 30 && runoffSections > 100);
    CHECK(fallbacks == 0);
    // Mostly flowing bends: bends (everything but straights) are the majority.
    CHECK(perKind[static_cast<int>(FeatureKind::Straight)] * 3 < features);
    (void)metres;
}

TEST(track_sections_join_smoothly_and_queries_roundtrip)
{
    Track t(2024);
    t.ensureGenerated(600);
    const TrackParams &tp = t.params();
    float maxJoinErr = 0, maxRoundTrip = 0, maxGradeJump = 0, maxEdgeJump = 0, maxGrade = 0;
    for (uint32_t i = 1; i <= 600; ++i)
    {
        const TrackSection &p = t.section(i - 1), &s = t.section(i);
        maxJoinErr = std::fmax(maxJoinErr, dm::length(p.centreAt(p.length) - s.centreAt(0)));
        maxJoinErr = std::fmax(maxJoinErr, std::fabs(dm::wrapAngle(p.endYaw - s.startYaw)));
        maxGradeJump = std::fmax(maxGradeJump, std::fabs(p.gradeAt(p.length - 0.01f) - s.gradeAt(0.01f)));
        for (int side = -1; side <= 1; side += 2)
        {
            maxEdgeJump = std::fmax(maxEdgeJump, std::fabs(p.edgeAt(p.length, side, tp) - s.edgeAt(0, side, tp)));
            maxEdgeJump = std::fmax(maxEdgeJump, std::fabs(p.uphillness(p.length, side) - s.uphillness(0, side)));
        }
        for (float f = 0.05f; f < 1.0f; f += 0.1f) maxGrade = std::fmax(maxGrade, std::fabs(s.gradeAt(s.length * f)));
        for (float f : {0.1f, 0.5f, 0.9f})
            for (float d : {-5.0f, 0.0f, 3.0f})
                for (uint32_t hint : {i - 1, i, i + 1})
                {
                    dm::Vec3 pt = s.pointAt(s.length * f, d) + dm::Vec3{0, 0.3f, 0};
                    RoadQuery q = t.query(pt, hint);
                    float err = q.section == i ? std::fabs(q.s - s.length * f) + std::fabs(q.d - d) : 1e9f;
                    // Very short link sections: a point may legitimately land in a neighbour.
                    if (q.section != i && s.length < 8.0f) err = 0;
                    maxRoundTrip = std::fmax(maxRoundTrip, err);
                }
    }
    std::printf("    600 sections: join %.1e, grade jump %.1e, edge/terrain jump %.1e, steepest grade %.1f%%\n", maxJoinErr,
                maxGradeJump, maxEdgeJump, 100 * maxGrade);
    CHECK_MSG(maxJoinErr < 1e-3f, "join error %g", maxJoinErr);
    CHECK_MSG(maxGradeJump < 0.01f, "grade jump %g", maxGradeJump);   // C1 height profile
    CHECK_MSG(maxEdgeJump < 1e-3f, "edge / terrain jump %g", maxEdgeJump); // runoff & roadside continuous
    CHECK_MSG(maxGrade < 0.2f, "steepest grade %.3f", maxGrade);
    CHECK_MSG(maxRoundTrip < 1e-2f, "query round trip error %g", maxRoundTrip);
}

TEST(track_never_self_intersects_and_paces_difficulty)
{
    // Thousands of sections x many seeds: every road point stays well clear
    // (horizontally -- no bridges either) of all road more than 40 m of arc
    // back, the heading stays inside the descent band, nothing tight follows
    // a long straight, and back-to-back hairpins get a link straight.
    const int seeds = std::getenv("DRIFTO_SOAK") ? 100 : 30;
    const uint32_t n = 4000;
    float worst = 1e9f, worstBand = 0;
    int pacingViolations = 0, hairpinLinks = 0, backToBack = 0;
    double km = 0;
    for (int sd = 0; sd < seeds; ++sd)
    {
        Track t(static_cast<uint32_t>(sd) * 2654435761u + 7u);
        t.ensureGenerated(n);
        const TrackParams &tp = t.params();
        const float clearanceNeeded = 2.0f * (tp.roadHalfWidth + tp.shoulderWidth + tp.runoffWidth); // edge to edge
        struct P { float x, z, dist; };
        std::unordered_map<int64_t, std::vector<P>> grid;
        auto key = [](int64_t cx, int64_t cz) {
            return static_cast<int64_t>((static_cast<uint64_t>(cx) << 32) ^ (static_cast<uint64_t>(cz) & 0xffffffffull));
        };
        float straightRun = 1e9f; // the start straight counts as long
        FeatureKind prevKind = FeatureKind::Start;
        for (uint32_t i = 0; i < t.sectionCount(); ++i)
        {
            const TrackSection &s = t.section(i);
            worstBand = std::fmax(worstBand, std::fabs(dm::wrapAngle(s.endYaw - tp.descentYaw)));
            if (s.featurePart == 0 && i > 0)
            {
                bool tight = s.feature == FeatureKind::Hairpin || s.feature == FeatureKind::Tightening ||
                             s.feature == FeatureKind::Chicane;
                if (tight && straightRun > tp.longStraight) ++pacingViolations;
                if (s.feature == FeatureKind::Hairpin && prevKind == FeatureKind::Hairpin)
                {
                    ++backToBack;
                    hairpinLinks += s.type == SectionType::Line;
                }
                straightRun = s.feature == FeatureKind::Straight ? straightRun + s.length : 0;
                if (s.feature != FeatureKind::Straight) straightRun = 0;
                prevKind = s.feature;
            }
            else if (s.feature == FeatureKind::Straight)
                straightRun += s.length;
            int m = static_cast<int>(std::ceil(s.length / 2.0f));
            for (int k = 0; k <= m; ++k)
            {
                float sa = s.length * static_cast<float>(k) / static_cast<float>(m);
                dm::Vec3 c = s.centreAt(sa);
                float dist = s.startDistance + sa;
                int64_t cx = static_cast<int64_t>(std::floor(c.x / 20.0f)), cz = static_cast<int64_t>(std::floor(c.z / 20.0f));
                for (int64_t ox = -1; ox <= 1; ++ox)
                    for (int64_t oz = -1; oz <= 1; ++oz)
                    {
                        auto it = grid.find(key(cx + ox, cz + oz));
                        if (it == grid.end()) continue;
                        for (const P &p : it->second)
                            if (dist - p.dist >= 40.0f)
                                worst = std::fmin(worst, std::hypot(c.x - p.x, c.z - p.z));
                    }
                grid[key(cx, cz)].push_back({c.x, c.z, dist});
            }
        }
        km += t.section(n).startDistance / 1000.0;
        CHECK_MSG(worst > clearanceNeeded, "seed %d: road %.1f m from older road (needs %.1f)", sd, worst, clearanceNeeded);
    }
    std::printf("    %d seeds x %u sections (%.0f km): closest older road %.1f m, max |heading - fall line| %.1f deg, "
                "%d back-to-back hairpins (%d with a link)\n",
                seeds, n, km, worst, dm::degrees(worstBand), backToBack, hairpinLinks);
    CHECK(worstBand <= dm::radians(88.0f) + 1e-3f);
    CHECK_MSG(pacingViolations == 0, "%d tight features straight off a long straight", pacingViolations);
    CHECK(hairpinLinks == backToBack);
}

TEST(track_meshes_are_sane)
{
    Track t(55);
    t.ensureGenerated(120);
    for (uint32_t i = 0; i <= 120; ++i)
    {
        SectionGeometry g = buildSectionGeometry(t, i);
        const gfx::MeshData *meshes[] = {&g.road, &g.terrain, &g.props};
        for (const gfx::MeshData *m : meshes)
        {
            bool idxOk = m->indices.size() % 3 == 0 && m->normals.size() == m->positions.size() &&
                         m->colors.size() == m->positions.size();
            for (uint32_t ix : m->indices) idxOk = idxOk && ix < m->positions.size();
            CHECK(idxOk);
            int bad = 0;
            for (size_t k = 0; k + 2 < m->indices.size(); k += 3)
            {
                const dm::Vec3 &a = m->positions[m->indices[k]], &b = m->positions[m->indices[k + 1]],
                               &c = m->positions[m->indices[k + 2]];
                dm::Vec3 fn = dm::cross(b - a, c - a);
                if (dm::lengthSq(fn) > 1e-10f && dm::dot(fn, m->normals[m->indices[k]]) <= 0) ++bad;
            }
            CHECK_MSG(bad == 0, "section %u: %d triangles wound against their normal", i, bad);
        }
        bool roadUp = true;
        for (const dm::Vec3 &n : g.road.normals) roadUp = roadUp && n.y > 0.9f;
        CHECK(roadUp);
        CHECK(!g.road.positions.empty() && !g.terrain.positions.empty());
        bool treesOffRoad = true;
        const TrackSection &sec = t.section(i);
        for (const TreePlacement &tp : g.trees)
        {
            RoadQuery q = t.query(tp.position, i);
            treesOffRoad = treesOffRoad && std::fabs(q.d) > q.edge + 1.0f;
            bool known = false;
            for (const std::string &n : treeModelNames()) known = known || n == tp.model;
            treesOffRoad = treesOffRoad && known;
        }
        CHECK_MSG(treesOffRoad, "section %u (%s)", i, featureName(sec.feature));
    }
    CHECK(treeModelNames().size() == 38);
}

// ---------------------------------------------------------------------------
// physics (single car)
// ---------------------------------------------------------------------------

TEST(physics_is_deterministic)
{
    Track t(9876);
    Pcg32 rng(5, 5);
    std::vector<net::CarInput> inputs(1800);
    for (size_t i = 0; i < inputs.size(); ++i)
        inputs[i] = driveInput(static_cast<uint32_t>(i), rng.range(-1.0f, 1.0f), rng.nextFloat() < 0.15f);
    auto run = [&](size_t from, size_t to, CarSimState s, Track &tr) {
        for (size_t i = from; i < to; ++i) { tr.ensureGenerated(s.section + 10); stepCar(s, inputs[i], tr); }
        return s;
    };
    Track t2(9876);
    CarSimState a = run(0, inputs.size(), spawnOnGrid(t, 0), t);
    CarSimState b = run(0, inputs.size(), spawnOnGrid(t2, 0), t2);
    CHECK(hashState(a) == hashState(b));
    CHECK(sameBits(a.position, b.position) && sameBits(a.velocity, b.velocity));
    // Rewind + replay lands on the identical bits (what reconciliation relies on).
    CarSimState mid = run(0, 900, spawnOnGrid(t, 0), t);
    CarSimState copy = mid;
    CHECK(hashState(run(900, inputs.size(), mid, t)) == hashState(run(900, inputs.size(), copy, t)));
    std::printf("    after %zu random ticks: section %u, %.1f m, deaths %u, hash %016llx\n", inputs.size(), a.section,
                a.distance, a.deaths, static_cast<unsigned long long>(hashState(a)));
}

TEST(physics_rests_stably_on_suspension)
{
    Track t(1);
    CarSimState s = spawnOnGrid(t, 3);
    dm::Vec3 start = s.position;
    for (int i = 0; i < 600; ++i) stepCar(s, holdInput(i), t);
    float moved = dm::length(s.position - start);
    CHECK_MSG(moved < 0.05f, "moved %.4f m at rest", moved);
    CHECK_MSG(dm::length(s.velocity) < 0.01f, "speed %.4f at rest", dm::length(s.velocity));
    CHECK(s.wheelContactMask == 0xF && s.alive);
    float h = s.position.y - t.query(s.position, 0).height;
    CHECK_MSG(std::fabs(h - defaultCarParams().rideHeight) < 0.03f, "ride height %.3f", h);
}

TEST(physics_zero_steer_stays_on_straight)
{
    Track t(77, allStraights());
    CarSimState s = spawnCar(t, 0, 5.0f, 0.0f);
    float maxLat = 0, maxDrift = 0;
    for (int i = 0; i < 60 * 25; ++i)
    {
        t.update(s.section);
        stepCar(s, driveInput(i, 0.0f, false), t);
        maxLat = std::fmax(maxLat, std::fabs(s.lateral));
        maxDrift = std::fmax(maxDrift, std::fabs(s.driftAngle));
    }
    CHECK(s.alive && s.deaths == 0);
    CHECK_MSG(maxLat < 0.1f, "max lateral %.4f m", maxLat);
    CHECK_MSG(maxDrift < 1.0f, "max drift angle %.3f deg", maxDrift);
    CHECK(!s.drifting);
    float top = std::sqrt(defaultCarParams().driveForce / defaultCarParams().drag);
    CHECK_MSG(s.speed > 0.97f * top && s.speed < 1.01f * top, "speed %.2f (v_top %.2f)", s.speed, top);
    std::printf("    25 s auto-throttle: %.1f m, %.1f m/s (%.0f km/h) [v_top %.1f], max |d| %.4f m\n", s.distance,
                s.speed, s.speed * 3.6f, top, maxLat);
}

TEST(physics_tyres_are_frictionless_like_the_original)
{
    // Kick the car sideways with no throttle: only v^2 drag slows the side
    // slip (drifto's "Slippery" material), nothing grips.
    Track t(77, allStraights());
    CarSimState s = spawnCar(t, 1, 5.0f, 0.0f);
    s.velocity = dm::rightOf(s.rotation) * 6.0f;
    for (int i = 0; i < 30; ++i) stepCar(s, holdInput(i), t);
    float expected = 6.0f / (1.0f + defaultCarParams().drag * 6.0f * 0.5f); // dv/dt = -k v^2 for 0.5 s
    CHECK_MSG(std::fabs(s.speed - expected) < 0.15f, "side speed %.3f after 0.5 s (pure drag: %.3f)", s.speed, expected);
}

TEST(physics_hard_steer_at_speed_drifts)
{
    Track t(5, allStraights());
    CarSimState s = spawnCar(t, 0, 2.0f, 0.0f, 20.0f);
    bool drifted = false;
    float maxAngle = 0;
    for (int i = 0; i < 40; ++i)
    {
        stepCar(s, driveInput(i, 1.0f, false), t);
        drifted = drifted || s.drifting;
        maxAngle = std::fmax(maxAngle, std::fabs(s.driftAngle));
    }
    CHECK_MSG(drifted && maxAngle > 30.0f, "max angle %.1f", maxAngle);
    std::printf("    0.67 s of full lock at 20 m/s: drift angle %.1f deg\n", maxAngle);
    // A tiny correction is below the drifting threshold.
    CarSimState g = spawnCar(t, 0, 2.0f, 0.0f, 20.0f);
    bool gDrift = false;
    for (int i = 0; i < 3; ++i) { stepCar(g, driveInput(i, 0.05f, false), t); gDrift = gDrift || g.drifting; }
    CHECK(!gDrift);
}

TEST(physics_respawn_is_on_road_aligned_ghosted_and_deterministic)
{
    Track t(8, allStraights());
    int wallSide = 1; // every edge is a barrier
    // Head-on into the wall at 20 m/s: wrecked, then respawned.
    CarSimState s = spawnCar(t, 2, 10.0f, 0.0f, 0.0f);
    s.velocity = t.section(2).rightAt(10.0f) * (20.0f * wallSide) + t.section(2).forwardAt(10.0f) * 5.0f;
    float crashDistance = 0;
    bool died = false, respawned = false;
    int respawnTick = -1, deathTick = -1;
    for (int i = 0; i < 60 * 4 && !respawned; ++i)
    {
        stepCar(s, driveInput(i, 1.0f * wallSide, false), t); // keep steering into it
        if (!s.alive && !died) { died = true; crashDistance = s.distance; deathTick = i; }
        if (died && s.alive) { respawned = true; respawnTick = i - deathTick; }
    }
    CHECK(died && respawned && s.deaths == 1);
    if (!respawned) return;
    const CarParams &p = defaultCarParams();
    CHECK_MSG(respawnTick > 0 && respawnTick <= static_cast<int>(p.respawnDelay * 60) + 2, "respawn after %d ticks", respawnTick);
    RoadQuery q = t.query(s.position, s.section);
    float yawErr = std::fabs(dm::wrapAngle(std::atan2(-dm::forwardOf(s.rotation).x, -dm::forwardOf(s.rotation).z) -
                                           t.section(q.section).yawAt(q.s)));
    std::printf("    respawn: %.1f m behind the crash, lateral %.2f m, yaw err %.2f deg, %.1f m/s, ghost %.2f s\n",
                crashDistance - s.distance, q.d, dm::degrees(yawErr), s.speed, s.ghostTimer);
    CHECK(std::fabs(q.d) < 0.5f);                                     // on the centreline
    CHECK(yawErr < dm::radians(2.0f));                                // aligned with the road
    CHECK(std::fabs(s.position.y - q.height - p.rideHeight) < 0.1f);  // wheels on the surface
    CHECK(s.distance < crashDistance && crashDistance - s.distance < p.respawnBackoff + 3.0f);
    CHECK(s.speed >= p.respawnMinSpeed - 0.5f);
    CHECK(s.ghostTimer > 1.0f && s.steerRamp < 0.2f);
    CHECK(dm::length(s.angularVelocity) < 0.5f && s.spinRate == 0.0f);
    // Steering comes back gradually, not as a snap.
    CarSimState r = s;
    stepCar(r, driveInput(0, 1.0f, false), t);
    CHECK(std::fabs(r.angularVelocity.y) < 0.6f);
    // Ghosted: another car can sit on top of it without contact.
    CarSimState other = s;
    other.ghostTimer = 0;
    other.position.x += 0.5f;
    CHECK(!collideCars(s, false, other, false, p, defaultRaceParams()));
    // ...and the ghost flag survives the network round trip.
    CHECK((toNetState(s, 1, 0).flags & kCarFlagGhost) != 0);
}

TEST(physics_walls_keep_car_on_road_and_cost_speed)
{
    Track t(8, allStraights());
    int wallSide = 1; // every edge is a barrier
    CarSimState s = spawnCar(t, 1, 2.0f, 0.0f, 12.0f);
    float maxOut = 0;
    bool touched = false;
    for (int i = 0; i < 60 * 4; ++i)
    {
        t.update(s.section);
        stepCar(s, driveInput(i, 0.3f * wallSide, false), t);
        maxOut = std::fmax(maxOut, s.lateral * wallSide);
        touched = touched || s.lateral * wallSide > t.edgeOffset() - 2.5f;
    }
    CHECK(touched);
    CHECK_MSG(maxOut < t.edgeOffset(), "centre reached %.2f m (edge %.2f)", maxOut, t.edgeOffset());
    CHECK(s.alive);
    // Head-on into the wall at speed wrecks.
    CarSimState h = spawnCar(t, 1, 2.0f, 0.0f, 0.0f);
    h.velocity = t.section(1).rightAt(2.0f) * (20.0f * wallSide);
    for (int i = 0; i < 60; ++i) stepCar(h, holdInput(i), t);
    CHECK(h.deaths == 1);
}

TEST(physics_bot_survives_the_switchback)
{
    // Tuning regression: the look-ahead bot must get down two minutes of the
    // default switchback (almost) without wrecking.
    for (uint32_t seed : {1234u, 42u})
    {
        Track t(seed);
        CarSimState s = spawnOnGrid(t, 0);
        BotDriver bot;
        float maxDrift = 0, sumV = 0;
        for (int i = 0; i < 60 * 120; ++i)
        {
            t.update(s.section);
            stepCar(s, bot.drive(s, t, defaultCarParams(), i), t);
            maxDrift = std::fmax(maxDrift, std::fabs(s.driftAngle));
            sumV += s.speed;
        }
        CHECK_MSG(s.deaths <= 1, "seed %u: %u wrecks", seed, s.deaths);
        CHECK_MSG(s.section > 25, "seed %u: only reached section %u", seed, s.section);
        std::printf("    seed %u: 120 s -> section %u, %.0f m, avg %.1f m/s, max drift %.0f deg, wrecks %u\n", seed,
                    s.section, s.distance, sumV / 7200, maxDrift, s.deaths);
    }
}

TEST(physics_bot_handles_every_section_type)
{
    // The bot (and the smaller, re-tuned car) across many seeds: every kind
    // of feature gets driven through, with few wrecks.
    const int nk = static_cast<int>(FeatureKind::Count);
    double metres[static_cast<int>(FeatureKind::Count)] = {};
    int wrecksPer[static_cast<int>(FeatureKind::Count)] = {};
    double km = 0;
    int wrecks = 0;
    uint32_t rescues = 0;
    for (uint32_t seed = 0; seed < 10; ++seed)
    {
        Track t(9000 + seed * 31);
        CarSimState s = spawnOnGrid(t, 0);
        BotDriver bot;
        for (int i = 0; i < 60 * 90; ++i)
        {
            t.update(s.section);
            CarSimState before = s;
            stepCar(s, bot.drive(s, t, defaultCarParams(), i), t);
            const TrackSection &sec = t.section(before.section);
            if (s.alive) metres[static_cast<int>(sec.feature)] += s.speed * kFixedDt;
            if (before.alive && !s.alive) { ++wrecks; ++wrecksPer[static_cast<int>(sec.feature)]; }
        }
        km += s.distance / 1000.0;
        rescues += s.rescues;
    }
    std::printf("    10 seeds x 90 s: %.1f km, %d wrecks (%.2f/km), driven:", km, wrecks, wrecks / km);
    for (int k = 1; k < nk; ++k) std::printf(" %s %.0fm/%dw", featureName(static_cast<FeatureKind>(k)), metres[k], wrecksPer[k]);
    std::printf("\n");
    for (int k = 1; k < nk; ++k) CHECK_MSG(metres[k] > 100.0, "%s driven %.0f m", featureName(static_cast<FeatureKind>(k)), metres[k]);
    CHECK_MSG(wrecks / km < 0.3, "%.2f wrecks/km", wrecks / km);
    CHECK(rescues == 0);
}

TEST(physics_net_state_roundtrip)
{
    Track t(3);
    CarSimState s = spawnOnGrid(t, 2);
    for (int i = 0; i < 300; ++i) stepCar(s, driveInput(i, 0.3f, i > 200), t);
    net::CarState n = toNetState(s, 7, 1234);
    CHECK(n.playerId == 7 && n.lastInputTick == 1234);
    CarSimState r = fromNetState(n);
    CHECK(sameBits(r.position, s.position) && sameBits(r.velocity, s.velocity) &&
          sameBits(r.angularVelocity, s.angularVelocity));
    CHECK(r.alive == s.alive && r.drifting == s.drifting && r.section == s.section);
    CHECK(sameBits(r.nitro, s.nitro) && sameBits(r.distance, s.distance) && r.nitroActive == s.nitroActive);
    CarSimState local = s;
    local.steer = 0.77f;
    applyNetState(local, n);
    CHECK(local.steer == 0.77f);
}

TEST(camera_and_skidmarks_smoke)
{
    Track t(4, allStraights());
    CarSimState s = spawnCar(t, 0, 2.0f, 0.0f, 20.0f);
    ChaseCamera cam;
    SkidMarks marks;
    for (int i = 0; i < 90; ++i)
    {
        stepCar(s, driveInput(i, i < 30 ? 1.0f : -0.4f, false), t);
        cam.update(s.position, s.rotation, s.velocity, kFixedDt);
        marks.update(t, s.section, s.position, s.rotation, s.drifting, s.driftAngle);
    }
    float d = dm::length(cam.position - s.position);
    CHECK_MSG(d > 3.0f && d < 15.0f, "camera distance %.2f", d);
    CHECK(marks.pointCount() > 4);
    gfx::MeshData m;
    marks.buildMesh(m);
    CHECK(!m.indices.empty() && m.indices.size() % 6 == 0);
    CHECK(smokeAlpha(30.0f, true) > 0.1f && smokeAlpha(30.0f, false) == 0.0f);
}

TEST(cosmetic_gearbox_shifts_like_an_arcade_tach)
{
    // Drive a real car up to top speed, then burn nitro, then coast down.
    Track t(21, allStraights());
    CarSimState s = spawnCar(t, 0, 5.0f, 0.0f);
    s.nitro = 1.0f;
    Gearbox gb(std::sqrt(defaultCarParams().driveForce / defaultCarParams().drag));
    int lastGear = 1, maxGear = 1;
    std::vector<float> drops;
    bool limiterAtTop = false, sixthOnNitro = false, sixthWithout = false;
    float topMin = 1e9f, topMax = 0, peakRpm = 0; // rpm while cruising at top speed / overall
    for (int i = 0; i < 60 * 30; ++i)
    {
        bool burn = i >= 60 * 18 && i < 60 * 20;          // 2 s of nitro at top speed
        float throttle = i < 60 * 22 ? 1.0f : 0.0f;       // then lift off
        net::CarInput in = driveInput(i, 0.0f, burn);
        in.throttle = throttle;
        stepCar(s, in, t);
        float before = gb.rpm();
        gb.update(s.speed, s.nitroActive, s.driftAngle, kFixedDt);
        if (gb.gear() > lastGear && gb.gear() <= 5) drops.push_back(before - gb.rpm());
        maxGear = std::max(maxGear, gb.gear());
        if (i > 60 * 15 && i < 60 * 18)
        {
            limiterAtTop = limiterAtTop || gb.revLimiter();
            topMin = std::min(topMin, gb.rpm());
            topMax = std::max(topMax, gb.rpm());
        }
        peakRpm = std::max(peakRpm, gb.rpm());
        if (gb.gear() == 6) { if (s.nitroActive) sixthOnNitro = true; else if (i > 60 * 21) sixthWithout = true; }
        lastGear = gb.gear();
    }
    std::printf("    upshift drops:");
    for (float d : drops) std::printf(" %.0f", d);
    std::printf(" rpm; max gear %d; top-speed rpm %.0f..%.0f (limiter %d); peak %.0f; 6th on nitro %d; end gear %d at %.1f m/s\n",
                maxGear, topMin, topMax, limiterAtTop, peakRpm, sixthOnNitro, gb.gear(), s.speed);
    CHECK(drops.size() == 4);
    for (float d : drops) CHECK(d > 1500.0f && d < 2500.0f);
    // Cruising at top speed: a steady tone under the redline, never the limiter.
    CHECK(!limiterAtTop && topMax < 7600.0f && topMax - topMin < 150.0f);
    CHECK(peakRpm < gb.redlineRpm);
    CHECK(sixthOnNitro && !sixthWithout);
    CHECK(gb.gear() < 5);  // coasted down => downshifted
    // Idle at rest, never above the redline, wheelspin raises rpm in a drift.
    Gearbox g2;
    g2.update(0.0f, false, 0.0f, 0.5f);
    CHECK(g2.rpm() >= 899.0f && g2.rpm() < 1000.0f && g2.gear() == 1);
    Gearbox a, b;
    for (int i = 0; i < 60; ++i) { a.update(5.0f, false, 0.0f, kFixedDt); b.update(5.0f, false, 40.0f, kFixedDt); }
    CHECK(b.rpm() > a.rpm() * 1.08f && b.rpm() <= b.redlineRpm);
}

// ---------------------------------------------------------------------------
// nitro + slipstream
// ---------------------------------------------------------------------------

namespace {
// A car on a straight sliding at `angle` degrees (nose left of travel) at 15 m/s.
CarSimState slidingCar(const Track &t, float angleDeg)
{
    CarSimState s = spawnCar(t, 1, 5.0f, 0.0f, 0.0f);
    dm::Vec3 travel = t.section(1).forwardAt(5.0f);
    s.rotation = dm::quatYaw(t.section(1).yawAt(5.0f) + dm::radians(angleDeg));
    s.velocity = travel * 15.0f;
    s.nitro = 0.2f;
    return s;
}
} // namespace

TEST(nitro_recharges_only_while_drifting_and_scales_with_angle)
{
    Track t(11, allStraights());
    float gain[4];
    const float angles[4] = {0.0f, 2.0f, 15.0f, 40.0f};
    for (int k = 0; k < 4; ++k)
    {
        CarSimState s = slidingCar(t, angles[k]);
        float before = s.nitro;
        stepCar(s, holdInput(0), t);
        gain[k] = s.nitro - before;
        CHECK((angles[k] > defaultCarParams().driftAngleThreshold) == s.drifting);
    }
    std::printf("    gauge gain per tick at 0/2/15/40 deg: %.5f %.5f %.5f %.5f\n", gain[0], gain[1], gain[2], gain[3]);
    CHECK(gain[0] == 0.0f && gain[1] == 0.0f); // not drifting (threshold 3 deg)
    CHECK(gain[2] > 0.0f && gain[3] > 2.0f * gain[2]);
    // Rubber band multiplier scales it.
    CarSimState a = slidingCar(t, 15.0f), b = slidingCar(t, 15.0f);
    a.nitroRechargeScale = 0.5f;
    b.nitroRechargeScale = 2.0f;
    stepCar(a, holdInput(0), t);
    stepCar(b, holdInput(0), t);
    CHECK(std::fabs((b.nitro - 0.2f) / (a.nitro - 0.2f) - 4.0f) < 0.01f);
    // Straight-line driving never recharges.
    CarSimState c = spawnCar(t, 0, 5.0f, 0.0f);
    c.nitro = 0.2f;
    for (int i = 0; i < 600; ++i) stepCar(c, driveInput(i, 0.0f, false), t);
    CHECK(c.nitro == 0.2f);
}

TEST(nitro_burns_for_speed_and_costs_steering)
{
    Track t(12, allStraights());
    CarSimState a = spawnCar(t, 0, 5.0f, 0.0f), b = a;
    a.nitro = b.nitro = 1.0f;
    float topNoNitro = 0;
    for (int i = 0; i < 60 * 12; ++i) stepCar(a, driveInput(i, 0.0f, false), t);
    topNoNitro = a.speed;
    for (int i = 0; i < 60 * 12; ++i) stepCar(b, driveInput(i, 0.0f, i >= 60 * 10), t); // burn the last 2 s
    std::printf("    speed after 12 s: %.1f m/s plain, %.1f m/s with a 2 s burn; gauge left %.2f\n", topNoNitro,
                b.speed, b.nitro);
    CHECK(b.speed > topNoNitro + 4.0f);
    CHECK(b.nitro < 0.2f);
    CHECK(a.nitro == 1.0f);
    // An empty gauge does nothing.
    CarSimState e = spawnCar(t, 0, 5.0f, 0.0f, 20.0f), f = e;
    e.nitro = 0;
    for (int i = 0; i < 60; ++i) { stepCar(e, driveInput(i, 0.0f, true), t); stepCar(f, driveInput(i, 0.0f, false), t); }
    CHECK(sameBits(e.position, f.position) && !e.nitroActive);
    // Same steering input turns the car less while burning.
    CarSimState g = spawnCar(t, 0, 5.0f, 0.0f, 18.0f), h = g;
    g.nitro = h.nitro = 1.0f;
    for (int i = 0; i < 20; ++i) { stepCar(g, driveInput(i, 1.0f, false), t); stepCar(h, driveInput(i, 1.0f, true), t); }
    float yawPlain = std::fabs(dm::wrapAngle(t.section(0).yawAt(5) - std::atan2(-dm::forwardOf(g.rotation).x, -dm::forwardOf(g.rotation).z)));
    float yawNitro = std::fabs(dm::wrapAngle(t.section(0).yawAt(5) - std::atan2(-dm::forwardOf(h.rotation).x, -dm::forwardOf(h.rotation).z)));
    std::printf("    yaw after 1/3 s full lock: %.1f deg plain, %.1f deg on nitro\n", dm::degrees(yawPlain), dm::degrees(yawNitro));
    CHECK(yawNitro < 0.7f * yawPlain);
}

TEST(nitro_needs_a_minimum_gauge_to_start)
{
    // A burn only STARTS with >= nitroMinStart in the gauge; a running burn
    // may empty it, and holding the key afterwards can't sip the recharge.
    Track t(13, allStraights());
    const CarParams &P = defaultCarParams();
    const float below = P.nitroMinStart * 0.8f;
    CarSimState a = spawnCar(t, 0, 5.0f, 0.0f, 15.0f);
    a.nitro = below;
    for (int i = 0; i < 30; ++i) stepCar(a, driveInput(i, 0.0f, true), t);
    CHECK(!a.nitroActive && a.nitro == below);

    CarSimState b = spawnCar(t, 0, 5.0f, 0.0f, 15.0f);
    b.nitro = P.nitroMinStart;
    int burnTicks = 0;
    for (int i = 0; i < 120 && (i == 0 || b.nitroActive); ++i)
    {
        stepCar(b, driveInput(i, 0.0f, true), t);
        burnTicks += b.nitroActive;
    }
    std::printf("    a %.0f%% gauge burns %d ticks down to %.3f, then stops\n", 100.0f * P.nitroMinStart, burnTicks, b.nitro);
    CHECK(burnTicks >= 5 && b.nitro == 0.0f && !b.nitroActive);
    // Recharged a little, key still held: no restart below the threshold...
    b.nitro = below;
    for (int i = 0; i < 10; ++i) stepCar(b, driveInput(i, 0.0f, true), t);
    CHECK(!b.nitroActive && b.nitro == below);
    // ...and at it the burn starts again, and keeps going below it.
    b.nitro = P.nitroMinStart + 0.01f;
    stepCar(b, driveInput(0, 0.0f, true), t);
    CHECK(b.nitroActive);
    for (int i = 0; i < 4; ++i) stepCar(b, driveInput(i, 0.0f, true), t);
    CHECK(b.nitroActive && b.nitro < P.nitroMinStart);

    // The latch is the replicated nitroActive flag: a client rebuilding the
    // state from the host's CarState (reconcile / replay) decides the same.
    CarSimState burning = b, idle = b;
    idle.nitroActive = false; // same gauge, but not burning last tick
    for (const CarSimState *src : {&burning, &idle})
    {
        CarSimState host = *src, replica = spawnCar(t, 0, 50.0f, 1.0f, 3.0f);
        applyNetState(replica, toNetState(host, 1, 0));
        replica.nitro = host.nitro; // (the wire quantizes the gauge to a byte; compare the rule, not rounding)
        stepCar(host, driveInput(0, 0.0f, true), t);
        stepCar(replica, driveInput(0, 0.0f, true), t);
        CHECK(host.nitroActive == replica.nitroActive);
        CHECK(host.nitroActive == (src == &burning));
    }
}

TEST(slipstream_only_inside_the_wake)
{
    Track t(13, allStraights());
    const RaceParams &rp = defaultRaceParams();
    CarSimState lead = spawnCar(t, 2, 30.0f, 0.0f, 20.0f);
    auto follower = [&](float behind, float lateral, float speed) {
        return spawnCar(t, 2, 30.0f - behind, lateral, speed);
    };
    // Wake sizes scale with the car (kCarScale).
    const float k = kCarScale;
    CHECK(inSlipstream(follower(8 * k, 0, 20), lead, rp));
    CHECK(inSlipstream(follower(18 * k, 2.0f * k, 20), lead, rp)); // wide at the far end of the cone
    CHECK(!inSlipstream(follower(4 * k, 2.0f * k, 20), lead, rp)); // ...narrow right behind
    CHECK(!inSlipstream(follower(25 * k, 0, 20), lead, rp));       // too far
    CHECK(!inSlipstream(follower(8 * k, 3.0f * k, 20), lead, rp)); // beside the wake
    CHECK(!inSlipstream(follower(-8 * k, 0, 20), lead, rp));       // ahead of it
    CarSimState slowLead = lead;
    slowLead.velocity = slowLead.velocity * 0.2f;
    CHECK(!inSlipstream(follower(8 * k, 0, 20), slowLead, rp));   // no wake at low speed
    CarSimState sideways = follower(8 * k, 0, 20);
    sideways.velocity = dm::rightOf(sideways.rotation) * 20.0f;
    CHECK(!inSlipstream(sideways, lead, rp));                  // not travelling the same way

    // In the world step: a car tucked in behind gains on one alone.
    CarSimState l1 = spawnCar(t, 1, 30.0f, 0.0f, 20.0f), f1 = spawnCar(t, 1, 30.0f - 8.0f * k, 0.0f, 20.0f);
    CarSimState solo = f1;
    float gapStart = l1.distance - f1.distance;
    Track t2(13, allStraights());
    for (int i = 0; i < 60 * 3; ++i)
    {
        std::vector<WorldCar> w{{1, &l1, driveInput(i, 0, false), false}, {2, &f1, driveInput(i, 0, false), false}};
        stepWorld(w, t, defaultCarParams(), rp);
        std::vector<WorldCar> w2{{2, &solo, driveInput(i, 0, false), false}};
        stepWorld(w2, t2, defaultCarParams(), rp);
    }
    std::printf("    3 s in the wake: gap %.2f -> %.2f m, %.2f m ahead of a car without a tow\n", gapStart,
                l1.distance - f1.distance, f1.distance - solo.distance);
    CHECK(f1.slipstream && !l1.slipstream);
    CHECK(f1.distance > solo.distance + 1.0f);
}

// ---------------------------------------------------------------------------
// car-to-car collisions
// ---------------------------------------------------------------------------

namespace {
bool overlapping(const CarSimState &a, const CarSimState &b)
{
    // SAT on the two boxes (same as collideCars' test, independent impl. via corners).
    const CarParams &p = defaultCarParams();
    auto axes = [](const CarSimState &s, dm::Vec3 &r, dm::Vec3 &f) {
        r = dm::rightOf(s.rotation); r.y = 0; r = dm::normalize(r);
        f = dm::forwardOf(s.rotation); f.y = 0; f = dm::normalize(f);
    };
    dm::Vec3 ar, af, br, bf;
    axes(a, ar, af);
    axes(b, br, bf);
    dm::Vec3 d = b.position - a.position;
    d.y = 0;
    for (dm::Vec3 L : {ar, af, br, bf})
    {
        float ra = p.halfWidth * std::fabs(dm::dot(ar, L)) + p.halfLength * std::fabs(dm::dot(af, L));
        float rb = p.halfWidth * std::fabs(dm::dot(br, L)) + p.halfLength * std::fabs(dm::dot(bf, L));
        if (std::fabs(dm::dot(d, L)) >= ra + rb - 1e-3f) return false;
    }
    return true;
}
// SAT penetration depth of two car boxes (0 if apart).
float penetration(const dm::Vec3 &pa, const dm::Quat &qa, const dm::Vec3 &pb, const dm::Quat &qb)
{
    const CarParams &p = defaultCarParams();
    auto flatAxis = [](dm::Vec3 v) { v.y = 0; return dm::normalize(v); };
    dm::Vec3 ar = flatAxis(dm::rightOf(qa)), af = flatAxis(dm::forwardOf(qa));
    dm::Vec3 br = flatAxis(dm::rightOf(qb)), bf = flatAxis(dm::forwardOf(qb));
    dm::Vec3 d = pb - pa;
    d.y = 0;
    float pen = 1e9f;
    for (dm::Vec3 L : {ar, af, br, bf})
    {
        float ra = p.halfWidth * std::fabs(dm::dot(ar, L)) + p.halfLength * std::fabs(dm::dot(af, L));
        float rb = p.halfWidth * std::fabs(dm::dot(br, L)) + p.halfLength * std::fabs(dm::dot(bf, L));
        pen = std::fmin(pen, ra + rb - std::fabs(dm::dot(d, L)));
    }
    return pen > 0 ? pen : 0.0f;
}
} // namespace

TEST(collision_separates_and_spins)
{
    Track t(14, allStraights());
    // Overlapping side by side: pushed apart, velocities separate.
    const float k = kCarScale; // car-sized offsets
    CarSimState a = spawnCar(t, 1, 10.0f, -0.6f * k, 15.0f), b = spawnCar(t, 1, 10.0f, 0.6f * k, 15.0f);
    a.velocity += dm::rightOf(a.rotation) * 3.0f; // A drifting into B
    CHECK(overlapping(a, b));
    bool hit = collideCars(a, false, b, false, defaultCarParams(), defaultRaceParams());
    CHECK(hit);
    CHECK(!overlapping(a, b));
    float vrel = dm::dot(b.velocity - a.velocity, dm::rightOf(a.rotation));
    CHECK_MSG(vrel >= 0, "relative normal velocity %.3f", vrel);
    // Momentum is conserved (equal masses).
    CHECK(dm::length((a.velocity + b.velocity) - (dm::forwardOf(a.rotation) * 30.0f + dm::rightOf(a.rotation) * 3.0f)) <
          1e-3f);

    // Off-centre rear-end: the struck car gets spin.
    CarSimState c = spawnCar(t, 1, 20.0f, 0.0f, 10.0f), d = spawnCar(t, 1, 20.0f - 4.1f * k, 1.2f * k, 18.0f);
    collideCars(c, false, d, false, defaultCarParams(), defaultRaceParams());
    std::printf("    off-centre rear-end: spin %.2f rad/s (front car), %.2f rad/s (rear car)\n", c.spinRate, d.spinRate);
    CHECK(std::fabs(c.spinRate) > 0.3f);
    // Proxies are never pushed.
    CarSimState e = spawnCar(t, 1, 10.0f, -0.6f * k, 15.0f), f = spawnCar(t, 1, 10.0f, 0.6f * k, 15.0f), f0 = f;
    collideCars(e, false, f, true, defaultCarParams(), defaultRaceParams());
    CHECK(sameBits(f.position, f0.position) && sameBits(f.velocity, f0.velocity));
}

TEST(collision_no_tunnelling_at_closing_speed)
{
    // 30 m/s faster car rams a slower one from behind, and a head-on pass
    // at 50 m/s closing: never ends up on the other side of the other car.
    // (Takedowns off: a 30 m/s rear-end is now a takedown, the wrecked car
    // is a ghost and is meant to be driven through -- see the takedown tests.)
    Track t(15, allStraights());
    RaceParams noTakedowns;
    noTakedowns.takedowns = false;
    for (float closing : {30.0f, 50.0f})
    {
        CarSimState fast = spawnCar(t, 2, 5.0f, 0.3f, 0.0f), slow = spawnCar(t, 2, 20.0f, 0.0f, 0.0f);
        bool headOn = closing > 40.0f;
        fast.velocity = dm::forwardOf(fast.rotation) * (headOn ? 25.0f : 35.0f);
        if (headOn) slow.velocity = dm::forwardOf(slow.rotation) * -25.0f;
        else slow.velocity = dm::forwardOf(slow.rotation) * 5.0f;
        bool contact = false, passed = false;
        for (int i = 0; i < 90; ++i)
        {
            std::vector<WorldCar> w{{1, &fast, holdInput(i), false}, {2, &slow, holdInput(i), false}};
            stepWorld(w, t, defaultCarParams(), noTakedowns);
            contact = contact || dm::length(fast.position - slow.position) < 2.0f * defaultCarParams().halfLength + 0.3f;
            float order = dm::dot(slow.position - fast.position, t.section(2).forwardAt(10.0f));
            if (std::fabs(fast.lateral - slow.lateral) < 1.5f && order < 0) passed = true;
            CHECK(!overlapping(fast, slow));
        }
        CHECK(contact);
        CHECK_MSG(!passed, "closing %.0f m/s: tunnelled", closing);
    }
}

TEST(collision_world_step_is_deterministic)
{
    auto run = [](std::vector<CarSimState> &cars) {
        Track t(16);
        for (int i = 0; i < 4; ++i) cars.push_back(spawnOnGrid(t, i));
        BotDriver bots[4];
        for (int i = 0; i < 4; ++i) bots[i].laneBias = (i % 2 ? 1.5f : -1.5f) * (i < 2 ? 1 : -1); // fight for lines
        int contacts = 0;
        for (int k = 0; k < 60 * 40; ++k)
        {
            std::vector<WorldCar> w;
            uint32_t maxSec = 0;
            for (int i = 0; i < 4; ++i)
            {
                // Bots plus a periodic swerve into the neighbour.
                net::CarInput in = bots[i].drive(cars[i], t, defaultCarParams(), k);
                if ((k / 45) % 4 == i) in.steer = (i % 2) ? -1.0f : 1.0f;
                w.push_back({i + 1, &cars[i], in, false});
                maxSec = std::max(maxSec, cars[i].section);
            }
            t.ensureGenerated(maxSec + 12);
            std::vector<CarSimState> before = cars;
            stepWorld(w, t, defaultCarParams(), defaultRaceParams());
            for (int i = 0; i < 4; ++i) contacts += std::fabs(cars[i].spinRate) > std::fabs(before[i].spinRate) + 0.05f;
        }
        uint64_t h = 1469598103934665603ull;
        for (const CarSimState &c : cars) h = (h ^ hashState(c)) * 1099511628211ull;
        return std::make_pair(h, contacts);
    };
    std::vector<CarSimState> a, b;
    auto ra = run(a), rb = run(b);
    std::printf("    40 s, 4 bots swerving into each other: %d spin-inducing contacts, hash %016llx\n", ra.second,
                static_cast<unsigned long long>(ra.first));
    CHECK(ra.first == rb.first);
    CHECK(ra.second > 5);
}


// ---------------------------------------------------------------------------
// takedowns and contact feel
// ---------------------------------------------------------------------------

namespace {
// Two cars on a straight, `gap` metres apart along the road, both driving
// straight ahead (throttle on), stepped together until `ticks` run out.
struct RamResult
{
    uint32_t victimDeaths = 0, attackerDeaths = 0, attackerTakedowns = 0;
    float attackerSpeedBefore = 0, attackerSpeedAfter = 0;
    bool sawVictimEvent = false, sawAttackerEvent = false;
};
RamResult ram(float attackerSpeed, float victimSpeed, bool headOn, const RaceParams &race = defaultRaceParams())
{
    Track t(15, allStraights());
    CarSimState att = spawnCar(t, 2, 10.0f, 0.0f, attackerSpeed), vic = spawnCar(t, 2, 13.6f, 0.0f, victimSpeed);
    if (headOn) vic.velocity = dm::forwardOf(vic.rotation) * -victimSpeed; // coming the other way
    RamResult r;
    bool touched = false;
    for (int i = 0; i < 120; ++i)
    {
        CarSimState before = att;
        std::vector<WorldCar> w{{1, &att, driveInput(i, 0.0f, false), false}, {2, &vic, driveInput(i, 0.0f, false), false}};
        stepWorld(w, t, defaultCarParams(), race);
        if (!touched && att.impactCar > 0)
        {
            touched = true;
            r.attackerSpeedBefore = before.speed;
            r.attackerSpeedAfter = dm::length(att.velocity);
        }
        r.sawVictimEvent |= vic.takedownEvent == 2;
        r.sawAttackerEvent |= att.takedownEvent == 1;
    }
    r.victimDeaths = vic.deaths;
    r.attackerDeaths = att.deaths;
    r.attackerTakedowns = att.takedowns;
    return r;
}
} // namespace

TEST(takedown_hard_rear_end_wrecks_the_victim_attacker_keeps_momentum)
{
    // 26 vs 12 m/s: ~14 m/s closing, all of it the attacker's => takedown.
    RamResult r = ram(26.0f, 12.0f, false);
    std::printf("    rear-end 26 -> 12 m/s: victim deaths %u, attacker takedowns %u, attacker %.1f -> %.1f m/s\n",
                r.victimDeaths, r.attackerTakedowns, r.attackerSpeedBefore, r.attackerSpeedAfter);
    CHECK(r.victimDeaths == 1 && r.sawVictimEvent);
    CHECK(r.attackerTakedowns == 1 && r.sawAttackerEvent && r.attackerDeaths == 0);
    CHECK_MSG(r.attackerSpeedAfter > 0.85f * r.attackerSpeedBefore, "attacker %.1f -> %.1f", r.attackerSpeedBefore,
              r.attackerSpeedAfter);
    // Below the threshold: an ordinary bump.
    RamResult soft = ram(18.0f, 12.0f, false);
    CHECK(soft.victimDeaths == 0 && soft.attackerTakedowns == 0 && !soft.sawVictimEvent);
    // Equal head-on: nobody dominates => no takedown (it is just a big hit).
    RamResult head = ram(12.0f, 12.0f, true);
    CHECK(head.victimDeaths == 0 && head.attackerTakedowns == 0);
    // Switched off: no takedown at any speed.
    RaceParams off;
    off.takedowns = false;
    CHECK(ram(26.0f, 12.0f, false, off).victimDeaths == 0);
}

TEST(takedown_respects_proxies_and_is_deterministic)
{
    Track t(15, allStraights());
    const CarParams &p = defaultCarParams();
    auto pair = [&](CarSimState &a, CarSimState &b) {
        a = spawnCar(t, 2, 20.0f - 2.0f * p.halfLength + 0.05f, 0.0f, 25.0f);
        b = spawnCar(t, 2, 20.0f, 0.0f, 12.0f);
    };
    CarSimState a, b;
    // Client predicting its attacking car against a proxy victim: the proxy
    // is untouched (the host decides), our car scores and keeps going.
    pair(a, b);
    CarSimState b0 = b;
    collideCars(a, false, b, true, p, defaultRaceParams());
    CHECK(b.alive && sameBits(b.velocity, b0.velocity));
    CHECK(a.takedownEvent == 1 && a.alive);
    // Our car is the victim of a proxy: wrecked locally, like on the host.
    pair(a, b);
    CarSimState a0 = a;
    collideCars(a, true, b, false, p, defaultRaceParams());
    CHECK(!b.alive && b.crashed && b.takedownEvent == 2);
    CHECK(sameBits(a.velocity, a0.velocity) && a.takedownEvent == 0);
    // Same inputs => same bits.
    CarSimState c, d, e, f;
    pair(c, d);
    pair(e, f);
    collideCars(c, false, d, false, p, defaultRaceParams());
    collideCars(e, false, f, false, p, defaultRaceParams());
    CHECK(hashState(c) == hashState(e) && hashState(d) == hashState(f));
}

TEST(collision_hits_favour_the_hitter_light_touches_stay_symmetric)
{
    // Why contacts felt "glued": restitution 0.3 + Coulomb friction 0.35 left
    // both cars with nearly the same sideways velocity after a hit (and our
    // tyres have no grip to kill it), so they slid off together. Now real
    // hits push the victim, not the hitter.
    Track t(14, allStraights());
    const float k = kCarScale;
    auto sideHit = [&](float lateralSpeed, float &dA, float &dB) {
        CarSimState a = spawnCar(t, 1, 10.0f, -0.6f * k, 18.0f), b = spawnCar(t, 1, 10.0f, 0.6f * k, 18.0f);
        dm::Vec3 r = dm::rightOf(a.rotation);
        a.velocity += r * lateralSpeed;
        float a0 = dm::dot(a.velocity, r), b0 = dm::dot(b.velocity, r);
        collideCars(a, false, b, false, defaultCarParams(), defaultRaceParams());
        dA = std::fabs(dm::dot(a.velocity, r) - a0);
        dB = std::fabs(dm::dot(b.velocity, r) - b0);
    };
    float dA, dB;
    sideHit(2.0f, dA, dB);
    std::printf("    side touch 2 m/s: hitter dv %.2f, hit car dv %.2f\n", dA, dB);
    CHECK(std::fabs(dA - dB) < 1e-3f); // symmetric, momentum conserving
    sideHit(8.0f, dA, dB);
    std::printf("    side hit   8 m/s: hitter dv %.2f, hit car dv %.2f\n", dA, dB);
    CHECK_MSG(dA < 0.6f * dB, "hitter %.2f vs victim %.2f", dA, dB);
}

TEST(wall_touch_prototype_is_off_by_default)
{
    CHECK(!defaultRaceParams().wallTouchWrecks);
    // A 3 m/s brush along the barrier: survivable by default, a wreck with
    // the prototype on.
    for (bool proto : {false, true})
    {
        Track t(14, allStraights());
        CarSimState c = spawnCar(t, 1, 10.0f, 3.5f, 18.0f);
        c.velocity += t.section(1).rightAt(10.0f) * 3.0f;
        RaceParams r;
        r.wallTouchWrecks = proto;
        r.wallTouchSpeed = 1.0f;
        for (int i = 0; i < 60; ++i)
        {
            std::vector<WorldCar> w{{1, &c, holdInput(i), false}};
            stepWorld(w, t, defaultCarParams(), r);
        }
        CHECK_MSG(c.deaths == (proto ? 1u : 0u), "prototype %d: %u deaths", proto, c.deaths);
    }
}

// ---------------------------------------------------------------------------
// Reynolds bots
// ---------------------------------------------------------------------------

TEST(bot_behaviours_containment_separation_evasion)
{
    Track t(14, allStraights());
    const TrackSection &sec = t.section(1);
    // Containment: heading for the right barrier => feelers push left.
    {
        CarSimState c = spawnCar(t, 1, 10.0f, 2.5f, 18.0f);
        c.velocity += sec.rightAt(10.0f) * 6.0f;
        BotDriver b;
        b.drive(c, t, defaultCarParams(), 0);
        CHECK_MSG(dm::dot(b.debug.containment, sec.rightAt(10.0f)) < -1.0f, "containment %.2f",
                  dm::dot(b.debug.containment, sec.rightAt(10.0f)));
    }
    // Separation: a car right alongside => pushed away from it.
    {
        CarSimState me = spawnCar(t, 1, 20.0f, -0.8f, 18.0f), other = spawnCar(t, 1, 20.0f, 1.0f, 18.0f);
        BotDriver b;
        b.drive(me, t, defaultCarParams(), 0, {{2, &other}});
        CHECK(dm::dot(b.debug.separation, sec.rightAt(20.0f)) < -0.5f);
    }
    // Evasion: a car closing fast from behind in our lane => flee sideways.
    {
        CarSimState me = spawnCar(t, 1, 30.0f, 0.3f, 14.0f), rammer = spawnCar(t, 1, 22.0f, 0.0f, 27.0f);
        BotDriver b;
        b.profile = botPreset(BotPreset::Racer);
        b.drive(me, t, defaultCarParams(), 0, {{2, &rammer}});
        CHECK(b.debug.evading);
        CHECK_MSG(dm::dot(b.debug.evasion, sec.rightAt(30.0f)) > 1.0f, "evasion %.2f",
                  dm::dot(b.debug.evasion, sec.rightAt(30.0f))); // we are right of it: go right
    }
}

TEST(bot_bully_hunts_for_takedowns_racer_overtakes)
{
    // A slow car (coasting, no throttle) ahead on a straight. The Bully
    // pursues and rams it (takedown); the Racer goes round it cleanly.
    for (BotPreset preset : {BotPreset::Bully, BotPreset::Racer})
    {
        Track t(21, allStraights());
        CarSimState hunter = spawnCar(t, 1, 5.0f, 0.0f, 18.0f), slow = spawnCar(t, 1, 30.0f, 0.5f, 10.0f);
        hunter.nitro = 1.0f;
        BotDriver b;
        b.profile = botPreset(preset);
        bool pursued = false, overtook = false;
        for (int i = 0; i < 60 * 6; ++i)
        {
            t.ensureGenerated(hunter.section + 12);
            net::CarInput in = b.drive(hunter, t, defaultCarParams(), i, {{1, &hunter}, {2, &slow}});
            pursued |= b.debug.pursuitTarget == 2;
            overtook |= b.debug.overtaking;
            std::vector<WorldCar> w{{1, &hunter, in, false}, {2, &slow, holdInput(i), false}};
            stepWorld(w, t, defaultCarParams(), defaultRaceParams());
        }
        std::printf("    %s: pursued %d, overtook %d, takedowns %u, own wrecks %u, ahead by %.1f m\n",
                    botPresetName(preset), pursued, overtook, hunter.takedowns, hunter.deaths, hunter.distance - slow.distance);
        if (preset == BotPreset::Bully)
            CHECK(pursued && hunter.takedowns >= 1 && slow.deaths >= 1);
        else
            CHECK(!pursued && overtook && hunter.takedowns == 0 && slow.deaths == 0 && hunter.distance > slow.distance);
        CHECK(hunter.deaths == 0);
    }
}

TEST(bot_reynolds_beats_the_old_planner_and_presets_differ)
{
    // Same seeds, 60 s solo: the new Racer wrecks no more than the frozen
    // old bot and covers clearly more road; the Rookie is the crash-prone one.
    double oldM = 0, newM = 0;
    uint32_t oldW = 0, newW = 0, rookieW = 0;
    for (uint32_t seed : {1000u, 1017u, 1034u})
    {
        Track t1(seed), t2(seed), t3(seed);
        CarSimState a = spawnOnGrid(t1, 0), b = spawnOnGrid(t2, 0), c = spawnOnGrid(t3, 0);
        bench::OldBot ob;
        BotDriver nb, rb;
        nb.profile = botPreset(BotPreset::Racer);
        rb.profile = botPreset(BotPreset::Rookie);
        for (int i = 0; i < 60 * 60; ++i)
        {
            t1.update(a.section);
            t2.update(b.section);
            t3.update(c.section);
            stepCar(a, ob.drive(a, t1, defaultCarParams(), i), t1);
            stepCar(b, nb.drive(b, t2, defaultCarParams(), i), t2);
            stepCar(c, rb.drive(c, t3, defaultCarParams(), i), t3);
        }
        oldM += a.distance;
        newM += b.distance;
        oldW += a.deaths;
        newW += b.deaths;
        rookieW += c.deaths;
    }
    std::printf("    3 x 60 s: old %.0f m / %u wrecks, Reynolds Racer %.0f m / %u wrecks, Rookie %u wrecks\n", oldM, oldW,
                newM, newW, rookieW);
    CHECK(newW <= oldW);
    CHECK_MSG(newM > 1.1 * oldM, "new %.0f m vs old %.0f m", newM, oldM);
    CHECK(rookieW > newW);
}

TEST(bot_nitro_burns_when_controllable_full_gauge_and_rams)
{
    // Full gauge on an open straight: every preset burns soon.
    for (BotPreset preset : {BotPreset::Rookie, BotPreset::Racer, BotPreset::Bully, BotPreset::Ace})
    {
        Track t(22, allStraights());
        CarSimState c = spawnCar(t, 1, 5.0f, 0.0f, 12.0f);
        c.nitro = 1.0f;
        BotDriver b;
        b.profile = botPreset(preset);
        int firstBurn = -1;
        for (int i = 0; i < 120 && firstBurn < 0; ++i)
        {
            t.ensureGenerated(c.section + 12);
            stepCar(c, b.drive(c, t, defaultCarParams(), i), t);
            if (c.nitroActive) firstBurn = i;
        }
        std::printf("    %s: full gauge on a straight, first burn after %d ticks\n", botPresetName(preset), firstBurn);
        CHECK(firstBurn >= 0 && firstBurn < 30);
    }
    // Ram: a Bully behind a slow car, half a gauge (no full-gauge reason to
    // burn) -- it burns while it hunts, and gets its takedown.
    {
        Track t(21, allStraights());
        CarSimState hunter = spawnCar(t, 1, 5.0f, 0.0f, 14.0f), slow = spawnCar(t, 1, 35.0f, 0.5f, 10.0f);
        hunter.nitro = 0.5f;
        BotDriver b;
        b.profile = botPreset(BotPreset::Bully);
        int huntBurn = 0;
        for (int i = 0; i < 60 * 4; ++i)
        {
            t.ensureGenerated(hunter.section + 12);
            net::CarInput in = b.drive(hunter, t, defaultCarParams(), i, {{1, &hunter}, {2, &slow}});
            huntBurn += b.debug.pursuitTarget == 2 && in.nitro;
            std::vector<WorldCar> w{{1, &hunter, in, false}, {2, &slow, holdInput(i), false}};
            stepWorld(w, t, defaultCarParams(), defaultRaceParams());
        }
        std::printf("    Bully ram: %d ticks on nitro while hunting, takedowns %u, own wrecks %u\n", huntBurn,
                    hunter.takedowns, hunter.deaths);
        CHECK(huntBurn > 10 && hunter.takedowns >= 1 && hunter.deaths == 0);
    }
    // Real tracks, 60 s solo: Racer and Ace sit on a full gauge far less
    // than they did (~70-80% of the time before), use nitro, and still don't
    // wreck.
    for (BotPreset preset : {BotPreset::Racer, BotPreset::Ace})
    {
        int full = 0, burn = 0, ticks = 0;
        uint32_t wrecks = 0;
        for (uint32_t seed : {1000u, 1017u})
        {
            Track t(seed);
            CarSimState c = spawnOnGrid(t, 0);
            BotDriver b;
            b.profile = botPreset(preset);
            for (int i = 0; i < 60 * 60; ++i)
            {
                t.update(c.section);
                t.ensureGenerated(c.section + 30);
                full += c.alive && c.nitro >= 0.999f;
                stepCar(c, b.drive(c, t, defaultCarParams(), i), t);
                burn += c.nitroActive;
                ++ticks;
            }
            wrecks += c.deaths;
        }
        std::printf("    %s 2 x 60 s: full gauge %.0f%% of the time, burning %.0f%%, %u wrecks\n", botPresetName(preset),
                    100.0 * full / ticks, 100.0 * burn / ticks, wrecks);
        CHECK(full < ticks * 6 / 10 && burn > ticks / 25 && wrecks == 0);
    }
}

TEST(bot_pack_race_is_deterministic)
{
    auto run = []() {
        Track t(77);
        std::vector<CarSimState> cars;
        BotDriver bots[4];
        const BotPreset ps[4] = {BotPreset::Rookie, BotPreset::Racer, BotPreset::Bully, BotPreset::Ace};
        for (int i = 0; i < 4; ++i)
        {
            cars.push_back(spawnOnGrid(t, i));
            bots[i].profile = botPreset(ps[i]);
            bots[i].seed = 100u + static_cast<uint32_t>(i);
        }
        uint32_t takedowns = 0;
        for (int k = 0; k < 60 * 30; ++k)
        {
            uint32_t maxSec = 0;
            for (const CarSimState &c : cars) maxSec = std::max(maxSec, c.section);
            t.ensureGenerated(maxSec + 14);
            std::vector<BotOpponent> field;
            for (int i = 0; i < 4; ++i) field.push_back({i + 1, &cars[i]});
            std::vector<WorldCar> w;
            for (int i = 0; i < 4; ++i) w.push_back({i + 1, &cars[i], bots[i].drive(cars[i], t, defaultCarParams(), k, field), false});
            stepWorld(w, t, defaultCarParams(), defaultRaceParams());
        }
        uint64_t h = 1469598103934665603ull;
        for (const CarSimState &c : cars) { h = (h ^ hashState(c)) * 1099511628211ull; takedowns += c.takedowns; }
        return std::make_pair(h, takedowns);
    };
    auto a = run(), b = run();
    std::printf("    30 s, Rookie/Racer/Bully/Ace: %u takedowns, hash %016llx\n", a.second,
                static_cast<unsigned long long>(a.first));
    CHECK(a.first == b.first);
}

TEST(session_bot_profiles_and_takedown_events)
{
    LoopbackTransport lb(1);
    SessionConfig cfg;
    cfg.countdownSeconds = 0.5f;
    MultiplayerSession s(lb, cfg);
    s.hostStartMatch(99, {1}, {2, 3, 4});
    s.setBotProfile(2, botPreset(BotPreset::Bully));
    s.setBotProfile(3, botPreset(BotPreset::Ace));
    CHECK(s.botProfile(2) && std::strcmp(s.botProfile(2)->name, "Bully") == 0);
    CHECK(s.botProfile(3) && std::strcmp(s.botProfile(3)->name, "Ace") == 0);
    CHECK(s.botProfile(1) == nullptr); // the local player is not a bot
    // The local car parks on the grid (no throttle): the bots come past it.
    int takedownEvents = 0, wreckEvents = 0;
    for (int i = 0; i < 60 * 20 && s.active(); ++i)
    {
        s.tick(holdInput(i));
        for (const ContactEvent &e : s.takeContactEvents())
        {
            takedownEvents += e.kind == ContactEvent::Takedown;
            wreckEvents += e.kind == ContactEvent::Wreck;
        }
    }
    std::printf("    20 s offline with presets: %d takedown events, %d wreck events\n", takedownEvents, wreckEvents);
    CHECK(takedownEvents <= wreckEvents); // every takedown has a wrecked victim
}

// ---------------------------------------------------------------------------
// elimination race
// ---------------------------------------------------------------------------

TEST(elimination_stage_sequence)
{
    EliminationRace race;
    race.start({1, 2, 3});
    std::vector<float> stagesSeen{race.timer()};
    // Cars 2 and 3 swap last place every 0.5 s by a clear margin.
    float d1 = 100, d2 = 50, d3 = 40;
    for (int i = 0; i < 60 * 10; ++i)
    {
        if (i % 30 == 0) std::swap(d2, d3);
        auto ev = race.update({{1, d1}, {2, d2}, {3, d3}}, kFixedDt);
        for (const auto &e : ev)
            if (e.kind == EliminationRace::Event::LastPlaceChanged) stagesSeen.push_back(race.stageTime(e.stage));
        if (race.racingCount() < 3) break;
    }
    std::printf("    stage times on each change of last place:");
    for (float s : stagesSeen) std::printf(" %.0f", s);
    std::printf("\n");
    const float expect[] = {32, 16, 8, 4, 2, 1, 1};
    CHECK(stagesSeen.size() >= 7);
    for (size_t i = 0; i < 7 && i < stagesSeen.size(); ++i) CHECK(stagesSeen[i] == expect[i]);
    // Swapping every 0.5 s outlasts the 1 s stage... until the clock wins:
    // someone is eliminated and the clock resets to 32 s.
    EliminationRace r2;
    r2.start({1, 2, 3});
    bool eliminated = false;
    for (int i = 0; i < 60 * 60 && !eliminated; ++i)
    {
        auto ev = r2.update({{1, 100}, {2, 50}, {3, 40}}, kFixedDt);
        for (const auto &e : ev) eliminated = eliminated || e.kind == EliminationRace::Event::Eliminated;
    }
    CHECK(eliminated && r2.isEliminated(3) && r2.stage() == 0 && std::fabs(r2.timer() - 32.0f) < 1e-3f);
    // Small wobbles inside the overtake margin don't advance the stage.
    EliminationRace r3;
    r3.start({1, 2});
    for (int i = 0; i < 120; ++i) r3.update({{1, 50.0f + (i % 2 ? 0.5f : -0.5f)}, {2, 50.0f}}, kFixedDt);
    CHECK(r3.stage() == 0);
}

TEST(elimination_order_and_winner)
{
    EliminationRace race;
    race.start({4, 7, 9, 12});
    // Fixed order: 9 leads, then 4, 12, 7 last. Nobody overtakes.
    std::vector<int32_t> eliminatedIds;
    int32_t winner = 0;
    float t = 0;
    for (int i = 0; i < 60 * 200 && !race.finished(); ++i)
    {
        t += kFixedDt;
        for (const auto &e : race.update({{9, 400}, {4, 300}, {12, 200}, {7, 100}}, kFixedDt))
        {
            if (e.kind == EliminationRace::Event::Eliminated) eliminatedIds.push_back(e.playerId);
            if (e.kind == EliminationRace::Event::Winner) winner = e.playerId;
        }
    }
    CHECK(eliminatedIds == std::vector<int32_t>({7, 12, 4}));
    CHECK(winner == 9 && race.winnerId() == 9 && race.finished());
    CHECK_MSG(std::fabs(t - 96.0f) < 0.1f, "three 32 s clocks, finished at %.2f s", t);
    CHECK(race.finishingOrder() == std::vector<int32_t>({9, 4, 12, 7}));
    CHECK(race.eliminatedOrder(7) == 1 && race.eliminatedOrder(12) == 2 && race.eliminatedOrder(4) == 3);
    CHECK(race.place(9) == 1 && race.place(7) == 0);
}

TEST(rubber_band_recharge_multipliers)
{
    EliminationRace race;
    race.start({1, 2, 3, 4});
    race.update({{1, 400}, {2, 300}, {3, 250}, {4, 100}}, kFixedDt);
    std::printf("    4 cars at 400/300/250/100 m: x%.2f x%.2f x%.2f x%.2f\n", race.rechargeMultiplierFor(1),
                race.rechargeMultiplierFor(2), race.rechargeMultiplierFor(3), race.rechargeMultiplierFor(4));
    CHECK(race.rechargeMultiplierFor(1) == 0.5f);
    CHECK(race.rechargeMultiplierFor(4) == 2.0f);
    CHECK(std::fabs(race.rechargeMultiplierFor(2) - 1.0f) < 1e-4f);  // 1/3 of the way back
    CHECK(std::fabs(race.rechargeMultiplierFor(3) - 1.25f) < 1e-4f); // 1/2 of the way back
    // Two cars left: simply 0.5 / 2.0, however close.
    EliminationRace two;
    two.start({1, 2});
    two.update({{1, 101}, {2, 100}}, kFixedDt);
    CHECK(two.rechargeMultiplierFor(1) == 0.5f && two.rechargeMultiplierFor(2) == 2.0f);
}

TEST(offline_race_with_bots_runs_to_a_winner)
{
    LoopbackTransport lb(1);
    SessionConfig cfg;
    cfg.resultsSeconds = 1.0f;
    MultiplayerSession s(lb, cfg);
    s.hostStartMatch(2024, {1}, {2, 3, 4});
    BotDriver me;
    float spreadSum = 0;
    int n = 0, nitroTicks = 0, slipTicks = 0;
    bool sawResults = false;
    for (int i = 0; i < 60 * 300 && s.active(); ++i)
    {
        s.tick(me.drive(*s.localCar(), s.track(), s.params(), i));
        if (s.phase() == net::MatchPhase::Results) sawResults = true;
        if (s.phase() != net::MatchPhase::Racing) continue;
        float lo = 1e9f, hi = -1e9f;
        for (const CarView &v : s.carViews(1))
        {
            if (v.eliminated) continue;
            lo = std::fmin(lo, v.distance);
            hi = std::fmax(hi, v.distance);
            nitroTicks += v.nitroActive;
            slipTicks += v.slipstream;
        }
        spreadSum += hi - lo;
        ++n;
    }
    const RaceInfo &ri = s.raceInfo();
    std::printf("    winner %d, order", ri.winnerId);
    for (int32_t id : ri.finishingOrder) std::printf(" %d", id);
    std::printf(", racing %.0f s, mean spread %.1f m, nitro %d car-ticks, slipstream %d car-ticks, wrecks/takedowns",
                n / 60.0f, spreadSum / n, nitroTicks, slipTicks);
    for (int id = 1; id <= 4; ++id)
        if (const CarSimState *c = s.hostCar(id)) std::printf(" %u/%u", c->deaths, c->takedowns);
    std::printf("\n");
    CHECK(sawResults && !s.active());
    CHECK(ri.winnerId != 0 && ri.finishingOrder.size() == 4 && ri.finishingOrder.front() == ri.winnerId);
    CHECK(nitroTicks > 0 && slipTicks > 0);
    // (Was 60 m before takedowns: a takedown costs its victim ~30 m of road.)
    CHECK_MSG(spreadSum / n < 90.0f, "mean spread %.1f m", spreadSum / n);
}

// ---------------------------------------------------------------------------
// robustness: spread field, nitro into walls, soak
// ---------------------------------------------------------------------------

TEST(track_spread_field_never_queries_missing_sections)
{
    // The leader ~22 sections ahead of the last car. Sections are never
    // destroyed (the "rear buffer" only drops *meshes*), so queries for the
    // trailing car stay exact however far the leader has generated.
    Track t(4242);
    std::vector<CarSimState> cars;
    for (uint32_t sec : {1u, 7u, 14u, 23u}) cars.push_back(spawnCar(t, sec, 3.0f, 0.0f, 12.0f));
    BotDriver bots[4];
    TrackSection before = t.section(1);
    bool ok = true;
    uint32_t rescues = 0;
    for (int k = 0; k < 60 * 20; ++k)
    {
        uint32_t lo = ~0u, hi = 0;
        for (const CarSimState &c : cars) { lo = std::min(lo, c.section); hi = std::max(hi, c.section); }
        t.ensureGenerated(hi + 12);
        t.update(hi); // the leader moves the live window
        std::vector<WorldCar> w;
        for (int i = 0; i < 4; ++i) w.push_back({i + 1, &cars[i], bots[i].drive(cars[i], t, defaultCarParams(), k), false});
        stepWorld(w, t, defaultCarParams(), defaultRaceParams());
        for (const CarSimState &c : cars)
        {
            ok = ok && c.section + 1 < t.sectionCount();
            RoadQuery q = t.query(c.position, c.section);
            ok = ok && q.section == c.section && std::fabs(q.s - c.sectionS) < 1e-3f;
        }
        (void)lo;
    }
    for (const CarSimState &c : cars) rescues += c.rescues;
    CHECK(ok);
    CHECK(sectionsIdentical(before, t.section(1)) && t.firstLiveSection() > 1);
    CHECK(rescues == 0);
}

TEST(nitro_at_top_speed_into_walls_never_goes_under_the_road)
{
    const CarParams &p = defaultCarParams();
    Track t(31337);
    t.ensureGenerated(40);
    int runs = 0, wrecks = 0;
    float worst = 0;
    uint32_t rescues = 0;
    // Straights and hairpins, angles from a graze to head-on, both sides.
    for (uint32_t sec = 1; sec < 30; sec += 3)
        for (float angle : {10.0f, 30.0f, 60.0f, 90.0f})
            for (int side = -1; side <= 1; side += 2)
            {
                const TrackSection &ts = t.section(sec);
                CarSimState s = spawnCar(t, sec, ts.length * 0.4f, 0.0f, 0.0f);
                s.nitro = 1.0f;
                float yaw = ts.yawAt(ts.length * 0.4f) - side * dm::radians(angle);
                s.rotation = dm::quatYaw(yaw);
                s.velocity = dm::forwardOf(s.rotation) * 32.0f; // above nitro top speed
                for (int i = 0; i < 120; ++i)
                {
                    stepCar(s, driveInput(i, side * 1.0f, true), t, p);
                    if (s.alive)
                    {
                        RoadQuery q = t.query(s.position, s.section);
                        worst = std::fmax(worst, q.height - s.position.y);
                    }
                }
                wrecks += s.deaths > 0;
                rescues += s.rescues;
                ++runs;
            }
    std::printf("    %d nitro runs into barriers: %d wrecked (hard hits), deepest body below road %.2f m, rescues %u\n",
                runs, wrecks, worst, rescues);
    CHECK(rescues == 0);
    CHECK_MSG(worst < 0.5f, "body %.2f m under the road", worst);
}

TEST(soak_bot_races_no_nan_no_fall_through)
{
    // DRIFTO_SOAK=<races per seed> for the long version (e.g. 20).
    int racesPerSeed = std::getenv("DRIFTO_SOAK") ? std::atoi(std::getenv("DRIFTO_SOAK")) : 2;
    int races = 0, finished = 0, nonFinite = 0, underRoad = 0;
    uint32_t rescues = 0, wrecks = 0;
    double simSeconds = 0;
    for (uint32_t seedBase : {11u, 222u, 3333u})
        for (int r = 0; r < racesPerSeed; ++r)
        {
            LoopbackTransport lb(1);
            SessionConfig cfg;
            cfg.resultsSeconds = 0.5f;
            MultiplayerSession s(lb, cfg);
            s.hostStartMatch(seedBase * 7919u + r, {}, {1, 2, 3, 4}); // all four are bots
            BotDriver me;
            Pcg32 rng(seedBase, r);
            int k = 0;
            for (; k < 60 * 400 && s.active(); ++k)
            {
                net::CarInput in = me.drive(*s.localCar(), s.track(), s.params(), k);
                if (rng.nextFloat() < 0.02f) in.steer = rng.range(-1.0f, 1.0f); // random jolts
                s.tick(in);
                for (int id = 1; id <= 4; ++id)
                {
                    const CarSimState *c = s.hostCar(id);
                    if (!c) continue;
                    bool finite = std::isfinite(c->position.x) && std::isfinite(c->position.y) &&
                                  std::isfinite(c->position.z) && std::isfinite(c->velocity.x) &&
                                  std::isfinite(c->rotation.w) && std::isfinite(c->nitro);
                    nonFinite += !finite;
                    if (finite && c->alive && s.phase() == net::MatchPhase::Racing)
                        underRoad += c->position.y < s.track().query(c->position, c->section).height - 0.5f;
                }
            }
            simSeconds += k / 60.0;
            ++races;
            finished += s.raceInfo().winnerId != 0;
            for (int id = 1; id <= 4; ++id)
                if (const CarSimState *c = s.hostCar(id)) { rescues += c->rescues; wrecks += c->deaths; }
        }
    std::printf("    %d races (%.0f s simulated): %d with a winner, %u wrecks, %u rescues, %d non-finite, %d under-road samples\n",
                races, simSeconds, finished, wrecks, rescues, nonFinite, underRoad);
    CHECK(finished == races);
    CHECK(nonFinite == 0 && underRoad == 0 && rescues == 0);
}

// ---------------------------------------------------------------------------
// multiplayer session with the fake transport
// ---------------------------------------------------------------------------

namespace {

// Netcode tests want long races: an elimination clock that never runs out.
RaceParams noClock()
{
    RaceParams r;
    for (float &t : r.stageTimes) t = 1e6f;
    return r;
}

struct NetRig
{
    gametest::FakeHub hub;
    gametest::FakeEndpoint hostEp{hub, 1};
    gametest::FakeEndpoint clientEp{hub, 2};
    MultiplayerSession host;
    MultiplayerSession client;
    BotDriver hostBot, clientBot;
    std::vector<dm::Vec3> hostCarHistory; // host's own car per host tick
    uint32_t tick = 0;

    explicit NetRig(const SessionConfig &cfg, const RaceParams &race = noClock())
        : host(hostEp, cfg, defaultCarParams(), race), client(clientEp, cfg, defaultCarParams(), race)
    {
        hostCarHistory.push_back({});
        hostBot.laneBias = -1.5f;
        clientBot.laneBias = 1.5f;
        host.hostStartMatch(31337, {1, 2});
        client.beginMatch(hostEp.startedSeed, {1, 2}, false);
    }

    std::function<net::CarInput(int who, net::CarInput in)> tweak; // optional input override

    void step()
    {
        ++tick;
        hub.advance();
        net::CarInput hin = hostBot.drive(*host.localCar(), host.track(), host.params(), tick);
        if (tweak) hin = tweak(1, hin);
        host.tick(hin);
        if (host.active()) hostCarHistory.push_back(host.localCar()->position);
        net::CarInput cin = clientBot.drive(*client.localCar(), client.track(), client.params(), tick);
        if (tweak) cin = tweak(2, cin);
        client.tick(cin);
    }
};

} // namespace

TEST(session_prediction_exact_without_loss)
{
    SessionConfig cfg;
    cfg.countdownSeconds = 1.0f;
    NetRig rig(cfg);
    rig.hub.latencyTicks = 5; // 83 ms one way
    for (int i = 0; i < 60 * 4; ++i) rig.step();
    CHECK(rig.client.phase() == net::MatchPhase::Racing);
    rig.client.resetStats();
    for (int i = 0; i < 60 * 25; ++i) rig.step();
    const auto &st = rig.client.stats();
    double mean = st.reconciliations ? st.sumCorrection / st.reconciliations : 0;
    std::printf("    25 s racing @ 166 ms RTT: %u reconciliations, %u replayed ticks, mean corr %.4f m, max %.4f m\n",
                st.reconciliations, st.replayedTicks, mean, st.maxCorrection);
    CHECK(st.reconciliations > 600);
    CHECK(st.hardResets == 0);
    CHECK_MSG(mean < 0.05, "mean correction %.4f", mean);
    uint32_t lastApplied = 0;
    const CarSimState *auth = rig.host.hostCar(2, &lastApplied);
    const CarSimState *pred = rig.client.predictedStateAt(lastApplied);
    CHECK(auth && pred);
    CHECK(rig.client.localTick() >= lastApplied + 5);
}

TEST(session_reconciliation_converges_with_latency_jitter_loss)
{
    SessionConfig cfg;
    cfg.countdownSeconds = 1.0f;
    NetRig rig(cfg);
    rig.hub.latencyTicks = 6;
    rig.hub.jitterTicks = 4;
    rig.hub.lossRate = 0.15f;
    for (int i = 0; i < 60 * 3; ++i) rig.step();
    rig.client.resetStats();
    for (int i = 0; i < 60 * 25; ++i) rig.step();
    auto lossy = rig.client.stats();
    double mean = lossy.reconciliations ? lossy.sumCorrection / lossy.reconciliations : 0;
    std::printf("    25 s @ 100-166 ms one-way, 15%% loss: %u snapshots, mean corr %.4f m, max %.4f m, host starved %u\n",
                lossy.snapshotsReceived, mean, lossy.maxCorrection, rig.host.stats().hostStarvedTicks);
    CHECK(lossy.snapshotsReceived > 350);
    CHECK_MSG(mean < 0.5, "mean correction %.4f", mean);
    rig.hub.lossRate = 0;
    rig.hub.jitterTicks = 0;
    for (int i = 0; i < 60 * 2; ++i) rig.step();
    rig.client.resetStats();
    for (int i = 0; i < 60 * 2; ++i) rig.step();
    const auto &clean = rig.client.stats();
    double cleanMean = clean.reconciliations ? clean.sumCorrection / clean.reconciliations : 0;
    std::printf("    after healing: mean corr %.5f m, visual offset %.5f m\n", cleanMean, rig.client.visualCorrection());
    // Residual corrections on a clean link come from car-to-car contacts
    // predicted against dead-reckoned proxies (twisty track, close cars).
    CHECK_MSG(cleanMean < 0.15, "mean correction after healing %.5f", cleanMean);
    CHECK(rig.client.localCar()->alive || rig.client.localEliminated());
}

TEST(session_bumping_under_latency_with_proxies)
{
    // Host and client race side by side and keep leaning into each other at
    // 166 ms RTT. Compare client prediction with remote proxies (default)
    // against ignoring remote cars in prediction.
    struct Result { double meanCorr; float maxCorr; float maxJerk; int contacts; uint32_t wrecks; float maxOverlap; int overlapTicks; };
    auto runOnce = [](bool proxies) {
        SessionConfig cfg;
        cfg.countdownSeconds = 1.0f;
        cfg.remoteProxies = proxies;
        cfg.trackParams = allStraights();
        NetRig rig(cfg);
        rig.hub.latencyTicks = 5;
        // The car in front cruises on the centreline at 55 % throttle; the
        // one behind (full throttle, 0.7 m off-centre) keeps catching it and
        // rear-ending it off-centre, bouncing back, catching up again...
        rig.tweak = [&rig](int who, net::CarInput in) {
            const CarSimState *me = who == 1 ? rig.host.localCar() : rig.client.localCar();
            const Track &t = who == 1 ? rig.host.track() : rig.client.track();
            const CarSimState *other = rig.host.hostCar(who == 1 ? 2 : 1);
            const CarSimState *mineAuth = rig.host.hostCar(who);
            bool behind = mineAuth->distance < other->distance;
            AutopilotParams ap;
            ap.lookAhead = 14.0f;
            ap.lateral = behind ? 0.7f : 0.0f;
            net::CarInput out = autopilotInput(*me, t, ap, in.tick);
            out.throttle = behind ? 1.0f : 0.55f;
            return out;
        };
        for (int i = 0; i < 60 * 3; ++i) rig.step();
        rig.client.resetStats();
        dm::Vec3 p0, p1;
        float maxJerk = 0, maxOverlap = 0;
        int n = 0, contacts = 0, overlapTicks = 0;
        for (int i = 0; i < 60 * 20; ++i)
        {
            float spin1 = rig.host.hostCar(1)->spinRate, spin2 = rig.host.hostCar(2)->spinRate;
            rig.step();
            contacts += std::fabs(rig.host.hostCar(1)->spinRate) > std::fabs(spin1) + 0.05f ||
                        std::fabs(rig.host.hostCar(2)->spinRate) > std::fabs(spin2) + 0.05f;
            if (getenv("BUMPDBG") && i % 15 == 0) { const CarSimState *a = rig.host.hostCar(1), *b = rig.host.hostCar(2);
                std::printf("      t=%d  1: s%.1f d%.2f v%.1f   2: s%.1f d%.2f v%.1f spin %.2f\n", i, a->distance, a->lateral, a->speed, b->distance, b->lateral, b->speed, b->spinRate); }
            std::vector<CarView> views = rig.client.carViews(0.0f);
            dm::Vec3 p = views.front().position;
            // What the client player sees: its car vs the drawn remote car.
            for (const CarView &v : views)
                if (!v.local)
                {
                    float pen = penetration(views.front().position, views.front().rotation, v.position, v.rotation);
                    maxOverlap = std::fmax(maxOverlap, pen);
                    overlapTicks += pen > 0.1f;
                }
            if (n >= 1 && dm::length(p - p1) > 3.0f) n = 0; // respawn teleport, not jitter
            if (rig.client.phase() != net::MatchPhase::Racing) break; // Results freezes the cars
            if (n >= 2) maxJerk = std::fmax(maxJerk, dm::length(p - p1 * 2.0f + p0)); // 2nd difference per tick
            p0 = p1;
            p1 = p;
            ++n;
        }
        const auto &st = rig.client.stats();
        return Result{st.reconciliations ? st.sumCorrection / st.reconciliations : 0, st.maxCorrection, maxJerk,
                      contacts, rig.client.localCar()->deaths, maxOverlap, overlapTicks};
    };
    Result on = runOnce(true), off = runOnce(false);
    for (const Result *r : {&on, &off})
        std::printf("    %s %d host contacts; client sees cars overlap >10 cm on %d ticks (max %.2f m); "
                    "corrections mean %.3f / max %.3f m; render jerk max %.3f m\n",
                    r == &on ? "with proxies:   " : "without proxies:", r->contacts, r->overlapTicks, r->maxOverlap,
                    r->meanCorr, r->maxCorr, r->maxJerk);
    CHECK(on.contacts > 10 && off.contacts > 10);
    CHECK(on.maxOverlap < off.maxOverlap && on.overlapTicks < off.overlapTicks);
    CHECK_MSG(on.maxOverlap < 0.3f, "visible overlap %.2f m", on.maxOverlap);
    CHECK(on.meanCorr < 0.05 && on.maxCorr < 0.5f);
    CHECK_MSG(on.maxJerk < 0.3f, "render jerk %.3f", on.maxJerk);
}

TEST(session_contact_events_once_each)
{
    // The rear-ending scenario: the client's own-car bump events come from
    // its newest predicted tick only, so replays never re-report a contact;
    // their count tracks what the host simulated for that car.
    SessionConfig cfg;
    cfg.countdownSeconds = 1.0f;
    cfg.trackParams = allStraights();
    NetRig rig(cfg);
    rig.hub.latencyTicks = 5;
    // Host cruises at 70 % throttle in the middle; the client alternates 2 s
    // of full throttle (catches up, rams) with 2 s at 40 % (drops back).
    rig.tweak = [&rig](int who, net::CarInput in) {
        const CarSimState *me = who == 1 ? rig.host.localCar() : rig.client.localCar();
        const Track &t = who == 1 ? rig.host.track() : rig.client.track();
        AutopilotParams ap;
        ap.lookAhead = 14.0f;
        ap.lateral = who == 1 ? 0.0f : 0.4f * kCarScale;
        net::CarInput out = autopilotInput(*me, t, ap, in.tick);
        out.throttle = who == 1 ? 0.7f : ((rig.tick / 120) % 2 == 0 ? 1.0f : 0.4f);
        return out;
    };
    for (int i = 0; i < 60 * 3; ++i) rig.step();
    rig.host.takeContactEvents();
    rig.client.takeContactEvents();
    int hostBumps = 0, clientOwnBumps = 0, clientRemote = 0, sameTickDupes = 0;
    for (int i = 0; i < 60 * 20; ++i)
    {
        rig.step();
        for (const ContactEvent &e : rig.host.takeContactEvents())
            hostBumps += e.kind == ContactEvent::Bump && e.playerId == 2;
        int ownThisTick = 0;
        for (const ContactEvent &e : rig.client.takeContactEvents())
        {
            if (e.playerId == 2 && e.kind == ContactEvent::Bump) { ++clientOwnBumps; ++ownThisTick; }
            if (e.playerId != 2) ++clientRemote;
        }
        sameTickDupes += ownThisTick > 1;
    }
    std::printf("    20 s of bumping: host saw %d bump ticks on the client's car, client predicted %d (+%d remote events)\n",
                hostBumps, clientOwnBumps, clientRemote);
    CHECK(hostBumps > 10);
    CHECK(sameTickDupes == 0);
    CHECK_MSG(clientOwnBumps > hostBumps / 2 && clientOwnBumps < hostBumps * 2, "client %d vs host %d", clientOwnBumps, hostBumps);
}

TEST(session_remote_cars_drawn_at_the_present)
{
    // Default (extrapolated): the client draws the host's car close to where
    // it is *now*. Interpolation mode: ~latency + 100 ms behind.
    for (bool extrapolate : {true, false})
    {
        SessionConfig cfg;
        cfg.countdownSeconds = 1.0f;
        cfg.extrapolateRemotes = extrapolate;
        NetRig rig(cfg);
        rig.hub.latencyTicks = 5;
        rig.hub.lossRate = 0.05f;
        for (int i = 0; i < 60 * 12; ++i) rig.step();
        const CarView *remote = nullptr;
        std::vector<CarView> views = rig.client.carViews(0.0f);
        for (const CarView &v : views)
            if (v.playerId == 1) remote = &v;
        CHECK(remote != nullptr);
        if (!remote) continue;
        float best = 1e9f;
        int bestLag = -1;
        size_t n = rig.hostCarHistory.size();
        for (int lag = 0; lag < 30 && lag < static_cast<int>(n); ++lag)
        {
            float e = dm::length(rig.hostCarHistory[n - 1 - lag] - remote->position);
            if (e < best) { best = e; bestLag = lag; }
        }
        std::printf("    %s: remote car matches the host's car from %d ticks ago (err %.2f m)\n",
                    extrapolate ? "extrapolated" : "interpolated", bestLag, best);
        if (extrapolate) CHECK(bestLag <= 6 && best < 3.0f); // dead reckoning through bends: < a car length
        else CHECK(bestLag >= 6 && bestLag <= 18 && best < 0.6f);
    }
}

TEST(session_phases_flow_and_host_migration)
{
    SessionConfig cfg;
    cfg.countdownSeconds = 1.0f;
    cfg.raceSeconds = 6.0f; // cap: the 32 s clock won't finish first
    cfg.resultsSeconds = 1.0f;
    NetRig rig(cfg, defaultRaceParams());
    rig.hub.latencyTicks = 4;
    CHECK(rig.hostEp.matchesStarted == 1);
    CHECK(!rig.hostEp.phases.empty() && rig.hostEp.phases[0] == net::MatchPhase::Countdown);
    for (int i = 0; i < 50; ++i) rig.step();
    CHECK(rig.host.phase() == net::MatchPhase::Countdown);
    CHECK(rig.client.localCar()->speed < 0.5f);
    CHECK(!rig.client.raceInfo().clockRunning);
    for (int i = 0; i < 30; ++i) rig.step();
    CHECK(rig.host.phase() == net::MatchPhase::Racing && rig.client.phase() == net::MatchPhase::Racing);
    for (int i = 0; i < 60; ++i) rig.step();
    CHECK(rig.client.raceInfo().clockRunning && rig.client.raceInfo().timer < 32.0f &&
          rig.client.raceInfo().timer > 29.0f);
    CHECK(rig.client.raceInfo().lastPlaceId != 0);

    // Host leaves mid-race: the client becomes host and carries on.
    for (int i = 0; i < 60; ++i) rig.step();
    float timerBefore = rig.client.raceInfo().timer;
    uint8_t stageBefore = rig.client.raceInfo().stage;
    float distBefore = rig.client.localCar()->distance;
    rig.hub.hostId = 2;
    rig.client.onHostChanged(true);
    CHECK(rig.client.isHost());
    BotDriver b;
    rig.hub.advance();
    rig.client.tick(b.drive(*rig.client.localCar(), rig.client.track(), rig.client.params(), 0));
    // The race clock carries over (one tick later).
    CHECK_MSG(rig.client.raceInfo().stage == stageBefore && std::fabs(timerBefore - kFixedDt - rig.client.raceInfo().timer) < 0.05f,
              "clock %.3f (stage %d) -> %.3f (stage %d)", timerBefore, stageBefore, rig.client.raceInfo().timer,
              rig.client.raceInfo().stage);
    for (int i = 0; i < 90; ++i)
    {
        rig.hub.advance();
        rig.client.tick(b.drive(*rig.client.localCar(), rig.client.track(), rig.client.params(), i));
    }
    CHECK(rig.client.phase() == net::MatchPhase::Racing);
    CHECK_MSG(rig.client.localCar()->distance > distBefore + 15.0f, "distance %.1f -> %.1f", distBefore,
              rig.client.localCar()->distance);
    CHECK(rig.clientEp.snapshotsPublished > 40);
    // The old host leaves the room: out of the race => the client wins.
    rig.client.onRosterChanged({2});
    for (int i = 0; i < 60 * 8 && rig.client.active(); ++i)
        rig.client.tick(b.drive(*rig.client.localCar(), rig.client.track(), rig.client.params(), i));
    CHECK(rig.client.raceInfo().winnerId == 2);
    CHECK(!rig.client.active());
    std::vector<net::MatchPhase> &ph = rig.clientEp.phases;
    CHECK(ph.size() == 2 && ph[0] == net::MatchPhase::Results && ph[1] == net::MatchPhase::Lobby);
}

TEST(session_car_choice_reaches_every_peer)
{
    // The cosmetic car pick rides on the inputs (CarInput::carModel) to the
    // host, which republishes it in the snapshot flags (kCarModelRx7).
    // Picks are the opposite of the slot defaults (look parity: id 1 AE86,
    // id 2 RX-7) so a default can't pass for a delivered choice.
    SessionConfig cfg;
    cfg.countdownSeconds = 1.0f;
    NetRig rig(cfg);
    rig.hub.latencyTicks = 4;
    rig.host.setLocalCarModel(1);   // host: RX-7
    rig.client.setLocalCarModel(0); // client: AE86
    auto modelIn = [](const MultiplayerSession &s, int32_t id) {
        for (const CarView &v : s.carViews(0.0f))
            if (v.playerId == id) return v.model;
        return -1;
    };
    for (int i = 0; i < 30; ++i) rig.step(); // still the countdown
    CHECK(rig.host.phase() == net::MatchPhase::Countdown);
    CHECK(modelIn(rig.host, 1) == 1 && modelIn(rig.host, 2) == 0);
    CHECK(modelIn(rig.client, 2) == 0 && modelIn(rig.client, 1) == 1);
    CHECK(rig.client.carModelOf(1) == 1 && rig.host.carModelOf(2) == 0);
    // Changed mid-race: everyone follows within a round trip.
    for (int i = 0; i < 60; ++i) rig.step();
    rig.client.resetStats();
    rig.client.setLocalCarModel(1);
    rig.host.setLocalCarModel(0);
    for (int i = 0; i < 20; ++i) rig.step();
    CHECK(modelIn(rig.host, 2) == 1 && modelIn(rig.client, 1) == 0);
    // Purely cosmetic: the client's prediction still matches the host.
    CHECK(rig.client.stats().hardResets == 0 && rig.client.stats().reconciliations > 5 &&
          rig.client.stats().maxCorrection < 0.05f);
    // Host migration: the new host keeps everyone's car.
    rig.hub.hostId = 2;
    rig.client.onHostChanged(true);
    CHECK(rig.client.carModelOf(1) == 0 && rig.client.carModelOf(2) == 1);

    // Offline: bots keep the slot default, the player's pick is honoured.
    LoopbackTransport lb(1);
    MultiplayerSession off(lb, cfg);
    off.setLocalCarModel(1);
    off.hostStartMatch(5, {1}, {2, 3, 4});
    CHECK(modelIn(off, 1) == 1 && modelIn(off, 2) == 1 && modelIn(off, 3) == 0 && modelIn(off, 4) == 1);
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const TestCase &tc : registry())
    {
        if (filter && !std::strstr(tc.name, filter)) continue;
        int before = g_failures;
        std::printf("[ RUN  ] %s\n", tc.name);
        tc.fn();
        std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", tc.name);
        ++run;
    }
    std::printf("\n%d tests, %d checks, %d failures\n", run, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
