// net_transport.h -- adapts net::NetClient to game::INetTransport so the
// MultiplayerSession (game/) never depends on the real network objects.
#pragma once

#include "game/include/multiplayer_session.h"
#include "net/include/net_client.h"

namespace client {

class NetClientTransport : public game::INetTransport
{
public:
    explicit NetClientTransport(net::NetClient &net) : net_(net) {}

    int32_t localPlayerId() const override { return net_.localPlayerId(); }
    void sendInput(const net::CarInput &input) override { net_.sendInput(input); }
    bool takeLatestSnapshot(net::WorldSnapshot &out) override { return net_.takeLatestSnapshot(out); }
    std::vector<net::NetClient::PlayerInput> takeInputs() override { return net_.takeInputs(); }
    void publishSnapshot(const net::WorldSnapshot &snapshot) override { net_.publishSnapshot(snapshot); }
    void startMatch(uint32_t trackSeed, uint32_t startTick) override { net_.startMatch(trackSeed, startTick); }
    void setPhase(net::MatchPhase phase) override { net_.setPhase(phase); }

private:
    net::NetClient &net_;
};

} // namespace client
