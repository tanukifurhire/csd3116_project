// net_tests.cpp -- end-to-end tests for the Drifto net module.
//
// Spawns two local deployments:
//   A: api_server :17600 + game_server "gsA" (UDP 17700/17701, TCP 17443)
//   B: api_server :17601 + game_server "gsB" (TCP 17444, UDP advertised but
//      blocked with --simulate-udp-blocked) -- for the Auto -> TCP fallback
// then runs each scenario in its own forked process using real NetClients
// (headless fake players). Every scenario prints PASS/FAIL lines plus
// latency/throughput numbers for the snapshot path.
//
// Usage: ./net_tests [--insecure] [--only NAME] [--bin DIR] [--base-port N]
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../include/net_client.h"
#include "../src/net_common.h"

using namespace net;

namespace {

// ---------------------------------------------------------------- config
bool g_insecure = false;
int g_base = 17600;
std::string g_bin = ".";
std::string g_work;
pid_t g_gsA = 0;

int apiA() { return g_base; }
int apiB() { return g_base + 1; }
int udpA() { return g_base + 100; }
int tcpA() { return g_base - 157; } // 17443 by default: the "443" of the tests
int udpB() { return g_base + 110; }
int tcpB() { return g_base - 156; } // 17444

uint64_t nowUs() { return wire::monoUs(); }
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int g_fails = 0, g_passes = 0;
void check(bool ok, const std::string &what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    fflush(stdout);
    (ok ? g_passes : g_fails)++;
}

const char *evName(EventType t)
{
    switch (t)
    {
    case EventType::LoginOk: return "LoginOk";
    case EventType::LoginFailed: return "LoginFailed";
    case EventType::RoomList: return "RoomList";
    case EventType::JoinedRoom: return "JoinedRoom";
    case EventType::JoinFailed: return "JoinFailed";
    case EventType::LeftRoom: return "LeftRoom";
    case EventType::RosterChanged: return "RosterChanged";
    case EventType::HostChanged: return "HostChanged";
    case EventType::MatchStarted: return "MatchStarted";
    case EventType::PhaseChanged: return "PhaseChanged";
    case EventType::TransportChanged: return "TransportChanged";
    case EventType::Disconnected: return "Disconnected";
    }
    return "?";
}
const char *trName(Transport t) { return t == Transport::Udp ? "UDP" : t == Transport::Tcp443 ? "TCP443" : "None"; }

// ---------------------------------------------------------------- fake player
struct Player
{
    std::string name;
    NetClient nc;
    std::vector<Event> log;
    std::vector<bool> used;

    void pump()
    {
        for (auto &e : nc.pollEvents())
        {
            log.push_back(e);
            used.push_back(false);
        }
    }
    // Waits for the first not-yet-consumed event of 'type' matching 'pred'.
    bool wait(EventType type, int timeoutMs, Event *out = nullptr,
              const std::function<bool(const Event &)> &pred = nullptr)
    {
        uint64_t end = nowUs() + (uint64_t)timeoutMs * 1000;
        do
        {
            pump();
            for (size_t i = 0; i < log.size(); i++)
                if (!used[i] && log[i].type == type && (!pred || pred(log[i])))
                {
                    used[i] = true;
                    if (out) *out = log[i];
                    return true;
                }
            sleepMs(1);
        } while (nowUs() < end);
        return false;
    }
    void dump()
    {
        pump();
        printf("    events of %s:", name.c_str());
        for (auto &e : log) printf(" %s%s", evName(e.type), e.type == EventType::TransportChanged ? (std::string("(") + trName(e.transport) + ")").c_str() : "");
        printf("\n");
    }
};

std::unique_ptr<Player> makePlayer(const std::string &name, int apiPort, TransportPref tp, int cert)
{
    auto p = std::make_unique<Player>();
    p->name = name;
    NetClient::Config c;
    c.apiHost = "127.0.0.1";
    c.apiPort = apiPort;
    c.transport = tp;
    c.certIndex = cert;
    c.insecure = g_insecure;
    if (!p->nc.start(c))
    {
        check(false, name + ": NetClient::start");
        return nullptr;
    }
    return p;
}

bool login(Player &p, const std::string &pw)
{
    p.nc.login(p.name, pw);
    Event e;
    bool ok = p.wait(EventType::LoginOk, 8000, &e);
    if (!ok)
    {
        Event f;
        if (p.wait(EventType::LoginFailed, 1, &f)) printf("    login failed: %s\n", f.message.c_str());
    }
    return ok;
}

// Creates a room with 'host' and joins 'others'; returns roomId (0 on failure).
uint32_t setupRoom(Player &host, std::vector<Player *> others, const std::string &roomName)
{
    host.nc.createRoom(roomName);
    Event j;
    if (!host.wait(EventType::JoinedRoom, 5000, &j))
    {
        Event f;
        if (host.wait(EventType::JoinFailed, 1, &f)) printf("    create failed: %s\n", f.message.c_str());
        return 0;
    }
    for (Player *o : others)
    {
        o->nc.joinRoom(j.roomId);
        Event k;
        if (!o->wait(EventType::JoinedRoom, 5000, &k)) return 0;
    }
    return j.roomId;
}

bool waitTransport(Player &p, Transport t, int timeoutMs)
{
    return p.wait(EventType::TransportChanged, timeoutMs, nullptr, [&](const Event &e) { return e.transport == t; });
}

WorldSnapshot makeSnap(uint32_t tick, int ncars, const std::vector<int32_t> &ids)
{
    WorldSnapshot s;
    s.hostTick = tick;
    s.phase = MatchPhase::Racing;
    s.phaseTimer = 1.5f;
    s.eliminationTimer = 7.25f - (tick % 7);
    s.eliminationStage = (uint8_t)(tick % 6);
    s.lastPlacePlayerId = ids.empty() ? 3 : ids.back();
    s.winnerPlayerId = (tick % 2) ? 0 : (ids.empty() ? 1 : ids.front());
    for (int i = 0; i < ncars; i++)
    {
        CarState c;
        c.playerId = ids.empty() ? i + 1 : ids[i % ids.size()];
        c.lastInputTick = tick - 2;
        c.position = {tick * 0.5f + i, 1.25f, -tick * 0.25f};
        float a = tick * 0.01f + i;
        c.rotation = {0, std::sin(a / 2), 0, std::cos(a / 2)};
        c.velocity = {12.34f, -0.5f, -30.01f};
        c.angularVelocity = {0.1f, 1.234f, -0.2f};
        c.driftScore = 1000.5f + tick;
        c.distance = tick * 3.0f;
        c.trackSection = tick / 10;
        c.flags = (uint8_t)(kCarAlive | kCarDrifting | ((tick + i) % 2 ? kCarNitro : 0) | (i == 1 ? kCarSlipstream : 0) |
                            (i == 3 ? kCarEliminated : 0) | (i == 2 ? kCarGhost : 0) | (i == 0 ? kCarModelRx7 : 0));
        c.nitro = ((tick + i) % 101) / 100.f;
        c.place = (uint8_t)(i == 3 ? 0 : i + 1);
        c.eliminatedOrder = (uint8_t)(i == 3 ? 1 : 0);
        s.cars.push_back(c);
    }
    return s;
}

bool snapClose(const WorldSnapshot &a, const WorldSnapshot &b)
{
    if (a.hostTick != b.hostTick || a.cars.size() != b.cars.size() || a.phase != b.phase) return false;
    if (std::fabs(a.eliminationTimer - b.eliminationTimer) > 1e-6f || a.eliminationStage != b.eliminationStage ||
        a.lastPlacePlayerId != b.lastPlacePlayerId || a.winnerPlayerId != b.winnerPlayerId)
        return false;
    for (size_t i = 0; i < a.cars.size(); i++)
    {
        const CarState &x = a.cars[i], &y = b.cars[i];
        if (x.playerId != y.playerId || x.lastInputTick != y.lastInputTick || x.trackSection != y.trackSection ||
            x.flags != y.flags)
            return false;
        if (dm::length(x.position - y.position) > 1e-4f) return false;
        if (std::fabs(x.rotation.y - y.rotation.y) > 2e-4f || std::fabs(x.rotation.w - y.rotation.w) > 2e-4f) return false;
        if (dm::length(x.velocity - y.velocity) > 0.02f) return false;
        if (dm::length(x.angularVelocity - y.angularVelocity) > 0.002f) return false;
        if (std::fabs(x.driftScore - y.driftScore) > 1e-3f) return false;
        if (std::fabs(x.nitro - y.nitro) > 0.5f / 255.f + 1e-6f) return false; // gauge quantized to a byte
        if (x.place != y.place || x.eliminatedOrder != y.eliminatedOrder) return false;
    }
    return true;
}

struct LatStats
{
    std::vector<double> ms;
    size_t published = 0;
    std::set<uint32_t> ticks;
    void print(const char *label) const
    {
        if (ms.empty())
        {
            printf("    %s: nothing received\n", label);
            return;
        }
        std::vector<double> v = ms;
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double d : v) sum += d;
        printf("    %s: received %zu/%zu (%.1f%%), latency avg %.3f ms, p50 %.3f, p99 %.3f, max %.3f\n", label,
               ticks.size(), published, 100.0 * ticks.size() / std::max<size_t>(1, published), sum / v.size(),
               v[v.size() / 2], v[(size_t)(v.size() * 0.99)], v.back());
    }
};

