// car_physics.h -- deterministic fixed-step car model, a faithful port of
// drifto-old's Car prefab (Car_PlayerMovement, Car_Suspension, Car_Gravity,
// Car_Death) plus the elimination-race additions (nitro, slipstream, spin
// from car-to-car hits).
//
// Networking contract: stepCar() is a pure function of
// (CarSimState, CarInput, Track, CarParams, dt). All state lives in the
// copyable CarSimState -- no statics, no wall-clock, no frame-rate
// dependence -- so the host can simulate every car from inputs and clients
// can rewind to a snapshot and replay their input history. Multi-car
// interaction (slipstream, collisions) lives in race.h's stepWorld().
//
// The original model (all kept):
//   * one rigid body, mass 1 => every force is an acceleration;
//   * drag = drag * |v|^2 against v                     (GetDragForce)
//   * drive = driveForce * throttle along the heading,  (GetDriveForce)
//     only while the rear wheels are grounded; throttle is always 1;
//   * steering = direct yaw angular acceleration about the body up axis,
//     only while grounded, plus angular drag on every axis;
//   * NO tyre friction: the "Slippery" material means sideways velocity only
//     decays through drag and the drive force pulling the car round -- the
//     car drifts by construction;
//   * 4 raycast spring/damper wheels along the body up axis (Car_Suspension);
//   * gravity along -up while grounded / nearly level (Car_Gravity);
//   * wrecked by hitting a barrier too hard (Car_Death).
//
// Scaling: our track is the original's * 0.75 (real-size cars). The
// constants keep the original's dimensionless ratios -- drag*R (how much the
// car must drift to make a hairpin) and maxYawRate*R/vTop (steering
// authority) -- see the per-field comments.
//
// Deviations kept on purpose (listed with reasons in the report):
//   * steering input is smoothed in the sim (keyboard is digital; the
//     original read an analog touch position);
//   * fall / off-road / flipped death + respawn (the original only died on
//     barrier impacts; we need it for open drop-offs and for racing);
//   * drift detection ignores speeds below minDriftSpeed (angle noise at rest);
//   * nitro (with its controllability penalty), slipstream and collision spin.
#pragma once

#include <cmath>
#include <cstdint>

#include "common/dmath.h"
#include "game/include/track.h"
#include "net/include/net_client.h"

namespace game {

constexpr int kTickRate = 60;

// Size of the car relative to the road (1 = the real ~4.2 m AE86 on our
// 9 m road). Everything tied to the car's size derives from it: body box,
// wheel / suspension points, ride height, inertia, the drawn model, camera
// distance, slipstream wake, grid spacing, skid width.
constexpr float kCarScale = 0.7f;
constexpr float kFixedDt = 1.0f / 60.0f;

enum Wheel : int { WheelFL = 0, WheelFR = 1, WheelRL = 2, WheelRR = 3 };

struct CarParams
{
    // --- body (Car.prefab Rigidbody: mass 1, angularDrag 36, own gravity) --
    float mass = 1.0f;
    // local diag: pitch (X), yaw (Y), roll (Z); scales with size^2 (mass 1),
    // which keeps the suspension's damping ratios unchanged.
    dm::Vec3 inertia{1.9f * kCarScale * kCarScale, 2.0f * kCarScale * kCarScale, 0.65f * kCarScale * kCarScale};
    float halfWidth = 0.85f * kCarScale;  // body box (barriers, car-to-car)
    float halfLength = 2.15f * kCarScale;
    float rideHeight = 0.45f * kCarScale - 0.0245f; // COM above road at rest: mount - sag
    float maxAngularSpeed = 7.0f;        // Unity's default maxAngularVelocity

    // --- engine / drag (Car_PlayerMovement: driveForce 6, drag 0.02) -------
    // First port kept the original's ratios (drag*R = 0.47 => drive 12.9,
    // drag 0.0267, vTop 22 m/s, i.e. exactly as hard as drifto).
    // RE-TUNED with the smaller car (kCarScale 0.7): the user found turning,
    // hairpins especially, too hard. With frictionless tyres the velocity can
    // only swing round at ~drive/speed, so turning authority = drive/vTop:
    // 17/21 = 0.81 instead of 12.9/22 = 0.59 (+38%) -- a hairpin is now
    // takeable at ~17 m/s instead of needing 13. vTop = sqrt(17/0.0386) = 21 m/s.
    float driveForce = 17.0f;
    float drag = 0.0386f;

