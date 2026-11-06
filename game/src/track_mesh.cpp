// Purpose: turns the analytic Track into renderable geometry. Each section is
// built independently by buildSectionGeometry() into separate meshes (road,
// terrain, props such as guardrails) plus a list of tree placements, so the
// renderer can create and drop GPU meshes per section as the car advances.
// The geometry samples the same TrackSection functions the physics uses
// (pointAt, edgeAt, normalAt), so what is drawn matches what is driven on.
// All output is CPU-side gfx::MeshData with per-vertex colour; no GL calls
// happen here. Randomness (colour jitter, trees) is hashed from the section
// index and track seed, so every peer builds the same scenery.
// track_mesh.cpp -- procedural road / terrain / guardrail / tree geometry.
#include "game/include/track_mesh.h"

#include <cmath>
#include <cstdio>

#include "game/include/rng.h"

namespace game {

namespace {

// Flat-colour palette in linear space.
// ---- colours (linear albedo) ------------------------------------------------
const dm::Vec3 kAsphalt{0.16f, 0.16f, 0.18f};
const dm::Vec3 kLineWhite{0.85f, 0.85f, 0.82f};
const dm::Vec3 kLineYellow{0.90f, 0.70f, 0.15f};
const dm::Vec3 kKerbRed{0.75f, 0.12f, 0.10f};
const dm::Vec3 kKerbWhite{0.88f, 0.88f, 0.88f};
const dm::Vec3 kGravel{0.46f, 0.42f, 0.36f};
const dm::Vec3 kGrass{0.30f, 0.50f, 0.22f};
const dm::Vec3 kGrassDark{0.22f, 0.40f, 0.18f};
const dm::Vec3 kRock{0.42f, 0.40f, 0.37f};
const dm::Vec3 kDirt{0.40f, 0.32f, 0.22f};
const dm::Vec3 kRail{0.78f, 0.80f, 0.83f};
const dm::Vec3 kPost{0.35f, 0.36f, 0.38f};

// Deterministic noise in [0, 1) from three integers (top 24 bits of splitmix64).
// Cheap deterministic hash noise in [0, 1) for colour jitter.
float hashNoise(uint32_t a, uint32_t b, uint32_t c)
{
    uint64_t h = splitmix64((static_cast<uint64_t>(a) << 32) ^ (static_cast<uint64_t>(b) << 16) ^ c);
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

dm::Vec3 jitter(dm::Vec3 c, float n, float amount) { return c * (1.0f + (n - 0.5f) * 2.0f * amount); }

// Append a 4-vertex quad with per-end normals (a/b = left/right edge, 0/1 =
// start/end of the segment).
// Smooth-shaded quad (shared normal), a = left/near-s, b = right; 0 = s_i, 1 = s_i+1.
// Winding (a0, b0, b1), (a0, b1, a1) is CCW seen from +normal for our layout.
void addQuad(gfx::MeshData &m, dm::Vec3 a0, dm::Vec3 b0, dm::Vec3 a1, dm::Vec3 b1, dm::Vec3 n0, dm::Vec3 n1,
             dm::Vec3 color)
{
    uint32_t base = static_cast<uint32_t>(m.positions.size());
    m.positions.insert(m.positions.end(), {a0, b0, a1, b1});
    m.normals.insert(m.normals.end(), {n0, n0, n1, n1});
    m.colors.insert(m.colors.end(), {color, color, color, color});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 3, base, base + 3, base + 2});
}

// Append one flat-shaded triangle, flipping the winding so the face normal
// points to the `outward` side. Degenerate triangles are skipped.
// Flat-shaded triangle; flips the winding if the face would point away from `outward`.
void addTriFlat(gfx::MeshData &m, dm::Vec3 a, dm::Vec3 b, dm::Vec3 c, dm::Vec3 color, dm::Vec3 outward)
{
    dm::Vec3 n = dm::cross(b - a, c - a);
    if (dm::lengthSq(n) < 1e-12f) return;
    if (dm::dot(n, outward) < 0) { std::swap(b, c); n = -n; }
    n = dm::normalize(n);
    uint32_t base = static_cast<uint32_t>(m.positions.size());
    m.positions.insert(m.positions.end(), {a, b, c});
    m.normals.insert(m.normals.end(), {n, n, n});
    m.colors.insert(m.colors.end(), {color, color, color});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
}

void addQuadFlat(gfx::MeshData &m, dm::Vec3 a0, dm::Vec3 b0, dm::Vec3 a1, dm::Vec3 b1, dm::Vec3 color,
                 dm::Vec3 outward)
{
    addTriFlat(m, a0, b0, b1, color, outward);
    addTriFlat(m, a0, b1, a1, color, outward);
}

// Box from three axis directions: for each of the 6 faces, two triangles.
// Axis-aligned-in-frame box: centre c, frame axes (r, u, f) with half sizes h.
void addBox(gfx::MeshData &m, dm::Vec3 c, dm::Vec3 r, dm::Vec3 u, dm::Vec3 f, dm::Vec3 h, dm::Vec3 color)
{
    const dm::Vec3 ax[3] = {r * h.x, u * h.y, f * h.z};
    for (int a = 0; a < 3; ++a)
        for (int sgn = -1; sgn <= 1; sgn += 2)
        {
            dm::Vec3 n = ax[a] * static_cast<float>(sgn);
            dm::Vec3 t1 = ax[(a + 1) % 3], t2 = ax[(a + 2) % 3];
            dm::Vec3 fc = c + n;
            dm::Vec3 p0 = fc - t1 - t2, p1 = fc + t1 - t2, p2 = fc + t1 + t2, p3 = fc - t1 + t2;
            addTriFlat(m, p0, p1, p2, color, n);
            addTriFlat(m, p0, p2, p3, color, n);
        }
}

// Cone of `segments` sides on a circular base (for the fallback trees); the
// base disc is darkened, `twist` rotates the vertex ring (rad).
void addCone(gfx::MeshData &m, dm::Vec3 base, float radius, float height, int segments, dm::Vec3 color,
             float twist)
{
    dm::Vec3 apex = base + dm::Vec3{0, height, 0};
    for (int i = 0; i < segments; ++i)
    {
        float a0 = twist + 2.0f * dm::kPi * i / segments, a1 = twist + 2.0f * dm::kPi * (i + 1) / segments;
        dm::Vec3 p0 = base + dm::Vec3{radius * std::cos(a0), 0, radius * std::sin(a0)};
        dm::Vec3 p1 = base + dm::Vec3{radius * std::cos(a1), 0, radius * std::sin(a1)};
        dm::Vec3 mid = (p0 + p1) * 0.5f;
        addTriFlat(m, p0, p1, apex, color, mid - base + dm::Vec3{0, radius * 0.3f, 0});
        addTriFlat(m, p0, p1, base, color * 0.8f, {0, -1, 0});
    }
}

// Longitudinal samples for a section: finer on arcs (1.6 m) than straights (3 m).
int sampleCount(const TrackSection &sec)
{
    float step = sec.isTurn() ? 1.6f : 3.0f;
    int n = static_cast<int>(std::ceil(sec.length / step));
    return n < 2 ? 2 : n;
}

// Limit the terrain strip on the inside of a bend to the turn radius so the
// strip never folds over the centre of tight hairpins.
// How far the terrain strip may extend on `side` without crossing the
// centre of a hairpin (inside of a turn).
float maxStripWidth(const TrackSection &sec, int side, float edge)
{
    const float full = 16.0f;
    if (!sec.isTurn()) return full;
    if (side != sec.insideSide()) return full;
    float w = sec.radius - edge - 1.0f;
    return w < full ? w : full;
}

// Road surface: nine lateral strips (gravel, kerb, edge line, asphalt, centre
// line, ...) between per-sample breakpoints, lifted slightly for paint and
// kerbs to avoid z-fighting.
void buildRoad(const Track &track, const TrackSection &sec, gfx::MeshData &m)
{
    const TrackParams &tp = track.params();
    const float hw = tp.roadHalfWidth, kerb = tp.kerbWidth;
    const float line = 0.18f, centre = 0.07f;
    // Lateral breakpoints left to right; the outer ones follow the per-side
    // edge (runoff widens it). Strip kinds: 0 asphalt, 1 edge line,
    // 2 centre line, 3 kerb, 4 gravel/runoff.
    const int kinds[9] = {4, 3, 1, 0, 2, 0, 1, 3, 4};
    auto breaks = [&](float s, float out[10]) {
        float eL = sec.edgeAt(s, -1, tp), eR = sec.edgeAt(s, 1, tp);
        const float b[10] = {-eL, -hw - kerb, -hw, -hw + line, -centre, centre, hw - line, hw, hw + kerb, eR};
        for (int k = 0; k < 10; ++k) out[k] = b[k];
    };
    const int n = sampleCount(sec);
    for (int i = 0; i < n; ++i)
    {
        float s0 = sec.length * i / n, s1 = sec.length * (i + 1) / n;
        float sm = 0.5f * (s0 + s1);
        dm::Vec3 n0 = sec.normalAt(s0), n1 = sec.normalAt(s1);
        float dist = sec.startDistance + sm;
        float b0[10], b1[10];
        breaks(s0, b0);
        breaks(s1, b1);
        for (int k = 0; k < 9; ++k)
        {
            dm::Vec3 color = kAsphalt;
            float lift = 0.0f;
            switch (kinds[k])
            {
            case 0: color = jitter(kAsphalt, hashNoise(sec.index, i, static_cast<uint32_t>(k + 100)), 0.06f); break;
            case 1: color = kLineWhite; lift = 0.005f; break;
            case 2:
                // Dashed centre line: 3 m paint, 3 m gap.
                color = (static_cast<int>(dist / 3.0f) % 2 == 0) ? kLineYellow : kAsphalt;
                lift = 0.005f;
                break;
            case 3:
                if (sec.isTurn())
                {
                    // Red/white kerb blocks in bends.
                    color = (static_cast<int>(dist / 1.5f) % 2 == 0) ? kKerbRed : kKerbWhite;
                    lift = 0.03f;
                }
                else
                    color = jitter(kGravel, hashNoise(sec.index, i, static_cast<uint32_t>(k)), 0.08f);
                break;
            case 4:
                color = jitter(sec.style == RoadsideStyle::Runoff ? kGravel * 1.12f : kGravel,
                               hashNoise(sec.index, i, static_cast<uint32_t>(k)), 0.08f);
                break;
            }
            dm::Vec3 up0 = n0 * lift, up1 = n1 * lift;
            addQuad(m, sec.pointAt(s0, b0[k]) + up0, sec.pointAt(s0, b0[k + 1]) + up0, sec.pointAt(s1, b1[k]) + up1,
                    sec.pointAt(s1, b1[k + 1]) + up1, n0, n1, color);
        }
    }
}

// Widest edge offset (road to barrier) of a section on a side, including the
// runoff of the previous section it blends from.
// Widest drivable half-width a section reaches on a side (for strip limits).
float maxEdge(const Track &track, const TrackSection &sec, int side)
{
    int i = side > 0 ? 1 : 0;
    return track.edgeOffset() + std::fmax(sec.runoff[i], sec.prevRunoff[i]);
}

// Terrain strips beside the road on both sides: lateral rings at xsFull metres
// beyond the edge, raised by terrainOffset() according to uphillness.
void buildTerrain(const Track &track, const TrackSection &sec, gfx::MeshData &m)
{
    const TrackParams &tp = track.params();
    // Lateral sample distances beyond the shoulder. The last one is a long
    // skirt that only opens up on drop-off sides (so you never see sky under
    // the mountainside); on uphill sides it collapses onto the 16 m point.
    const float xsFull[] = {0.0f, 0.7f, 2.0f, 4.0f, 7.0f, 11.0f, 16.0f, 16.0f};
    const int nx = sizeof xsFull / sizeof xsFull[0];
    const float skirt = 30.0f;
    const int n = sampleCount(sec);
    for (int side = -1; side <= 1; side += 2)
    {
        const float maxW = maxStripWidth(sec, side, maxEdge(track, sec, side));
        auto xAt = [&](int k, float u) {
            float x = xsFull[k] + (k == nx - 1 ? skirt * (u < 0 ? -u : 0.0f) : 0.0f);
            return x < maxW ? x : maxW;
        };
        for (int i = 0; i < n; ++i)
        {
            float s0 = sec.length * i / n, s1 = sec.length * (i + 1) / n;
            float u0 = sec.uphillness(s0, side), u1 = sec.uphillness(s1, side);
            auto pt = [&](float s, float u, float x) {
                dm::Vec3 p = sec.pointAt(s, side * (sec.edgeAt(s, side, tp) + x));
                p.y += terrainOffset(u, x);
                return p;
            };
            for (int k = 0; k + 1 < nx; ++k)
            {
                float xa0 = xAt(k, u0), xb0 = xAt(k + 1, u0), xa1 = xAt(k, u1), xb1 = xAt(k + 1, u1);
                if (xb0 - xa0 < 0.05f && xb1 - xa1 < 0.05f) continue;
                dm::Vec3 p00 = pt(s0, u0, xa0), p01 = pt(s0, u0, xb0);
                dm::Vec3 p10 = pt(s1, u1, xa1), p11 = pt(s1, u1, xb1);
                // Order left-to-right in d so the winding helper sees (a = smaller d).
                dm::Vec3 a0 = side < 0 ? p01 : p00, b0 = side < 0 ? p00 : p01;
                dm::Vec3 a1 = side < 0 ? p11 : p10, b1 = side < 0 ? p10 : p11;
                float run = std::fmax(xb0 - xa0, 0.05f);
                float slope = std::fabs(p01.y - p00.y) / run;
                float nz = hashNoise(sec.index * 31u + static_cast<uint32_t>(side + 1), static_cast<uint32_t>(i),
                                     static_cast<uint32_t>(k));
                dm::Vec3 color = k == 0 ? kDirt : (slope > 0.75f ? kRock : (nz > 0.5f ? kGrass : kGrassDark));
                color = jitter(color, nz, 0.08f);
                addQuadFlat(m, a0, b0, a1, b1, color, {0, 1, 0});
            }
        }
    }
}

// Guardrail (two-sided band) plus a post about every 2.5 m, only where the
// section's side is a Rail rather than a rock wall.
void buildRails(const Track &track, const TrackSection &sec, gfx::MeshData &m)
{
    const TrackParams &tp = track.params();
    const int n = sampleCount(sec);
    const float railLow = 0.45f, railHigh = 0.75f, inset = 0.1f;
    for (int side = -1; side <= 1; side += 2)
    {
        auto dAt = [&](float s) { return side * (sec.edgeAt(s, side, tp) + inset); };
        float sinceLastPost = 1e9f;
        for (int i = 0; i < n; ++i)
        {
            float s0 = sec.length * i / n, s1 = sec.length * (i + 1) / n;
            bool rail0 = sec.sideKind(s0, side) == SideKind::Rail;
            bool rail1 = sec.sideKind(s1, side) == SideKind::Rail;
            if (!rail0 || !rail1) { sinceLastPost = 1e9f; continue; }
            dm::Vec3 b0 = sec.pointAt(s0, dAt(s0)), b1 = sec.pointAt(s1, dAt(s1));
            dm::Vec3 up{0, 1, 0};
            dm::Vec3 outward = sec.rightAt(0.5f * (s0 + s1)) * static_cast<float>(side);
            // Two-sided band.
            addQuadFlat(m, b0 + up * railLow, b1 + up * railLow, b0 + up * railHigh, b1 + up * railHigh, kRail, outward);
            addQuadFlat(m, b0 + up * railLow, b1 + up * railLow, b0 + up * railHigh, b1 + up * railHigh, kRail * 0.85f,
                        -outward);
            sinceLastPost += s1 - s0;
            if (sinceLastPost >= 2.5f)
            {
                sinceLastPost = 0;
                dm::Vec3 r = sec.rightAt(s0), f = sec.forwardAt(s0);
                addBox(m, sec.pointAt(s0, dAt(s0) + side * 0.08f) + up * 0.4f, r, up, f, {0.06f, 0.4f, 0.06f}, kPost);
            }
        }
    }
}

// Scatter trees by a per-section seeded RNG. Values are drawn in a fixed
// order per tree so skipping one does not change the others.
void placeTrees(const Track &track, const TrackSection &sec, std::vector<TreePlacement> &out)
{
    const std::vector<std::string> &names = treeModelNames();
    Pcg32 rng(splitmix64((static_cast<uint64_t>(track.seed()) << 32) ^ sec.index), 0x74726565ull /* "tree" */);
    const TrackParams &tp = track.params();
    // Forest sections are dense, cliffs sparse on the drop side.
    float density = sec.style == RoadsideStyle::Forest ? 1.8f : (sec.style == RoadsideStyle::Runoff ? 0.7f : 1.0f);
    for (int side = -1; side <= 1; side += 2)
    {
        float maxW = maxStripWidth(sec, side, maxEdge(track, sec, side)) - 1.0f;
        int count = static_cast<int>(sec.length / 9.0f * density) + 1; // modest: llvmpipe-friendly
        for (int k = 0; k < count; ++k)
        {
            // Always draw the same number of values so placements don't
            // depend on which branch was taken for earlier trees.
            float s = rng.range(0.0f, sec.length);
            float x = rng.range(3.0f, 16.0f);
            int nameIdx = rng.range(0, static_cast<int>(names.size()));
            float yaw = rng.range(0.0f, 2.0f * dm::kPi);
            float scale = rng.range(0.8f, 1.3f);
            float keep = rng.nextFloat();
            float u = sec.uphillness(s, side);
            float keepBelow = sec.style == RoadsideStyle::Cliff && u < -0.3f ? 0.7f : 0.3f;
            if (x > maxW || keep < keepBelow) continue;
            TreePlacement t;
            t.model = names[nameIdx];
            t.fallbackVariant = nameIdx % 3;
            t.position = sec.pointAt(s, side * (sec.edgeAt(s, side, tp) + x));
            t.position.y += terrainOffset(u, x) - 0.15f; // sink the root a little
            t.yaw = yaw;
            t.scale = scale;
            out.push_back(t);
        }
    }
}

// Standard Hermite smoothstep.
float smoothstep(float e0, float e1, float x)
{
    float t = dm::clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

// Names of the tree models, tree_type<t>_<nn>, generated once.
const std::vector<std::string> &treeModelNames()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        const int counts[8] = {6, 5, 5, 5, 5, 4, 4, 4};
        char buf[32];
        for (int t = 0; t < 8; ++t)
            for (int i = 1; i <= counts[t]; ++i)
            {
                std::snprintf(buf, sizeof buf, "tree_type%d_%02d", t, i);
                v.emplace_back(buf);
            }
        return v;
    }();
    return names;
}

