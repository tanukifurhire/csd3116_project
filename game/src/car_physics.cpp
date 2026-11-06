// Purpose: deterministic single-car rigid-body simulation used by every peer.
// Responsibilities: spawn helpers, the fixed-step stepCar(), and conversions
// between the local CarSimState and the wire format net::CarState.
// Main loop: stepCar() is called once per fixed simulation tick (dt seconds,
// not per rendered frame) for the local car (client-side prediction), for
// replays during reconciliation, and for every car on the host (the host is authoritative).
// The result depends only on (state, input, track, params, dt), so identical
// inputs reproduce identical states; hashState() lets the tests verify that.
// Model: the car is a rigid body (mass, diagonal inertia) driven by forces:
// per-wheel spring/damper raycasts against the track surface, gravity along
// the body -up, quadratic drag, engine force, and a direct yaw acceleration
// for steering. Walls are position constraints with an impulse response.
// Units: metres, seconds, radians for state; degrees only where named Deg.
// Threading: no shared mutable state (the default params are immutable), so
// the functions are safe to call from any thread on separate states.
// car_physics.cpp -- see car_physics.h for the model overview.
#include "game/include/car_physics.h"

#include <cmath>
#include <cstring>

namespace game {

namespace {

const dm::Vec3 kWorldUp{0, 1, 0};

// Move v towards target by at most maxDelta (no overshoot).
float moveTowards(float v, float target, float maxDelta)
{
    if (v < target) return v + maxDelta < target ? v + maxDelta : target;
    return v - maxDelta > target ? v - maxDelta : target;
}

// Car orientation that lies on the road at arc length s of a section: yaw
// from the section heading, pitch from its grade (rotation about local X).
dm::Quat roadOrientation(const TrackSection &sec, float s)
{
    // Yaw about world up, then pitch about the car's right axis to match the
    // slope (positive pitch = nose up).
    float pitch = std::atan(sec.gradeAt(s));
    return dm::quatYaw(sec.yawAt(s)) * dm::quatAxisAngle({1, 0, 0}, pitch);
}

// Mark the car as wrecked (once). Records the impact speed used later to
// compute the respawn speed and bumps the death counter.
void killCar(CarSimState &s)
{
    if (!s.alive) return;
    s.alive = false;
    s.crashed = true;
    s.crashSpeed = s.speed > dm::length(s.velocity) ? s.speed : dm::length(s.velocity);
    s.deadTimer = 0;
    s.drifting = false;
    s.nitroActive = false;
    s.deaths += 1;
}

// True when every component is a finite number (NaN/Inf guard).
bool finite(const dm::Vec3 &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Respawn (our design -- drifto restarted the scene): on the centreline a
// little behind the last on-road point, aligned with the road, wheels on the
// surface, no rotation, carrying a fraction of the pre-crash speed; ghosted
// for a moment and with steering fading back in. Pure function of the state.
void respawn(CarSimState &s, const Track &track, const CarParams &p)
{
    uint32_t sec = s.lastGoodSection;
    float along = s.lastGoodS - p.respawnBackoff;
    while (along < 0.0f && sec > 0)
    {
        --sec;
        along += track.section(sec).length;
    }
    const TrackSection &ts = track.section(sec);
    along = dm::clampf(along, 0.5f, std::fmax(0.5f, ts.length - 0.5f));
    float speed = std::fmax(p.respawnMinSpeed, p.respawnSpeedFraction * s.crashSpeed);
    CarSimState fresh = spawnCar(track, sec, along, 0.0f, speed, p);
    fresh.nitro = s.nitro;
    fresh.nitroRechargeScale = s.nitroRechargeScale;
    fresh.deaths = s.deaths;
    fresh.takedowns = s.takedowns;
    fresh.rescues = s.rescues;
    fresh.ticks = s.ticks;
    fresh.ghostTimer = p.respawnGhostTime;
    fresh.steerRamp = 0.0f;
    s = fresh;
}

} // namespace

// Shared default tuning; function-local static so it is built once.
const CarParams &defaultCarParams()
{
    static const CarParams params{}; // immutable: safe to share
    return params;
}

// Create a car resting on the road at arc length s / lateral offset d of the
// given section, facing along the road, with initial forward speed (m/s).
CarSimState spawnCar(const Track &track, uint32_t section, float s, float d, float speed, const CarParams &params)
{
    const TrackSection &sec = track.section(section);
    CarSimState st;
    st.section = sec.index;
    st.sectionS = s;
    st.lateral = d;
    st.rotation = roadOrientation(sec, s);
    st.position = sec.pointAt(s, d) + sec.normalAt(s) * params.rideHeight;
    st.velocity = dm::forwardOf(st.rotation) * speed;
    st.speed = speed;
    st.gravityDir = -sec.normalAt(s);
    st.distance = sec.startDistance + s;
    st.nitro = params.nitroStart;
    st.lastGoodSection = sec.index;
    st.lastGoodS = s;
    // Start the suspension at its loaded rest so the first tick doesn't kick.
    float sag = -(params.gravity * params.mass * 0.25f) / params.springConstant;
    for (float &l : st.suspensionLast) l = sag;
    return st;
}

// Starting-grid placement for slot 0..3 (clamped): two columns, rows behind
// the start line, all at speed 0.
CarSimState spawnOnGrid(const Track &track, int slot, const CarParams &params)
{
    // Two abreast, rows 8 car-scaled metres apart, the right column staggered
    // back: close enough that the first bend is contested.
    slot = slot < 0 ? 0 : (slot > 3 ? 3 : slot);
    float d = (slot % 2 == 0) ? -1.6f : 1.6f;
    float s = 30.0f - static_cast<float>(slot / 2) * 8.0f * kCarScale - static_cast<float>(slot % 2) * 3.0f * kCarScale;
    return spawnCar(track, 0, s, d, 0.0f, params);
}

// Signed angle (degrees) between the car's heading and its velocity,
// measured in the car's own ground plane. 0 when (almost) stationary.
float driftAngleDeg(const dm::Quat &rotation, const dm::Vec3 &velocity)
{
    dm::Vec3 up = dm::upOf(rotation), fwd = dm::forwardOf(rotation);
    dm::Vec3 vp = velocity - up * dm::dot(velocity, up);
    float sp = dm::length(vp);
    if (sp < 1e-3f) return 0.0f;
    dm::Vec3 vd = vp / sp;
    // Angle from velocity to heading about the car's up axis (+ = CCW = nose left).
    float sinA = dm::dot(dm::cross(vd, fwd), up);
    float cosA = dm::dot(vd, fwd);
    return dm::degrees(std::atan2(sinA, cosA));
}

// Advance one car by dt seconds. rawInput is the player's command for this
// tick; s is updated in place. Order of work: input smoothing, suspension,
// nitro, forces, integration, wall constraints, track lookup, drift/nitro
// recharge, death checks, then the lost/fall-through safety net and respawn.
void stepCar(CarSimState &s, const net::CarInput &rawInput, const Track &track, const CarParams &p, float dt)
{
    s.ticks += 1;

    // Dead cars get no input; they keep simulating (and falling) until respawn.
    net::CarInput in = rawInput;
    if (!s.alive) in = holdInput(rawInput.tick);
    const float steerTarget = dm::clampf(in.steer, -1.0f, 1.0f);
    const float throttle = dm::clampf(in.throttle, 0.0f, 1.0f);

    s.impactWall = 0; // per-tick event values (stepWorld fills impactCar after us)
    s.ghostTimer = std::fmax(0.0f, s.ghostTimer - dt);
    s.steerRamp = std::fmin(1.0f, s.steerRamp + dt / p.respawnSteerRamp);

    // Steering smoothing is part of the simulated state so replays match.
    {
        bool releasing = steerTarget == 0.0f || steerTarget * s.steer < 0.0f;
        s.steer = moveTowards(s.steer, steerTarget, (releasing ? p.steerReturnRate : p.steerRate) * dt);
    }

    const dm::Vec3 fwd = dm::forwardOf(s.rotation);
    const dm::Vec3 up = dm::upOf(s.rotation);

    dm::Vec3 force{0, 0, 0};  // world, N (== m/s^2 with mass 1)
    dm::Vec3 torque{0, 0, 0}; // world, about the COM
// Forces and torques are accumulated here and integrated once below.

    // ---- suspension: one ray per wheel along -up (Car_Suspension) ---------
    // distFromRest = how far the ground is below the wheel mount along -up;
    // the spring only pushes when the ground is above the mount (negative),
    // damping uses the change since last tick, exactly like the original.
    s.wheelContactMask = 0;
    for (int i = 0; i < 4; ++i)
    {
        dm::Vec3 mount = s.position + dm::rotate(s.rotation, p.wheelLocal[i]);
        RoadQuery q = track.query(mount, s.section);
        bool hit = false;
        float dist = 0.0f;
        if (q.supported)
        {
            dm::Vec3 origin = mount + up * p.rayStartAbove;
            dm::Vec3 planePoint{mount.x, q.height, mount.z};
            float denom = -dm::dot(q.normal, up); // n . rayDir, rayDir = -up
            if (denom < -0.2f)
            {
                float t = dm::dot(q.normal, planePoint - origin) / denom;
                dist = t - p.rayStartAbove;
                if (t >= 0.0f && dist <= p.maxDistance)
                {
                    hit = true;
                    s.wheelContact[i] = origin - up * t;
                    s.wheelContactMask |= static_cast<uint8_t>(1u << i);
                }
            }
        }
        if (hit)
        {
            float dampingForce = ((dist - s.suspensionLast[i]) / dt) * p.dampingConstant;
            float mag = dm::clampf(-dist * p.springConstant - dampingForce, 0.0f, p.maxSpringForce);
            dm::Vec3 f = up * mag;
            force += f;
            torque += dm::cross(mount - s.position, f);
        }
        s.suspensionLast[i] = hit ? dist : 0.0f;
    }
    // Wheel layout: indices 0-1 are the front wheels (mask 0x3), 2-3 the rear
    // wheels (mask 0xC). Rear wheels provide drive and steering authority.
    const bool frontGrounded = (s.wheelContactMask & 0x3u) != 0;
    const bool rearGrounded = (s.wheelContactMask & 0xCu) != 0;

    // ---- nitro (new): burns while held, only with the rear wheels down ----
    const bool burning = in.nitro && s.nitro > 0.0f && rearGrounded && s.alive && throttle > 0.0f;
    s.nitroActive = burning;
    if (burning) s.nitro = std::fmax(0.0f, s.nitro - p.nitroBurnRate * dt);

    // ---- gravity (Car_Gravity): along -up while grounded or nearly level --
    if (rearGrounded || frontGrounded || dm::dot(up, kWorldUp) > std::cos(dm::radians(p.gravityMaxAngleDeg)))
        s.gravityDir = -up;
    force += s.gravityDir * (p.gravity * p.mass);

    // ---- drag: |v|^2 * drag against v (GetDragForce) ---------------------
    // Nitro and slipstream lower it (=> higher top speed).
    float dragK = p.drag * (burning ? p.nitroDragScale : 1.0f) * (s.slipstream ? p.slipstreamDragScale : 1.0f);
    float speed = dm::length(s.velocity);
    force -= s.velocity * (dragK * speed);

    // ---- engine + steering: rear wheels must be grounded -------------------
    if (rearGrounded)
    {
        float drive = p.driveForce + (burning ? p.nitroAccel : 0.0f) + (s.slipstream ? p.slipstreamAccel : 0.0f);
        force += fwd * (drive * throttle * p.mass);

        // Steering = direct yaw angular acceleration about the body up axis
        // (rb.angularVelocity += -transform.up * steer * maxAngAccel * dt).
        // Positive steer is right, i.e. negative rotation about +Y. Nitro's
        // price is steering authority.
        float authority = (burning ? p.nitroSteerScale : 1.0f) * s.steerRamp;
        s.angularVelocity += up * (-s.steer * p.maxYawAccel * authority * dt);
    }

    // Velocity uses force/mass; angular velocity uses torque converted to the
    // body frame, divided by the per-axis inertia, and rotated back to world.
    // ---- integrate (semi-implicit Euler) ------------------------------------
    s.velocity += force * (dt / p.mass);

    dm::Vec3 torqueLocal = dm::rotate(dm::conjugate(s.rotation), torque);
    dm::Vec3 alphaLocal{torqueLocal.x / p.inertia.x, torqueLocal.y / p.inertia.y, torqueLocal.z / p.inertia.z};
    s.angularVelocity += dm::rotate(s.rotation, alphaLocal) * dt;

    // Rigidbody.angularDrag on every axis (exponential form, see CarParams).
    s.angularVelocity *= std::exp(-p.angularDrag * dt);
    {
        float w = dm::length(s.angularVelocity);
        if (w > p.maxAngularSpeed) s.angularVelocity *= p.maxAngularSpeed / w;
    }
    // Collision spin decays on its own, much slower than the angular drag.
    s.spinRate *= std::exp(-p.spinDecay * dt);

    s.position += s.velocity * dt;
    // Orientation update: q += 0.5 * dt * (w, 0) * q, then renormalise.
    {
        dm::Vec3 w = s.angularVelocity + up * s.spinRate;
        dm::Quat dq = dm::Quat{w.x, w.y, w.z, 0.0f} * s.rotation;
        float h = 0.5f * dt;
        s.rotation = dm::normalize(dm::Quat{s.rotation.x + dq.x * h, s.rotation.y + dq.y * h,
                                            s.rotation.z + dq.z * h, s.rotation.w + dq.w * h});
    }

    // ---- barriers: walls / guardrails along the road edges ------------------
    // Body corners are kept inside the edge where the side is solid. Like the
    // original's "Crashable" cubes with the Slippery material: no friction,
    // no bounce, and an impact above crashImpactSpeed wrecks the car.
    {
        const float cx[4] = {-p.halfWidth, p.halfWidth, -p.halfWidth, p.halfWidth};
        const float cz[4] = {-p.halfLength, -p.halfLength, p.halfLength, p.halfLength};
        for (int i = 0; i < 4; ++i)
        {
            dm::Vec3 corner = s.position + dm::rotate(s.rotation, dm::Vec3{cx[i], 0.0f, cz[i]});
            RoadQuery q = track.query(corner, s.section, /*withNormal=*/false);
            const float limit = q.edge - 0.05f; // per side: runoff widens it
            if (std::fabs(q.d) <= limit) continue;
            if (corner.y < q.height - 1.5f) continue; // already below the road: falling
            int side = q.d > 0 ? 1 : -1;
            const TrackSection &sec = track.section(q.section);
            dm::Vec3 n = sec.rightAt(q.s) * static_cast<float>(side); // outward, horizontal
            float pen = std::fabs(q.d) - limit;
            s.position -= n * pen;
            float vn = dm::dot(s.velocity, n);
            if (vn > 0.0f)
            {
                s.impactWall = std::fmax(s.impactWall, vn); // audio / effects hook
                if (vn > p.crashImpactSpeed) killCar(s);
                // Normal impulse vn*(1+e); Coulomb friction scrubs up to
                // mu * that from the tangential speed.
                float jn = vn * (1.0f + p.wallRestitution);
                s.velocity -= n * jn;
                dm::Vec3 vt = s.velocity - n * dm::dot(s.velocity, n);
                vt.y = 0;
                float vtLen = dm::length(vt);
                // Only real impacts scrub speed: the always-on drive pressing
                // a stopped car into the wall must not glue it there.
                if (vtLen > 1e-4f && p.wallFriction > 0 && vn > p.wallFrictionMinImpact)
                    s.velocity -= vt * (std::fmin(p.wallFriction * jn, vtLen) / vtLen);
            }
        }
    }

    // ---- where are we on the track ------------------------------------------
    // The track query also yields the section index so the next tick starts its
    // search near the car (sectionHint) instead of scanning the whole track.
    RoadQuery here = track.query(s.position, s.section, /*withNormal=*/false);
    s.section = here.section;
    s.sectionS = here.s;
    s.lateral = here.d;
    s.distance = here.distance;
    s.speed = dm::length(s.velocity);

    // ---- drift detection (Car_PlayerMovement.IsDrifting) + nitro recharge --
    {
        float angle = rearGrounded ? driftAngleDeg(s.rotation, s.velocity) : 0.0f;
        dm::Vec3 vPlanar = s.velocity - dm::upOf(s.rotation) * dm::dot(s.velocity, dm::upOf(s.rotation));
        float planarSpeed = dm::length(vPlanar);
        s.driftAngle = angle;
        s.drifting = s.alive && planarSpeed > p.minDriftSpeed && std::fabs(angle) > p.driftAngleThreshold &&
                     std::fabs(angle) < 110.0f; // spinning backwards isn't a drift
        if (s.drifting && !burning)
        {
            float k = std::fmin(std::fabs(angle) / p.nitroRechargeRefAngle, p.nitroRechargeMaxFactor);
            s.nitro = std::fmin(1.0f, s.nitro + p.nitroRechargeRate * k * s.nitroRechargeScale * dt);
        }
    }

    // ---- death (Car_Death + leaving the road) --------------------------------
    if (s.alive)
    {
        bool upright = dm::dot(dm::upOf(s.rotation), kWorldUp) > 0.3f;
        if (here.onRoad && upright && s.wheelContactMask != 0)
        {
            s.lastGoodSection = here.section;
            s.lastGoodS = here.s;
        }
        s.offRoadTimer = here.supported ? 0.0f : s.offRoadTimer + dt;
        s.flippedTimer = upright ? 0.0f : s.flippedTimer + dt;
        if (s.offRoadTimer > p.maxOffRoadTime || s.flippedTimer > p.maxFlippedTime) killCar(s);
    }
    // Safety net: never let a car live (or fall forever) under the road --
    // whatever got it there, it comes straight back. Also catches NaNs.
    bool lost = !finite(s.position) || !finite(s.velocity) || !finite(s.angularVelocity) ||
                !std::isfinite(s.rotation.w) || !std::isfinite(s.spinRate);
    if (lost || s.position.y < here.height - (s.alive ? p.rescueDepth : p.fallDepth))
    {
        if (s.alive && !lost) ++s.rescues; // a live car under the road = a fall-through
        if (lost) { s.velocity = {0, 0, 0}; s.speed = 0; }
        killCar(s);
        if (p.autoRespawn) respawn(s, track, p);
        return;
    }
    if (!s.alive)
    {
        s.deadTimer += dt;
        if (p.autoRespawn && s.deadTimer >= p.respawnDelay) respawn(s, track, p);
    }
}

// Pack the simulation state into the compact network snapshot. Only the
// fields peers need are sent; flags are bit-packed.
// --------------------------------------------------------------------------
// network conversions
// --------------------------------------------------------------------------

net::CarState toNetState(const CarSimState &s, int32_t playerId, uint32_t lastInputTick)
{
    net::CarState n;
    n.playerId = playerId;
    n.lastInputTick = lastInputTick;
    n.position = s.position;
    n.rotation = s.rotation;
    n.velocity = s.velocity;
    n.angularVelocity = s.angularVelocity;
    n.distance = s.distance;
    n.trackSection = s.section;
    n.nitro = s.nitro;
    n.flags = static_cast<uint8_t>((s.alive ? net::kCarAlive : 0) | (s.drifting ? net::kCarDrifting : 0) |
                                   (s.crashed ? net::kCarCrashed : 0) | (s.nitroActive ? net::kCarNitro : 0) |
                                   (s.slipstream ? net::kCarSlipstream : 0) | (s.ghostTimer > 0 ? kCarFlagGhost : 0));
    return n;
}

// Overwrite the replicated fields from a received snapshot. Fields that are
// not sent (timers, steering, suspension memory) are left as they were.
void applyNetState(CarSimState &s, const net::CarState &n)
{
    s.position = n.position;
    s.rotation = n.rotation;
    s.velocity = n.velocity;
    s.angularVelocity = n.angularVelocity;
    s.distance = n.distance;
    s.section = n.trackSection;
    s.nitro = n.nitro;
    s.alive = (n.flags & net::kCarAlive) != 0;
    s.drifting = (n.flags & net::kCarDrifting) != 0;
    s.crashed = (n.flags & net::kCarCrashed) != 0;
    s.nitroActive = (n.flags & net::kCarNitro) != 0;
    s.slipstream = (n.flags & net::kCarSlipstream) != 0;
    // Remaining ghost time isn't sent: keep ours if we agree, else a short one.
    if (!(n.flags & kCarFlagGhost)) s.ghostTimer = 0;
    else if (s.ghostTimer <= 0) s.ghostTimer = 0.25f;
    s.speed = dm::length(n.velocity);
    s.driftAngle = driftAngleDeg(n.rotation, n.velocity);
}

// Build a fresh sim state from a snapshot (e.g. for a remote car's first sight).
CarSimState fromNetState(const net::CarState &n)
{
    CarSimState s;
    applyNetState(s, n);
    s.gravityDir = -dm::upOf(n.rotation);
    return s;
}

void wreckCar(CarSimState &s) { killCar(s); }

uint64_t hashState(const CarSimState &s)
{
    // FNV-1a over every field's bytes (field by field: no struct padding).
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void *data, size_t n) {
        const unsigned char *b = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    auto v3 = [&mix](const dm::Vec3 &v) { mix(&v.x, 4); mix(&v.y, 4); mix(&v.z, 4); };
    v3(s.position);
    mix(&s.rotation.x, 4); mix(&s.rotation.y, 4); mix(&s.rotation.z, 4); mix(&s.rotation.w, 4);
    v3(s.velocity);
    v3(s.angularVelocity);
    v3(s.gravityDir);
    mix(&s.distance, 4); mix(&s.section, 4); mix(&s.nitro, 4);
    unsigned char flags[5] = {s.alive, s.drifting, s.crashed, s.nitroActive, s.slipstream};
    mix(flags, 5);
    mix(&s.nitroRechargeScale, 4); mix(&s.spinRate, 4);
    mix(s.suspensionLast, sizeof s.suspensionLast);
    mix(&s.steer, 4); mix(&s.sectionS, 4); mix(&s.lateral, 4); mix(&s.driftAngle, 4); mix(&s.speed, 4);
    mix(&s.offRoadTimer, 4); mix(&s.flippedTimer, 4); mix(&s.deadTimer, 4);
    mix(&s.ghostTimer, 4); mix(&s.steerRamp, 4); mix(&s.lastGoodSection, 4); mix(&s.lastGoodS, 4);
    mix(&s.crashSpeed, 4); mix(&s.rescues, 4);
    mix(&s.deaths, 4); mix(&s.ticks, 4); mix(&s.wheelContactMask, 1);
    mix(&s.takedowns, 4);
    return h;
}

} // namespace game
