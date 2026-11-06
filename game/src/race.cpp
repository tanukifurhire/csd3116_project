// Multi-car rules: slipstream, car-to-car collisions, the world step, the
// elimination race state machine, the nitro rubber band and the bot driver.
// Collisions work in the XZ (road) plane with oriented boxes, using only yaw
// rotation; Vec2f holds (x, z). Main loop: stepWorld() runs once per fixed
// tick on the host for all cars and on a client for its predicted car (other
// cars are "proxies": they push the local car but are never pushed back).
// Elimination rules: the car in last place on track progress (distance, m)
// runs a countdown whose length is stageTimes[stage] (32,16,8,4,2,1 s). When
// last place changes hands (with an overtake margin) the stage advances and
// the clock restarts; when the clock hits zero that car is eliminated, the
// stage resets to 0, and the last survivor wins. Everything here is
// deterministic and single-threaded.
// race.cpp -- see race.h.
#include "game/include/race.h"

#include <algorithm>
#include <cmath>

namespace game {

namespace {

struct Vec2f
{
    float x, z;
};
inline Vec2f v2(const dm::Vec3 &v) { return {v.x, v.z}; }
inline float dot2(Vec2f a, Vec2f b) { return a.x * b.x + a.z * b.z; }
inline Vec2f sub2(Vec2f a, Vec2f b) { return {a.x - b.x, a.z - b.z}; }
inline Vec2f add2(Vec2f a, Vec2f b) { return {a.x + b.x, a.z + b.z}; }
inline Vec2f mul2(Vec2f a, float s) { return {a.x * s, a.z * s}; }
inline Vec2f norm2(Vec2f a)
{
    float l = std::sqrt(dot2(a, a));
    return l > 1e-6f ? Vec2f{a.x / l, a.z / l} : Vec2f{0, 0};
}
// Planar 2D cross product: torque about +Y of force f at offset r.
// (r x F).y for vectors in the XZ plane: torque about +Y.
inline float cross2(Vec2f r, Vec2f f) { return r.z * f.x - r.x * f.z; }

// Oriented box of a car in the road plane.
// Footprint of a car on the road plane: centre, unit axes, half extents (m).
struct Box
{
    Vec2f c, ax[2]; // ax[0] = right, ax[1] = forward (unit)
    float h[2];     // half width, half length
    void corners(Vec2f out[4]) const
    {
        int k = 0;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sz = -1; sz <= 1; sz += 2)
                out[k++] = add2(c, add2(mul2(ax[0], sx * h[0]), mul2(ax[1], sz * h[1])));
    }
    bool contains(Vec2f p) const
    {
        Vec2f d = sub2(p, c);
        return std::fabs(dot2(d, ax[0])) <= h[0] + 1e-4f && std::fabs(dot2(d, ax[1])) <= h[1] + 1e-4f;
    }
    float radiusAlong(Vec2f n) const { return h[0] * std::fabs(dot2(ax[0], n)) + h[1] * std::fabs(dot2(ax[1], n)); }
};

// Box from the car's position and yaw, using the physics half sizes.
Box boxOf(const CarSimState &s, const CarParams &p)
{
    Box b;
    b.c = v2(s.position);
    b.ax[0] = norm2(v2(dm::rightOf(s.rotation)));
    b.ax[1] = norm2(v2(dm::forwardOf(s.rotation)));
    b.h[0] = p.halfWidth;
    b.h[1] = p.halfLength;
    return b;
}

// Yaw rate used for contact velocities: body yaw + collision spin.
float yawRate(const CarSimState &s) { return dm::dot(s.angularVelocity, dm::upOf(s.rotation)) + s.spinRate; }

// Rigid-body velocity of the point at offset r from the centre: v + w x r.
Vec2f pointVelocity(const CarSimState &s, Vec2f r)
{
    float w = yawRate(s);
    return {s.velocity.x + w * r.z, s.velocity.z - w * r.x};
}

} // namespace

const RaceParams &defaultRaceParams()
{
    static const RaceParams params{};
    return params;
}

// ---------------------------------------------------------------------------
// slipstream
// ---------------------------------------------------------------------------

