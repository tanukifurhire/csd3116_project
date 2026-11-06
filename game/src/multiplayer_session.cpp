// Elimination race session: the layer between the transport (net::NetClient or
// a loopback) and the simulation. One class, two roles chosen at runtime.
// HOST (authoritative): every fixed tick it collects each player's input into
// a per-player jitter queue, applies exactly one input per car per tick (or
// repeats the last one when starved), drives bots, runs stepWorld() for all
// cars, advances the elimination clock and phase machine, and publishes a
// snapshot every snapshotIntervalTicks (plus on phase changes). Each car in
// the snapshot carries lastInputTick, the newest input the host applied.
// CLIENT: predicts its own car every tick with the same stepCar/stepWorld
// code (client-side prediction), stores (input, state) per tick in a ring
// buffer (history_), and when a snapshot arrives rewinds to lastInputTick,
// overwrites the replicated fields with the host's, and replays the later
// inputs (reconciliation). The positional error is not applied instantly: it
// is stored as visualOffset_/visualRotOffset_ and decays exponentially, so
// the drawn car glides to the corrected path. Remote cars are drawn
// dead-reckoned (or interpolated behind the host clock when extrapolateRemotes
// is off) and, during prediction, act as proxies the local car collides with.
// Host migration: becomeHostFromSnapshot()/becomeClient() convert between the
// two roles from the newest snapshot.
// Timing: tick() is called once per fixed simulation step (kFixedDt, kTickRate
// Hz) by the main loop; carViews(alpha) is called per rendered frame with
// the fraction of a tick elapsed, for smooth drawing. Single-threaded: all of
// this runs on the main thread; the transport hands data over via take*().
// Phases: Lobby -> Countdown -> Racing -> Results -> Lobby; only the host
// advances them, clients follow the snapshots.
// multiplayer_session.cpp -- see multiplayer_session.h for the protocol.
#include "game/include/multiplayer_session.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "game/include/rng.h"

namespace game {

namespace {

// Length of a phase in seconds from the config (Lobby has no timer).
float phaseDuration(const SessionConfig &cfg, net::MatchPhase p)
{
    switch (p)
    {
    case net::MatchPhase::Countdown: return cfg.countdownSeconds;
    case net::MatchPhase::Racing: return cfg.raceSeconds;
    case net::MatchPhase::Results: return cfg.resultsSeconds;
    case net::MatchPhase::Lobby: break;
    }
    return 0.0f;
}

// Fill a render view from a network state (remote cars).
void fillView(CarView &v, const net::CarState &c)
{
    v.playerId = c.playerId;
    v.position = c.position;
    v.rotation = c.rotation;
    v.velocity = c.velocity;
    v.section = c.trackSection;
    v.distance = c.distance;
    v.nitro = c.nitro;
    v.driftAngle = driftAngleDeg(c.rotation, c.velocity);
    v.alive = (c.flags & net::kCarAlive) != 0;
    v.drifting = (c.flags & net::kCarDrifting) != 0;
    v.nitroActive = (c.flags & net::kCarNitro) != 0;
    v.slipstream = (c.flags & net::kCarSlipstream) != 0;
    v.eliminated = (c.flags & net::kCarEliminated) != 0;
    v.ghost = (c.flags & kCarFlagGhost) != 0;
    v.place = c.place;
    v.eliminatedOrder = c.eliminatedOrder;
}

// Fill a render view from two consecutive sim states, blended by alpha in
// [0, 1] (position and velocity lerp, orientation slerp); discrete fields
// come from the newer state.
void fillView(CarView &v, const CarSimState &a, const CarSimState &b, float alpha)
{
    v.position = dm::lerp(a.position, b.position, alpha);
    v.rotation = dm::slerp(a.rotation, b.rotation, alpha);
    v.velocity = dm::lerp(a.velocity, b.velocity, alpha);
    v.section = b.section;
    v.distance = b.distance;
    v.driftAngle = b.driftAngle;
    v.nitro = b.nitro;
    v.alive = b.alive;
    v.drifting = b.drifting;
    v.nitroActive = b.nitroActive;
    v.slipstream = b.slipstream;
    v.ghost = b.ghostTimer > 0;
    v.airborne = b.alive && b.wheelContactMask == 0;
}

// Final ranking from a snapshot (clients do not run the race rules).
std::vector<int32_t> finishingOrderFrom(const std::vector<net::CarState> &cars, int32_t winner)
{
    std::vector<const net::CarState *> v;
    for (const net::CarState &c : cars) v.push_back(&c);
    // Winner, then still-racing cars by place (time cap), then latest eliminated first.
    std::sort(v.begin(), v.end(), [winner](const net::CarState *a, const net::CarState *b) {
        if ((a->playerId == winner) != (b->playerId == winner)) return a->playerId == winner;
        if ((a->eliminatedOrder == 0) != (b->eliminatedOrder == 0)) return a->eliminatedOrder == 0;
        if (a->eliminatedOrder == 0) return a->place < b->place;
        return a->eliminatedOrder > b->eliminatedOrder;
    });
    std::vector<int32_t> ids;
    for (const net::CarState *c : v) ids.push_back(c->playerId);
    return ids;
}

} // namespace

MultiplayerSession::MultiplayerSession(INetTransport &net, const SessionConfig &config, const CarParams &params,
                                       const RaceParams &race)
    : net_(net), cfg_(config), params_(params), raceParams_(race), track_(1, config.trackParams), race_(race),
      history_(kHistory)
{
    // Wrecks respawn; only the elimination clock ends a race.
    params_.autoRespawn = true; // wrecks respawn; the elimination clock decides the race
}

// Bots exist only on the host; ids are tracked so views can label them.
bool MultiplayerSession::isBot(int32_t id) const
{
    return std::find(botIds_.begin(), botIds_.end(), id) != botIds_.end();
}

// Car appearance (0..3) derived from the player's index in the sorted roster.
int MultiplayerSession::lookFor(int32_t id) const
{
    auto it = std::find(playerIds_.begin(), playerIds_.end(), id);
    return it == playerIds_.end() ? 0 : static_cast<int>(it - playerIds_.begin()) % 4;
}

// Grid order shuffled by the seed (everyone computes the same), so pole
// position isn't always the lowest player id.
std::vector<int32_t> MultiplayerSession::gridOrder() const
{
    std::vector<int32_t> ids = playerIds_;
    uint32_t seed = seed_;
    std::sort(ids.begin(), ids.end(), [seed](int32_t a, int32_t b) {
        uint64_t ha = splitmix64((static_cast<uint64_t>(seed) << 32) ^ static_cast<uint32_t>(a));
        uint64_t hb = splitmix64((static_cast<uint64_t>(seed) << 32) ^ static_cast<uint32_t>(b));
        return ha != hb ? ha < hb : a < b;
    });
    return ids;
}

// Append to the event feed shown on the HUD; keeps the newest 8.
void MultiplayerSession::pushFeed(FeedEvent::Kind kind, int32_t id, uint8_t stage)
{
    feed_.push_back({kind, id, stage, 0.0f});
    if (feed_.size() > 8) feed_.erase(feed_.begin());
}

// --------------------------------------------------------------------------
// lifecycle
// --------------------------------------------------------------------------

// Host entry point: set up the match, tell the other peers (seed + start
// tick) and start the countdown.
void MultiplayerSession::hostStartMatch(uint32_t trackSeed, const std::vector<int32_t> &playerIds,
                                        const std::vector<int32_t> &botIds)
{
    beginMatch(trackSeed, playerIds, true, botIds);
    net_.startMatch(trackSeed, hostTick_);
    setPhaseInternal(net::MatchPhase::Countdown, true);
}

// Reset all per-match state for a track seed and roster (players + bots +
// us, sorted, de-duplicated, capped at kMaxRoomPlayers) and place everyone
// on the grid. Host: creates authoritative HostCar entries (bots get
// randomised driving traits from a hash of seed+id). Client: creates the
// local predicted car and a Remote entry for each other car. Called on both
// sides; a repeat call with identical arguments is ignored.
void MultiplayerSession::beginMatch(uint32_t trackSeed, const std::vector<int32_t> &playerIds, bool weAreHost,
                                    const std::vector<int32_t> &botIds)
{
    int32_t localId = net_.localPlayerId();
    std::vector<int32_t> ids = playerIds;
    ids.insert(ids.end(), botIds.begin(), botIds.end());
    if (std::find(ids.begin(), ids.end(), localId) == ids.end()) ids.push_back(localId);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.size() > static_cast<size_t>(net::kMaxRoomPlayers)) ids.resize(net::kMaxRoomPlayers);