// Host publishes at 'hz' for 'seconds'; receivers poll (busy, 50us) and
// measure host->server->client one-way latency (same process clock).
std::vector<LatStats> runSnapshots(Player &host, std::vector<Player *> rx, double hz, double seconds, int ncars,
                                   bool verify, bool *allExact)
{
    std::vector<LatStats> st(rx.size());
    std::map<uint32_t, uint64_t> sentAt;
    std::map<uint32_t, WorldSnapshot> sentSnap;
    uint64_t start = nowUs(), next = start;
    uint64_t end = start + (uint64_t)(seconds * 1e6);
    uint32_t tick = 1000;
    std::vector<int32_t> ids;
    for (auto &r : host.nc.roster()) ids.push_back(r.playerId);
    if (allExact) *allExact = true;
    // drain stale
    WorldSnapshot tmp;
    for (Player *p : rx) p->nc.takeLatestSnapshot(tmp);
    while (nowUs() < end + 300000)
    {
        uint64_t t = nowUs();
        if (t < end && t >= next)
        {
            WorldSnapshot s = makeSnap(++tick, ncars, ids);
            sentAt[tick] = nowUs();
            if (verify) sentSnap[tick] = s;
            host.nc.publishSnapshot(s);
            next += (uint64_t)(1e6 / hz);
            for (auto &x : st) x.published++;
        }
        for (size_t i = 0; i < rx.size(); i++)
        {
            WorldSnapshot got;
            if (rx[i]->nc.takeLatestSnapshot(got))
            {
                uint64_t r = nowUs();
                auto it = sentAt.find(got.hostTick);
                if (it == sentAt.end()) continue;
                st[i].ms.push_back((r - it->second) / 1000.0);
                st[i].ticks.insert(got.hostTick);
                if (verify && allExact && !snapClose(sentSnap[got.hostTick], got)) *allExact = false;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return st;
}

// Each sender sends 'n' consecutive ticks; returns per-sender count of
// distinct ticks the host got, and whether there were any duplicates.
bool g_inputContentOk = true; // every relayed input decoded to what was sent

CarInput expectedInput(uint32_t t)
{
    CarInput in;
    in.tick = t;
    in.steer = std::sin(t * 0.1f);
    in.throttle = 0.75f;
    in.brake = (t % 5) ? 0.f : 0.5f;
    in.handbrake = (t % 7) == 0;
    in.nitro = (t % 3) == 0;
    in.carModel = (t % 4) == 1 ? 1 : 0; // cosmetic car choice: spare input flag bit
    return in;
}

void checkInput(const NetClient::PlayerInput &pi, int32_t hostId)
{
    if (pi.playerId == hostId) return;
    CarInput e = expectedInput(pi.input.tick);
    if (std::fabs(pi.input.steer - e.steer) > 0.5f / 127.f + 1e-6f || std::fabs(pi.input.throttle - e.throttle) > 0.5f / 255.f + 1e-6f ||
        std::fabs(pi.input.brake - e.brake) > 0.5f / 255.f + 1e-6f || pi.input.handbrake != e.handbrake || pi.input.nitro != e.nitro ||
        pi.input.carModel != e.carModel)
        g_inputContentOk = false;
}

std::map<int32_t, std::set<uint32_t>> runInputs(Player &host, std::vector<Player *> senders, int n, int spacingMs,
                                                bool includeHost, bool *dups)
{
    int32_t hostId = host.nc.localPlayerId();
    std::map<int32_t, std::set<uint32_t>> got;
    *dups = false;
    host.nc.takeInputs();
    for (int t = 1; t <= n; t++)
    {
        for (Player *s : senders)
        {
            s->nc.sendInput(expectedInput(t));
        }
        if (includeHost)
        {
            CarInput in;
            in.tick = t;
            host.nc.sendInput(in);
        }
        for (auto &pi : host.nc.takeInputs())
        {
            checkInput(pi, hostId);
            if (!got[pi.playerId].insert(pi.input.tick).second) *dups = true;
        }
        sleepMs(spacingMs);
    }
    uint64_t end = nowUs() + 500000;
    while (nowUs() < end)
    {
        for (auto &pi : host.nc.takeInputs())
        {
            checkInput(pi, hostId);
            if (!got[pi.playerId].insert(pi.input.tick).second) *dups = true;
        }
        sleepMs(2);
    }
    return got;
}

bool waitRoster(Player &p, size_t n, int timeoutMs)
{
    uint64_t end = nowUs() + (uint64_t)timeoutMs * 1000;
    while (nowUs() < end)
    {
        p.pump();
        if (p.nc.roster().size() == n) return true;
        sleepMs(5);
    }
    return false;
}

// ================================================================ scenarios
void scenarioLogin()
{
    auto a = makePlayer("alice", apiA(), TransportPref::Auto, 1);
    auto b = makePlayer("alice", apiA(), TransportPref::Auto, 2);
    auto c = makePlayer("alice", apiA(), TransportPref::Auto, 3);
    auto d = makePlayer("waytoolongname", apiA(), TransportPref::Auto, 4);
    if (!a || !b || !c || !d) return;
    uint64_t t0 = nowUs();
    check(login(*a, "pw-alice"), "login ok (first use registers the account)");
    printf("    login round trip incl. DDS discovery + security handshake: %.0f ms\n", (nowUs() - t0) / 1000.0);
    check(a->nc.loggedIn() && a->nc.localPlayerId() > 0, "loggedIn()/localPlayerId() after LoginOk");
    Event e;
    bool ok;
    b->nc.login("alice", "wrong");
    ok = b->wait(EventType::LoginFailed, 8000, &e) && e.message == "bad password";
    check(ok, "bad password rejected: \"" + e.message + "\"");
    c->nc.login("alice", "pw-alice");
    ok = c->wait(EventType::LoginFailed, 8000, &e) && e.message.find("already") != std::string::npos;
    check(ok, "duplicate login rejected: \"" + e.message + "\"");
    d->nc.login("waytoolongname", "x");
    ok = d->wait(EventType::LoginFailed, 8000, &e);
    check(ok, "invalid name rejected: \"" + e.message + "\"");
    a->nc.stop();
    sleepMs(300);
    c->nc.login("alice", "pw-alice");
    check(c->wait(EventType::LoginOk, 8000), "re-login after the first session logged out");
}

void scenarioRoomUdp()
{
    auto h = makePlayer("host1", apiA(), TransportPref::Auto, 1);
    auto p1 = makePlayer("guest1", apiA(), TransportPref::Auto, 2);
    auto p2 = makePlayer("guest2", apiA(), TransportPref::Auto, 3);
    if (!h || !p1 || !p2) return;
    check(login(*h, "pw") && login(*p1, "pw") && login(*p2, "pw"), "3 players logged in");

    h->nc.createRoom("udp-room");
    Event j;
    check(h->wait(EventType::JoinedRoom, 5000, &j) && j.isHost, "creator gets JoinedRoom with isHost=true");
    uint32_t room = j.roomId;
    p1->nc.requestRoomList();
    Event rl;
    bool listed = p1->wait(EventType::RoomList, 5000, &rl) &&
                  std::any_of(rl.rooms.begin(), rl.rooms.end(), [&](const RoomInfo &r) { return r.roomId == room && r.name == "udp-room" && r.players == 1 && r.maxPlayers == kMaxRoomPlayers; });
    check(listed, "room appears in RoomList (maxPlayers " + std::to_string(kMaxRoomPlayers) + ")");
    // join one after the other: join order decides who is promoted later
    Event j1, j2;
    p1->nc.joinRoom(room);
    check(p1->wait(EventType::JoinedRoom, 5000, &j1) && !j1.isHost && j1.roomId == room, "guest1 joined, isHost=false");
    p2->nc.joinRoom(room);
    check(p2->wait(EventType::JoinedRoom, 5000, &j2) && !j2.isHost, "guest2 joined, isHost=false");
    check(waitRoster(*h, 3, 5000), "host roster has 3 players");
    auto ro = h->nc.roster();
    int hosts = 0;
    for (auto &r : ro) hosts += r.isHost && r.playerId == h->nc.localPlayerId();
    check(hosts == 1 && h->nc.isHost() && !p1->nc.isHost(), "roster host flag = creator only");

    p1->nc.setReady(true);
    bool readySeen = h->wait(EventType::RosterChanged, 5000, nullptr, [&](const Event &e) {
        for (auto &r : e.roster)
            if (r.playerId == p1->nc.localPlayerId() && r.ready) return true;
        return false;
    });
    check(readySeen, "ready flag propagates to the host's roster");

    uint64_t t0 = nowUs();
    bool tr = waitTransport(*h, Transport::Udp, 6000) && waitTransport(*p1, Transport::Udp, 6000) &&
              waitTransport(*p2, Transport::Udp, 6000);
    check(tr, "all three on UDP (TransportChanged(Udp))");
    printf("    game-link handshake done %.0f ms after the wait started; rtt host %.2f ms, guest1 %.2f ms\n",
           (nowUs() - t0) / 1000.0, h->nc.rttMs(), p1->nc.rttMs());
    sleepMs(300);

    bool dups;
    auto got = runInputs(*h, {p1.get(), p2.get()}, 240, 4, true, &dups);
    size_t g1 = got[p1->nc.localPlayerId()].size(), g2 = got[p2->nc.localPlayerId()].size(),
           gh = got[h->nc.localPlayerId()].size(); // (computed before the check() messages below)
    check(g1 >= 235 && g2 >= 235, "host received guests' inputs (" + std::to_string(g1) + "/240, " + std::to_string(g2) + "/240)");
    check(gh == 240, "host's own inputs short-circuited locally (" + std::to_string(gh) + "/240)");
    check(!dups, "no duplicate (playerId, tick) despite 5x redundancy");
    check(g_inputContentOk, "input contents round-trip (steer/throttle/brake quantized, handbrake + nitro + car model bits)");
    WorldSnapshot none;
    check(!p1->nc.takeLatestSnapshot(none), "no snapshot before host publishes");
    check(p1->nc.takeInputs().empty(), "non-host gets no inputs (routed to host only)");

    bool exact;
    auto st = runSnapshots(*h, {p1.get(), p2.get()}, 30, 2.0, 3, true, &exact);
    check(st[0].ticks.size() >= 55 && st[1].ticks.size() >= 55, "non-hosts receive host snapshots at 30 Hz");
    check(exact, "snapshot contents survive quantization (incl. nitro, place, eliminatedOrder, CarFlag bits, "
                 "eliminationTimer/Stage, lastPlace/winner ids)");
    st[0].print("UDP 30 Hz, 3 cars, guest1");
    st[1].print("UDP 30 Hz, 3 cars, guest2");
    st = runSnapshots(*h, {p1.get()}, 60, 2.0, 4, false, nullptr);
    st[0].print("UDP 60 Hz, 4 cars");
    st = runSnapshots(*h, {p1.get()}, 1000, 2.0, 4, false, nullptr);
    st[0].print("UDP 1000 Hz, 4 cars (stress)");
    {
        drifto_CarW cw;
        printf("    wire size: CarW %zu B (CarState in memory %zu B), 4-car SnapDn payload ~%zu B, InputUp ~%zu B\n",
               sizeof cw, sizeof(CarState), 4 * sizeof cw + 44, 5 * sizeof(drifto_InputW) + 24);
    }

    h->nc.startMatch(1234, 600);
    h->nc.setPhase(MatchPhase::Countdown);
    h->nc.setPhase(MatchPhase::Racing);
    for (Player *p : {h.get(), p1.get(), p2.get()})
    {
        Event m;
        bool ok = p->wait(EventType::MatchStarted, 5000, &m) && m.trackSeed == 1234 && m.startTick == 600;
        Event c1, c2;
        ok = ok && p->wait(EventType::PhaseChanged, 5000, &c1) && p->wait(EventType::PhaseChanged, 5000, &c2);
        ok = ok && c1.phase == MatchPhase::Countdown && c2.phase == MatchPhase::Racing;
        check(ok, p->name + ": reliable MatchStarted(1234,600) + PhaseChanged Countdown, Racing in order");
    }
    p1->nc.startMatch(999, 1); // not host: must be ignored
    sleepMs(500);
    check(!p2->wait(EventType::MatchStarted, 1), "startMatch from a non-host is dropped by the server");
    sleepMs(500);
    p2->nc.requestRoomList();
    check(p2->wait(EventType::RoomList, 5000, &rl) &&
              std::any_of(rl.rooms.begin(), rl.rooms.end(), [&](const RoomInfo &r) { return r.roomId == room && r.inProgress; }),
          "room shows inProgress in RoomList");

    // late joiner gets the current match via transient-local CtlDn
    auto p3 = makePlayer("late1", apiA(), TransportPref::Auto, 4);
    login(*p3, "pw");
    p3->nc.joinRoom(room);
    Event m3, ph3;
    check(p3->wait(EventType::MatchStarted, 8000, &m3) && m3.trackSeed == 1234 && p3->wait(EventType::PhaseChanged, 3000, &ph3) &&
              ph3.phase == MatchPhase::Racing,
          "late joiner receives MatchStarted + current phase");

    // 4-player cap: the room now has host1, guest1, guest2, late1
    check(waitRoster(*h, (size_t)kMaxRoomPlayers, 5000), "room has 4 players (the cap)");
    auto p5 = makePlayer("fifth", apiA(), TransportPref::Auto, 5);
    login(*p5, "pw");
    p5->nc.joinRoom(room);
    Event jf;
    bool full = p5->wait(EventType::JoinFailed, 5000, &jf) && jf.message == "room full";
    check(full, "5th player gets JoinFailed \"" + jf.message + "\"");
    check(!p5->nc.inRoom() && h->nc.roster().size() == (size_t)kMaxRoomPlayers, "rosters unchanged after the refused join");
    p2->nc.requestRoomList();
    bool listedFull = p2->wait(EventType::RoomList, 5000, &rl) &&
                      std::any_of(rl.rooms.begin(), rl.rooms.end(), [&](const RoomInfo &r) {
                          return r.roomId == room && r.players == kMaxRoomPlayers && r.maxPlayers == kMaxRoomPlayers;
                      });
    check(listedFull, "RoomList shows 4/4");

    // host migration
    h->nc.leaveRoom();
    Event hc1, hc2;
    check(h->wait(EventType::LeftRoom, 3000), "old host gets LeftRoom");
    check(p1->wait(EventType::HostChanged, 5000, &hc1) && hc1.isHost, "host leaves => guest1 (next) gets HostChanged(isHost=true)");
    check(p2->wait(EventType::HostChanged, 5000, &hc2) && !hc2.isHost, "guest2 gets HostChanged(isHost=false)");
    check(p1->nc.isHost() && waitRoster(*p2, 3, 3000), "roster updated (3 left), isHost() true on the new host");
    sleepMs(200);
    got = runInputs(*p1, {p2.get()}, 60, 4, false, &dups);
    size_t g3 = got[p2->nc.localPlayerId()].size();
    check(g3 >= 58, "inputs now routed to the new host (" + std::to_string(g3) + "/60)");
    st = runSnapshots(*p1, {p2.get(), p3.get()}, 30, 1.0, 3, false, nullptr);
    check(st[0].ticks.size() >= 25 && st[1].ticks.size() >= 25, "new host's snapshots reach the others");
    h->nc.publishSnapshot(makeSnap(1, 1, {}));
    sleepMs(200);
}

void scenarioTcpOnly()
{
    auto h = makePlayer("tcphost", apiA(), TransportPref::Tcp443Only, 1);
    auto g = makePlayer("tcpguest", apiA(), TransportPref::Tcp443Only, 2);
    if (!h || !g) return;
    check(login(*h, "pw") && login(*g, "pw"), "logged in");
    uint32_t room = setupRoom(*h, {g.get()}, "tcp-room");
    check(room != 0, "room created + joined");
    check(waitTransport(*h, Transport::Tcp443, 8000) && waitTransport(*g, Transport::Tcp443, 8000),
          "both report TransportChanged(Tcp443) (test port " + std::to_string(tcpA()) + ")");
    check(h->nc.transport() == Transport::Tcp443, "transport() == Tcp443");
    sleepMs(300);
    bool dups;
    auto got = runInputs(*h, {g.get()}, 120, 4, false, &dups);
    size_t gn = got[g->nc.localPlayerId()].size();
    check(gn >= 118, "inputs over TCP (" + std::to_string(gn) + "/120)");
    bool exact;
    auto st = runSnapshots(*h, {g.get()}, 30, 2.0, 3, true, &exact);
    check(st[0].ticks.size() >= 55 && exact, "snapshots over TCP");
    st[0].print("TCP 30 Hz, 3 cars");
    st = runSnapshots(*h, {g.get()}, 60, 2.0, 4, false, nullptr);
    st[0].print("TCP 60 Hz, 4 cars");
    st = runSnapshots(*h, {g.get()}, 1000, 2.0, 4, false, nullptr);
    st[0].print("TCP 1000 Hz, 4 cars (stress)");
    h->nc.startMatch(77, 5);
    h->nc.setPhase(MatchPhase::Countdown);
    Event m, p;
    check(g->wait(EventType::MatchStarted, 5000, &m) && m.trackSeed == 77 && g->wait(EventType::PhaseChanged, 5000, &p) &&
              p.phase == MatchPhase::Countdown,
          "match control over TCP");
    printf("    rtt over TCP: %.2f ms\n", g->nc.rttMs());
}

void scenarioMixed()
{
    auto h = makePlayer("mixhost", apiA(), TransportPref::Tcp443Only, 1);
    auto g = makePlayer("mixguest", apiA(), TransportPref::UdpOnly, 2);
    if (!h || !g) return;
    check(login(*h, "pw") && login(*g, "pw"), "logged in");
    uint32_t room = setupRoom(*h, {g.get()}, "mixed-room");
    check(room != 0 && waitTransport(*h, Transport::Tcp443, 8000) && waitTransport(*g, Transport::Udp, 8000),
          "host on TCP, guest on UDP, same room");
    sleepMs(300);
    bool dups;
    auto got = runInputs(*h, {g.get()}, 60, 4, false, &dups);
    check(got[g->nc.localPlayerId()].size() >= 58, "UDP guest -> TCP host inputs");
    auto st = runSnapshots(*h, {g.get()}, 30, 1.0, 3, false, nullptr);
    check(st[0].ticks.size() >= 25, "TCP host -> UDP guest snapshots");
}

void scenarioAutoFallback()
{
    auto h = makePlayer("fbhost", apiB(), TransportPref::Auto, 1);
    auto g = makePlayer("fbguest", apiB(), TransportPref::Auto, 2);
    auto u = makePlayer("fbudp", apiB(), TransportPref::UdpOnly, 3);
    if (!h || !g || !u) return;
    check(login(*h, "pw") && login(*g, "pw") && login(*u, "pw"), "logged in (deployment B, UDP blocked)");
    uint64_t t0 = nowUs();
    uint32_t room = setupRoom(*h, {g.get(), u.get()}, "fallback-room");
    check(room != 0, "room on gsB");
    bool okH = waitTransport(*h, Transport::Tcp443, 12000);
    double tH = (nowUs() - t0) / 1e6;
    bool okG = waitTransport(*g, Transport::Tcp443, 12000);
    check(okH && okG, "Auto falls back to TCP when UDP gets no ack");
    printf("    fallback complete %.2f s after join (UDP handshake budget 3 s)\n", tH);
    h->pump();
    bool sawUdp = std::any_of(h->log.begin(), h->log.end(), [](const Event &e) { return e.type == EventType::TransportChanged && e.transport == Transport::Udp; });
    check(!sawUdp, "never claimed UDP");
    check(u->nc.transport() == Transport::None, "UdpOnly client stays without transport (no silent switch)");
    sleepMs(300);
    bool dups;
    auto got = runInputs(*h, {g.get()}, 60, 4, false, &dups);
    check(got[g->nc.localPlayerId()].size() >= 58, "inputs after fallback");
    auto st = runSnapshots(*h, {g.get()}, 30, 1.0, 3, false, nullptr);
    check(st[0].ticks.size() >= 25, "snapshots after fallback");
}

void scenarioMidSession()
{
    auto h = makePlayer("msHost", apiA(), TransportPref::Auto, 1);
    auto g = makePlayer("msGuest", apiA(), TransportPref::Auto, 2);
    if (!h || !g) return;
    check(login(*h, "pw") && login(*g, "pw"), "logged in");
    uint32_t room = setupRoom(*h, {g.get()}, "ms-room");
    check(room && waitTransport(*h, Transport::Udp, 6000) && waitTransport(*g, Transport::Udp, 6000), "both on UDP");
    sleepMs(300);
    kill(g_gsA, SIGUSR1); // game server starts dropping UDP
    uint64_t t0 = nowUs();
    Event lost;
    check(g->wait(EventType::TransportChanged, 6000, &lost, [](const Event &e) { return e.transport == Transport::None; }),
          "UDP loss detected mid-session (TransportChanged(None))");
    printf("    loss detected after %.2f s\n", (nowUs() - t0) / 1e6);
    check(waitTransport(*g, Transport::Tcp443, 10000) && waitTransport(*h, Transport::Tcp443, 10000),
          "switched to TCP mid-session");
    printf("    back on TCP %.2f s after UDP was cut\n", (nowUs() - t0) / 1e6);
    kill(g_gsA, SIGUSR1); // restore for later scenarios
    sleepMs(300);
    auto st = runSnapshots(*h, {g.get()}, 30, 1.0, 3, false, nullptr);
    check(st[0].ticks.size() >= 25, "snapshots flow again after the switch");
}

void scenarioTimeout()
{
    int fd[2];
    if (pipe(fd) != 0) return;
    pid_t ghost = fork();
    if (ghost == 0)
    {
        close(fd[0]);
        auto p = makePlayer("ghost", apiA(), TransportPref::Auto, 5);
        if (p && login(*p, "pw"))
        {
            p->nc.createRoom("ghost-room");
            Event j;
            if (p->wait(EventType::JoinedRoom, 5000, &j))
            {
                if (write(fd[1], &j.roomId, sizeof j.roomId) < 0) _exit(2);
            }
        }
        for (;;) sleepMs(1000); // until SIGKILL: no LeaveRoom, no logout
    }
    close(fd[1]);
    uint32_t room = 0;
    if (read(fd[0], &room, sizeof room) != sizeof room) room = 0;
    close(fd[0]);
    check(room != 0, "ghost process created room " + std::to_string(room));
    auto w = makePlayer("watcher", apiA(), TransportPref::Auto, 4);
    if (!w) return;
    login(*w, "pw");
    w->nc.joinRoom(room);
    Event j;
    check(w->wait(EventType::JoinedRoom, 5000, &j) && !j.isHost, "watcher joined as non-host");
    sleepMs(500);
    kill(ghost, SIGKILL);
    waitpid(ghost, nullptr, 0);
    uint64_t t0 = nowUs();
    Event hc;
    check(w->wait(EventType::HostChanged, 12000, &hc) && hc.isHost, "crashed host times out => watcher promoted");
    printf("    promotion %.2f s after the host process was killed (session timeout 6 s)\n", (nowUs() - t0) / 1e6);
    check(w->nc.roster().size() == 1, "dropped player removed from roster");
}

void scenarioSecurity()
{
    if (g_insecure)
    {
        printf("  (skipped: --insecure)\n");
        return;
    }
    // A client identity must not be able to read Up topics or write Down topics.
    std::string err;
    std::string xml = dds::loadConfig("net/config", "tcp_client.xml", {{"HOST", "127.0.0.1"}, {"PORT", std::to_string(tcpA())}}, "acl");
    if (!dds::acquireDomain(wire::kGameTcpDomain, xml, &err))
    {
        check(false, err);
        return;
    }
    dds::Security sec;
    sec.identity = "client1";
    dds_entity_t p = dds::createParticipant(wire::kGameTcpDomain, sec, &err);
    check(p > 0, "client1 participant with DDS Security");
    dds_entity_t t = dds_create_topic(p, &drifto_InputUp_desc, wire::kTopicInputUp, nullptr, nullptr);
    dds_entity_t r = dds_create_reader(p, t, nullptr, nullptr);
    check(r < 0, std::string("client cannot subscribe to Drifto_Input_Up: ") + (r < 0 ? dds_strretcode(r) : "ALLOWED?!"));
    dds_entity_t t2 = dds_create_topic(p, &drifto_SnapDn_desc, wire::kTopicSnapDn, nullptr, nullptr);
    dds_entity_t w = dds_create_writer(p, t2, nullptr, nullptr);
    check(w < 0, std::string("client cannot publish Drifto_Snap_Dn: ") + (w < 0 ? dds_strretcode(w) : "ALLOWED?!"));
    dds_entity_t t3 = dds_create_topic(p, &drifto_InputUp_desc, wire::kTopicInputUp, nullptr, nullptr);
    dds_entity_t w3 = dds_create_writer(p, t3, nullptr, nullptr);
    check(w3 > 0, "client can publish Drifto_Input_Up");
    dds_delete(p);
    dds::releaseDomain(wire::kGameTcpDomain);
}

// ================================================================ LAN scenarios
// LAN mode needs no servers. Multicast does not work in WSL2's default NAT
// networking (probed below), so the multi-process scenarios also give every
// process lanPeers = 127.0.0.1 -- exactly the documented fallback.

std::unique_ptr<Player> makeLanPlayer(const std::string &name, int cert, std::vector<std::string> peers = {})
{
    auto p = std::make_unique<Player>();
    p->name = name;
    NetClient::Config c;
    c.mode = NetClient::Mode::Lan;
    c.certIndex = cert;
    c.insecure = g_insecure;
    c.lanPeers = peers;
    if (!p->nc.start(c))
    {
        check(false, name + ": NetClient::start (LAN)");
        return nullptr;
    }
    return p;
}

// Polls requestRoomList until 'pred' holds for the list (or timeout).
bool waitRoomList(Player &p, int timeoutMs, const std::function<bool(const std::vector<RoomInfo> &)> &pred,
                  std::vector<RoomInfo> *out = nullptr)
{
    uint64_t end = nowUs() + (uint64_t)timeoutMs * 1000;
    while (nowUs() < end)
    {
        p.nc.requestRoomList();
        Event e;
        if (p.wait(EventType::RoomList, 1000, &e))
        {
            if (out) *out = e.rooms;
            if (pred(e.rooms)) return true;
        }
        sleepMs(100);
    }
    return false;
}

bool hasRoom(const std::vector<RoomInfo> &rooms, uint32_t id, int players = -1)
{
    for (auto &r : rooms)
        if (r.roomId == id && (players < 0 || r.players == players)) return true;
    return false;
}

bool joinLan(Player &p, uint32_t room, Event *out = nullptr)
{
    if (!waitRoomList(p, 8000, [&](const std::vector<RoomInfo> &r) { return hasRoom(r, room); })) return false;
    p.nc.joinRoom(room);
    return p.wait(EventType::JoinedRoom, 8000, out);
}

// Forks a process that hosts a LAN room (call before any NetClient exists in
// this process). It writes the roomId to the returned fd, then keeps hosting:
// snapshots at 30 Hz, until killed.
pid_t forkLanHost(const std::string &name, const std::string &roomName, std::vector<std::string> peers, bool startMatch,
                  int *readFd, bool multicastOff = false)
{
    int fd[2];
    if (pipe(fd) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0)
    {
        close(fd[0]);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (multicastOff) setenv("DRIFTO_LAN_MULTICAST", "0", 1);
        auto h = makeLanPlayer(name, 1, peers);
        uint32_t room = 0;
        if (h)
        {
            h->nc.login(name, "");
            h->nc.createRoom(roomName);
            Event j;
            if (h->wait(EventType::JoinedRoom, 5000, &j)) room = j.roomId;
            if (startMatch)
            {
                h->nc.startMatch(4242, 77);
                h->nc.setPhase(MatchPhase::Racing);
            }
        }
        if (write(fd[1], &room, sizeof room) < 0) _exit(2);
        for (uint32_t t = 1;; t++)
        {
            if (h && h->nc.isHost()) h->nc.publishSnapshot(makeSnap(t, 1, {}));
            if (h) h->nc.pollEvents();
            sleepMs(33);
        }
    }
    close(fd[1]);
    *readFd = fd[0];
    return pid;
}

uint32_t readRoom(int fd)
{
    uint32_t room = 0;
    if (read(fd, &room, sizeof room) != sizeof room) room = 0;
    close(fd);
    return room;
}

void scenarioLanRoom()
{
    auto h = makeLanPlayer("lhost", 1);
    auto b = makeLanPlayer("lguest1", 2);
    auto c = makeLanPlayer("lguest2", 3);
    auto d = makeLanPlayer("lguest3", 4);
    auto x = makeLanPlayer("LGuest1", 5); // same name as b, different case
    auto e5 = makeLanPlayer("lfifth", 5);
    if (!h || !b || !c || !d || !x || !e5) return;
    for (Player *p : {h.get(), b.get(), c.get(), d.get(), x.get(), e5.get()}) p->nc.login(p->name, "");
    bool allOk = true;
    for (Player *p : {h.get(), b.get(), c.get(), d.get(), x.get(), e5.get()}) allOk &= p->wait(EventType::LoginOk, 2000);
    check(allOk, "LAN login is local and instant (no password, no server)");
    Event bad;
    auto bn = makeLanPlayer("bad name!", 1);
    bn->nc.login("bad name!", "");
    bool badOk = bn->wait(EventType::LoginFailed, 2000, &bad);
    check(badOk, "invalid name rejected locally: \"" + bad.message + "\"");

    h->nc.createRoom("lan-room");
    Event j;
    bool ok = h->wait(EventType::JoinedRoom, 3000, &j) && j.isHost && j.localPlayerId == 1;
    uint32_t room = j.roomId;
    check(ok, "createRoom -> JoinedRoom(isHost, playerId 1)");
    check(waitTransport(*h, Transport::Udp, 2000) && h->nc.transport() == Transport::Udp, "host transport() == Udp");

    std::vector<RoomInfo> rl;
    uint64_t t0 = nowUs();
    ok = waitRoomList(*b, 5000, [&](const std::vector<RoomInfo> &r) { return hasRoom(r, room, 1); }, &rl);
    check(ok && rl[0].maxPlayers == kMaxRoomPlayers && rl[0].name == "lan-room", "room discovered in RoomList (1/4, name)");
    printf("    room visible to another client %.0f ms after asking\n", (nowUs() - t0) / 1000.0);

    Event jb;
    t0 = nowUs();
    ok = joinLan(*b, room, &jb) && !jb.isHost && jb.localPlayerId == 2;
    check(ok, "guest joins -> JoinedRoom(playerId 2, not host)");
    printf("    join round trip %.0f ms\n", (nowUs() - t0) / 1000.0);
    Event f;
    x->nc.joinRoom(room);
    // (room list first, joinRoom needs the room to be known)
    if (!x->wait(EventType::JoinFailed, 1000, &f))
    {
        waitRoomList(*x, 5000, [&](const std::vector<RoomInfo> &r) { return hasRoom(r, room); });
        x->nc.joinRoom(room);
        x->wait(EventType::JoinFailed, 6000, &f);
    }
    check(f.message == "name taken", "duplicate name (case-insensitive) -> JoinFailed \"" + f.message + "\"");
    Event j2, j3;
    ok = joinLan(*c, room, &j2) && joinLan(*d, room, &j3);
    check(ok && j2.localPlayerId == 3 && j3.localPlayerId == 4, "two more join (playerIds 3, 4)");
    check(waitRoster(*h, 4, 3000) && waitRoster(*d, 4, 3000), "host and guests see a 4-player roster");
    auto ro = d->nc.roster();
    int hosts = 0;
    for (auto &r : ro) hosts += r.isHost ? (r.playerId == 1 ? 1 : 100) : 0;
    check(hosts == 1 && h->nc.isHost() && !d->nc.isHost(), "roster host flag = creator only");
    Event f5;
    ok = waitRoomList(*e5, 5000, [&](const std::vector<RoomInfo> &r) { return hasRoom(r, room, 4); });
    check(ok, "RoomList shows 4/4");
    e5->nc.joinRoom(room);
    ok = e5->wait(EventType::JoinFailed, 6000, &f5) && f5.message == "room full";
    check(ok, "5th player -> JoinFailed \"" + f5.message + "\"");
    Event fg;
    e5->nc.joinRoom(0x1234567u);
    ok = e5->wait(EventType::JoinFailed, 3000, &fg) && fg.message == "host gone";
    check(ok, "unknown room -> JoinFailed \"" + fg.message + "\"");

    b->nc.setReady(true);
    bool readySeen = h->wait(EventType::RosterChanged, 3000, nullptr, [&](const Event &ev) {
        for (auto &r : ev.roster)
            if (r.playerId == 2 && r.ready) return true;
        return false;
    });
    check(readySeen, "ready flag reaches the host's roster");
    sleepMs(600);
    float rtt = b->nc.rttMs();
    check(rtt > 0 && b->nc.transport() == Transport::Udp, "guest rttMs() measured to the host (" + std::to_string(rtt) + " ms), transport Udp");

    g_inputContentOk = true;
    bool dups;
    auto got = runInputs(*h, {b.get(), c.get(), d.get()}, 240, 4, true, &dups);
    size_t g1 = got[2].size(), g2 = got[3].size(), g3 = got[4].size(), gh = got[1].size();
    check(g1 >= 235 && g2 >= 235 && g3 >= 235, "host gets guests' inputs (" + std::to_string(g1) + "," + std::to_string(g2) + "," +
                                                 std::to_string(g3) + " /240)");
    check(gh == 240 && !dups && g_inputContentOk, "host short-circuit, no duplicates, contents intact (incl. nitro bit)");
    check(b->nc.takeInputs().empty(), "non-host consumes no inputs");
    bool exact;
    auto st = runSnapshots(*h, {b.get(), c.get(), d.get()}, 30, 2.0, 4, true, &exact);
    check(st[0].ticks.size() >= 55 && st[1].ticks.size() >= 55 && st[2].ticks.size() >= 55 && exact,
          "guests receive the host's snapshots, contents intact");
    st[0].print("LAN 30 Hz, 4 cars, guest");
    st = runSnapshots(*h, {b.get()}, 60, 2.0, 4, false, nullptr);
    st[0].print("LAN 60 Hz, 4 cars");

    h->nc.startMatch(999, 600);
    h->nc.setPhase(MatchPhase::Countdown);
    h->nc.setPhase(MatchPhase::Racing);
    for (Player *p : {h.get(), b.get(), c.get(), d.get()})
    {
        Event m, c1, c2;
        bool k = p->wait(EventType::MatchStarted, 3000, &m) && m.trackSeed == 999 && m.startTick == 600 &&
                 p->wait(EventType::PhaseChanged, 3000, &c1) && p->wait(EventType::PhaseChanged, 3000, &c2) &&
                 c1.phase == MatchPhase::Countdown && c2.phase == MatchPhase::Racing;
        check(k, p->name + ": MatchStarted + Countdown, Racing");
    }
    c->nc.startMatch(1, 1);
    sleepMs(300);
    check(!d->wait(EventType::MatchStarted, 1), "startMatch from a non-host is ignored");
    check(waitRoomList(*e5, 4000, [&](const std::vector<RoomInfo> &r) {
              for (auto &x2 : r)
                  if (x2.roomId == room && x2.inProgress) return true;
              return false;
          }),
          "room listed as inProgress");

    // graceful host leave: lowest remaining id (2) takes over
    h->nc.leaveRoom();
    Event hb, hc, hd;
    check(h->wait(EventType::LeftRoom, 3000), "host gets LeftRoom");
    check(b->wait(EventType::HostChanged, 4000, &hb) && hb.isHost, "host left => playerId 2 HostChanged(isHost=true)");
    check(c->wait(EventType::HostChanged, 4000, &hc) && !hc.isHost && d->wait(EventType::HostChanged, 4000, &hd) && !hd.isHost,
          "others HostChanged(isHost=false)");
    check(waitRoster(*c, 3, 3000) && b->nc.isHost(), "roster down to 3, isHost() on the new host");
    check(waitRoomList(*e5, 5000, [&](const std::vector<RoomInfo> &r) { return hasRoom(r, room, 3); }),
          "room re-announced by the new host (same roomId, 3/4)");
    got = runInputs(*b, {c.get()}, 60, 4, false, &dups);
    check(got[3].size() >= 58, "inputs now reach the new host (" + std::to_string(got[3].size()) + "/60)");
    st = runSnapshots(*b, {c.get(), d.get()}, 30, 1.0, 3, false, nullptr);
    check(st[0].ticks.size() >= 25 && st[1].ticks.size() >= 25, "new host's snapshots reach the others");
    Event jl, ml, pl;
    ok = joinLan(*e5, room, &jl) && e5->wait(EventType::MatchStarted, 3000, &ml) && ml.trackSeed == 999 &&
         e5->wait(EventType::PhaseChanged, 3000, &pl) && pl.phase == MatchPhase::Racing;
    check(ok, "late joiner after migration gets the running match (MatchStarted + Racing)");
}

void scenarioLanCrash()
{
    int fd;
    pid_t ghost = forkLanHost("ghost", "crash-room", {"127.0.0.1"}, true, &fd);
    uint32_t room = readRoom(fd);
    check(room != 0, "host process (separate) created room " + std::to_string(room));
    auto w1 = makeLanPlayer("watch1", 2, {"127.0.0.1"});
    auto w2 = makeLanPlayer("watch2", 3, {"127.0.0.1"});
    auto o = makeLanPlayer("observer", 4, {"127.0.0.1"});
    if (!w1 || !w2 || !o) return;
    for (Player *p : {w1.get(), w2.get(), o.get()}) p->nc.login(p->name, "");
    Event j1, j2, m1;
    uint64_t t0 = nowUs();
    bool ok = joinLan(*w1, room, &j1);
    printf("    cross-process discovery + join %.0f ms\n", (nowUs() - t0) / 1000.0);
    ok = ok && joinLan(*w2, room, &j2);
    check(ok && j1.localPlayerId == 2 && j2.localPlayerId == 3, "two players in another process join (ids 2, 3)");
    check(w1->wait(EventType::MatchStarted, 3000, &m1) && m1.trackSeed == 4242, "late joiners get the host's running match");
    WorldSnapshot ws;
    sleepMs(300);
    check(w2->nc.takeLatestSnapshot(ws), "snapshots arrive across processes");
    kill(ghost, SIGKILL);
    waitpid(ghost, nullptr, 0);
    t0 = nowUs();
    Event h1, h2;
    check(w1->wait(EventType::HostChanged, 8000, &h1) && h1.isHost, "host killed => lowest id (2) promoted");
    printf("    promotion %.2f s after the host process was killed (host-loss timeout 3 s)\n", (nowUs() - t0) / 1e6);
    check(w2->wait(EventType::HostChanged, 8000, &h2) && !h2.isHost, "the other player gets HostChanged(isHost=false)");
    check(waitRoster(*w2, 2, 4000) && w1->nc.roster().size() == 2, "dead host removed from the roster");
    check(waitRoomList(*o, 6000, [&](const std::vector<RoomInfo> &r) { return hasRoom(r, room, 2); }),
          "room re-announced (same roomId, 2 players)");
    bool dups;
    auto got = runInputs(*w1, {w2.get()}, 60, 4, false, &dups);
    check(got[3].size() >= 58, "inputs to the new host");
    auto st = runSnapshots(*w1, {w2.get()}, 30, 1.0, 2, false, nullptr);
    check(st[0].ticks.size() >= 25, "snapshots from the new host");
}

void scenarioLanDeadRoom()
{
    int fd;
    pid_t ghost = forkLanHost("lonely", "dead-room", {"127.0.0.1"}, false, &fd);
    uint32_t room = readRoom(fd);
    auto o = makeLanPlayer("observer", 2, {"127.0.0.1"});
    if (!o) return;
    o->nc.login("observer", "");
    check(waitRoomList(*o, 8000, [&](const std::vector<RoomInfo> &r) { return hasRoom(r, room); }), "room listed while its host runs");
    kill(ghost, SIGKILL);
    waitpid(ghost, nullptr, 0);
    uint64_t t0 = nowUs();
    bool gone = waitRoomList(*o, 8000, [&](const std::vector<RoomInfo> &r) { return !hasRoom(r, room); });
    check(gone, "dead host's room disappears from RoomList");
    printf("    gone %.2f s after the host was killed (expiry 3 s)\n", (nowUs() - t0) / 1e6);
    // and a host that leaves gracefully closes its room at once
    auto h = makeLanPlayer("closer", 3);
    h->nc.login("closer", "");
    h->nc.createRoom("closing");
    Event j;
    h->wait(EventType::JoinedRoom, 3000, &j);
    check(waitRoomList(*o, 5000, [&](const std::vector<RoomInfo> &r) { return hasRoom(r, j.roomId); }), "new room listed");
    h->nc.leaveRoom();
    t0 = nowUs();
    check(waitRoomList(*o, 5000, [&](const std::vector<RoomInfo> &r) { return !hasRoom(r, j.roomId); }), "room closed by its leaving host");
    printf("    closed room gone after %.2f s\n", (nowUs() - t0) / 1e6);
}

void scenarioLanUnicast()
{
    // Everyone with multicast OFF, so only the lanPeers unicast path can work.
    setenv("DRIFTO_LAN_MULTICAST", "0", 1);
    // 1) control: host and seeker both without lanPeers -> must NOT see each other
    int fd;
    pid_t lonely = forkLanHost("noPeerH", "invisible", {}, false, &fd, true);
    uint32_t hidden = readRoom(fd);
    int nfd[2];
    if (pipe(nfd) != 0) return;
    pid_t neg = fork();
    if (neg == 0)
    {
        close(nfd[0]);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        auto p = makeLanPlayer("blind", 3, {});
        uint8_t seen = 1;
        if (p)
        {
            p->nc.login("blind", "");
            seen = waitRoomList(*p, 4000, [&](const std::vector<RoomInfo> &r) { return hasRoom(r, hidden); }) ? 1 : 0;
        }
        if (write(nfd[1], &seen, 1) < 0) _exit(2);
        _exit(0);
    }
    close(nfd[1]);
    uint8_t seen = 1;
    if (read(nfd[0], &seen, 1) != 1) seen = 1;
    close(nfd[0]);
    waitpid(neg, nullptr, 0);
    kill(lonely, SIGKILL);
    waitpid(lonely, nullptr, 0);
    check(hidden != 0 && seen == 0, "control: multicast off and no lanPeers on either side -> room NOT found");

    // 2) host still without peers; the joiner only knows the host's address
    pid_t ghost = forkLanHost("ucHost", "unicast-room", {}, false, &fd, true);
    uint32_t room = readRoom(fd);
    auto g = makeLanPlayer("ucGuest", 2, {"127.0.0.1"});
    if (!g) return;
    g->nc.login("ucGuest", "");
    Event j;
    uint64_t t0 = nowUs();
    bool ok = joinLan(*g, room, &j);
    check(room != 0 && ok, "multicast off, joiner has lanPeers=<host address>: room found and joined");
    printf("    unicast discovery + join %.0f ms\n", (nowUs() - t0) / 1000.0);
    WorldSnapshot ws;
    sleepMs(500);
    check(g->nc.takeLatestSnapshot(ws), "snapshots over the unicast path");
    kill(ghost, SIGKILL);
    waitpid(ghost, nullptr, 0);
}

bool multicastLoopbackWorks()
{
    int r = socket(AF_INET, SOCK_DGRAM, 0), s = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(r, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(19999);
    bind(r, (struct sockaddr *)&a, sizeof a);
    struct ip_mreq mr;
    mr.imr_multiaddr.s_addr = inet_addr("239.255.0.77");
    mr.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(r, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr);
    struct timeval tv = {1, 0};
    setsockopt(r, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    a.sin_addr.s_addr = inet_addr("239.255.0.77");
    sendto(s, "x", 1, 0, (struct sockaddr *)&a, sizeof a);
    char buf[4];
    bool ok = recv(r, buf, sizeof buf, 0) == 1;
    close(r);
    close(s);
    return ok;
}

void scenarioLanMulticast()
{
    if (!multicastLoopbackWorks())
    {
        printf("  (SKIPPED: IP multicast does not loop back on this machine -- WSL2 NAT networking; see\n"
               "   docs/MULTIPLAYER.md. Discovery across processes is covered via lanPeers above.)\n");
        return;
    }
    int fd;
    pid_t ghost = forkLanHost("mcHost", "mc-room", {}, false, &fd);
    uint32_t room = readRoom(fd);
    auto g = makeLanPlayer("mcGuest", 2, {});
    if (!g) return;
    g->nc.login("mcGuest", "");
    Event j;
    check(joinLan(*g, room, &j), "pure multicast discovery across processes: room found and joined");
    kill(ghost, SIGKILL);
    waitpid(ghost, nullptr, 0);
}

// ================================================================ harness
pid_t spawn(const std::string &log, const std::vector<std::string> &argv)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        prctl(PR_SET_PDEATHSIG, SIGTERM); // never outlive the test run
        int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, 1);
        dup2(fd, 2);
        std::vector<char *> a;
        for (auto &s : argv) a.push_back((char *)s.c_str());
        a.push_back(nullptr);
        execv(a[0], a.data());
        perror("execv");
        _exit(127);
    }
    return pid;
}

std::string readFile(const std::string &p)
{
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool waitLog(const std::string &file, const std::string &needle, int timeoutMs)
{
    uint64_t end = nowUs() + (uint64_t)timeoutMs * 1000;
    while (nowUs() < end)
    {
        if (readFile(file).find(needle) != std::string::npos) return true;
        sleepMs(50);
    }
    return false;
}

bool portFree(int port, int type)
{
    int s = socket(AF_INET, type, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    bool ok = bind(s, (struct sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}

struct Scenario
{
    const char *name;
    const char *desc;
    void (*fn)();
    bool lan = false; // needs no servers
};

} // namespace

int main(int argc, char **argv)
{
    std::string only;
    bool lanOnly = false;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--insecure") g_insecure = true;
        else if (a == "--lan-only") lanOnly = true;
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--bin" && i + 1 < argc) g_bin = argv[++i];
        else if (a == "--base-port" && i + 1 < argc) g_base = atoi(argv[++i]);
        else
        {
            printf("usage: %s [--insecure] [--lan-only] [--only NAME] [--bin DIR] [--base-port N]\n", argv[0]);
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    g_work = "build/net/test_run";
    mkdir("build", 0755);
    mkdir("build/net", 0755);
    mkdir(g_work.c_str(), 0755);
    unlink((g_work + "/players.auth").c_str());
    std::vector<std::string> sec;
    if (g_insecure) sec.push_back("--insecure");
    auto args = [&](std::vector<std::string> v) {
        v.insert(v.end(), sec.begin(), sec.end());
        return v;
    };
    printf("net_tests: security %s, logs in %s/\n", g_insecure ? "OFF (--insecure)" : "ON (DDS Security)", g_work.c_str());
    bool needServers = !lanOnly && (only.empty() || only.rfind("lan_", 0) != 0);
    std::vector<pid_t> servers;
    auto killAll = [&] {
        for (pid_t p : servers) kill(p, SIGTERM);
        for (pid_t p : servers) waitpid(p, nullptr, 0);
    };
    if (needServers)
    {
        for (int p : {apiA(), apiB(), tcpA(), tcpB()})
            if (!portFree(p, SOCK_STREAM))
            {
                printf("TCP port %d is busy -- leftover servers? (pkill -x api_server; pkill -x game_server)\n", p);
                return 1;
            }
        for (int p : {udpA(), udpA() + 1})
            if (!portFree(p, SOCK_DGRAM))
            {
                printf("UDP port %d is busy -- leftover game_server?\n", p);
                return 1;
            }
        pid_t apiAp = spawn(g_work + "/api_A.log", args({g_bin + "/api_server", "--port", std::to_string(apiA()), "--auth", g_work + "/players.auth"}));
        pid_t apiBp = spawn(g_work + "/api_B.log", args({g_bin + "/api_server", "--port", std::to_string(apiB()), "--auth", g_work + "/players.auth"}));
        sleepMs(200);
        g_gsA = spawn(g_work + "/gs_A.log", args({g_bin + "/game_server", "--api", "127.0.0.1:" + std::to_string(apiA()), "--id", "gsA",
                                                "--udp-port", std::to_string(udpA()), "--tcp-port", std::to_string(tcpA()),
                                                "--public-addr", "127.0.0.1"}));
        pid_t gsBp = spawn(g_work + "/gs_B.log", args({g_bin + "/game_server", "--api", "127.0.0.1:" + std::to_string(apiB()), "--id", "gsB",
                                                      "--udp-port", std::to_string(udpB()), "--tcp-port", std::to_string(tcpB()),
                                                      "--udp-domain", "4", "--tcp-domain", "5", "--public-addr", "127.0.0.1",
                                                      "--simulate-udp-blocked"}));
        servers = {apiAp, apiBp, g_gsA, gsBp};
        bool up = waitLog(g_work + "/api_A.log", "'gsA' registered", 15000) && waitLog(g_work + "/api_B.log", "'gsB' registered", 15000);
        if (!up)
        {
            printf("servers did not come up; logs:\n%s%s%s%s", readFile(g_work + "/api_A.log").c_str(), readFile(g_work + "/gs_A.log").c_str(),
                   readFile(g_work + "/api_B.log").c_str(), readFile(g_work + "/gs_B.log").c_str());
            killAll();
            return 1;
        }
        printf("servers up: api A :%d + gsA (udp %d, tcp %d), api B :%d + gsB (udp %d BLOCKED, tcp %d)\n\n", apiA(), udpA(), tcpA(),
               apiB(), udpB(), tcpB());
    }
    else
        printf("LAN scenarios only: no servers started\n\n");

    const Scenario scenarios[] = {
        {"login", "login ok / bad password / duplicate / invalid", scenarioLogin},
        {"room_udp", "rooms, roster, inputs, snapshots, match control, late join, host migration (Auto -> UDP)", scenarioRoomUdp},
        {"tcp_only", "TransportPref::Tcp443Only end to end", scenarioTcpOnly},
        {"mixed", "TCP host + UDP guest in one room", scenarioMixed},
        {"auto_fallback", "Auto falls back to TCP when UDP is blocked", scenarioAutoFallback},
        {"midsession", "UDP dies mid-session -> TCP", scenarioMidSession},
        {"timeout", "crashed host times out -> promotion", scenarioTimeout},
        {"security", "DDS Security access control on topics", scenarioSecurity},
        {"lan_room", "LAN: discovery, join, roster, cap, dup name, inputs, snapshots, match control, host leave", scenarioLanRoom, true},
        {"lan_crash", "LAN: host process killed -> lowest id promoted, room re-announced", scenarioLanCrash, true},
        {"lan_dead_room", "LAN: dead / closed rooms leave the room list", scenarioLanDeadRoom, true},
        {"lan_unicast", "LAN: multicast off, lanPeers unicast path (+ negative control)", scenarioLanUnicast, true},
        {"lan_multicast", "LAN: pure multicast discovery across processes", scenarioLanMulticast, true},
    };
    int totalFail = 0, totalRun = 0;
    for (const Scenario &s : scenarios)
    {
        if (!only.empty() && only != s.name) continue;
        if (lanOnly && !s.lan) continue;
        printf("=== %s: %s\n", s.name, s.desc);
        fflush(stdout);
        uint64_t t0 = nowUs();
        pid_t c = fork();
        if (c == 0)
        {
            s.fn();
            printf("  -> %d passed, %d failed\n", g_passes, g_fails);
            fflush(stdout);
            _exit(g_fails == 0 ? 0 : 1);
        }
        int status = 0;
        bool done = false;
        uint64_t end = nowUs() + 90 * 1000000ull;
        while (nowUs() < end)
        {
            if (waitpid(c, &status, WNOHANG) == c)
            {
                done = true;
                break;
            }
            sleepMs(20);
        }
        if (!done)
        {
            kill(c, SIGKILL);
            waitpid(c, &status, 0);
            printf("  [FAIL] scenario timed out\n");
        }
        bool ok = done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        printf("=== %s: %s (%.1f s)\n\n", s.name, ok ? "OK" : "FAILED", (nowUs() - t0) / 1e6);
        totalRun++;
        if (!ok) totalFail++;
    }
    killAll();
    printf("net_tests: %d/%d scenarios passed\n", totalRun - totalFail, totalRun);
    return totalFail == 0 ? 0 : 1;
}
