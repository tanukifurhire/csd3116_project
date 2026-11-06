// race.h -- the multi-car part of the simulation and the elimination race.
//
//   stepWorld()       one fixed tick for every car: slipstream detection,
//                     stepCar() per car, then car-to-car collisions. Pure
//                     function of (states, inputs, track, params).
//   EliminationRace   places, the last-place countdown (32,16,8,4,2,1 s),
//                     eliminations, the winner and the nitro rubber band.
//
// Both run only on the host for the authoritative game. Clients run
// stepWorld for their own predicted car against "proxy" remote cars (their
// latest snapshot state dead-reckoned to the predicted tick): proxies push
// the local car exactly like the host would, but are never pushed back --
// the host's snapshot is the truth for them.
#pragma once

#include <cstdint>
#include <vector>

#include "game/include/car_physics.h"
#include "game/include/track.h"
#include "net/include/net_client.h"

namespace game {

struct RaceParams
{
    // --- slipstream ---------------------------------------------------------------
    // Wake dimensions scale with the car (kCarScale): 14 m x +-1.75 m at 0.7.
    float slipstreamRange = 20.0f * kCarScale;     // metres behind the leading car
    float slipstreamMinGap = 1.0f * kCarScale;     // ignore overlapping cars
    float slipstreamHalfWidth = 2.5f * kCarScale;  // lateral half-width of the wake at full range
    float slipstreamNearHalfWidth = 1.2f * kCarScale; // ... and right behind the car (narrow cone)
    float slipstreamMinSpeed = 8.0f;   // the car making the wake must be this fast
    float slipstreamAlign = 0.85f;     // cos(angle) between the two travel directions
    float slipstreamMaxHeight = 2.0f;

    // --- car-to-car collisions (oriented boxes in the road plane) ----------
    float restitution = 0.3f;
    float friction = 0.35f;
    float spinScale = 1.4f;            // yaw from off-centre hits (feeds CarSimState::spinRate)
    float maxHeightDiff = 1.3f * kCarScale; // cars on different levels don't touch
    float penetrationSlop = 0.01f;

    // --- elimination ----------------------------------------------------------------
    float stageTimes[6] = {32, 16, 8, 4, 2, 1};
    // Last place only changes hands once the old last car is this far ahead
    // of the new one: stops two side-by-side cars from burning through the
    // stages in a second through progress noise.
    float overtakeMargin = 1.5f;

    // --- nitro rubber band (recharge multiplier only) ------------------------
    float rechargeLeader = 0.5f;
    float rechargeLast = 2.0f;
};

const RaceParams &defaultRaceParams();

// One car in a world step.
struct WorldCar
{
    int32_t id = 0;
    CarSimState *state = nullptr;
    net::CarInput input;
    bool proxy = false; // not stepped and never pushed (client-side remote car)
};

// Cars are processed in id order regardless of the vector order.
void stepWorld(std::vector<WorldCar> &cars, const Track &track, const CarParams &params, const RaceParams &race,
               float dt = kFixedDt);

// Building blocks (exposed for tests).
bool inSlipstream(const CarSimState &follower, const CarSimState &leader, const RaceParams &race);
// Resolves one pair; returns true if they were touching. Responses are only
// applied to non-proxy cars.
bool collideCars(CarSimState &a, bool aProxy, CarSimState &b, bool bProxy, const CarParams &params,
                 const RaceParams &race);

// Rubber band: 0.5x for the leader, 2.0x for the last car, linear in track
// progress in between. `progress` = distances of the cars still racing.
float rechargeMultiplier(float progress, float leaderProgress, float lastProgress, const RaceParams &race);

class EliminationRace
{
public:
    struct Event
    {
        enum Kind : uint8_t { LastPlaceChanged, Eliminated, Winner };
        Kind kind;
        int32_t playerId;
        uint8_t stage; // stage after the event
    };
    struct Progress
    {
        int32_t playerId;
        float distance;
    };

    explicit EliminationRace(const RaceParams &params = defaultRaceParams()) : p_(params) {}

    // Race goes green: everyone racing, stage 0 (32 s).
    void start(const std::vector<int32_t> &playerIds);
    // Continue a race from a snapshot (host migration).
    void restore(const net::WorldSnapshot &snap);
    // Players who left the room count as eliminated now.
    void removePlayer(int32_t playerId, std::vector<Event> *events = nullptr);
    // One tick. `progress` lists the cars still racing (others are ignored).
    std::vector<Event> update(const std::vector<Progress> &progress, float dt);
    // Overall time cap reached: leader wins.
    void finishByLeader();

    bool started() const { return started_; }
    bool finished() const { return finished_; }
    float timer() const { return timer_; }
    uint8_t stage() const { return stage_; }
    int32_t lastPlaceId() const { return lastPlaceId_; }
    int32_t winnerId() const { return winnerId_; }
    int racingCount() const;
    bool isEliminated(int32_t playerId) const { return eliminatedOrder(playerId) != 0; }
    uint8_t eliminatedOrder(int32_t playerId) const;
    uint8_t place(int32_t playerId) const; // 1 = leader, 0 = eliminated / unknown
    float rechargeMultiplierFor(int32_t playerId) const;
    // Winner first, then the last car eliminated, ..., first eliminated last.
    std::vector<int32_t> finishingOrder() const;
    float stageTime(uint8_t stage) const { return p_.stageTimes[stage < 5 ? stage : 5]; }

private:
    struct Racer
    {
        int32_t id;
        float distance = 0;
        uint8_t eliminatedOrder = 0;
        uint8_t place = 0;
        float multiplier = 1.0f;
    };
    Racer *find(int32_t id);
    const Racer *find(int32_t id) const;
    void rank();
    int32_t currentLast() const;
    void eliminate(int32_t id, std::vector<Event> &events);

    RaceParams p_;
    std::vector<Racer> racers_;
    bool started_ = false, finished_ = false;
    float timer_ = 0;
    uint8_t stage_ = 0;
    int32_t lastPlaceId_ = 0;
    int32_t winnerId_ = 0;
    uint8_t eliminatedCount_ = 0;
};

// Race bot: drives drifto's frictionless car by look-ahead -- it simulates
// its own car (stepCar is cheap and deterministic) for a few two-stage
// steering plans and keeps the one that stays off the barriers, is not too
// fast for the next hairpin and makes the most progress. Ignores other cars.
struct BotDriver
{
    float cornerFactor = 0.76f; // corner speed target sqrt(drive * R * f)
    float speedLook = 40.0f;    // metres past the plan scanned for the tightest curve
    float nitroMinGauge = 0.35f;
    float laneBias = 0.0f;      // preferred lateral offset (variety between bots)
    int replanTicks = 5;

    float steer1 = 0, steer2 = 0;
    int ticksLeft = 0;
    // Recovery: stuck (e.g. nose against a wall) => hold full lock towards
    // the road direction for a moment instead of dithering.
    float stuckTime = 0, recoverTime = 0, recoverSteer = 0;

    net::CarInput drive(const CarSimState &car, const Track &track, const CarParams &params, uint32_t tick);
};

} // namespace game