    // The host receives its own MatchStarted echo: ignore it.
    if (active_ && trackSeed == seed_ && ids == playerIds_ && weAreHost == isHost_) return;

    seed_ = trackSeed;
    track_.reset(trackSeed);
    active_ = true;
    isHost_ = weAreHost;
    localId_ = localId;
    playerIds_ = ids;
    botIds_ = botIds;
    phase_ = net::MatchPhase::Countdown;
    phaseTimer_ = cfg_.countdownSeconds;
    hostTick_ = 0;
    info_ = RaceInfo{};
    info_.total = info_.racing = static_cast<int>(ids.size());
    feed_.clear();
    race_ = EliminationRace(raceParams_);

    cars_.clear();
    remotes_.clear();
    lastCars_.clear();
    for (HistoryEntry &h : history_) h.tick = 0;
    visualOffset_ = {0, 0, 0};
    visualRotOffset_ = dm::Quat{};
    hostClock_ = 0;
    hostClockValid_ = false;
    lastSnapshotTick_ = 0;
    haveSnapshot_ = false;
    snapLocalTick_ = localTick_;
    lastSnapshotLocal_ = localTick_;
    localMultiplier_ = 1.0f;

    std::vector<int32_t> grid = gridOrder();
    for (size_t slot = 0; slot < grid.size(); ++slot)
    {
        int32_t id = grid[slot];
        CarSimState spawn = spawnOnGrid(track_, static_cast<int>(slot), params_);
        if (isHost_)
        {
            HostCar hc;
            hc.state = hc.prev = spawn;
            hc.last = holdInput(0);
            hc.bot = isBot(id);
            // A little variety between bots.
            uint64_t h = splitmix64(static_cast<uint64_t>(trackSeed) * 31u + static_cast<uint32_t>(id));
            hc.brain.cornerFactor = 0.73f + 0.06f * static_cast<float>(h & 0xff) / 255.0f;
            hc.brain.laneBias = (static_cast<float>((h >> 8) & 0xff) / 255.0f - 0.5f) * 1.5f;
            hc.brain.nitroMinGauge = 0.25f + 0.3f * static_cast<float>((h >> 16) & 0xff) / 255.0f;
            cars_[id] = hc;
        }
        else if (id == localId_)
            local_ = localPrev_ = spawn;
        else
        {
            Remote r;
            r.latest = toNetState(spawn, id, 0);
            r.samples.push_back({0, r.latest});
            remotes_[id] = r;
        }
        lastCars_[id] = toNetState(spawn, id, 0);
    }
}

