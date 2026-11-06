// bot_driver.h -- race bots built from Craig Reynolds' steering behaviours
// ("Steering Behaviors For Autonomous Characters", GDC 1999).
//
// Every tick the bot turns what it sees (its own car, the road, the CURRENT
// states of the other cars -- never their inputs) into a set of steering
// vectors, each a desired change of the car's planar velocity in m/s:
//
//   containment   feelers (velocity direction and +-26 deg) probe the road
//                 edges; a feeler past the safe margin pushes back inward
//   separation    keep a car-length or two of air to every nearby car
//   evasion       a car closing fast from behind (a ram) -> flee sideways
//                 from its predicted position
//   pursuit       aggressive bots hunt a slower car ahead: seek its
//                 predicted intercept point to ram it (takedown)
//   path follow   predict where we'll be in 0.3 s, project that onto the
//                 racing line, seek a point further along it; the target
//                 speed is an "arrival" profile so we are slow enough for
//                 the next corner (overtakes / blocks shift the line)
//
// Action selection (who to hunt, which side to pass, hold back or not) is
// decided once per tick from the present; the behaviours themselves form
// a steering FIELD: steeringField() evaluates them at any car state, with
// the other cars dead-reckoned. They are blended by Reynolds' prioritised,
// truncated running sum (higher priorities spend the budget first) into a
// desired velocity.
//
// Locomotion (Reynolds' third layer) has to turn that into steer for a
// frictionless car whose nose yaws at only ~1.3 rad/s: the ONLY thing that
// changes the velocity is the drive force along the nose, so the desired
// acceleration (desired - current velocity over a time constant, plus drag
// compensation) IS where the nose must point (steerTowards(); overspeed =
// acceleration against the travel direction = throw it sideways and scrub).
// Because the nose is slow, that direct mapping alone reacts too late for
// hairpins, so every few ticks a set of candidate steering plans -- the
// direct mapping in closed loop plus 2-stage constant plans -- is
// simulated ~0.75 s ahead with stepCar and scored by how well it follows
// the steering field along the way (field re-evaluated at the simulated
// states), with wrecks, barrier hits and car contacts (unaligned collision
// avoidance; the hunter's prey is a reward instead) as costs.
//
// Bots drive through the same net::CarInput (steer, nitro; auto throttle)
// and the same stepCar as humans. drive() is deterministic for a given
// (driver state, inputs); it only runs on the host, so its own state does
// not need replicating.
#pragma once

#include <cstdint>
#include <vector>

#include "game/include/car_physics.h"
#include "game/include/track.h"
#include "net/include/net_client.h"

namespace game {

// Per-bot tuning ("personality").
struct BotProfile
{
    const char *name = "Racer";
    float pace = 1.0f;           // corner-speed targets x this (1 = on the limit)
    float topSpeedFactor = 0.9f; // fraction of the nitro top speed it allows itself (nitro use)
    float aggression = 0.35f;    // 0..1: pursuit / ramming / blocking vs. giving way
    int reactionTicks = 1;       // steering lag (60 Hz ticks, max 15)
    float steerNoise = 0.0f;     // 0..1: smooth deterministic steering error
    float feelerScale = 1.0f;    // containment feeler length factor (short = crash-prone)
    float lineSkill = 0.8f;      // 0 = centreline, 1 = full out-in-out racing line
    int planHorizon = 45;        // locomotion look-ahead, ticks (0 = direct mapping only)
    int replanTicks = 5;         // re-plan period, ticks
    int planCandidates = 5;      // 5 = 25 two-stage plans, 3 = 9 (plus the closed-loop plan)
};

enum class BotPreset : uint8_t { Rookie, Racer, Bully, Ace, Count };
BotProfile botPreset(BotPreset preset);
const char *botPresetName(BotPreset preset);

// Another car as the bot sees it: its current (start-of-tick) state.
struct BotOpponent
{
    int32_t id = 0;
    const CarSimState *state = nullptr;
};

struct BotDriver
{
    BotProfile profile;
    uint32_t seed = 0;          // steering-noise stream (set per bot)
    float cornerFactor = 0.76f; // base corner speed target sqrt(drive * R * f)
    float nitroMinGauge = 0.35f;
    float laneBias = 0.0f;      // preferred lateral offset (variety between bots)
    float takedownSpeed = 9.0f; // closing speed needed for a takedown (RaceParams::takedownSpeed)

    // What the behaviours wanted last tick (tests / debug overlay).
    struct Debug
    {
        dm::Vec3 containment, separation, evasion, pursuit, path, desiredVelocity, desiredHeading;
        float targetSpeed = 0;
        float lineOffset = 0;
        float planCost = 0;
        int32_t pursuitTarget = 0;
        bool evading = false, overtaking = false, planned = false, nitro = false;
    };
    Debug debug;

    // drive() state.
    float steerHistory[16] = {};
    uint32_t historyPos = 0;
    int32_t pursuitId = 0;
    float lineShift = 0;        // smoothed overtake / block offset (m)
    int planTicks = 0;          // ticks until the next re-plan
    float planSteer = 0;
    bool planReactive = true;   // the closed-loop direct mapping won the last plan
    float stuckTime = 0, recoverTime = 0, recoverSteer = 0;

    net::CarInput drive(const CarSimState &car, const Track &track, const CarParams &params, uint32_t tick,
                        const std::vector<BotOpponent> &others = {});
};

} // namespace game
