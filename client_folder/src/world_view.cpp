// Client-side scene composition: owns the GPU meshes/models for the track,
// trees, cars, skid marks and smoke, and issues the draw calls each frame.
// Track sections are streamed: only sections near the cars are built (a few
// per frame, nearest first) and far ones are destroyed. Each section is
// merged into one mesh (road + terrain + guardrails) to keep draw calls low.
// Two entry points tie it to the main loop: onSimTick() runs once per fixed
// simulation tick (skid marks and smoke emission, so their density does not
// depend on frame rate) and draw() runs once per rendered frame (wheel
// spin/steer animation, nitro flames and particle ageing use the frame dt).
// Everything here is visual only and never feeds back into the simulation.
// All GL work stays on the render thread through gfx::Renderer.
// world_view.cpp -- see world_view.h.
#include "client_folder/include/world_view.h"

#include <algorithm>
#include <cmath>

#include "game/include/car_physics.h"

namespace client {

namespace {

// Append b to a (one mesh per track section keeps draw calls low on llvmpipe).
void appendMesh(gfx::MeshData &a, const gfx::MeshData &b)
{
    uint32_t base = static_cast<uint32_t>(a.positions.size());
    a.positions.insert(a.positions.end(), b.positions.begin(), b.positions.end());
    a.normals.insert(a.normals.end(), b.normals.begin(), b.normals.end());
    a.colors.insert(a.colors.end(), b.colors.begin(), b.colors.end());
    for (uint32_t i : b.indices) a.indices.push_back(base + i);
}

} // namespace

// Body paint colour of a look: painted solid onto the car models' body
// panels, used for the box car and the HUD swatches; look is wrapped to 0..3.
dm::Vec3 lookColor(int look)
{
    const dm::Vec3 c[4] = {{0.95f, 0.95f, 0.95f}, {0.95f, 0.75f, 0.2f}, {0.95f, 0.3f, 0.25f}, {0.35f, 0.55f, 1.0f}};
    return c[((look % 4) + 4) % 4];
}

const char *lookName(int look)
{
    const char *n[4] = {"AE86", "RX-7", "AE86", "RX-7"};
    return n[((look % 4) + 4) % 4];
}

WorldView::WorldView(gfx::Renderer &renderer) : r_(renderer) {}

WorldView::~WorldView() = default;

// Load car models and create the shared meshes: box-car and tree fallbacks
// (used when a model file is missing) and the large valley-floor disc (a
// triangle fan of `seg` segments).
void WorldView::initAssets()
{
    available_.clear();
    for (const std::string &n : r_.availableModels()) available_.insert(n);
    loadCarModel("ae86", ae86_);
    loadCarModel("rx7", rx7_);
    boxCar_ = r_.createMesh(game::buildBoxCarMesh({0.92f, 0.92f, 0.9f}, game::defaultCarParams().rideHeight,
                                                  4.2f * game::kCarScale, 1.65f * game::kCarScale));
    for (int i = 0; i < 3; ++i) fallbackTrees_[i] = r_.createMesh(game::buildFallbackTreeMesh(i));

    // Valley floor: a big disc, drawn far below the car and faded by fog.
    gfx::MeshData v;
    const int seg = 32;
    const float radius = 1500.0f;
    const dm::Vec3 green{0.20f, 0.33f, 0.17f};
    v.positions.push_back({0, 0, 0});
    v.normals.push_back({0, 1, 0});
    v.colors.push_back(green);
    for (int i = 0; i < seg; ++i)
    {
        float a = 2.0f * dm::kPi * i / seg;
        v.positions.push_back({radius * std::cos(a), 0, -radius * std::sin(a)});
        v.normals.push_back({0, 1, 0});
        v.colors.push_back(green * 0.9f);
    }
    for (int i = 0; i < seg; ++i)
        v.indices.insert(v.indices.end(), {0u, 1u + static_cast<uint32_t>(i), 1u + static_cast<uint32_t>((i + 1) % seg)});
    valley_ = r_.createMesh(v);
}

void WorldView::shutdown()
{
    for (auto &kv : sections_) destroySection(kv.second);
    sections_.clear();
    for (auto &kv : anims_)
        if (kv.second.marksMesh) r_.destroyMesh(kv.second.marksMesh);
    anims_.clear();
    if (boxCar_) r_.destroyMesh(boxCar_);
    if (valley_) r_.destroyMesh(valley_);
    boxCar_ = valley_ = 0;
    for (gfx::MeshHandle &h : fallbackTrees_)
    {
        if (h) r_.destroyMesh(h);
        h = 0;
    }
}

// Load "<name>" as a single model and/or as separate body + wheel models.
// The model is rescaled to a plausible length (about 4.2 m) times kCarScale,
// wheel anchor points are read from model markers; if any marker is missing
// the split body/wheel mode is disabled.
void WorldView::loadCarModel(const std::string &name, CarModel &m)
{
    m = CarModel{};
    if (hasModel(name)) m.full = r_.loadModel(name);
    if (hasModel(name + "_body") && hasModel(name + "_wheel"))
    {
        m.body = r_.loadModel(name + "_body");
        m.wheel = r_.loadModel(name + "_wheel");
    }
    gfx::ModelHandle ref = m.full ? m.full : m.body;
    if (!ref) return;
    // Converter output is real size (~4.2 m); rescale anything implausible.
    gfx::Bounds b = r_.modelBounds(ref);
    float len = b.max.z - b.min.z;
    m.scale = (len > 2.5f && len < 7.0f) ? 1.0f : (len > 1e-3f ? 4.2f / len : 1.0f);
    m.scale *= game::kCarScale; // the car's size relative to the road
    const char *keys[4] = {"wheel_fl", "wheel_fr", "wheel_rl", "wheel_rr"};
    bool ok = true;
    for (int i = 0; i < 4; ++i) ok = r_.modelPoint(ref, keys[i], m.wheelPos[i]) && ok;
    m.wheelRadius = r_.modelValue(ref, "wheel_radius", 0.3f);
    if (!ok) { m.body = m.wheel = 0; } // can't place separate wheels
}

// Drop everything built for the previous track and adopt the new seed.
// The tree "season" (suffix) is derived from the seed so all peers agree.
void WorldView::reset(const game::Track &track)
{
    for (auto &kv : sections_) destroySection(kv.second);
    sections_.clear();
    for (auto &kv : anims_)
    {
        kv.second.marks.clear();
        if (kv.second.marksMesh) r_.destroyMesh(kv.second.marksMesh);
    }
    anims_.clear();
    particles_.clear();
    seed_ = track.seed();
    haveTrack_ = true;
    // Pick a tree "season" from the seed (when GFX's variants exist).
    const char *suffixes[4] = {"", "_fall", "", "_dry"};
    treeSuffix_ = suffixes[seed_ % 4];
    if (!treeSuffix_.empty() && !hasModel("tree_type0_01" + treeSuffix_)) treeSuffix_.clear();
}

void WorldView::destroySection(SectionGfx &s)
{
    if (s.road) r_.destroyMesh(s.road);
    s = SectionGfx{};
}

// Keep the set of built sections matching the cars: from viewBehind metres
// before the rearmost of {camera car, field} to viewAhead beyond the
// frontmost, capped at maxSections. Far sections are freed; missing ones are
// built under a per-frame budget (64 on a fresh track, else 3), nearest first.
void WorldView::streamSections(const game::Track &track, uint32_t current, uint32_t fieldFirst, uint32_t fieldLast)
{
    if (!haveTrack_ || track.seed() != seed_) reset(track);
    uint32_t lo = std::min(current, fieldFirst), hi = std::max(current, fieldLast);
    const uint32_t count = track.sectionCount();
    uint32_t first = std::min(lo, count - 1), last = std::min(hi, count - 1);
    while (first > 0 && track.section(lo).startDistance - track.section(first).startDistance < viewBehind) --first;
    const float aheadEnd = track.section(last).startDistance + track.section(last).length + viewAhead;
    while (last + 1 < count && track.section(last + 1).startDistance < aheadEnd) ++last;
    // Keep the count bounded around the camera car if the field is huge.
    if (last - first + 1 > static_cast<uint32_t>(maxSections))
    {
        uint32_t half = static_cast<uint32_t>(maxSections) / 2;
        first = std::max(first, current > half ? current - half : 0);
        last = std::min(last, first + static_cast<uint32_t>(maxSections) - 1);
    }

    for (auto it = sections_.begin(); it != sections_.end();)
    {
        if (it->first + 1 < first || it->first > last + 1)
        {
            destroySection(it->second);
            it = sections_.erase(it);
        }
        else
            ++it;
    }
    // Build at most a few per frame, nearest first, to avoid hitches
    // (everything at once on the first frame of a new track).
    int budget = sections_.empty() ? 64 : 3;
    auto build = [&](uint32_t i) {
        game::SectionGeometry g = game::buildSectionGeometry(track, i);
        gfx::MeshData all = std::move(g.road);
        appendMesh(all, g.terrain);
        appendMesh(all, g.props);
        SectionGfx sg;
        sg.road = r_.createMesh(all);
        for (const game::TreePlacement &tp : g.trees)
        {
            TreeInstance ti;
            std::string name = tp.model + treeSuffix_;
            if (!hasModel(name)) name = tp.model;
            ti.model = hasModel(name) ? r_.loadModel(name) : 0;
            ti.fallback = tp.fallbackVariant;
            ti.xform = dm::trs(tp.position, dm::quatYaw(tp.yaw), {tp.scale, tp.scale, tp.scale});
            sg.trees.push_back(ti);
        }
        sections_[i] = std::move(sg);
        --budget;
    };
    for (uint32_t i = current; i <= last && budget > 0; ++i)
        if (!sections_.count(i)) build(i);
    for (uint32_t i = first; i < current && budget > 0; ++i)
        if (!sections_.count(i)) build(i);
}

// Small LCG for visual randomness; kept separate from gameplay RNG streams.
float WorldView::rand01()
{
    rng_ = rng_ * 1664525u + 1013904223u; // visual-only LCG (never affects gameplay)
    return static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
}

// Per-tick visuals for every non-eliminated car: extend skid marks (and
// re-upload the mesh only when dirty), and spawn smoke particles at the rear
// wheels while drifting (capped at 400 particles, ~60% spawn chance per
// wheel). Finally free the animation state of cars that are gone.
void WorldView::onSimTick(const game::Track &track, const std::vector<game::CarView> &cars)
{
    for (const game::CarView &c : cars)
    {
        if (c.eliminated) continue;
        CarAnim &a = anims_[c.playerId];
        a.marks.update(track, c.section, c.position, c.rotation, c.drifting && c.alive, c.driftAngle);
        if (a.marks.dirty())
        {
            gfx::MeshData m;
            a.marks.buildMesh(m);
            if (!a.marksMesh) a.marksMesh = r_.createMesh(m);
            else r_.updateMesh(a.marksMesh, m);
        }
        // Tyre smoke from the rear wheels (Car_SkidMarks.tyreSmoke).
        float alpha = game::smokeAlpha(c.driftAngle, c.drifting && c.alive);
        if (alpha <= 0) continue;
        game::WheelContacts wc = game::wheelContactsFromPose(track, c.section, c.position, c.rotation);
        for (int w = game::WheelRL; w <= game::WheelRR; ++w)
        {
            if (!wc.valid[w] || rand01() > 0.6f || particles_.size() > 400) continue;
            Particle p;
            p.pos = wc.point[w] + dm::Vec3{0, 0.25f, 0};
            p.vel = c.velocity * 0.15f + dm::Vec3{(rand01() - 0.5f) * 1.2f, 0.6f + rand01() * 0.6f, (rand01() - 0.5f) * 1.2f};
            p.age = 0;
            p.life = 1.0f + rand01() * 0.6f;
            p.size = (0.7f + rand01() * 0.3f) * game::kCarScale;
            p.alpha = alpha;
            particles_.push_back(p);
        }
    }
    // Forget animation state of cars that left.
    for (auto it = anims_.begin(); it != anims_.end();)
    {
        bool present = false;
        for (const game::CarView &c : cars) present = present || c.playerId == it->first;
        if (!present)
        {
            if (it->second.marksMesh) r_.destroyMesh(it->second.marksMesh);
            it = anims_.erase(it);
        }
        else
            ++it;
    }
}

// Choose the model: even looks AE86, odd looks RX-7, with fallback to the other one.
const WorldView::CarModel &WorldView::carModelFor(int look) const
{
    // Even looks are the AE86 (the original's car), odd ones the RX-7;
    // whichever exists if only one was converted.
    bool wantRx7 = (look % 2) == 1;
    const CarModel &a = ae86_, &b = rx7_;
    bool haveA = a.full || a.body, haveB = b.full != 0;
    if (wantRx7) return haveB ? b : a;
    return haveA ? a : b;
}

// Draw one car and update its animation state with the frame dt (s).
// Wheel steering follows -driftAngle, clamped, eased at ~10/s; wheel spin
// integrates forward speed / wheel radius. Every car is drawn as body + four
// wheels when the model has them (so all cars steer); else the single model.
void WorldView::drawCar(const game::CarView &car, CarAnim &anim, float dt, float time)
{
    const game::CarParams &p = game::defaultCarParams();
    const CarModel &m = carModelFor(car.look);
    gfx::DrawParams dp;
    // The look's colour replaces the body paint only (the renderer leaves
    // tyres, glass, lights and decals alone); the box car is painted whole.
    dp.usePaint = true;
    dp.paint = lookColor(car.look);
    if (!m.full && !m.body) dp.tint = lookColor(car.look);
    if (!car.alive) dp.tint = dp.tint * 0.45f; // wrecked
    if (car.ghost && car.alive)
    {
        dp.alpha = 0.45f; // just respawned: ghosted (transparent pass, no outline)
        dp.castShadow = false;
    }
    // Camera-motion blur treats the world as static, so it smears anything
    // that moves: the followed car (which moves with the camera) and the
    // rivals around it alike, and the smear came and went with the camera's
    // swing (flicker). Cars opt out; the background still blurs.
    dp.motionBlur = false;
    dm::Mat4 world = dm::trs(car.position, car.rotation);
    dm::Mat4 ground = world * dm::translate({0, -p.rideHeight, 0}) * dm::scale({m.scale, m.scale, m.scale});

    // Front wheels point along the velocity (Car_PlayerMovement.Update:
    // wheelAngle = -driftAngle clamped to maxVisualSteeringAngle, eased at
    // maxVisualSteeringSpeed = 10/s).
    float targetSteer = dm::radians(dm::clampf(-car.driftAngle, -p.maxVisualSteeringAngle, p.maxVisualSteeringAngle));
    if (dm::length(car.velocity) < 2.0f) targetSteer = 0;
    anim.steer += (targetSteer - anim.steer) * std::fmin(1.0f, dt * 10.0f);
    float forwardSpeed = dm::dot(car.velocity, dm::forwardOf(car.rotation));
    anim.spin = std::fmod(anim.spin - forwardSpeed / std::fmax(0.1f, m.wheelRadius) * dt, 2.0f * dm::kPi);

    // Nitro flame: smoothed intensity, ignition burst on the rising edge.
    bool burning = car.nitroActive && car.alive;
    anim.nitro += ((burning ? 1.0f : 0.0f) - anim.nitro) * (1.0f - std::exp(-8.0f * dt));
    if (burning && !anim.wasBurning) anim.ignition = 1.0f;
    else anim.ignition = std::fmax(0.0f, anim.ignition - dt / 0.3f);
    anim.wasBurning = burning;

    gfx::ModelHandle flameRef = 0;
    if (m.body && m.wheel)
    {
        // Body + four spinning wheels, the front pair steered.
        r_.drawModel(m.body, ground, dp);
        for (int i = 0; i < 4; ++i)
        {
            bool right = (i == game::WheelFR || i == game::WheelRR);
            bool front = i <= game::WheelFR;
            float yaw = (front ? anim.steer : 0.0f) + (right ? dm::kPi : 0.0f);
            dm::Mat4 wm = ground * dm::translate(m.wheelPos[i]) * dm::toMat4(dm::quatYaw(yaw)) *
                          dm::toMat4(dm::quatAxisAngle({1, 0, 0}, right ? -anim.spin : anim.spin));
            gfx::DrawParams wp = dp;
            wp.castShadow = false;
            r_.drawModel(m.wheel, wm, wp);
        }
        flameRef = m.body;
    }
    else if (m.full)
    {
        r_.drawModel(m.full, ground, dp);
        flameRef = m.full;
    }
    else
        r_.drawMesh(boxCar_, world, dp);
    if (anim.nitro > 0.01f || anim.ignition > 0.0f)
    {
        if (flameRef) r_.drawCarNitro(flameRef, ground, anim.nitro, time, anim.ignition);
        else r_.drawNitroFlame(world, {0.4f, -0.1f, 2.1f}, anim.nitro, time, anim.ignition);
    }
}

// Draw the whole scene for this frame: valley floor under the camera car,
// section meshes and trees, skid marks, cars, then smoke billboards.
// Particles age and drift here (velocity damping exp(-1.5 dt), slight rise).
void WorldView::draw(const std::vector<game::CarView> &cars, float dt, float time, int32_t cameraTargetId)
{
    gfx::DrawParams flat;
    flat.outline = false;
    flat.castShadow = false;
    bool haveFocus = false;
    dm::Vec3 focus{0, 0, 0};
    for (const game::CarView &c : cars)
        if (c.playerId == cameraTargetId)
        {
            focus = c.position;
            haveFocus = true;
            if (valley_) r_.drawMesh(valley_, dm::translate({c.position.x, c.position.y - valleyDepth, c.position.z}), flat);
        }
    if (!haveFocus && !cars.empty()) focus = cars.front().position;
    // Trees: every built section's trees within treeAhead metres (straight
    // line) of the camera car. Choosing them by section index along the road
    // made trees pop in and out in plain view on switchbacks, where the road
    // 150 m ahead or one section behind can be right beside the camera. The
    // outer 25 m grow trees out of the ground instead of popping them in.
    const float growBand = 25.0f;
    gfx::DrawParams tp;
    tp.castShadow = false;
    tp.outline = treeOutlines;
    for (const auto &kv : sections_)
    {
        const SectionGfx &s = kv.second;
        if (s.road) r_.drawMesh(s.road, dm::Mat4{}, flat);
        for (const TreeInstance &t : s.trees)
        {
            dm::Vec3 base{t.xform.at(0, 3), t.xform.at(1, 3), t.xform.at(2, 3)};
            float grow = (treeAhead - dm::length(base - focus)) / growBand;
            if (grow <= 0.0f) continue;
            dm::Mat4 xf = grow < 1.0f ? t.xform * dm::scale({grow, grow, grow}) : t.xform;
            if (t.model) r_.drawModel(t.model, xf, tp);
            else r_.drawMesh(fallbackTrees_[t.fallback % 3], xf, tp);
        }
    }

    gfx::DrawParams marks;
    marks.outline = false;
    marks.castShadow = false;
    marks.alpha = 0.6f;
    for (auto &kv : anims_)
        if (kv.second.marksMesh) r_.drawMesh(kv.second.marksMesh, dm::Mat4{}, marks);

    for (const game::CarView &c : cars)
        if (!c.eliminated) drawCar(c, anims_[c.playerId], dt, time);

    // Smoke particles.
    for (Particle &p : particles_)
    {
        p.age += dt;
        p.pos += p.vel * dt;
        p.vel = p.vel * std::exp(-1.5f * dt) + dm::Vec3{0, 0.4f * dt, 0};
        p.size += dt * 1.8f;
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(), [](const Particle &p) { return p.age >= p.life; }),
                     particles_.end());
    for (const Particle &p : particles_)
    {
        float k = 1.0f - p.age / p.life;
        r_.drawBillboard(p.pos, p.size, {0.92f, 0.92f, 0.94f}, p.alpha * k);
    }
}

} // namespace client