// Leave the match and return to the lobby state.
void MultiplayerSession::end()
{
    active_ = false;
    phase_ = net::MatchPhase::Lobby;
    cars_.clear();
    remotes_.clear();
}

// Client side: adopt a phase announced by the host.
void MultiplayerSession::onPhaseChanged(net::MatchPhase phase)
{
    if (isHost_ || !active_) return; // the host drives phases itself
    if (phase != phase_)
    {
        phase_ = phase;
        phaseTimer_ = phaseDuration(cfg_, phase);
    }
}

// Players who left: the host marks their cars as gone and removes them from
// the race (counted as eliminated, feeding the event log); everyone drops
// the leavers' remote entries.
void MultiplayerSession::onRosterChanged(const std::vector<int32_t> &playerIds)
{
    std::set<int32_t> keep(playerIds.begin(), playerIds.end());
    keep.insert(localId_);
    keep.insert(botIds_.begin(), botIds_.end());
    if (isHost_)
    {
        for (auto &kv : cars_)
        {
            if (keep.count(kv.first) || kv.second.left) continue;
            kv.second.left = true;
            std::vector<EliminationRace::Event> events;
            race_.removePlayer(kv.first, &events);
            for (const auto &e : events)
                pushFeed(e.kind == EliminationRace::Event::Winner ? FeedEvent::Winner : FeedEvent::Eliminated,
                         e.playerId, e.stage);
        }
    }
    for (auto it = remotes_.begin(); it != remotes_.end();)
        it = keep.count(it->first) ? std::next(it) : remotes_.erase(it);
}

// Role change after the previous host disappeared (or a new election).
void MultiplayerSession::onHostChanged(bool weAreHost)
{
    if (!active_) { isHost_ = weAreHost; return; }
    if (weAreHost && !isHost_)
        becomeHostFromSnapshot();
    else if (!weAreHost && isHost_)
        becomeClient();
    else if (!weAreHost)
    {
        // Host moved between two other players: its tick counter continues
        // from *its* newest snapshot, which may be behind ours. Accept it.
        lastSnapshotTick_ = 0;
        haveSnapshot_ = false;
        hostClockValid_ = false;
        for (auto &kv : remotes_)
        {
            if (kv.second.samples.size() > 1) kv.second.samples.erase(kv.second.samples.begin(), kv.second.samples.end() - 1);
            if (!kv.second.samples.empty()) kv.second.samples.back().hostTick = 0;
        }
    }
}

// Promote this peer to host. Every car is rebuilt from the newest snapshot,
// including ours, so a prediction that ran ahead during a host stall cannot
// grant progress. The race state and clock are restored from the snapshot.
void MultiplayerSession::becomeHostFromSnapshot()
{
    isHost_ = true;
    cars_.clear();
    hostTick_ = haveSnapshot_ ? lastSnapshot_.hostTick : static_cast<uint32_t>(hostClock_);
    for (const auto &kv : lastCars_)
    {
        const net::CarState &cs = kv.second;
        HostCar hc;
        if (kv.first == localId_)
        {
            // Our own car comes from the same snapshot as everyone else's (not
            // from our prediction, which may be seconds ahead after a host
            // stall -- that would hand the new host free progress).
            hc.state = local_;
            applyNetState(hc.state, cs);
            hc.lastApplied = hc.lastQueued = localTick_;
        }
        else
        {
            hc.state = fromNetState(cs); // newest snapshot: the host clock resumes from it too
            float sag = -(params_.gravity * params_.mass * 0.25f) / params_.springConstant;
            for (float &l : hc.state.suspensionLast) l = sag;
            hc.lastApplied = hc.lastQueued = cs.lastInputTick;
            // Until their inputs reach us, keep driving straight on.
            hc.last = driveInput(cs.lastInputTick, 0.0f, false);
        }
        hc.prev = hc.state;
        hc.left = (cs.flags & net::kCarEliminated) != 0 && cs.eliminatedOrder == 0;
        cars_[kv.first] = hc;
    }
    if (haveSnapshot_) race_.restore(lastSnapshot_);
    remotes_.clear();
    visualOffset_ = {0, 0, 0};
    visualRotOffset_ = dm::Quat{};
}

// Demote from host to client: our car becomes the predicted car, other cars
// become remotes seeded with their current state.
void MultiplayerSession::becomeClient()
{
    isHost_ = false;
    auto me = cars_.find(localId_);
    if (me != cars_.end()) local_ = localPrev_ = me->second.state;
    for (HistoryEntry &h : history_) h.tick = 0;
    HistoryEntry &h = history_[localTick_ % kHistory];
    h.tick = localTick_;
    h.input = holdInput(localTick_);
    h.state = local_;
    remotes_.clear();
    for (auto &kv : cars_)
        if (kv.first != localId_)
        {
            Remote r;
            r.latest = toNetState(kv.second.state, kv.first, 0);
            r.samples.push_back({0, r.latest});
            remotes_[kv.first] = r;
        }
    cars_.clear();
    snapLocalTick_ = localTick_;
    lastSnapshotTick_ = 0;
    haveSnapshot_ = false;
    hostClockValid_ = false;
}

