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

#include "game/include/bot_driver.h"
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
    // Crash feel (spinScale was 1.4). Our tyres have no grip, so whatever
    // sideways velocity a contact leaves is kept: with e = 0.3 both cars
    // leave a sideswipe with similar sideways speed and slide off together
    // (re-touching for ~5 ticks), and spinScale 1.4 -- spin decays slowly,
    // 2.2/s -- yawed BOTH cars ~28 deg in a 3 m/s passing bump: that is what
    // made recovering hard. spinScale 0.7 halves the yaw kick; the ram bias
    // below keeps a real hitter on its line. (e = 0.5 separated a sideswipe
    // in one tick, but against the client's immovable proxies it doubled
    // the predicted bump count -- jittery online -- so e stays 0.3.)
    float restitution = 0.3f;
    float friction = 0.35f;
    float spinScale = 0.7f;            // yaw from off-centre hits (feeds CarSimState::spinRate)
    float maxHeightDiff = 1.3f * kCarScale; // cars on different levels don't touch
    float penetrationSlop = 0.01f;
    // Contact feel. Real hits (closing speed along the normal above
    // rammingSpeed) favour the car that did the hitting: it acts up to
    // (1 + ramBias) / (1 - ramBias) times heavier, so it is not dragged off
    // its line and the victim takes the impact. Light touches stay
    // symmetric (momentum conserving).
    float rammingSpeed = 4.0f;      // m/s: bias starts here...
    float rammingSpeedFull = 10.0f; // ...and is full here
    float ramBias = 0.5f;           // 0 = always symmetric

    // --- takedowns ----------------------------------------------------------------
    // A car hit much harder than it hits back is WRECKED (the barrier-wreck
    // path: dead, ghosted, respawned) when the closing speed along the
    // contact normal is >= takedownSpeed and the attacker supplies enough of
    // it: dominance = (attacker approach - victim approach) / closing must be
    // >= takedownDominance (rear-end > 1, T-bone 1, equal head-on 0), and
    // the attacker must be clearly faster overall. Speeds are the bodies'
    // (centre of mass), so a spinning car's swinging tail is no ram. The
    // attacker only takes takedownAttackerShare of the impulse (it keeps
    // most of its momentum).
    bool takedowns = true;
    float takedownSpeed = 9.0f;
    float takedownDominance = 0.5f;
    float takedownSpeedAdvantage = 3.0f; // attacker |v| - victim |v| (m/s): clearly faster
    float takedownAim = 0.6f;            // attacker approach / its speed: cos(travel, normal)
    float takedownAttackerShare = 0.2f;

    // --- prototype (OFF by default): barrier touches wreck ------------------
    // When on, any barrier impact faster than wallTouchSpeed (m/s into the
    // wall) wrecks the car; CarParams::crashImpactSpeed applies regardless.
    bool wallTouchWrecks = false;
    float wallTouchSpeed = 1.0f;

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
// applied to non-proxy cars. A hard enough, one-sided hit is a takedown
// (RaceParams::takedowns): the victim is wrecked, both get takedownEvent.
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

// Race bot: see bot_driver.h (Reynolds steering behaviours + look-ahead
// safety layer).

} // namespace game