// True when `f` (follower) sits in the wake of `l` (leader): both alive, similar
// height, both fast, travelling in similar directions, and the follower is
// behind the leader between slipstreamMinGap and slipstreamRange metres,
// within a cone that widens from NearHalfWidth to HalfWidth with distance.
bool inSlipstream(const CarSimState &f, const CarSimState &l, const RaceParams &r)
{
    if (!f.alive || !l.alive) return false;
    if (std::fabs(f.position.y - l.position.y) > r.slipstreamMaxHeight) return false;
    Vec2f lv = v2(l.velocity), fv = v2(f.velocity);
    float ls = std::sqrt(dot2(lv, lv)), fs = std::sqrt(dot2(fv, fv));
    if (ls < r.slipstreamMinSpeed || fs < r.slipstreamMinSpeed) return false;
    Vec2f dir = mul2(lv, 1.0f / ls); // wake runs opposite the leader's travel
    if (dot2(dir, mul2(fv, 1.0f / fs)) < r.slipstreamAlign) return false;
    Vec2f rel = sub2(v2(f.position), v2(l.position));
    float behind = -dot2(rel, dir);
    if (behind < r.slipstreamMinGap || behind > r.slipstreamRange) return false;
    float lateral = std::fabs(cross2(rel, dir));
    float halfWidth = dm::lerpf(r.slipstreamNearHalfWidth, r.slipstreamHalfWidth, behind / r.slipstreamRange);
    return lateral <= halfWidth;
}

// ---------------------------------------------------------------------------
// collisions
// ---------------------------------------------------------------------------

