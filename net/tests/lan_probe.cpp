// lan_probe.cpp -- headless LAN-mode probe for cross-machine / cross-OS
// checks (e.g. native Windows <-> WSL). Prints every NetClient event.
//   lan_probe host NAME [--peer IP]... [--insecure] [--cert N] [--secs S]
//   lan_probe join NAME [--peer IP]... [--insecure] [--cert N] [--secs S]
// host: creates room "probe" and publishes 30 Hz snapshots; join: joins the
// first room named "probe", sends inputs, counts snapshots, prints RTT.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "../include/net_client.h"

using namespace net;

// Seconds on the steady clock.
static double now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Printable name; the array order must match enum EventType.
static const char *evName(EventType t)
{
    static const char *n[] = {"LoginOk", "LoginFailed", "RoomList", "JoinedRoom", "JoinFailed", "LeftRoom",
                              "RosterChanged", "HostChanged", "MatchStarted", "PhaseChanged", "TransportChanged",
                              "Disconnected"};
    return n[(int)t];
}

// Event loop at ~60 Hz. Host: wait for LoginOk, create the room, then publish
// snapshots and count inputs from others. Guest: poll the room list until
// "probe" appears, join it, then send an input per loop and count snapshots.
// Exit code 2 = bad usage, 1 = start() failed.
int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3)
    {
        std::printf("usage: lan_probe host|join NAME [--peer IP]... [--insecure] [--cert N] [--secs S]\n");
        return 2;
    }
    bool host = std::strcmp(argv[1], "host") == 0;
    std::string name = argv[2];
    NetClient::Config cfg;
    cfg.mode = NetClient::Mode::Lan;
    double secs = 30;
    for (int i = 3; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--peer" && i + 1 < argc) cfg.lanPeers.push_back(argv[++i]);
        else if (a == "--insecure") cfg.insecure = true;
        else if (a == "--cert" && i + 1 < argc) cfg.certIndex = std::atoi(argv[++i]);
        else if (a == "--secs" && i + 1 < argc) secs = std::atof(argv[++i]);
    }
    // (flags above only fill cfg; nothing touches the network until start())
    NetClient nc;
    if (!nc.start(cfg)) return 1;
    nc.login(name, "");
    // asked: JoinRoom already requested; joined: JoinedRoom event seen
    double t0 = now(), lastList = 0, lastSnap = 0;
    bool joined = false, asked = false;
    uint32_t tick = 0;
    int snaps = 0, inputs = 0;
    while (now() - t0 < secs)
    {
        for (const Event &e : nc.pollEvents())
        {
            std::printf("%6.2f %s %s room=%u pid=%d host=%d players=%zu rooms=%zu %s\n", now() - t0, name.c_str(),
                        evName(e.type), e.roomId, e.localPlayerId, e.isHost, e.roster.size(), e.rooms.size(),
                        e.message.c_str());
            if (e.type == EventType::LoginOk && host) nc.createRoom("probe");
            if (e.type == EventType::JoinedRoom) joined = true;
            if (e.type == EventType::RoomList && !host && !asked)
                for (const RoomInfo &r : e.rooms)
                    if (r.name == "probe")
                    {
                        nc.joinRoom(r.roomId);
                        asked = true;
                    }
        }
        double t = now();
        // guests re-request the room list every 0.5 s until the host's room shows up
        if (!host && !asked && t - lastList > 0.5)
        {
            nc.requestRoomList();
            lastList = t;
        }
        if (joined && host && t - lastSnap > 1.0 / 30)
        {
            WorldSnapshot s;
            s.hostTick = ++tick;
            s.cars.resize(1);
            nc.publishSnapshot(s);
            lastSnap = t;
            for (auto &pi : nc.takeInputs()) inputs += pi.playerId != nc.localPlayerId();
        }
        if (joined && !host)
        {
            CarInput in;
            in.tick = ++tick;
            nc.sendInput(in);
            WorldSnapshot s;
            if (nc.takeLatestSnapshot(s)) snaps++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    if (host) std::printf("%s: host got %d inputs from others\n", name.c_str(), inputs);
    else std::printf("%s: got %d snapshots, rtt %.2f ms, transport %d\n", name.c_str(), snaps, nc.rttMs(), (int)nc.transport());
    nc.stop();
    return 0;
}