    // --- steering (maxAngularAcceleration 9000 deg/s^2, angularDrag 36) ----
    // Unity/PhysX removed min(1, 36 * 0.02) = 72 % of the angular velocity
    // every 0.02 s step: an exponential rate of -ln(0.28)/0.02 = 63.6/s, and
    // a steady yaw rate of 157 * 0.28 / 0.72 = 1.22 rad/s. Keeping
    // yawRate * R / vTop (1.66) gave 2.08 rad/s; re-tuned to 2.4 (= 152.6 /
    // 63.6) together with the drive force above.
    float maxYawAccel = 152.6f;     // rad/s^2 at full lock: 2.4 rad/s yaw cap (was 2.08)
    float angularDrag = 63.6f;      // 1/s, every axis (like Rigidbody.angularDrag)
    float steerRate = 6.0f;         // input smoothing, units/s towards target
    float steerReturnRate = 10.0f;  // faster when releasing / reversing

    // --- suspension (Car_Suspension: k 100, c 15, maxForce 10, maxDist 0.5,
    //     minDistance -2, verticalOffset -0.13) ---------------------------------
    dm::Vec3 wheelLocal[4] = {{-0.78f * kCarScale, -0.45f * kCarScale, -1.28f * kCarScale},
                              {0.78f * kCarScale, -0.45f * kCarScale, -1.28f * kCarScale},
                              {-0.78f * kCarScale, -0.45f * kCarScale, 1.28f * kCarScale},
                              {0.78f * kCarScale, -0.45f * kCarScale, 1.28f * kCarScale}};
    float springConstant = 100.0f;
    float dampingConstant = 15.0f;
    float maxSpringForce = 10.0f; // per wheel
    float maxDistance = 0.5f * kCarScale; // ray reaches this far below the mount
    float rayStartAbove = 2.0f;   // -minDistance

    // --- gravity (Car_Gravity: 9.8, maxAngle 30) ----------------------------
    float gravity = 9.8f;
    float gravityMaxAngleDeg = 30.0f;

    // --- drift (driftAngleThreshold 3, maxVisualSteeringAngle 40) ----------
    float driftAngleThreshold = 3.0f;     // degrees
    float minDriftSpeed = 3.0f;           // m/s (deviation: angle is noise near rest)
    float maxVisualSteeringAngle = 40.0f; // > this: front tyres slide too (front skid marks)

    // --- nitro (new): only recharges while drifting ----------------------------
    float nitroStart = 0.25f;          // gauge at the start / after respawn
    float nitroAccel = 10.0f;          // extra drive while burning
    float nitroDragScale = 0.72f;      // lower drag => vTop 21 -> ~31 m/s
    float nitroBurnRate = 0.45f;       // gauge per second (full tank ~2.2 s)
    float nitroRechargeRate = 0.12f;   // gauge/s at nitroRechargeRefAngle, x race multiplier
    float nitroRechargeRefAngle = 30.0f;
    float nitroRechargeMaxFactor = 2.0f; // angle factor cap (60 deg+)
    float nitroSteerScale = 0.55f;     // the price: much less steering authority

    // --- slipstream (new; whether we're in a wake is decided by stepWorld) -
    float slipstreamAccel = 2.6f;      // flat extra drive
    float slipstreamDragScale = 0.86f; // vTop 21 -> ~23.4 m/s

    // --- collision spin (new): yaw from off-centre car-to-car hits ---------
    // The original's angular drag kills any yaw impulse within ~50 ms, so a
    // bump could never spin a car; hits feed a separate, slower-decaying
    // spin rate instead.
    float spinDecay = 2.2f;            // 1/s
    float maxSpinRate = 6.0f;          // rad/s

    // --- death / respawn ----------------------------------------------------------
    // DEVIATION. Car_Death wrecked the car on any barrier hit above 3 m/s
    // (impulse/mass) -- 3.8 m/s at our speeds -- and restarted the run: a
    // one-life endless runner. In a 4-car contact race with respawns that
    // makes the result a wreck lottery (even a look-ahead bot wrecks ~20x a
    // minute). Instead barriers have Coulomb friction (wall contact costs
    // speed, so wall-riding is not a shortcut) and only big impacts wreck.
    float crashImpactSpeed = 14.0f;
    float wallRestitution = 0.0f;
    float wallFriction = 0.6f;
    float wallFrictionMinImpact = 1.0f; // m/s: gentler contact slides freely (Slippery)
    float fallDepth = 4.0f;          // metres below the road surface
    float maxOffRoadTime = 2.0f;
    float maxFlippedTime = 1.5f;
    bool autoRespawn = true;
    float respawnDelay = 1.2f;       // wrecked time before the car comes back
    float respawnBackoff = 5.0f;     // metres behind the last on-road point
    float respawnSpeedFraction = 0.6f; // of the pre-crash speed...
    float respawnMinSpeed = 8.0f;    // ...but never a dead stop
    float respawnGhostTime = 1.5f;   // no car-to-car contact, drawn translucent
    float respawnSteerRamp = 0.3f;   // seconds for steering authority to come back
    float rescueDepth = 2.0f;        // safety net: body this far under the road => respawn now
};

const CarParams &defaultCarParams();

struct CarSimState
{
    // Replicated in net::CarState ----------------------------------------
    dm::Vec3 position;        // centre of mass, world
    dm::Quat rotation;
    dm::Vec3 velocity;
    dm::Vec3 angularVelocity; // world, rad/s
    float distance = 0;       // centreline distance from track start (race progress)
    uint32_t section = 0;
    float nitro = 0;          // gauge 0..1
    bool alive = true;
    bool drifting = false;
    bool crashed = false;
    bool nitroActive = false; // burning this tick
    bool slipstream = false;  // in a wake this tick (set by stepWorld before stepping)
    float ghostTimer = 0;     // > 0 after a respawn: no car-to-car contact (replicated as a flag)

