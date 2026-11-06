// multiplayer_session.h -- the elimination race on top of net_client.h's
// host-authoritative model. Transport-agnostic (INetTransport): the real
// NetClient online, LoopbackTransport offline (local host + bots), a fake
// in-process network in the tests.
//
// Host:
//   * every fixed tick: takeInputs() into per-player jitter queues, apply one
//     input per player per tick (repeat the last one if a queue is empty),
//     bots get BotDriver inputs, then stepWorld() advances every car together
//     (slipstream, physics, car-to-car collisions);
//   * EliminationRace computes places, the last-place clock, eliminations,
//     the winner and the nitro rubber band; all of it goes into the snapshot
//     (30 Hz), each car tagged with lastInputTick = newest input applied;
//   * phases: Countdown -> Racing (clock starts) -> Results (winner known)
//     -> Lobby, announced with startMatch / setPhase.
// Non-host:
//   * predicts its own car from local input with stepWorld() against PROXY
//     remote cars: each remote's newest snapshot state dead-reckoned to the
//     tick being predicted (velocity * time since the snapshot's moment,
//     capped). Proxies push us like the host would but are never pushed --
//     so bumps feel immediate, and reconciliation fixes the rest;
//   * on each snapshot: rewind to the stored state at lastInputTick, take the
//     host's replicated fields, replay later inputs (against the proxies of
//     the new snapshot); the jump is bled out visually;
//   * remote cars are drawn at the same dead-reckoned present (so what you
//     see is what you bump), smoothed when a new snapshot moves them.
//     SessionConfig::extrapolateRemotes = false switches to classic
//     100 ms interpolation.
// HostChanged: the new host seeds its cars and the race clock from the
// newest snapshot and continues.
//
// Phase rule shared by host and clients (keeps prediction exact): outside
// Racing every car is driven by holdInput() (no throttle). Clients send the
// input they actually simulated, so the host replays exactly the same.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "game/include/car_physics.h"
#include "game/include/race.h"
#include "game/include/track.h"
#include "net/include/net_client.h"

namespace game {

// The subset of net::NetClient the session needs.
class INetTransport
{
public:
    virtual ~INetTransport() = default;
    virtual int32_t localPlayerId() const = 0;
    virtual void sendInput(const net::CarInput &input) = 0;
    virtual bool takeLatestSnapshot(net::WorldSnapshot &out) = 0;
    virtual std::vector<net::NetClient::PlayerInput> takeInputs() = 0;
    virtual void publishSnapshot(const net::WorldSnapshot &snapshot) = 0;
    virtual void startMatch(uint32_t trackSeed, uint32_t startTick) = 0;
    virtual void setPhase(net::MatchPhase phase) = 0;
};

// Offline: we are the host and nobody else is listening.
class LoopbackTransport : public INetTransport
{
public:
    explicit LoopbackTransport(int32_t localId = 1) : id_(localId) {}
    int32_t localPlayerId() const override { return id_; }
    void sendInput(const net::CarInput &) override {}
    bool takeLatestSnapshot(net::WorldSnapshot &) override { return false; }
    std::vector<net::NetClient::PlayerInput> takeInputs() override { return {}; }
    void publishSnapshot(const net::WorldSnapshot &) override {}
    void startMatch(uint32_t, uint32_t) override {}
    void setPhase(net::MatchPhase) override {}

private:
    int32_t id_;
};

struct SessionConfig
{
    float countdownSeconds = 3.0f;
    float raceSeconds = 900.0f;       // safety cap; the elimination clock ends races
    float resultsSeconds = 10.0f;
    int snapshotIntervalTicks = 2;    // 30 Hz
    int maxBufferedInputs = 8;        // host jitter-queue cap per player
    float correctionDecayRate = 12.0f;// 1/s, visual smoothing of reconciliation jumps
    float snapCorrectionDistance = 6.0f; // bigger jumps are shown immediately
    bool remoteProxies = true;        // client: predicted car collides with dead-reckoned remotes
    bool extrapolateRemotes = true;   // draw remotes at the predicted present (else interpolate)
    float maxExtrapolation = 0.3f;    // seconds of dead reckoning at most
    float remoteSmoothingRate = 10.0f;// 1/s
    int interpolationDelayTicks = 6;  // 100 ms (interpolation mode)
    TrackParams trackParams;          // generator settings (tests use all-straights)
};

// Everything the renderer / HUD needs about one car this frame.
struct CarView
{
    int32_t playerId = 0;
    bool local = false;
    bool bot = false;
    int look = 0;             // 0..3: which paint (rank of the id in the room)
    int model = 0;            // which car: 0 = AE86, 1 = RX-7 (the player's choice; bots by look)
    dm::Vec3 position;
    dm::Quat rotation;
    dm::Vec3 velocity;
    uint32_t section = 0;
    float distance = 0;
    float driftAngle = 0;
    float nitro = 0;
    bool alive = true;
    bool drifting = false;
    bool nitroActive = false;
    bool slipstream = false;
    bool eliminated = false;
    bool ghost = false;       // just respawned: no contact, drawn translucent
    bool airborne = false;    // no wheel on the ground (simulated cars only; remote: false)
    uint8_t place = 0;        // 1 = leading, 0 = eliminated
    uint8_t eliminatedOrder = 0;
};

// Race state for the HUD (the same on host and clients).
struct RaceInfo
{
    bool clockRunning = false;  // Racing and >= 2 cars left
    float timer = 0;            // last-place countdown
    uint8_t stage = 0;          // 0..5 => 32,16,8,4,2,1 s
    float stageTime = 32;
    int32_t lastPlaceId = 0;
    int32_t winnerId = 0;
    int racing = 0;             // cars still in
    int total = 0;
    std::vector<int32_t> finishingOrder; // winner first (Results)
};

// A physical contact, reported exactly once (audio / effects). Host and
// offline: from the authoritative step. Client: its own car from the newest
// predicted tick only (never from reconciliation replays); remote cars from
// velocity jumps between snapshots. Two cars in one bump both report it, and
// a client may see the same bump predicted and then from a snapshot: merge
// nearby events (see client_folder's audio glue).
struct ContactEvent
{
    // Takedown: this car just took another one down (the victim reports Wreck).
    enum Kind : uint8_t { Bump, Barrier, Wreck, Takedown };
    Kind kind;
    int32_t playerId;
    dm::Vec3 position;
    float speed; // m/s of velocity change (impulse / mass)
};

struct FeedEvent
{
    enum Kind : uint8_t { Eliminated, Winner, LastPlace };
    Kind kind;
    int32_t playerId;
    uint8_t stage;
    float age = 0; // seconds since it happened (advanced by tick())
};

class MultiplayerSession
{
public:
    explicit MultiplayerSession(INetTransport &net, const SessionConfig &config = {},
                                const CarParams &params = defaultCarParams(),
                                const RaceParams &race = defaultRaceParams());