// Enter a phase and (host only, if announce) broadcast it.
void MultiplayerSession::setPhaseInternal(net::MatchPhase phase, bool announce)
{
    phase_ = phase;
    phaseTimer_ = phaseDuration(cfg_, phase);
    if (announce && isHost_) net_.setPhase(phase);
}

// Generate enough track ahead of the furthest car and let the track discard
// old sections behind the local car. Generation is deterministic, so every
// peer ends up with the same sections.
void MultiplayerSession::ensureTrack()
{
    uint32_t maxSection = 0, localSection = 0;
    if (isHost_)
    {
        for (const auto &kv : cars_)
        {
            maxSection = std::max(maxSection, kv.second.state.section);
            if (kv.first == localId_) localSection = kv.second.state.section;
        }
    }
    else
    {
        maxSection = localSection = local_.section;
        for (const auto &kv : lastCars_) maxSection = std::max(maxSection, kv.second.trackSection);
    }
    track_.ensureGenerated(maxSection + 30); // sections can be short: keep ~>300 m ahead
    track_.update(localSection);
}

// --------------------------------------------------------------------------
// host
// --------------------------------------------------------------------------

// Advance the session one fixed step. localTick_ counts our own ticks and
// is the clock our inputs are labelled with.
void MultiplayerSession::tick(const net::CarInput &localInput)
{
    if (!active_) return;
    ++localTick_;
    for (FeedEvent &e : feed_) e.age += kFixedDt;
    if (isHost_) hostTick(localInput);
    else clientTick(localInput);
}

// One authoritative host step: queue incoming inputs, pick one input per
// car, step the world, update the race clock and the phase machine, publish.
void MultiplayerSession::hostTick(const net::CarInput &localInput)
{
    ++hostTick_;
    ensureTrack();
    const bool racing = phase_ == net::MatchPhase::Racing;

    for (const net::NetClient::PlayerInput &pi : net_.takeInputs())
    {
        if (pi.playerId == localId_) continue; // our own input is applied directly
        auto it = cars_.find(pi.playerId);
        if (it == cars_.end() || it->second.bot) continue;
        HostCar &hc = it->second;
        if (pi.input.tick <= hc.lastQueued) continue; // duplicate / too late
        hc.queue.push_back(pi.input);
        hc.lastQueued = pi.input.tick;
    }

    std::vector<WorldCar> world;
    world.reserve(cars_.size());
    for (auto &kv : cars_)
    {
        HostCar &hc = kv.second;
        hc.prev = hc.state;
        net::CarInput in;
        if (kv.first == localId_)
        {
            in = racing ? localInput : holdInput(localTick_);
            in.tick = localTick_;
            hc.lastApplied = localTick_;
        }
        else if (hc.bot)
            in = racing ? hc.brain.drive(hc.state, track_, params_, hostTick_) : holdInput(hostTick_);
        else
        {
            // Jitter buffer: one input per tick; a long queue (burst after a
            // stall) is trimmed to cap the added latency.
            if (static_cast<int>(hc.queue.size()) > cfg_.maxBufferedInputs)
                while (hc.queue.size() > 2) { hc.queue.pop_front(); ++stats_.hostDroppedInputs; }
            if (!hc.queue.empty())
            {
                hc.last = hc.queue.front();
                hc.queue.pop_front();
                hc.lastApplied = hc.last.tick;
            }
            else
                ++stats_.hostStarvedTicks; // repeat the last input
            in = racing ? hc.last : holdInput(hc.last.tick);
        }
        // | hc.left) continue;|Cars that are out or gone, and everyone on the podium, are not stepped.
        if (race_.isEliminated(kv.first) || hc.left) continue; // out: no longer simulated
        if (phase_ == net::MatchPhase::Results) continue;       // podium: cars freeze
        hc.state.nitroRechargeScale = race_.started() ? race_.rechargeMultiplierFor(kv.first) : 1.0f;
        world.push_back({kv.first, &hc.state, in, false});
    }
    stepWorld(world, track_, params_, raceParams_);
    // The world step above covered slipstream, physics and car-to-car contacts;
    // now turn each car's outcome into effect events.
    for (const WorldCar &w : world) collectContacts(w.id, cars_[w.id].prev, *w.state);

    // Race clock.
    if (racing)
    {
        // Feed each live car's progress to the race rules and relay their events.
        std::vector<EliminationRace::Progress> progress;
        for (const WorldCar &w : world) progress.push_back({w.id, w.state->distance});
        for (const auto &e : race_.update(progress, kFixedDt))
            pushFeed(e.kind == EliminationRace::Event::Winner        ? FeedEvent::Winner
                     : e.kind == EliminationRace::Event::Eliminated ? FeedEvent::Eliminated
                                                                    : FeedEvent::LastPlace,
                     e.playerId, e.stage);
    }

    // Phase machine: Countdown -> Racing (race_.start when green) -> Results
    // (winner known, or time cap and the leader wins) -> Lobby (match over).
    net::MatchPhase before = phase_;
    phaseTimer_ -= kFixedDt;
    if (phase_ == net::MatchPhase::Countdown && phaseTimer_ <= 0)
    {
        setPhaseInternal(net::MatchPhase::Racing, true);
        std::vector<int32_t> ids;
        for (const auto &kv : cars_)
            if (!kv.second.left) ids.push_back(kv.first);
        race_.start(ids); // the clock starts when the race goes green
    }
    else if (phase_ == net::MatchPhase::Racing && (race_.finished() || phaseTimer_ <= 0))
    {
        race_.finishByLeader(); // no-op if the clock already decided it
        setPhaseInternal(net::MatchPhase::Results, true);
    }
    else if (phase_ == net::MatchPhase::Results && phaseTimer_ <= 0)
        setPhaseInternal(net::MatchPhase::Lobby, true);

    refreshHostInfo();
    if (hostTick_ % static_cast<uint32_t>(cfg_.snapshotIntervalTicks) == 0 || phase_ != before) publish();
    if (phase_ == net::MatchPhase::Lobby) active_ = false;
}