    // Local-only (restored from the client's own history on reconcile) ------
    float nitroRechargeScale = 1.0f; // race rubber-band multiplier (set by the race logic)
    float spinRate = 0;       // yaw rate from collisions (rad/s, + = CCW / left)
    dm::Vec3 gravityDir{0, -1, 0};
    float suspensionLast[4] = {0, 0, 0, 0};
    float steer = 0;          // smoothed steering, -1..1 (+ right)
    float sectionS = 0;       // arc length within `section`
    float lateral = 0;        // lateral offset from the centreline
    float driftAngle = 0;     // degrees, + = nose left of velocity
    float speed = 0;          // |v|, m/s
    float offRoadTimer = 0;
    float flippedTimer = 0;
    float deadTimer = 0;
    float steerRamp = 1.0f;   // 0..1 steering authority after a respawn
    uint32_t lastGoodSection = 0; // newest on-road, upright, grounded progress point
    float lastGoodS = 0;
    float crashSpeed = 0;     // speed when wrecked (respawn speed derives from it)
    uint32_t deaths = 0;
    uint32_t rescues = 0;     // safety-net respawns (should stay 0: means a fall-through)
    // Per-tick events for audio / effects (not part of the replicated state):
    // normal speed into a barrier this tick (m/s), and the strongest car-to-
    // car impulse this tick (m/s change), 0 when nothing happened.
    float impactWall = 0;
    float impactCar = 0;
    uint32_t ticks = 0;
    uint8_t wheelContactMask = 0; // bit i = wheel i touching the road
    dm::Vec3 wheelContact[4];     // contact points (valid per mask)
};

// Place a car at rest on the road at (section, s, d), facing along the road.
CarSimState spawnCar(const Track &track, uint32_t section, float s, float d, float speed = 0.0f,
                     const CarParams &params = defaultCarParams());
// Grid slot 0..3: two abreast, the right column staggered 3 m back.
CarSimState spawnOnGrid(const Track &track, int slot, const CarParams &params = defaultCarParams());

// THE fixed step for one car. Uses input.steer and input.nitro; throttle is
// taken from input.throttle (1 while racing, 0 to hold on the grid);
// brake/handbrake are ignored (drifto has neither).
void stepCar(CarSimState &state, const net::CarInput &input, const Track &track, const CarParams &params,
             float dt = kFixedDt);
inline void stepCar(CarSimState &state, const net::CarInput &input, const Track &track, float dt = kFixedDt)
{
    stepCar(state, input, track, defaultCarParams(), dt);
}

// Racing input: auto-throttle, steer, nitro.
inline net::CarInput driveInput(uint32_t tick, float steer, bool nitro)
{
    net::CarInput in;
    in.tick = tick;
    in.steer = steer;
    in.throttle = 1.0f;
    in.nitro = nitro;
    return in;
}
// Grid hold (countdown / results): no throttle, no steering, no nitro.
inline net::CarInput holdInput(uint32_t tick)
{
    net::CarInput in;
    in.tick = tick;
    return in;
}

// --- conversions for the network layer --------------------------------------
// place / eliminatedOrder are filled in by the race logic.
// The car is ghosted after a respawn (reserved by NET as net::kCarGhost).
constexpr uint8_t kCarFlagGhost = net::kCarGhost;
net::CarState toNetState(const CarSimState &state, int32_t playerId, uint32_t lastInputTick);
// Overwrite the replicated fields of `state`, keep the local-only ones.
void applyNetState(CarSimState &state, const net::CarState &net);
CarSimState fromNetState(const net::CarState &net);

// --- helpers for HUD / effects ---------------------------------------------
// Signed angle (degrees) between heading and planar velocity, + = nose left.
float driftAngleDeg(const dm::Quat &rotation, const dm::Vec3 &velocity);
inline bool isFrontWheelDrift(const CarSimState &s, const CarParams &p = defaultCarParams())
{
    return s.drifting && std::fabs(s.driftAngle) > p.maxVisualSteeringAngle;
}
// Bitwise hash of the full state, for determinism tests / desync detection.
uint64_t hashState(const CarSimState &state);

} // namespace game