    // --- lifecycle ---------------------------------------------------------
    // Host only: tells everyone to start (startMatch + Countdown) and begins
    // locally. `botIds` are extra cars driven by the host (offline mode).
    void hostStartMatch(uint32_t trackSeed, const std::vector<int32_t> &playerIds,
                        const std::vector<int32_t> &botIds = {});
    // On EventType::MatchStarted (idempotent for the host's own match).
    void beginMatch(uint32_t trackSeed, const std::vector<int32_t> &playerIds, bool weAreHost,
                    const std::vector<int32_t> &botIds = {});
    void onHostChanged(bool weAreHost);
    void onPhaseChanged(net::MatchPhase phase);
    // Players who left: out of the race (host), car removed.
    void onRosterChanged(const std::vector<int32_t> &playerIds);
    void end();
    // Host / offline: personality of a bot car (after hostStartMatch).
    void setBotProfile(int32_t playerId, const BotProfile &profile);
    const BotProfile *botProfile(int32_t playerId) const;
    // Our cosmetic car choice (0 = AE86, 1 = RX-7). Stamped on every input we
    // send (CarInput::carModel); the host republishes each player's choice in
    // the snapshot flags (net::kCarModelRx7). Can be set at any time.
    void setLocalCarModel(int model) { localCarModel_ = model ? 1 : 0; }
    int localCarModel() const { return localCarModel_; }
    // The car model drawn for a player (host: newest input; client: snapshot).
    int carModelOf(int32_t playerId) const;

    // --- per fixed tick ----------------------------------------------------
    void tick(const net::CarInput &localInput);

    // --- queries -------------------------------------------------------------
    bool active() const { return active_; }
    bool isHost() const { return isHost_; }
    net::MatchPhase phase() const { return phase_; }
    float phaseTimer() const { return phaseTimer_; }
    uint32_t localTick() const { return localTick_; }
    uint32_t hostTick() const { return hostTick_; }
    uint32_t trackSeed() const { return seed_; }
    int32_t localId() const { return localId_; }
    const Track &track() const { return track_; }
    Track &track() { return track_; }
    const CarParams &params() const { return params_; }
    const RaceInfo &raceInfo() const { return info_; }
    const std::vector<FeedEvent> &feed() const { return feed_; }
    // Contact events since the last call (oldest first).
    std::vector<ContactEvent> takeContactEvents()
    {
        std::vector<ContactEvent> v;
        v.swap(contacts_);
        return v;
    }
    bool isBot(int32_t playerId) const;
    bool localEliminated() const;
    // Client: seconds since the last host snapshot arrived (0 on the host).
    float secondsSinceSnapshot() const
    {
        return isHost_ || !active_ ? 0.0f : static_cast<float>(localTick_ - lastSnapshotLocal_) / kTickRate;
    }
    // Our car's current (predicted / authoritative) simulation state.
    const CarSimState *localCar() const;
    // alpha in [0,1): fraction of a tick since the last tick() (render interpolation).
    std::vector<CarView> carViews(float alpha) const;