// Copy race-rule state into info_ (the HUD-facing struct).
void MultiplayerSession::refreshHostInfo()
{
    info_.clockRunning = phase_ == net::MatchPhase::Racing && race_.started() && !race_.finished() &&
                         race_.racingCount() >= 2;
    info_.timer = race_.started() ? std::max(0.0f, race_.timer()) : race_.stageTime(0);
    info_.stage = race_.stage();
    info_.stageTime = race_.stageTime(info_.stage);
    info_.lastPlaceId = race_.lastPlaceId();
    info_.winnerId = race_.winnerId();
    info_.total = static_cast<int>(cars_.size());
    info_.racing = race_.started() ? race_.racingCount() : info_.total;
    if (race_.finished()) info_.finishingOrder = race_.finishingOrder();
}

// Build and send the world snapshot at the host's current tick. lastCars_
// keeps a copy so a promoted client can continue from it.
void MultiplayerSession::publish()
{
    net::WorldSnapshot snap;
    snap.hostTick = hostTick_;
    snap.phase = phase_;
    snap.phaseTimer = std::max(0.0f, phaseTimer_);
    snap.eliminationTimer = info_.timer;
    snap.eliminationStage = info_.stage;
    snap.lastPlacePlayerId = info_.lastPlaceId;
    snap.winnerPlayerId = info_.winnerId;
    for (const auto &kv : cars_)
    {
        net::CarState cs = toNetState(kv.second.state, kv.first, kv.second.lastApplied);
        cs.eliminatedOrder = race_.eliminatedOrder(kv.first);
        cs.place = race_.started() ? race_.place(kv.first) : 0;
        if (cs.eliminatedOrder != 0 || kv.second.left) cs.flags |= net::kCarEliminated;
        snap.cars.push_back(cs);
        lastCars_[kv.first] = cs;
    }
    net_.publishSnapshot(snap);
}

// --------------------------------------------------------------------------
// client
// --------------------------------------------------------------------------

// Remote car state dead-reckoned to our local tick `tick` (+alpha): the
// newest snapshot is "as of" our input tick snapLocalTick_.
// dt is converted to seconds and capped at maxExtrapolation; position moves
// along the velocity and the heading turns by the yaw rate. Dead or
// eliminated cars are not extrapolated.
net::CarState MultiplayerSession::deadReckon(const net::CarState &c, uint32_t tick, float alpha, uint32_t baseTick) const
{
    float dt = (static_cast<float>(static_cast<int64_t>(tick) - static_cast<int64_t>(baseTick)) + alpha) / kTickRate;
    dt = dm::clampf(dt, 0.0f, cfg_.maxExtrapolation);
    net::CarState r = c;
    if ((c.flags & net::kCarEliminated) || !(c.flags & net::kCarAlive)) return r;
    r.position = c.position + c.velocity * dt;
    float yawRate = dm::dot(c.angularVelocity, dm::upOf(c.rotation));
    r.rotation = dm::normalize(dm::quatAxisAngle(dm::upOf(c.rotation), yawRate * dt) * c.rotation);
    return r;
}

// One predicted tick of our car: stepWorld against proxy remote cars.
// Proxies are rebuilt from each remote's newest snapshot, dead-reckoned to
// `tick`; they push us but are never pushed. Without remotes it is a plain
// stepCar.
void MultiplayerSession::predictStep(CarSimState &state, const net::CarInput &input, uint32_t tick)
{
    state.nitroRechargeScale = localMultiplier_;
    std::vector<WorldCar> world;
    world.push_back({localId_, &state, input, false});
    if (cfg_.remoteProxies)
        for (auto &kv : remotes_)
        {
            const net::CarState &c = kv.second.latest;
            if (c.flags & net::kCarEliminated) continue;
            kv.second.proxy = fromNetState(deadReckon(c, tick, 0.0f));
            world.push_back({kv.first, &kv.second.proxy, net::CarInput{}, true});
        }
    if (world.size() > 1)
        stepWorld(world, track_, params_, raceParams_);
    else
        stepCar(state, input, track_, params_);
}

