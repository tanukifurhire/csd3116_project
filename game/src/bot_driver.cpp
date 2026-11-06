// bot_driver.cpp -- see bot_driver.h (Reynolds steering behaviours).
#include "game/include/bot_driver.h"

#include <algorithm>
#include <cmath>

#include "game/include/rng.h"

namespace game {

// ---------------------------------------------------------------------------
// presets
// ---------------------------------------------------------------------------

BotProfile botPreset(BotPreset preset)
{
    BotProfile p;
    switch (preset)
    {
    case BotPreset::Rookie: // slow to react, wobbly, short sight, no safety net
        p.name = "Rookie";
        p.pace = 0.9f;
        p.topSpeedFactor = 0.7f; // ~ no nitro
        p.aggression = 0.1f;
        p.reactionTicks = 7;
        p.steerNoise = 0.45f;
        p.feelerScale = 0.6f;
        p.lineSkill = 0.3f;
        p.planHorizon = 24;
        p.replanTicks = 8;
        p.planCandidates = 3;
        break;
    case BotPreset::Racer: // clean, quick, gives way
        p.name = "Racer";
        p.pace = 1.0f;
        p.topSpeedFactor = 0.9f;
        p.aggression = 0.3f;
        p.reactionTicks = 2;
        p.steerNoise = 0.06f;
        p.feelerScale = 1.0f;
        p.lineSkill = 0.8f;
        p.planHorizon = 45;
        p.replanTicks = 5;
        break;
    case BotPreset::Bully: // hunts slower cars for takedowns, blocks
        p.name = "Bully";
        p.pace = 0.97f;
        p.topSpeedFactor = 1.0f;
        p.aggression = 0.95f;
        p.reactionTicks = 2;
        p.steerNoise = 0.08f;
        p.feelerScale = 0.9f;
        p.lineSkill = 0.6f;
        p.planHorizon = 45;
        p.replanTicks = 5;
        break;
    case BotPreset::Ace: // fastest, opportunistic rams, never sloppy
        p.name = "Ace";
        p.pace = 1.05f;
        p.topSpeedFactor = 1.0f;
        p.aggression = 0.6f;
        p.reactionTicks = 0;
        p.steerNoise = 0.0f;
        p.feelerScale = 1.2f;
        p.lineSkill = 1.0f;
        p.planHorizon = 48;
        p.replanTicks = 3;
        break;
    default: break;
    }
    return p;
}

const char *botPresetName(BotPreset preset)
{
    switch (preset)
    {
    case BotPreset::Rookie: return "Rookie";
    case BotPreset::Racer: return "Racer";
    case BotPreset::Bully: return "Bully";
    case BotPreset::Ace: return "Ace";
    default: return "?";
    }
}

namespace {

// ---------------------------------------------------------------------------
// planar helpers (XZ plane, y = 0)
// ---------------------------------------------------------------------------

inline dm::Vec3 flat(dm::Vec3 v)
{
    v.y = 0;
    return v;
}
inline dm::Vec3 flatDir(dm::Vec3 v)
{
    v.y = 0;
    float l = dm::length(v);
    return l > 1e-6f ? v * (1.0f / l) : dm::Vec3{0, 0, 0};
}
// Signed angle from a to b about +Y, + = b is left of a (dm: yaw + = left).
inline float yawBetween(const dm::Vec3 &a, const dm::Vec3 &b)
{
    return std::atan2(a.z * b.x - a.x * b.z, a.x * b.x + a.z * b.z);
}
inline dm::Vec3 rotateYaw(const dm::Vec3 &v, float yaw) // + = left (CCW seen from above)
{
    float c = std::cos(yaw), s = std::sin(yaw);
    return {v.x * c + v.z * s, 0.0f, -v.x * s + v.z * c};
}
inline dm::Vec3 truncate(const dm::Vec3 &v, float maxLen)
{
    float l = dm::length(v);
    return l > maxLen && l > 1e-6f ? v * (maxLen / l) : v;
}

// Walk the centreline from (sec, s) by ds metres.
inline void advance(const Track &t, uint32_t &sec, float &s, float ds)
{
    s += ds;
    while (sec + 1 < t.sectionCount() && s > t.section(sec).length) { s -= t.section(sec).length; ++sec; }
    while (s < 0.0f && sec > 0) { --sec; s += t.section(sec).length; }
}
inline float curvatureAt(const Track &t, uint32_t sec, float s, float ahead)
{
    advance(t, sec, s, ahead);
    return t.section(sec).curvature();
}

// Racing line: lateral offset (+ right) at (sec, s), as a fraction of the
// usable half-width: outside on the approach, inside at the apex.
// curvature > 0 = left turn, whose inside is d < 0.
float racingLine(const Track &t, uint32_t sec, float s)
{
    auto sigma = [](float k) { return dm::clampf(k * 15.0f, -1.0f, 1.0f); };
    float k0 = curvatureAt(t, sec, s, 0.0f), kA = curvatureAt(t, sec, s, 18.0f);
    return dm::clampf(0.9f * sigma(kA) - 1.6f * sigma(k0), -1.0f, 1.0f);
}

// Reynolds' "arrival" generalised to a road: the fastest speed from which
// every curve within `look` metres can still be made, scrubbing at aBrake.
// turnSign: side of the most limiting curve (+ = left), for scrubbing.
float speedProfile(const Track &t, uint32_t sec, float s, float cornerFactor, float driveForce, float vCap, float look,
                   float *turnSign, float speedNow)
{
    const float aBrake = 4.5f;
    const float react = 0.5f * speedNow; // metres covered before the scrub bites
    float v = vCap, limitK = 0;
    for (float x = 0; x <= look; x += 3.0f)
    {
        float k = curvatureAt(t, sec, s, x);
        if (std::fabs(k) < 1e-4f) continue;
        float vc = std::sqrt(driveForce * cornerFactor / std::fabs(k));
        float vx = std::sqrt(vc * vc + 2.0f * aBrake * std::fmax(0.0f, x - react));
        if (vx < v) { v = vx; limitK = k; }
        if (std::fabs(k) > std::fabs(limitK) && x < 25.0f) limitK = k;
    }
    if (turnSign) *turnSign = limitK > 0 ? 1.0f : (limitK < 0 ? -1.0f : 0.0f);
    return v;
}

// ---------------------------------------------------------------------------
// the behaviours
// ---------------------------------------------------------------------------

// Action selection (once per tick, from the real present): who to hunt, how
// far to shift the racing line (overtake / block), whether to hold back.
// The behaviours below are then evaluated from these decisions at any
// (real or simulated) car state.
struct Field
{
    const Track &track;
    const CarParams &p;
    const BotProfile &prof;
    const std::vector<BotOpponent> &others;
    const CarSimState *self = nullptr; // excluded from `others`
    float cornerFactor = 0.76f;
    float laneBias = 0, lineShift = 0;
    float vCap = 28.0f;
    float holdBack = 1e9f; // speed cap behind a car we must not ram
    float takedownSpeed = 9.0f;
    float aggression = 0.3f;
    const CarSimState *prey = nullptr;
    int32_t preyId = 0;
};

struct Steering
{
    dm::Vec3 containment{0, 0, 0}, separation{0, 0, 0}, evasion{0, 0, 0}, pursuit{0, 0, 0}, path{0, 0, 0};
    dm::Vec3 total{0, 0, 0};
    dm::Vec3 desiredVelocity{0, 0, 0};
    float targetSpeed = 0, turnSign = 0, danger = 0, line = 0;
    bool evading = false;
};

// Containment: feelers ahead along the travel direction and +-26 deg; a
// feeler that reaches into the safety margin along a road edge pushes back
// inward (the centre feeler also against the travel direction: scrub).
dm::Vec3 containment(const Field &f, const CarSimState &s, const dm::Vec3 &pos, const dm::Vec3 &vdir, float speed,
                     float *severity)
{
    const float margin = f.p.halfWidth + 0.75f;
    const float len = f.prof.feelerScale * (4.0f + 0.5f * speed);
    const float angles[3] = {0.0f, 0.45f, -0.45f};
    const float lens[3] = {len, 0.65f * len, 0.65f * len};
    dm::Vec3 force{0, 0, 0};
    float worst = 0;
    for (int i = 0; i < 3; ++i)
    {
        dm::Vec3 dir = rotateYaw(vdir, angles[i]);
        // Near sample first (a far tip past a hairpin may be back on road).
        for (float frac : {0.5f, 1.0f})
        {
            dm::Vec3 tip = pos + dir * (lens[i] * frac);
            RoadQuery q = f.track.query(tip, s.section);
            float room = q.edge - std::fabs(q.d);
            if (room >= margin) continue;
            float depth = dm::clampf((margin - room) / margin, 0.0f, 2.5f) * (frac < 0.75f ? 1.3f : 1.0f);
            dm::Vec3 inward = f.track.section(q.section).rightAt(q.s) * (q.d > 0 ? -1.0f : 1.0f);
            float w = i == 0 ? 1.0f : 0.7f;
            force += inward * (depth * w * 6.0f);
            if (i == 0) force -= vdir * (depth * 3.0f);
            worst = std::fmax(worst, depth * w);
            break;
        }
    }
    if (severity) *severity = worst;
    return force;
}

// All behaviours at car state `s`, with the other cars dead-reckoned
// `tAhead` seconds (their current velocity -- never their inputs), blended
// by Reynolds' prioritised, truncated running sum.
Steering steeringField(const Field &f, const CarSimState &s, float tAhead)
{
    Steering out;
    const dm::Vec3 pos = s.position, vel = flat(s.velocity);
    const float speed = dm::length(vel);
    const dm::Vec3 vdir = speed > 3.0f ? vel * (1.0f / speed) : flatDir(dm::forwardOf(s.rotation));
    const dm::Vec3 right{-vdir.z, 0.0f, vdir.x};

    // Arrival: as fast as the road ahead allows.
    out.targetSpeed = std::fmin(speedProfile(f.track, s.section, s.sectionS, f.cornerFactor, f.p.driveForce, f.vCap,
                                             70.0f, &out.turnSign, speed),
                                f.holdBack);

    // 1. containment
    out.containment = containment(f, s, pos, vdir, speed, &out.danger);

    // 2. separation, 3. evasion
    for (const BotOpponent &o : f.others)
    {
        const CarSimState *os = o.state;
        if (!os || os == f.self || !os->alive || os->ghostTimer > tAhead) continue;
        if (std::fabs(os->position.y - pos.y) > 2.0f) continue;
        dm::Vec3 ov = flat(os->velocity);
        dm::Vec3 opos = os->position + ov * tAhead;
        dm::Vec3 away = flat(pos - opos);
        float dist = dm::length(away);
        if (dist > 30.0f) continue;
        const float sepR = 2.0f * f.p.halfLength + 2.0f;
        if (o.id != f.preyId && dist < sepR && dist > 1e-3f)
            out.separation += away * ((sepR - dist) / (sepR * dist) * 6.0f * (1.0f - 0.6f * f.aggression));
        // Evasion: a car behind us, in our lane, closing at ram speed.
        float along = -dm::dot(away, vdir); // + = it is ahead
        float lat = -dm::dot(away, right);  // + = it is right of us
        float closing = dm::dot(ov - vel, vdir);
        if (along < -1.0f && along > -25.0f && std::fabs(lat) < 2.4f && closing > 0.55f * f.takedownSpeed &&
            -along / closing < 1.6f)
        {
            float t = -along / closing;
            float side = -(lat + dm::dot(ov, right) * t); // flee from where it will be
            float sgn = side > 0 ? 1.0f : -1.0f;
            if (std::fabs(side) < 0.3f) sgn = s.lateral > 0 ? -1.0f : 1.0f; // towards the wider side
            out.evasion += right * (sgn * 5.0f * (1.0f - 0.7f * f.aggression));
            out.evading = true;
        }
    }

    // 4. pursuit: seek the prey's predicted position at intercept.
    if (f.prey)
    {
        dm::Vec3 ov = flat(f.prey->velocity);
        dm::Vec3 ppos = f.prey->position + ov * tAhead;
        float dist = dm::length(flat(ppos - pos));
        float closing = std::fmax(dm::dot(vel - ov, flatDir(ppos - pos)), 2.0f);
        float T = dm::clampf(dist / closing, 0.0f, 1.5f);
        dm::Vec3 predicted = ppos + ov * T;
        RoadQuery q = f.track.query(predicted, f.prey->section);
        const TrackSection &ts = f.track.section(q.section);
        float m = f.p.halfWidth + 0.8f;
        float d = dm::clampf(q.d, -(ts.edgeAt(q.s, -1, f.track.params()) - m), ts.edgeAt(q.s, 1, f.track.params()) - m);
        out.pursuit = flatDir(ts.pointAt(q.s, d) - pos) * out.targetSpeed - vel;
    }

    // 5. path following: predict, project onto the racing line, seek ahead.
    {
        dm::Vec3 future = pos + vel * 0.3f;
        RoadQuery q = f.track.query(future, s.section);
        uint32_t sec = q.section;
        float sa = q.s;
        advance(f.track, sec, sa, dm::clampf(6.0f + 0.45f * speed, 8.0f, 18.0f));
        const TrackSection &ts = f.track.section(sec);
        const float margin = f.p.halfWidth + 1.0f;
        float edgeL = ts.edgeAt(sa, -1, f.track.params()) - margin, edgeR = ts.edgeAt(sa, 1, f.track.params()) - margin;
        float d = racingLine(f.track, sec, sa) * std::fmin(edgeL, edgeR) * f.prof.lineSkill + f.laneBias + f.lineShift;
        d = dm::clampf(d, -edgeL, edgeR);
        out.line = d;
        out.path = flatDir(ts.pointAt(sa, d) - pos) * out.targetSpeed - vel; // desired - current velocity
    }

    // Blend: higher priorities spend the steering budget first.
    const float budget = 16.0f;
    auto accumulate = [&out, budget](const dm::Vec3 &force) {
        float left = budget - dm::length(out.total);
        if (left > 0) out.total += truncate(force, left);
    };
    accumulate(out.containment);
    accumulate(out.separation);
    accumulate(out.evasion);
    accumulate(f.prey ? out.pursuit * 0.8f + out.path * 0.2f : out.path);
    out.desiredVelocity = vel + out.total;
    return out;
}

// Unaligned collision avoidance (Reynolds), evaluated on a simulated state:
// cost of being about to touch another car (dead-reckoned `t` seconds), by
// how close and how fast. The prey is the exception: closing on it hard is
// what a hunter wants (a takedown).
float contactCost(const Field &f, const CarSimState &s, float t)
{
    float cost = 0;
    const dm::Vec3 vel = flat(s.velocity);
    for (const BotOpponent &o : f.others)
    {
        const CarSimState *os = o.state;
        if (!os || os == f.self || !os->alive || os->ghostTimer > t) continue;
        if (std::fabs(os->position.y - s.position.y) > 2.0f) continue;
        dm::Vec3 ov = flat(os->velocity);
        dm::Vec3 rel = flat(os->position + ov * t - s.position);
        float d = dm::length(rel);
        const float reach = 2.0f * f.p.halfLength + 0.4f;
        if (d > reach) continue;
        float closing = d > 1e-3f ? std::fmax(0.0f, dm::dot(vel - ov, rel * (1.0f / d))) : 0.0f;
        if (o.id == f.preyId)
            cost -= f.aggression * 6.0f * std::fmax(0.0f, closing - 0.5f * f.takedownSpeed);
        else
            cost += (1.2f - f.aggression) * (reach - d) * (6.0f + 2.0f * closing);
    }
    return cost;
}

// Direct mapping of a desired velocity to steer for the frictionless car:
// the drive force along the nose is the only actuator, so point the nose
// along the acceleration we want (within a slip limit), PD on the heading.
float steerTowards(const CarSimState &car, const CarParams &p, const dm::Vec3 &desired, float turnSign,
                   dm::Vec3 *headingOut)
{
    const float tau = 0.3f;
    dm::Vec3 vel = flat(car.velocity);
    float speed = dm::length(vel);
    dm::Vec3 heading = flatDir(dm::forwardOf(car.rotation));
    dm::Vec3 vdir = speed > 3.0f ? vel * (1.0f / speed) : heading;
    dm::Vec3 accel = (desired - vel) * (1.0f / tau) + vel * (p.drag * speed);
    float phi = dm::length(accel) > 1e-4f ? yawBetween(vdir, accel) : 0.0f;
    float over = speed - dm::length(desired);
    float limit = 1.15f + dm::clampf(over * 0.1f, 0.0f, 0.9f);
    if (std::fabs(phi) > 2.6f && turnSign != 0) phi = turnSign * limit; // pure braking: scrub into the turn
    phi = dm::clampf(phi, -limit, limit);
    dm::Vec3 noseDir = rotateYaw(vdir, phi);
    if (headingOut) *headingOut = noseDir;
    float err = yawBetween(heading, noseDir); // + = the nose must swing left
    float yawRate = dm::dot(car.angularVelocity, dm::upOf(car.rotation)) + car.spinRate;
    err -= yawRate * 0.08f; // where the nose will be once the smoothed steer acts
    return dm::clampf(-3.0f * err, -1.0f, 1.0f);
}

float noise(uint32_t seed, uint32_t tick)
{
    auto h = [seed](uint32_t k) {
        uint64_t x = splitmix64((static_cast<uint64_t>(seed) << 32) ^ k);
        return static_cast<float>(x >> 40) * (2.0f / 16777216.0f) - 1.0f;
    };
    const uint32_t period = 20;
    uint32_t k = tick / period;
    float f = static_cast<float>(tick % period) / period;
    f = f * f * (3.0f - 2.0f * f);
    return dm::lerpf(h(k), h(k + 1), f);
}

} // namespace

// ---------------------------------------------------------------------------
// drive
// ---------------------------------------------------------------------------

net::CarInput BotDriver::drive(const CarSimState &car, const Track &track, const CarParams &params, uint32_t tick,
                               const std::vector<BotOpponent> &others)
{
    const float dt = kFixedDt;
    debug = Debug{};
    if (!car.alive)
    {
        pursuitId = 0;
        planTicks = 0;
        lineShift = 0;
        stuckTime = recoverTime = 0;
        for (float &h : steerHistory) h = 0;
        return driveInput(tick, 0.0f, false);
    }
    // Stuck (nose in a wall, spun to a stop): full lock towards the road
    // direction for a moment instead of dithering.
    if (recoverTime > 0)
    {
        recoverTime -= dt;
        return driveInput(tick, recoverSteer, false);
    }
    stuckTime = car.speed < 3.0f ? stuckTime + dt : 0.0f;
    if (stuckTime > 0.7f)
    {
        dm::Vec3 fw = dm::forwardOf(car.rotation);
        float headingYaw = std::atan2(-fw.x, -fw.z);
        float err = dm::wrapAngle(track.section(car.section).yawAt(car.sectionS) - headingYaw);
        recoverSteer = err > 0 ? -1.0f : 1.0f;
        recoverTime = 1.2f;
        stuckTime = 0;
        planTicks = 0;
        return driveInput(tick, recoverSteer, false);
    }

    const BotProfile &prof = profile;
    Field f{track, params, prof, others};
    f.self = &car;
    f.cornerFactor = cornerFactor * prof.pace;
    f.laneBias = laneBias;
    f.vCap = 31.0f * prof.topSpeedFactor;
    f.takedownSpeed = takedownSpeed;
    f.aggression = dm::clampf(prof.aggression, 0.0f, 1.0f);
    const float aggr = f.aggression;

    // --- action selection: prey, overtaking, blocking, holding back --------
    const TrackSection &here = track.section(car.section);
    const dm::Vec3 roadFwd = flatDir(here.forwardAt(car.sectionS));
    const float myAlong = dm::dot(flat(car.velocity), roadFwd);
    const float edgeL = here.edgeAt(car.sectionS, -1, track.params()), edgeR = here.edgeAt(car.sectionS, 1, track.params());
    const float vRoad = speedProfile(track, car.section, car.sectionS, f.cornerFactor, params.driveForce, f.vCap, 70.0f,
                                     nullptr, car.speed);
    float bestPrey = 1e9f;
    int32_t newPrey = 0;
    for (const BotOpponent &o : others)
    {
        const CarSimState *os = o.state;
        if (!os || os == &car || !os->alive || os->ghostTimer > 0) continue;
        if (std::fabs(os->position.y - car.position.y) > 2.0f) continue;
        float gap = os->distance - car.distance; // + = ahead
        if (gap <= 0.0f)
        {
            // Block: an aggressive driver drifts across a car right behind.
            float closing = dm::dot(flat(os->velocity), roadFwd) - myAlong;
            if (aggr > 0.6f && gap > -15.0f && closing > 0.0f && closing < 0.55f * takedownSpeed)
                f.lineShift += dm::clampf(os->lateral - car.lateral, -1.5f, 1.5f) * (aggr - 0.6f) * 2.0f;
            continue;
        }
        if (gap > 45.0f) continue;
        float oAlong = dm::dot(flat(os->velocity), roadFwd);
        // Pursuit: can we reach it fast enough for a takedown?
        float potential = std::fmin(vRoad, f.vCap) - oAlong;
        if (aggr >= 0.5f && gap > 2.0f && potential >= takedownSpeed * (1.3f - 0.4f * aggr))
        {
            float score = gap - (o.id == pursuitId ? 15.0f : 0.0f); // sticky
            if (score < bestPrey) { bestPrey = score; newPrey = o.id; f.prey = os; }
            continue;
        }
        // Overtake a slower car in our lane: shift the line to the roomier side.
        float closingAlong = myAlong - oAlong;
        float latDiff = os->lateral - car.lateral;
        if (std::fabs(latDiff) < 2.2f && closingAlong > 0.5f && gap < 30.0f && gap / closingAlong < 2.5f)
        {
            float passR = os->lateral + 2.6f, passL = os->lateral - 2.6f;
            bool rightOk = passR < edgeR - params.halfWidth - 0.6f, leftOk = passL > -edgeL + params.halfWidth + 0.6f;
            float urgency = dm::clampf(1.5f - gap / (closingAlong * 2.5f), 0.3f, 1.0f);
            if (rightOk || leftOk)
            {
                bool goRight = rightOk && (!leftOk || std::fabs(passR - car.lateral) < std::fabs(passL - car.lateral));
                f.lineShift += ((goRight ? passR : passL) - car.lateral) * urgency;
                debug.overtaking = true;
            }
            // Cautious drivers never ram: hold back if a hit would be a takedown.
            if (aggr < 0.5f && closingAlong > 0.6f * takedownSpeed)
                f.holdBack = std::fmin(f.holdBack, oAlong + 0.5f * takedownSpeed);
        }
    }
    pursuitId = newPrey;
    f.preyId = newPrey;
    // Low-pass the line shift: commit to a side instead of weaving.
    lineShift += (dm::clampf(f.lineShift, -4.0f, 4.0f) - lineShift) * 0.08f;
    f.lineShift = lineShift;

    // --- the behaviours at the present ----------------------------------------
    Steering now = steeringField(f, car, 0.0f);
    dm::Vec3 noseDir;
    float reactive = steerTowards(car, params, now.desiredVelocity, now.turnSign, &noseDir);

    // --- locomotion: realise the steering over the next ~0.75 s ------------
    // The nose turns at ~1.3 rad/s at most, so a desired velocity has to be
    // asked for early. Candidate steering plans (the direct mapping above in
    // closed loop, plus two-stage constant plans) are simulated with
    // stepCar; each is scored by how well it follows the steering field
    // along the way (re-evaluated at the simulated states, other cars dead-
    // reckoned), with wrecks and barrier hits as hard costs.
    bool useReactive = true;
    if (prof.planHorizon > 0)
    {
        if (planTicks <= 0)
        {
            const int H = prof.planHorizon, h1 = std::min(15, H / 2);
            const int nCand = prof.planCandidates >= 5 ? 5 : 3;
            const float cand5[5] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}, cand3[3] = {-1.0f, 0.0f, 1.0f};
            const float *cand = nCand == 5 ? cand5 : cand3;
            auto evaluate = [&](int a, int b) { // a < 0: closed-loop direct mapping
                CarSimState s = car;
                float cost = 0;
                for (int k = 1; k <= H; ++k)
                {
                    float st;
                    if (a < 0)
                    {
                        if (k == 1) st = reactive;
                        else
                        {
                            Steering sf = steeringField(f, s, (k - 1) * dt);
                            st = steerTowards(s, params, sf.desiredVelocity, sf.turnSign, nullptr);
                        }
                    }
                    else st = k <= h1 ? cand[a] : cand[b];
                    stepCar(s, driveInput(0, st, false), track, params);
                    if (!s.alive) return 1e6f - 1e3f * k;
                    if (s.impactWall > 1.0f) cost += 2.0f * s.impactWall * s.impactWall;
                    if (k % 3 == 0) cost += contactCost(f, s, k * dt);
                    if (k % 9 == 0)
                    {
                        Steering sf = steeringField(f, s, k * dt);
                        dm::Vec3 err = flat(s.velocity) - sf.desiredVelocity;
                        cost += 0.15f * dm::dot(err, err);
                    }
                }
                return cost - 1.0f * (s.distance - car.distance);
            };
            float best = evaluate(-1, 0);
            planSteer = 0;
            planReactive = true;
            for (int a = 0; a < nCand; ++a)
                for (int b = 0; b < nCand; ++b)
                {
                    float c = evaluate(a, b);
                    if (c < best - 0.5f) { best = c; planSteer = cand[a]; planReactive = false; }
                }
            planTicks = prof.replanTicks;
            debug.planCost = best;
        }
        --planTicks;
        useReactive = planReactive;
    }
    float steer = useReactive ? reactive : planSteer;
    debug.planned = !useReactive;

