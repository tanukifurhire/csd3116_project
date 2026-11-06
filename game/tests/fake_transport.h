// fake_transport.h -- in-process stand-in for net::NetClient used by the
// game tests: a hub that delays packets by a configurable latency (+jitter)
// and drops a configurable fraction, deterministically (own PCG32).
//
// Mirrors the NetClient contract the session relies on:
//   * sendInput is best-effort; like the real net layer, each packet also
//     carries the previous `redundancy - 1` inputs so single drops are absorbed;
//   * takeInputs (host) returns everything delivered so far, de-duplicated;
//   * takeLatestSnapshot returns only the newest delivered snapshot.
#pragma once

#include <map>
#include <set>
#include <utility>
#include <vector>

#include "game/include/multiplayer_session.h"
#include "game/include/rng.h"

namespace gametest {

class FakeHub
{
public:
    int latencyTicks = 5;  // one-way
    int jitterTicks = 0;   // +0..jitter extra
    float lossRate = 0.0f; // per packet
    int redundancy = 3;
    int32_t hostId = 1;
    uint32_t now = 0;
    game::Pcg32 rng{12345, 99};

    struct InputPacket { uint32_t deliverAt; int32_t from; std::vector<net::CarInput> inputs; };
    struct SnapPacket { uint32_t deliverAt; int32_t to; net::WorldSnapshot snap; };
    std::vector<InputPacket> inputsInFlight;
    std::vector<SnapPacket> snapsInFlight;
    std::vector<int32_t> clients;

    // delivered, waiting to be taken
    std::vector<net::NetClient::PlayerInput> hostInbox;
    std::set<std::pair<int32_t, uint32_t>> seenInputs;
    std::map<int32_t, net::WorldSnapshot> latestSnap;
    std::map<int32_t, bool> hasSnap;

    bool dropped() { return lossRate > 0 && rng.nextFloat() < lossRate; }
    uint32_t delay() { return now + static_cast<uint32_t>(latencyTicks + (jitterTicks ? rng.range(0, jitterTicks + 1) : 0)); }

    void advance()
    {
        ++now;
        for (size_t i = 0; i < inputsInFlight.size();)
        {
            if (inputsInFlight[i].deliverAt <= now)
            {
                for (const net::CarInput &in : inputsInFlight[i].inputs)
                    if (seenInputs.insert({inputsInFlight[i].from, in.tick}).second)
                        hostInbox.push_back({inputsInFlight[i].from, in});
                inputsInFlight.erase(inputsInFlight.begin() + static_cast<long>(i));
            }
            else
                ++i;
        }
        for (size_t i = 0; i < snapsInFlight.size();)
        {
            if (snapsInFlight[i].deliverAt <= now)
            {
                const SnapPacket &p = snapsInFlight[i];
                // Newest wins (by hostTick), like takeLatestSnapshot; a
                // reordered older packet is ignored.
                if (!hasSnap[p.to] || p.snap.hostTick > latestSnap[p.to].hostTick)
                {
                    latestSnap[p.to] = p.snap;
                    hasSnap[p.to] = true;
                    pendingTaken[p.to] = false;
                }
                snapsInFlight.erase(snapsInFlight.begin() + static_cast<long>(i));
            }
            else
                ++i;
        }
    }
    std::map<int32_t, bool> pendingTaken;
};

class FakeEndpoint : public game::INetTransport
{
public:
    FakeEndpoint(FakeHub &hub, int32_t id) : hub_(hub), id_(id) { hub_.clients.push_back(id); }

    int32_t localPlayerId() const override { return id_; }

    void sendInput(const net::CarInput &input) override
    {
        recent_.push_back(input);
        if (static_cast<int>(recent_.size()) > hub_.redundancy) recent_.erase(recent_.begin());
        ++inputsSent;
        if (id_ == hub_.hostId) return; // host short-circuits locally (session applies directly)
        if (hub_.dropped()) return;
        hub_.inputsInFlight.push_back({hub_.delay(), id_, recent_});
    }

    bool takeLatestSnapshot(net::WorldSnapshot &out) override
    {
        if (!hub_.hasSnap[id_] || hub_.pendingTaken[id_]) return false;
        out = hub_.latestSnap[id_];
        hub_.pendingTaken[id_] = true;
        return true;
    }

    std::vector<net::NetClient::PlayerInput> takeInputs() override
    {
        if (id_ != hub_.hostId) return {};
        std::vector<net::NetClient::PlayerInput> v;
        v.swap(hub_.hostInbox);
        return v;
    }

    void publishSnapshot(const net::WorldSnapshot &snapshot) override
    {
        ++snapshotsPublished;
        for (int32_t c : hub_.clients)
        {
            if (c == id_) continue;
            if (hub_.dropped()) continue;
            hub_.snapsInFlight.push_back({hub_.delay(), c, snapshot});
        }
    }

    void startMatch(uint32_t seed, uint32_t startTick) override { startedSeed = seed; startedTick = startTick; ++matchesStarted; }
    void setPhase(net::MatchPhase phase) override { phases.push_back(phase); }

    int inputsSent = 0, snapshotsPublished = 0, matchesStarted = 0;
    uint32_t startedSeed = 0, startedTick = 0;
    std::vector<net::MatchPhase> phases;

private:
    FakeHub &hub_;
    int32_t id_;
    std::vector<net::CarInput> recent_;
};

} // namespace gametest