// One client step: reconcile with the newest snapshot, predict this tick with
// the input being sent, store it in the history ring and decay the visual
// correction offsets.
void MultiplayerSession::clientTick(const net::CarInput &rawInput)
{
    ensureTrack();

    // 1) Newest snapshot first: reconciles history up to localTick_ - 1.
    net::WorldSnapshot snap;
    if (net_.takeLatestSnapshot(snap)) processSnapshot(snap);
    if (!active_) return;

    // 2) Predict this tick with exactly the input we send.
    net::CarInput in = phase_ == net::MatchPhase::Racing && !localEliminated() ? rawInput : holdInput(localTick_);
    in.tick = localTick_;
    net_.sendInput(in);
    localPrev_ = local_;
    if (!localEliminated() && phase_ != net::MatchPhase::Results)
    {
        predictStep(local_, in, localTick_);
        collectContacts(localId_, localPrev_, local_); // newest tick only, never replays
    }
    HistoryEntry &h = history_[localTick_ % kHistory];
    h.tick = localTick_;
    h.input = in;
    h.state = local_;

    // 3) Decay visual corrections, advance clocks.
    // exp(-rate*dt) per tick: the decay is independent of the tick rate.
    float k = std::exp(-cfg_.correctionDecayRate * kFixedDt);
    visualOffset_ *= k;
    visualRotOffset_ = dm::slerp(dm::Quat{}, visualRotOffset_, k);
    float kr = std::exp(-cfg_.remoteSmoothingRate * kFixedDt);
    for (auto &kv : remotes_)
    {
        kv.second.offset *= kr;
        kv.second.rotOffset = dm::slerp(dm::Quat{}, kv.second.rotOffset, kr);
    }
    if (hostClockValid_) hostClock_ += 1.0;
    phaseTimer_ = std::max(0.0f, phaseTimer_ - kFixedDt);
    if (info_.clockRunning) info_.timer = std::max(0.0f, info_.timer - kFixedDt);
}

// Handle one host snapshot: drop stale ones, adopt phase and race info, keep a
// smoothed estimate of the host's tick (hostClock_), log eliminations,
// update remote cars (turning any position jump into a decaying offset and
// deriving contact events), then reconcile our own car.
void MultiplayerSession::processSnapshot(const net::WorldSnapshot &snap)
{
    if (haveSnapshot_ && snap.hostTick <= lastSnapshotTick_) return; // stale / reordered
    ++stats_.snapshotsReceived;
    lastSnapshotTick_ = snap.hostTick;
    lastSnapshotLocal_ = localTick_;
    haveSnapshot_ = true;
    phase_ = snap.phase;
    phaseTimer_ = snap.phaseTimer;
    hostTick_ = snap.hostTick;

    // hostClock_ follows the host tick: snap on first use or after a large gap,
    // otherwise nudge 10% of the error per snapshot to absorb jitter.
    if (!hostClockValid_) { hostClock_ = snap.hostTick; hostClockValid_ = true; }
    else
    {
        double err = static_cast<double>(snap.hostTick) - hostClock_;
        if (std::fabs(err) > 30.0) hostClock_ = snap.hostTick;
        else hostClock_ += err * 0.1;
    }

    // Race info + elimination feed (from changes between snapshots).
    for (const net::CarState &c : snap.cars)
    {
        auto prev = lastCars_.find(c.playerId);
        uint8_t was = prev != lastCars_.end() ? prev->second.eliminatedOrder : 0;
        if (c.eliminatedOrder != 0 && was == 0) pushFeed(FeedEvent::Eliminated, c.playerId, 0);
    }
    if (snap.winnerPlayerId != 0 && info_.winnerId != snap.winnerPlayerId) pushFeed(FeedEvent::Winner, snap.winnerPlayerId, 0);
    if (snap.phase == net::MatchPhase::Racing && snap.lastPlacePlayerId != info_.lastPlaceId && info_.lastPlaceId != 0 &&
        snap.eliminationStage > info_.stage)
        pushFeed(FeedEvent::LastPlace, snap.lastPlacePlayerId, snap.eliminationStage);
    int racing = 0;
    float leader = -1e30f, last = 1e30f, mine = 0;
    for (const net::CarState &c : snap.cars)
    {
        if (c.eliminatedOrder != 0 || (c.flags & net::kCarEliminated)) continue;
        ++racing;
        leader = std::max(leader, c.distance);
        last = std::min(last, c.distance);
        if (c.playerId == localId_) mine = c.distance;
    }
    info_.clockRunning = snap.phase == net::MatchPhase::Racing && racing >= 2 && snap.winnerPlayerId == 0;
    info_.timer = snap.eliminationTimer;
    info_.stage = snap.eliminationStage;
    info_.stageTime = raceParams_.stageTimes[std::min<int>(snap.eliminationStage, 5)];
    info_.lastPlaceId = snap.lastPlacePlayerId;
    info_.winnerId = snap.winnerPlayerId;
    info_.racing = racing;
    info_.total = static_cast<int>(snap.cars.size());
    if (snap.winnerPlayerId != 0 || snap.phase == net::MatchPhase::Results)
        info_.finishingOrder = finishingOrderFrom(snap.cars, snap.winnerPlayerId);
    // Same rubber band as the host, for predicting our nitro recharge.
    if (racing >= 2) localMultiplier_ = mine >= leader ? raceParams_.rechargeLeader
                                       : mine <= last  ? raceParams_.rechargeLast
                                                       : rechargeMultiplier(mine, leader, last, raceParams_);

    // Our snapshot moment, in our own ticks (proxies dead-reckon from it).
    const uint32_t oldBase = snapLocalTick_;
    for (const net::CarState &c : snap.cars)
        if (c.playerId == localId_)
            snapLocalTick_ = c.lastInputTick != 0 ? c.lastInputTick : localTick_ - 1;

    // Remote cars first (the replay below collides against their new proxies).
    std::set<int32_t> present;
    const net::CarState *ours = nullptr;
    for (const net::CarState &c : snap.cars)
    {
        present.insert(c.playerId);
        if (c.playerId == localId_) { ours = &c; continue; }
        Remote &r = remotes_[c.playerId];
        // Keep the drawn (dead-reckoned) car where it was: the jump becomes
        // an offset that decays.
        if (!r.samples.empty())
        {
            net::CarState oldNow = deadReckon(r.latest, localTick_ - 1, 0.0f, oldBase);
            net::CarState newNow = deadReckon(c, localTick_ - 1, 0.0f);
            dm::Vec3 jump = oldNow.position - newNow.position;
            if (dm::length(jump) < cfg_.snapCorrectionDistance)
            {
                r.offset += jump;
                r.rotOffset = dm::normalize(r.rotOffset * oldNow.rotation * dm::conjugate(newNow.rotation));
            }
            else
            {
                r.offset = {0, 0, 0};
                r.rotOffset = dm::Quat{};
            }
        }
        // Remote contacts: a velocity jump driving can't produce (drive +
        // nitro + drag stay under ~40 m/s^2) between consecutive snapshots.
        if (!r.samples.empty())
        {
            float dtSnap = std::max(1.0f, static_cast<float>(snap.hostTick - r.samples.back().hostTick)) / kTickRate;
            float dv = dm::length(c.velocity - r.latest.velocity) - 40.0f * dtSnap;
            bool wasAlive = (r.latest.flags & net::kCarAlive) != 0, isAlive = (c.flags & net::kCarAlive) != 0;
            if (wasAlive && !isAlive)
                contacts_.push_back({ContactEvent::Wreck, c.playerId, c.position, std::max(dv, params_.crashImpactSpeed)});
            else if (isAlive && dv > 2.0f && dm::length(c.position - r.latest.position) < 10.0f) // not a respawn
            {
                bool nearCar = false;
                for (const net::CarState &o : snap.cars)
                    if (o.playerId != c.playerId &&
                        dm::length(o.position - c.position) < 2.0f * params_.halfLength + 0.8f)
                        nearCar = true;
                contacts_.push_back({nearCar ? ContactEvent::Bump : ContactEvent::Barrier, c.playerId, c.position, dv});
            }
        }
        r.latest = c;
        if (r.samples.empty() || r.samples.back().hostTick < snap.hostTick) r.samples.push_back({snap.hostTick, c});
        while (r.samples.size() > 32) r.samples.pop_front();
        lastCars_[c.playerId] = c;
    }
    if (ours)
    {
        lastCars_[localId_] = *ours;
        if (ours->flags & net::kCarEliminated) applyNetState(local_, *ours); // spectating
        else reconcile(*ours);
    }
    for (auto it = remotes_.begin(); it != remotes_.end();)
        it = present.count(it->first) ? std::next(it) : remotes_.erase(it);

    lastSnapshot_ = snap;
    if (snap.phase == net::MatchPhase::Lobby) active_ = false;
}