// Height (m) of terrain at lateral distance x beyond the road edge given
// uphillness u (+ rising bank, - drop-off).
float terrainOffset(float u, float x)
{
    if (u >= 0)
        // Cut bank: rises ~2.2 m within 4 m, then a gentle mountainside.
        return u * (2.2f * smoothstep(0.0f, 4.0f, x) + 0.12f * (x > 4.0f ? x - 4.0f : 0.0f));
    // Drop-off: a short cliff at the edge, a steep slope, steeper still in the skirt.
    return u * (2.0f * smoothstep(0.0f, 1.5f, x) + 0.35f * (x > 1.5f ? x - 1.5f : 0.0f) +
                0.3f * (x > 16.0f ? x - 16.0f : 0.0f));
}

// Entry point: all geometry for track section `index`.
SectionGeometry buildSectionGeometry(const Track &track, uint32_t index)
{
    SectionGeometry g;
    g.section = index;
    const TrackSection &sec = track.section(index);
    buildRoad(track, sec, g.road);
    buildTerrain(track, sec, g.terrain);
    buildRails(track, sec, g.props);
    placeTrees(track, sec, g.trees);
    return g;
}

// Low-poly stand-in car mesh (boxes for body, cabin, lights, wheels);
// rideHeight (m) puts the ground at car-space y = -rideHeight.
gfx::MeshData buildBoxCarMesh(const dm::Vec3 &body, float rideHeight, float length, float width)
{
    // Designed at 4.2 m long; every dimension scales with the requested length.
    gfx::MeshData m;
    const dm::Vec3 r{1, 0, 0}, u{0, 1, 0}, f{0, 0, 1};
    const float k = length / 4.2f;
    float g = -rideHeight; // ground level in car space
    float hl = length * 0.5f, hw = width * 0.5f;
    addBox(m, {0, g + 0.55f * k, 0}, r, u, f, {hw, 0.28f * k, hl}, body);                            // lower body
    addBox(m, {0, g + 1.05f * k, 0.25f * k}, r, u, f, {hw * 0.85f, 0.24f * k, hl * 0.48f}, body * 0.9f); // cabin
    addBox(m, {0, g + 1.06f * k, -0.85f * k}, r, u, f, {hw * 0.8f, 0.2f * k, 0.02f * k}, {0.15f, 0.2f, 0.28f}); // windscreen
    const dm::Vec3 lamp{0.18f * k, 0.07f * k, 0.02f * k};
    addBox(m, {-hw * 0.6f, g + 0.6f * k, -hl - 0.01f}, r, u, f, lamp, {1.0f, 0.95f, 0.8f}); // lights
    addBox(m, {hw * 0.6f, g + 0.6f * k, -hl - 0.01f}, r, u, f, lamp, {1.0f, 0.95f, 0.8f});
    addBox(m, {-hw * 0.6f, g + 0.62f * k, hl + 0.01f}, r, u, f, lamp, {0.8f, 0.05f, 0.05f});
    addBox(m, {hw * 0.6f, g + 0.62f * k, hl + 0.01f}, r, u, f, lamp, {0.8f, 0.05f, 0.05f});
    const dm::Vec3 tyre{0.06f, 0.06f, 0.07f};
    float wz = hl * 0.62f;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2)
            addBox(m, {sx * (hw - 0.08f * k), g + 0.31f * k, sz * wz}, r, u, f, {0.12f * k, 0.31f * k, 0.31f * k}, tyre);
    return m;
}