// Resolve one car pair. Returns true when the boxes overlap. Steps: broad
// phase, SAT over the four box axes (smallest overlap = contact normal),
// contact point, positional split, then an impulse with restitution and
// Coulomb friction using yaw inertia only. Proxies are never moved or
// accelerated, so the same call yields the same result on host and client
// for the car that is simulated locally.
bool collideCars(CarSimState &a, bool aProxy, CarSimState &b, bool bProxy, const CarParams &p, const RaceParams &r)
{
    if (!a.alive || !b.alive) return false; // wrecks are ghosts until they respawn...
    if (a.ghostTimer > 0 || b.ghostTimer > 0) return false; // ...and for a moment after
    if (std::fabs(a.position.y - b.position.y) > r.maxHeightDiff) return false;
    Box A = boxOf(a, p), B = boxOf(b, p);
    Vec2f d = sub2(B.c, A.c);
    // Broad phase: skip pairs further apart than the box diagonals could reach.
    if (dot2(d, d) > 4.0f * (p.halfLength + p.halfWidth) * (p.halfLength + p.halfWidth)) return false;

    // Separating axis test over the 4 box axes; keep the shallowest axis.
    const Vec2f axes[4] = {A.ax[0], A.ax[1], B.ax[0], B.ax[1]};
    float pen = 1e9f;
    Vec2f n{0, 0};
    for (const Vec2f &L : axes)
    {
        float overlap = A.radiusAlong(L) + B.radiusAlong(L) - std::fabs(dot2(d, L));
        if (overlap <= 0) return false;
        if (overlap < pen)
        {
            pen = overlap;
            n = dot2(d, L) >= 0 ? L : mul2(L, -1.0f); // from A towards B
        }
    }

    // Contact point: corners of one box inside the other (else the midpoint
    // of the two support points along the normal).
    Vec2f ca[4], cb[4];
    A.corners(ca);
    B.corners(cb);
    Vec2f sum{0, 0};
    int count = 0;
    for (const Vec2f &c : ca)
        if (B.contains(c)) { sum = add2(sum, c); ++count; }
    for (const Vec2f &c : cb)
        if (A.contains(c)) { sum = add2(sum, c); ++count; }
    Vec2f contact;
    if (count > 0) contact = mul2(sum, 1.0f / count);
    else
    {
        Vec2f sa = add2(A.c, mul2(n, A.radiusAlong(n))), sb = sub2(B.c, mul2(n, B.radiusAlong(n)));
        contact = mul2(add2(sa, sb), 0.5f);
    }

    // Positional correction: split the overlap (a proxy never moves, and the
    // dynamic car only takes its own half, exactly what the host gives it).
    float push = 0.5f * (pen + r.penetrationSlop);
    if (!aProxy) { a.position.x -= n.x * push; a.position.z -= n.z * push; }
    if (!bProxy) { b.position.x += n.x * push; b.position.z += n.z * push; }

    // Impulse at the contact (equal masses, yaw inertia only).
    Vec2f ra = sub2(contact, A.c), rb = sub2(contact, B.c);
    Vec2f vrel = sub2(pointVelocity(b, rb), pointVelocity(a, ra));
    float vn = dot2(vrel, n);
    if (vn >= 0) return true; // already separating
    const float closing = -vn;

    // Who is doing the hitting: each car's own speed towards the other.
    // dominance > 0 = A is the attacker; |dominance| >= 1: the victim was
    // standing still or moving away along the normal (rear-end, T-bone).
    const float approachA = dot2(v2(a.velocity), n), approachB = -dot2(v2(b.velocity), n);
    const float dominance = (approachA - approachB) / closing;
    const bool aAttacks = dominance > 0;
    const float dom = std::fabs(dominance);

    // Ram bias: in a real hit the attacker acts heavier, so it is not
    // dragged off its line and the victim takes the impact.
    float bias = 0;
    if (r.ramBias > 0 && closing > r.rammingSpeed)
        bias = r.ramBias * dm::clampf((closing - r.rammingSpeed) / std::fmax(r.rammingSpeedFull - r.rammingSpeed, 1e-3f), 0.0f, 1.0f) *
               dm::clampf(dom, 0.0f, 1.0f);
    const float invM = 1.0f / p.mass, invI = 1.0f / p.inertia.y;
    const float wa = aAttacks ? 1.0f - bias : 1.0f + bias, wb = aAttacks ? 1.0f + bias : 1.0f - bias;
    const float invMa = invM * wa, invMb = invM * wb, invIa = invI * wa, invIb = invI * wb;

    float rna = cross2(ra, n), rnb = cross2(rb, n);
    float kn = invMa + invMb + rna * rna * invIa + rnb * rnb * invIb;
    float jn = -(1.0f + r.restitution) * vn / kn;

    // Coulomb friction along the sliding direction.
    Vec2f vt = sub2(vrel, mul2(n, vn));
    Vec2f t = norm2(vt);
    float rta = cross2(ra, t), rtb = cross2(rb, t);
    float kt = invMa + invMb + rta * rta * invIa + rtb * rtb * invIb;
    float jt = dm::clampf(-dot2(vrel, t) / kt, -r.friction * jn, r.friction * jn);

    // Takedown: a hard, one-sided hit wrecks the victim; the attacker only
    // feels takedownAttackerShare of the impulse and keeps its momentum.
    // Measured on the bodies (centre-of-mass velocities), not the contact
    // point: a spinning, drifting car's corners must not count as a ram.
    const float closingBody = approachA + approachB;
    const float speedA = std::sqrt(dot2(v2(a.velocity), v2(a.velocity))), speedB = std::sqrt(dot2(v2(b.velocity), v2(b.velocity)));
    const float advantage = aAttacks ? speedA - speedB : speedB - speedA;
    // ...and it has to be a ram: the attacker drives INTO the other car
    // (its travel direction close to the contact normal), not two drifting
    // cars rubbing sideways through a corner.
    const float aim = aAttacks ? approachA / std::fmax(speedA, 1e-3f) : approachB / std::fmax(speedB, 1e-3f);
    const bool takedown = r.takedowns && closingBody >= r.takedownSpeed && dom >= r.takedownDominance &&
                          advantage >= r.takedownSpeedAdvantage && aim >= r.takedownAim;
    const float shareA = takedown && aAttacks ? r.takedownAttackerShare : 1.0f;
    const float shareB = takedown && !aAttacks ? r.takedownAttackerShare : 1.0f;

    Vec2f J = add2(mul2(n, jn), mul2(t, jt)); // impulse on B (A gets -J)
    float jMag = std::sqrt(dot2(J, J)) * invM;
    a.impactCar = std::fmax(a.impactCar, jMag); // audio / effects hook (also on proxies: harmless)
    b.impactCar = std::fmax(b.impactCar, jMag);
    // Yaw goes into the spin rate (the body's angular drag would erase it).
    if (!aProxy)
    {
        a.velocity.x -= J.x * invMa * shareA;
        a.velocity.z -= J.z * invMa * shareA;
        a.spinRate = dm::clampf(a.spinRate - cross2(ra, J) * invIa * shareA * r.spinScale, -p.maxSpinRate, p.maxSpinRate);
    }
    if (!bProxy)
    {
        b.velocity.x += J.x * invMb * shareB;
        b.velocity.z += J.z * invMb * shareB;
        b.spinRate = dm::clampf(b.spinRate + cross2(rb, J) * invIb * shareB * r.spinScale, -p.maxSpinRate, p.maxSpinRate);
    }
    if (takedown)
    {
        CarSimState &attacker = aAttacks ? a : b, &victim = aAttacks ? b : a;
        const bool attackerProxy = aAttacks ? aProxy : bProxy, victimProxy = aAttacks ? bProxy : aProxy;
        if (!victimProxy)
        {
            wreckCar(victim);
            victim.takedownEvent = 2;
        }
        if (!attackerProxy)
        {
            attacker.takedownEvent = 1;
            ++attacker.takedowns;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// world step
// ---------------------------------------------------------------------------

// Advance all cars one fixed tick (dt seconds). Cars are processed in id order
// so results do not depend on vector order. Proxy cars are not stepped.
void stepWorld(std::vector<WorldCar> &cars, const Track &track, const CarParams &params, const RaceParams &race,
               float dt)
{
    std::vector<WorldCar *> order;
    order.reserve(cars.size());
    for (WorldCar &c : cars)
        if (c.state) order.push_back(&c);
    std::sort(order.begin(), order.end(), [](const WorldCar *x, const WorldCar *y) { return x->id < y->id; });

    // 1) Slipstream from the positions at the start of the tick.
    for (WorldCar *c : order)
    {
        if (c->proxy) continue;
        bool wake = false;
        for (WorldCar *o : order)
            if (o != c && inSlipstream(*c->state, *o->state, race)) { wake = true; break; }
        c->state->slipstream = wake;
    }
    // 2) Integrate every simulated car.
    for (WorldCar *c : order)
        if (!c->proxy)
        {
            CarSimState &s = *c->state;
            stepCar(s, c->input, track, params, dt);
            s.impactCar = 0;
            s.takedownEvent = 0;
            // Prototype (off by default): any real barrier touch wrecks.
            if (race.wallTouchWrecks && s.alive && s.impactWall > race.wallTouchSpeed) wreckCar(s);
        }
    // 3) Car-to-car contacts, pairs in id order (two passes for 3-4 car piles).
    bool touched = false;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < order.size(); ++i)
            for (size_t j = i + 1; j < order.size(); ++j)
            {
                if (order[i]->proxy && order[j]->proxy) continue;
                touched |= collideCars(*order[i]->state, order[i]->proxy, *order[j]->state, order[j]->proxy, params, race);
            }
    // 4) Contacts moved cars sideways: refresh their track position so race
    //    progress / places use where they really are.
    if (touched)
        for (WorldCar *c : order)
        {
            if (c->proxy) continue;
            CarSimState &s = *c->state;
            RoadQuery q = track.query(s.position, s.section);
            s.section = q.section;
            s.sectionS = q.s;
            s.lateral = q.d;
            s.distance = q.distance;
        }
}

// ---------------------------------------------------------------------------
// rubber band
// ---------------------------------------------------------------------------

// Nitro rubber band: interpolate the recharge multiplier from leader (slow)
// to last place (fast) by progress; degenerate spread falls back to the ends.
float rechargeMultiplier(float progress, float leader, float last, const RaceParams &r)
{
    float span = leader - last;
    if (span < 1e-3f) return progress >= leader ? r.rechargeLeader : r.rechargeLast;
    float t = dm::clampf((leader - progress) / span, 0.0f, 1.0f); // 0 = leader, 1 = last
    return dm::lerpf(r.rechargeLeader, r.rechargeLast, t);
}

// ---------------------------------------------------------------------------
// elimination race
// ---------------------------------------------------------------------------

EliminationRace::Racer *EliminationRace::find(int32_t id)
{
    for (Racer &r : racers_)
        if (r.id == id) return &r;
    return nullptr;
}

const EliminationRace::Racer *EliminationRace::find(int32_t id) const
{
    for (const Racer &r : racers_)
        if (r.id == id) return &r;
    return nullptr;
}

// Number of racers not yet eliminated. (Racer lookup by id is a linear scan: rooms hold only a few players.)
int EliminationRace::racingCount() const
{
    int n = 0;
    for (const Racer &r : racers_) n += r.eliminatedOrder == 0;
    return n;
}

uint8_t EliminationRace::eliminatedOrder(int32_t id) const
{
    const Racer *r = find(id);
    return r ? r->eliminatedOrder : 0;
}

uint8_t EliminationRace::place(int32_t id) const
{
    const Racer *r = find(id);
    return r ? r->place : 0;
}

float EliminationRace::rechargeMultiplierFor(int32_t id) const
{
    const Racer *r = find(id);
    return r ? r->multiplier : 1.0f;
}

// Begin a race for these players: stage 0, nobody eliminated.
void EliminationRace::start(const std::vector<int32_t> &ids)
{
    racers_.clear();
    for (int32_t id : ids) racers_.push_back(Racer{id});
    std::sort(racers_.begin(), racers_.end(), [](const Racer &a, const Racer &b) { return a.id < b.id; });
    started_ = true;
    finished_ = false;
    stage_ = 0;
    timer_ = stageTime(0);
    lastPlaceId_ = 0;
    winnerId_ = 0;
    eliminatedCount_ = 0;
    rank(); // last place is assigned by the first update(), from real progress
}

// Rebuild state from a snapshot (host migration): distances, elimination
// order, clock, stage, last place and winner come from the snapshot.
void EliminationRace::restore(const net::WorldSnapshot &snap)
{
    racers_.clear();
    eliminatedCount_ = 0;
    for (const net::CarState &c : snap.cars)
    {
        Racer r{c.playerId};
        r.distance = c.distance;
        r.eliminatedOrder = c.eliminatedOrder;
        eliminatedCount_ = std::max(eliminatedCount_, c.eliminatedOrder);
        racers_.push_back(r);
    }
    std::sort(racers_.begin(), racers_.end(), [](const Racer &a, const Racer &b) { return a.id < b.id; });
    started_ = snap.phase == net::MatchPhase::Racing || snap.phase == net::MatchPhase::Results;
    timer_ = snap.eliminationTimer;
    stage_ = snap.eliminationStage;
    lastPlaceId_ = snap.lastPlacePlayerId;
    winnerId_ = snap.winnerPlayerId;
    finished_ = winnerId_ != 0;
    rank();
}

// Recompute places (1 = furthest ahead) among non-eliminated racers and
// their nitro recharge multipliers. Leader gets the minimum, last place the
// maximum, others linearly in between.
void EliminationRace::rank()
{
    std::vector<Racer *> live;
    for (Racer &r : racers_)
    {
        r.place = 0;
        if (r.eliminatedOrder == 0) live.push_back(&r);
    }
    // Further along = better; ties go to the lower id (deterministic).
    std::sort(live.begin(), live.end(), [](const Racer *a, const Racer *b) {
        return a->distance != b->distance ? a->distance > b->distance : a->id < b->id;
    });
    for (size_t i = 0; i < live.size(); ++i) live[i]->place = static_cast<uint8_t>(i + 1);
    if (live.empty()) return;
    float leader = live.front()->distance, last = live.back()->distance;
    for (size_t i = 0; i < live.size(); ++i)
    {
        Racer *r = live[i];
        if (live.size() == 1) r->multiplier = 1.0f;
        else if (i == 0) r->multiplier = p_.rechargeLeader;
        else if (i + 1 == live.size()) r->multiplier = p_.rechargeLast;
        else r->multiplier = rechargeMultiplier(r->distance, leader, last, p_);
    }
}

// Id of the live racer with the worst place (0 if none).
int32_t EliminationRace::currentLast() const
{
    int32_t id = 0;
    uint8_t worst = 0;
    for (const Racer &r : racers_)
        if (r.eliminatedOrder == 0 && r.place > worst) { worst = r.place; id = r.id; }
    return id;
}

// Eliminate a racer: record its order, reset the clock to stage 0 and
// announce the winner when one racer remains.
void EliminationRace::eliminate(int32_t id, std::vector<Event> &events)
{
    Racer *r = find(id);
    if (!r || r->eliminatedOrder != 0) return;
    r->eliminatedOrder = ++eliminatedCount_;
    // An elimination puts the clock back to the first stage.
    stage_ = 0;
    timer_ = stageTime(0);
    events.push_back({Event::Eliminated, id, stage_});
    rank();
    lastPlaceId_ = currentLast();
    if (racingCount() == 1)
    {
        winnerId_ = lastPlaceId_; // the only one left
        finished_ = true;
        events.push_back({Event::Winner, winnerId_, stage_});
    }
}

// A player leaving mid-race counts as eliminated; before the start or after
// the finish they are simply dropped.
void EliminationRace::removePlayer(int32_t id, std::vector<Event> *events)
{
    std::vector<Event> local;
    if (started_ && !finished_) eliminate(id, events ? *events : local);
    else racers_.erase(std::remove_if(racers_.begin(), racers_.end(), [id](const Racer &r) { return r.id == id; }),
                       racers_.end());
}

// Final ranking: the winner first, then surviving racers by place, then the
// eliminated ones, latest eliminated first.
std::vector<int32_t> EliminationRace::finishingOrder() const
{
    std::vector<const Racer *> v;
    for (const Racer &r : racers_) v.push_back(&r);
    int32_t w = winnerId_;
    std::sort(v.begin(), v.end(), [w](const Racer *a, const Racer *b) {
        if ((a->id == w) != (b->id == w)) return a->id == w;
        if ((a->eliminatedOrder == 0) != (b->eliminatedOrder == 0)) return a->eliminatedOrder == 0;
        if (a->eliminatedOrder == 0) return a->place < b->place;
        return a->eliminatedOrder > b->eliminatedOrder; // eliminated later = finished higher
    });
    std::vector<int32_t> ids;
    for (const Racer *r : v) ids.push_back(r->id);
    return ids;
}

// Overall time cap: whoever is first wins.
void EliminationRace::finishByLeader()
{
    if (finished_ || !started_) return;
    rank();
    for (const Racer &r : racers_)
        if (r.place == 1) winnerId_ = r.id;
    finished_ = true;
}

// Advance the race clock by dt seconds with the latest progress of each car.
// Returns the events (last place changed / eliminated / winner) this tick.
std::vector<EliminationRace::Event> EliminationRace::update(const std::vector<Progress> &progress, float dt)
{
    std::vector<Event> events;
    if (!started_ || finished_) return events;
    for (const Progress &pr : progress)
        if (Racer *r = find(pr.playerId)) r->distance = pr.distance;
    rank();
    if (racers_.size() < 2) return events; // solo: nobody to race, no clock

    // Last place changing hands (with hysteresis) => next, shorter stage.
    int32_t candidate = currentLast();
    const Racer *cur = find(lastPlaceId_);
    if (!cur || cur->eliminatedOrder != 0)
        lastPlaceId_ = candidate; // first tick / after an elimination: no stage change
    else if (candidate != lastPlaceId_ && cur->distance > find(candidate)->distance + p_.overtakeMargin)
    {
        stage_ = static_cast<uint8_t>(std::min(stage_ + 1, 5));
        timer_ = stageTime(stage_);
        lastPlaceId_ = candidate;
        events.push_back({Event::LastPlaceChanged, candidate, stage_});
    }

    timer_ -= dt;
    if (timer_ <= 0) eliminate(lastPlaceId_, events);
    return events;
}

} // namespace game