// Client-side reconciliation against the host's state `cs` for our car.
// T = cs.lastInputTick. If the history still holds tick T: rewind to the
// stored state at T, overwrite it with the host's replicated fields and
// re-simulate inputs T+1..newest. Otherwise (too old / no input yet) do a
// hard reset to the host state. The difference between the old and the
// corrected prediction becomes the decaying visual offset, unless it exceeds
// snapCorrectionDistance (then snap with no smoothing).
void MultiplayerSession::reconcile(const net::CarState &cs)
{
    // local_ is the prediction after tick localTick_ - 1 here.
    const uint32_t newest = localTick_ - 1;
    const uint32_t T = cs.lastInputTick;
    CarSimState before = local_;
    CarSimState corrected;

    // Replay needs T inside the ring buffer with an intact entry.
    bool canReplay = T != 0 && T <= newest && newest - T < kHistory - 1 && history_[T % kHistory].tick == T &&
                     phase_ != net::MatchPhase::Results; // podium: the host froze the cars
    if (!canReplay)
    {
        corrected = local_;
        applyNetState(corrected, cs);
        ++stats_.hardResets;
    }
    else
    {
        // Rewind: our stored state at T with the host's replicated fields...
        corrected = history_[T % kHistory].state;
        applyNetState(corrected, cs);
        history_[T % kHistory].state = corrected;
        // ...then replay every input the host hasn't seen yet, against the
        // proxies of this snapshot.
        for (uint32_t t = T + 1; t <= newest; ++t)
        {
            HistoryEntry &h = history_[t % kHistory];
            predictStep(corrected, h.input, t);
            h.state = corrected;
            ++stats_.replayedTicks;
        }
    }
    local_ = corrected;

    dm::Vec3 delta = before.position - corrected.position;
    float mag = dm::length(delta);
    if (phase_ == net::MatchPhase::Results)
    {
        // Podium freeze: just take the host's pose, smoothly.
        if (mag < cfg_.snapCorrectionDistance) visualOffset_ += delta;
        return;
    }
    ++stats_.reconciliations;
    stats_.lastCorrection = mag;
    stats_.maxCorrection = std::max(stats_.maxCorrection, mag);
    stats_.sumCorrection += mag;

    if (mag > cfg_.snapCorrectionDistance)
    {
        visualOffset_ = {0, 0, 0};
        visualRotOffset_ = dm::Quat{};
    }
    else
    {
        visualOffset_ += delta;
        visualRotOffset_ = dm::normalize(visualRotOffset_ * before.rotation * dm::conjugate(corrected.rotation));
    }
}