    struct Stats
    {
        uint32_t snapshotsReceived = 0;
        uint32_t reconciliations = 0;
        uint32_t hardResets = 0;
        uint32_t replayedTicks = 0;
        float lastCorrection = 0;  // metres between old and corrected prediction
        float maxCorrection = 0;
        double sumCorrection = 0;
        uint32_t hostStarvedTicks = 0;
        uint32_t hostDroppedInputs = 0;
    };
    const Stats &stats() const { return stats_; }
    void resetStats() { stats_ = Stats{}; }

    // --- test / debug hooks --------------------------------------------------
    const CarSimState *predictedStateAt(uint32_t tick) const;
    const CarSimState *hostCar(int32_t playerId, uint32_t *lastInputTick = nullptr) const;
    float visualCorrection() const { return dm::length(visualOffset_); }

private:
    struct HostCar
    {
        CarSimState state, prev;
        std::deque<net::CarInput> queue;
        net::CarInput last;
        uint32_t lastApplied = 0;
        uint32_t lastQueued = 0;
        bool bot = false;
        bool left = false;
        uint8_t model = 0;  // cosmetic car choice (from the player's inputs; bots: by look)
        BotDriver brain;
    };
    struct HistoryEntry
    {
        uint32_t tick = 0;
        net::CarInput input;
        CarSimState state; // after applying input
    };
    struct RemoteSample
    {
        uint32_t hostTick;
        net::CarState car;
    };
    struct Remote
    {
        std::deque<RemoteSample> samples; // interpolation mode
        net::CarState latest;             // newest snapshot state
        dm::Vec3 offset{0, 0, 0};         // visual smoothing (extrapolation mode)
        dm::Quat rotOffset{};
        CarSimState proxy;                // reused buffer for prediction
    };

    static constexpr uint32_t kHistory = 256;

    void hostTick(const net::CarInput &localInput);
    void clientTick(const net::CarInput &input);
    void processSnapshot(const net::WorldSnapshot &snap);
    void reconcile(const net::CarState &cs);
    void predictStep(CarSimState &state, const net::CarInput &input, uint32_t tick);
    net::CarState deadReckon(const net::CarState &c, uint32_t tick, float alpha) const
    {
        return deadReckon(c, tick, alpha, snapLocalTick_);
    }
    net::CarState deadReckon(const net::CarState &c, uint32_t tick, float alpha, uint32_t baseTick) const;
    void setPhaseInternal(net::MatchPhase phase, bool announce);
    void becomeHostFromSnapshot();
    void becomeClient();
    void ensureTrack();
    void publish();
    void pushFeed(FeedEvent::Kind kind, int32_t id, uint8_t stage);
    void refreshHostInfo();
    std::vector<int32_t> gridOrder() const;
    int lookFor(int32_t playerId) const;

    INetTransport &net_;
    SessionConfig cfg_;
    CarParams params_;
    RaceParams raceParams_;
    Track track_;
    uint32_t seed_ = 0;
    bool active_ = false;
    bool isHost_ = false;
    int32_t localId_ = 0;
    std::vector<int32_t> playerIds_; // sorted, includes bots
    std::vector<int32_t> botIds_;

    net::MatchPhase phase_ = net::MatchPhase::Lobby;
    float phaseTimer_ = 0;
    uint32_t localTick_ = 0;
    uint32_t hostTick_ = 0;
    RaceInfo info_;
    std::vector<FeedEvent> feed_;
    std::vector<ContactEvent> contacts_;
    void collectContacts(int32_t id, const CarSimState &before, const CarSimState &after);

    // host
    std::map<int32_t, HostCar> cars_;
    EliminationRace race_;

    // client
    CarSimState local_, localPrev_;
    std::vector<HistoryEntry> history_;
    dm::Vec3 visualOffset_{0, 0, 0};
    dm::Quat visualRotOffset_{};
    std::map<int32_t, Remote> remotes_;
    std::map<int32_t, net::CarState> lastCars_; // newest snapshot car states (incl. ours)
    uint32_t snapLocalTick_ = 0; // our input tick the newest snapshot corresponds to
    uint32_t lastSnapshotLocal_ = 0; // our local tick when the newest snapshot arrived
    double hostClock_ = 0;
    bool hostClockValid_ = false;
    uint32_t lastSnapshotTick_ = 0;
    net::WorldSnapshot lastSnapshot_;
    bool haveSnapshot_ = false;
    float localMultiplier_ = 1.0f;
    uint8_t localCarModel_ = 0;

    Stats stats_;
};

} // namespace game