// Procedural tree used when a model asset is missing: pine, broadleaf or
// autumn cone variants.
gfx::MeshData buildFallbackTreeMesh(int variant)
{
    gfx::MeshData m;
    const dm::Vec3 trunk{0.36f, 0.24f, 0.14f};
    addBox(m, {0, 0.9f, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.18f, 0.9f, 0.18f}, trunk);
    switch (variant % 3)
    {
    case 0: // pine
        addCone(m, {0, 1.4f, 0}, 1.9f, 2.6f, 7, {0.16f, 0.38f, 0.20f}, 0.0f);
        addCone(m, {0, 2.8f, 0}, 1.5f, 2.3f, 7, {0.18f, 0.42f, 0.22f}, 0.4f);
        addCone(m, {0, 4.0f, 0}, 1.0f, 2.0f, 7, {0.20f, 0.46f, 0.24f}, 0.8f);
        break;
    case 1: // round-ish broadleaf (double cone)
        addCone(m, {0, 3.2f, 0}, 2.2f, 2.4f, 8, {0.30f, 0.52f, 0.20f}, 0.0f);
        addCone(m, {0, 3.2f, 0}, 2.2f, -1.6f, 8, {0.26f, 0.46f, 0.18f}, 0.0f);
        break;
    default: // autumn
        addCone(m, {0, 1.6f, 0}, 1.7f, 3.8f, 6, {0.72f, 0.42f, 0.14f}, 0.3f);
        break;
    }
    return m;
}

} // namespace game