// Turn state changes of one simulated tick into audio/effect events: wreck,
// barrier hit, car bump. The list is capped so it cannot grow unbounded when
// nobody consumes it.
void MultiplayerSession::collectContacts(int32_t id, const CarSimState &before, const CarSimState &after)
{
    if (before.alive && !after.alive)
        contacts_.push_back({ContactEvent::Wreck, id, after.position, std::max(after.impactWall, params_.crashImpactSpeed)});
    else if (after.impactWall > 1.0f)
        contacts_.push_back({ContactEvent::Barrier, id, after.position, after.impactWall});
    if (after.impactCar > 0.15f) contacts_.push_back({ContactEvent::Bump, id, after.position, after.impactCar});
    if (contacts_.size() > 64) contacts_.erase(contacts_.begin(), contacts_.begin() + 32); // nobody listening
}

// --------------------------------------------------------------------------
// queries
// --------------------------------------------------------------------------

// True once our car is out (host: from the race rules; client: from the last
// snapshot's flags).
bool MultiplayerSession::localEliminated() const
{
    if (isHost_)
    {
        auto it = cars_.find(localId_);
        return race_.isEliminated(localId_) || (it != cars_.end() && it->second.left);
    }
    auto it = lastCars_.find(localId_);
    return it != lastCars_.end() && (it->second.flags & net::kCarEliminated);
}

// Our car's simulated state: authoritative on the host, predicted on a client.
const CarSimState *MultiplayerSession::localCar() const
{
    if (isHost_)
    {
        auto it = cars_.find(localId_);
        return it == cars_.end() ? nullptr : &it->second.state;
    }
    return &local_;
}

// Stored prediction for a given local tick (nullptr if overwritten or unset).
const CarSimState *MultiplayerSession::predictedStateAt(uint32_t tick) const
{
    const HistoryEntry &h = history_[tick % kHistory];
    return h.tick == tick && tick != 0 ? &h.state : nullptr;
}

// Host only: a car's authoritative state and the newest input tick applied.
const CarSimState *MultiplayerSession::hostCar(int32_t playerId, uint32_t *lastInputTick) const
{
    auto it = cars_.find(playerId);
    if (it == cars_.end()) return nullptr;
    if (lastInputTick) *lastInputTick = it->second.lastApplied;
    return &it->second.state;
}

// Build per-frame render views; alpha in [0, 1] is how far we are between
// the previous and current fixed tick. Host: blend prev/state of each car.
// Client: our car is the predicted state plus the decaying visual offset;
// remote cars are dead-reckoned to the present or interpolated from their
// sample buffer at renderTick (host clock minus the interpolation delay).
std::vector<CarView> MultiplayerSession::carViews(float alpha) const
{
    std::vector<CarView> out;
    alpha = dm::clampf(alpha, 0.0f, 1.0f);
    if (isHost_)
    {
        for (const auto &kv : cars_)
        {
            CarView v;
            v.playerId = kv.first;
            v.local = kv.first == localId_;
            v.bot = kv.second.bot;
            v.look = lookFor(kv.first);
            fillView(v, kv.second.prev, kv.second.state, alpha);
            v.eliminatedOrder = race_.eliminatedOrder(kv.first);
            v.eliminated = v.eliminatedOrder != 0 || kv.second.left;
            v.place = race_.started() ? race_.place(kv.first) : 0;
            out.push_back(v);
        }
        return out;
    }

    CarView me;
    auto mine = lastCars_.find(localId_);
    if (mine != lastCars_.end()) fillView(me, mine->second);
    me.playerId = localId_;
    me.local = true;
    me.look = lookFor(localId_);
    fillView(me, localPrev_, local_, alpha);
    me.position += visualOffset_;
    me.rotation = dm::normalize(visualRotOffset_ * me.rotation);
    out.push_back(me);

    // Interpolation mode only: render time in host ticks.
    double renderTick = hostClock_ + alpha - cfg_.interpolationDelayTicks;
    for (const auto &kv : remotes_)
    {
        const Remote &r = kv.second;
        CarView v;
        if (cfg_.extrapolateRemotes)
        {
            // Same dead-reckoned present the prediction collides with.
            net::CarState c = deadReckon(r.latest, localTick_ - 1, alpha);
            fillView(v, c);
            v.position += r.offset;
            v.rotation = dm::normalize(r.rotOffset * v.rotation);
        }
        else
        {
            const std::deque<RemoteSample> &buf = r.samples;
            if (buf.empty()) continue;
            net::CarState a = buf.front().car, b = a;
            float t = 0;
            // Interpolation: before the first sample hold it; after the last extrapolate
            // briefly (max 0.25 s) along the velocity; otherwise find the bracketing
            // pair and lerp by t.
            if (renderTick <= buf.front().hostTick) {}
            else if (renderTick >= buf.back().hostTick)
            {
                a = b = buf.back().car;
                float dt = static_cast<float>(std::min((renderTick - buf.back().hostTick) / kTickRate, 0.25));
                b.position = a.position + a.velocity * dt;
                t = 1;
            }
            else
                for (size_t i = 0; i + 1 < buf.size(); ++i)
                    if (renderTick < buf[i + 1].hostTick)
                    {
                        a = buf[i].car;
                        b = buf[i + 1].car;
                        t = static_cast<float>((renderTick - buf[i].hostTick) /
                                               static_cast<double>(buf[i + 1].hostTick - buf[i].hostTick));
                        break;
                    }
            fillView(v, b);
            v.position = dm::lerp(a.position, b.position, t);
            v.rotation = dm::slerp(a.rotation, b.rotation, t);
            v.velocity = dm::lerp(a.velocity, b.velocity, t);
        }
        v.playerId = kv.first;
        v.look = lookFor(kv.first);
        v.bot = isBot(kv.first);
        out.push_back(v);
    }
    return out;
}

} // namespace game