    // Nitro: well under the speed the road ahead allows and nothing in the
    // way (it costs steering, so not mid-slide) -- or to make a ram count.
    bool nitro = car.nitro > nitroMinGauge && std::fabs(car.driftAngle) < 22.0f && now.danger < 0.2f &&
                 car.speed < now.targetSpeed - 3.0f && car.speed < f.vCap - 1.0f && std::fabs(steer) <= 0.5f;
    if (f.prey && car.nitro > 0.08f && std::fabs(car.driftAngle) < 30.0f && now.danger < 0.3f &&
        car.speed < now.targetSpeed - 1.0f)
        nitro = true;

    // --- the human factor: steering error and reaction lag -----------------
    if (prof.steerNoise > 0) steer = dm::clampf(steer + prof.steerNoise * 0.6f * noise(seed, tick), -1.0f, 1.0f);
    int lag = std::clamp(prof.reactionTicks, 0, 15);
    steerHistory[historyPos % 16] = steer;
    float out = steerHistory[(historyPos + 16 - static_cast<uint32_t>(lag)) % 16];
    ++historyPos;

    debug.containment = now.containment;
    debug.separation = now.separation;
    debug.evasion = now.evasion;
    debug.pursuit = now.pursuit;
    debug.path = now.path;
    debug.desiredVelocity = now.desiredVelocity;
    debug.desiredHeading = noseDir;
    debug.targetSpeed = now.targetSpeed;
    debug.lineOffset = now.line;
    debug.pursuitTarget = pursuitId;
    debug.evading = now.evading;
    debug.nitro = nitro;
    return driveInput(tick, out, nitro);
}

} // namespace game
