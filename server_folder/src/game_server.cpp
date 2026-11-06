// game_server.cpp -- the Drifto game service: a per-room packet router.
//
// It does ZERO simulation. The host client of each room runs the game; this
// process only forwards that room's packets between its clients:
//   client InputUp  -> InputDn  to the host only   (partition r<room>.p<host>)
//   host   SnapUp   -> SnapDn   to the whole room  (partition r<room>)
//   host   CtlUp    -> CtlDn    to the whole room  (reliable, transient-local)
//   client PingUp   -> PingDn   back to the sender (handshake / heartbeat / RTT)
// Every Up packet is checked against the room roster + per-player token the
// API server sent us (GsRoom), so nobody can inject into a room they're not in
// or act as host when they're not.
//
// Two DDS domains are served at once: UDP (default port 7700/7701) and TCP
// (default port 443, the fallback for networks that block UDP). Clients on
// either transport can share a room.
//
// Usage: ./game_server [--api HOST[:PORT]] [--public-addr HOST] [--udp-port N]
//                      [--tcp-port N] [--insecure] ... (see --help)
//
// Relationship to the API server: this process connects to it as a DDS client
// (TCP, kApiDomain). It sends a GsBeat heartbeat once a second (address, ports,
// domains, capacity, current room count), receives GsRoom (room roster, host
// and per-player tokens) for the rooms allocated to it, and reports each room's
// match phase back with GsRoomStatus. Rooms exist here only while the API
// server keeps them open; a room with no traffic for kRoomIdleUs is also
// dropped (covers a vanished API server). --capacity is the max number of rooms
// it advertises; the API server stops allocating to it once reached.
//
// Security: DDS Security is on unless --insecure (must match clients and API
// server). On top of that, every Up packet carries roomId + playerId + token
// and is dropped (counted in Stats::rejected) unless it matches the roster; the
// host-only topics additionally require playerId == room host.
//
// Threading: single thread. run() waits on a waitset (100 ms cap), then polls the
// API link and both game domains, and does periodic work (heartbeat 1 s, TCP
// socket tuning 250 ms, idle-room GC 5 s, stats 10 s). No locks are needed.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "../../net/src/net_common.h"
#include "../include/server.h"

using namespace net;
using namespace net::wire;

namespace {

// Timing constants (microseconds on the monoUs() clock) and transport indices.
constexpr uint64_t kSec = 1000000;
constexpr uint64_t kRecentUs = 3 * kSec;     // a member counts as "on" a transport if heard within this
constexpr uint64_t kRoomIdleUs = 120 * kSec; // GC for rooms whose API server vanished
constexpr int kUdp = 0, kTcp = 1;

// A DDS publisher plus its two writers; what the writers carry depends on the owner
// (see Member::pp and Room::rp).
struct PubPair
{
    dds_entity_t pub = 0, w1 = 0, w2 = 0;
};

// A player in a room. 'token' is the secret from the API server; lastSeen[] records,
// per transport (kUdp/kTcp), when a valid packet last arrived, which decides where
// replies and fan-out are sent.
struct Member
{
    uint64_t token = 0;
    PubPair pp[2];          // personal partition: w1 = InputDn (host only), w2 = PingDn
    uint64_t lastSeen[2] = {0, 0};
};

// Routing state of one room. 'ctl' is the latest match-control state (re-sent to late
// joiners via the transient-local writer); lastCtlReq/lastCtlFrom de-duplicate
// reliable retransmits from the host.
struct Room
{
    uint32_t id = 0;
    int32_t hostId = 0;
    std::map<int32_t, Member> members;
    PubPair rp[2]; // room partition: w1 = SnapDn, w2 = CtlDn
    drifto_CtlDn ctl{};
    bool hasCtl = false;
    uint32_t lastCtlReq = 0;
    int32_t lastCtlFrom = 0;
    uint64_t lastTrafficUs = 0;
};

// One game DDS domain (UDP or TCP): participant, the four Down topics we write, and
// the four Up readers we poll.
struct DomLink
{
    bool on = false;
    uint32_t dom = 0;
    int port = 0;
    dds_entity_t part = 0;
    dds_entity_t tInD = 0, tSnD = 0, tCtD = 0, tPiD = 0;
    dds_entity_t rIn = 0, rSn = 0, rCt = 0, rPi = 0;
};

// Counters for the periodic log line; 'rejected' counts packets failing validation.
struct Stats
{
    uint64_t inputs = 0, snaps = 0, snapFanout = 0, ctl = 0, pings = 0, rejected = 0;
};

class GameServer
{
public:
    bool init(const srv::Args &a);
    void run();
    void shutdown();

private:
    dds::Security sec_;
    std::string configDir_, id_, publicAddr_, apiHost_;
    int apiPort_ = kApiPort;
    uint32_t capacity_ = 64;
    bool simulateUdpBlocked_ = false;
    uint64_t incarnation_ = 0;
    int advertisedUdpPort_ = 0;
    DomLink d_[2];
    // API link
    bool apiDomain_ = false;
    dds_entity_t apiPart_ = 0, beatW_ = 0, roomR_ = 0, statusW_ = 0;
    dds_entity_t ws_ = 0;
    std::map<uint32_t, Room> rooms_;
    Stats st_;
    bool udpBlockedLogged_ = false;

    bool udpBlocked() const { return simulateUdpBlocked_ || srv::usr1Toggle(); }
    bool openGameDomain(int idx, std::string *err);
    bool openApi(std::string *err);
    void ensurePersonal(Room &r, int32_t pid, Member &m, int d);
    void ensureRoomPubs(Room &r);
    void deleteRoom(Room &r);
    Member *validate(uint32_t roomId, int32_t pid, uint64_t token, Room **room);
    void pollDomain(int d, uint64_t now);
    void pollApi(uint64_t now);
    void beat();
};

// Opens one game domain (idx = kUdp or kTcp): builds the XML config, joins the domain
// and creates the Up readers (inputs/pings/snapshots best effort, control reliable).
// For UDP the port base is derived from --udp-port and --udp-domain. Returns false
// and fills *err on failure.
bool GameServer::openGameDomain(int idx, std::string *err)
{
    DomLink &L = d_[idx];
    std::string xml;
    if (idx == kUdp)
    {
        // CycloneDDS UDP port mapping: port = base + 250*domain + 10 (+ participant offset);
        // recover the base from the requested port
        int base = L.port - 10 - 250 * (int)L.dom;
        if (base <= 0)
        {
            *err = "--udp-port too small for --udp-domain (need udpPort > 10 + 250*domain)";
            return false;
        }
        xml = dds::loadConfig(configDir_, "udp_server.xml",
                              {{"BASE", std::to_string(base)}, {"EXTERNAL", publicAddr_}}, "game-udp");
    }
    else
        xml = dds::loadConfig(configDir_, "tcp_server.xml",
                              {{"PORT", std::to_string(L.port)}, {"EXTERNAL", publicAddr_}}, "game-tcp");
    if (!dds::acquireDomain(L.dom, xml, err)) return false;
    L.part = dds::createParticipant(L.dom, sec_, err);
    if (L.part < 0) return false;
    dds_entity_t tInU = dds_create_topic(L.part, &drifto_InputUp_desc, kTopicInputUp, nullptr, nullptr);
    dds_entity_t tSnU = dds_create_topic(L.part, &drifto_SnapUp_desc, kTopicSnapUp, nullptr, nullptr);
    dds_entity_t tCtU = dds_create_topic(L.part, &drifto_CtlUp_desc, kTopicCtlUp, nullptr, nullptr);
    dds_entity_t tPiU = dds_create_topic(L.part, &drifto_PingUp_desc, kTopicPingUp, nullptr, nullptr);
    L.tInD = dds_create_topic(L.part, &drifto_InputDn_desc, kTopicInputDn, nullptr, nullptr);
    L.tSnD = dds_create_topic(L.part, &drifto_SnapDn_desc, kTopicSnapDn, nullptr, nullptr);
    L.tCtD = dds_create_topic(L.part, &drifto_CtlDn_desc, kTopicCtlDn, nullptr, nullptr);
    L.tPiD = dds_create_topic(L.part, &drifto_PingDn_desc, kTopicPingDn, nullptr, nullptr);
    // Inputs/snapshots/pings are time-critical and superseded by newer samples, so best
    // effort; match control must not be lost, so reliable with a deeper history.
    dds_qos_t *in = dds::qosBestEffort(16); // keyed by playerId: 16 per player
    dds_qos_t *sn = dds::qosBestEffort(4);
    dds_qos_t *ct = dds::qosReliable(256, false);
    dds_qos_t *pi = dds::qosBestEffort(4);
    L.rIn = dds_create_reader(L.part, tInU, in, nullptr);
    L.rSn = dds_create_reader(L.part, tSnU, sn, nullptr);
    L.rCt = dds_create_reader(L.part, tCtU, ct, nullptr);
    L.rPi = dds_create_reader(L.part, tPiU, pi, nullptr);
    dds_delete_qos(in);
    dds_delete_qos(sn);
    dds_delete_qos(ct);
    dds_delete_qos(pi);
    for (dds_entity_t r : {L.rIn, L.rSn, L.rCt, L.rPi})
    {
        if (r < 0)
        {
            *err = std::string("reader creation failed: ") + dds_strretcode(r);
            return false;
        }
        dds_set_status_mask(r, DDS_DATA_AVAILABLE_STATUS);
        dds_waitset_attach(ws_, r, r);
    }
    L.on = true;
    return true;
}

// Joins the API domain as a TCP client and creates the heartbeat writer, the room-status
// writer and a GsRoom reader restricted to this server's own partition (gsPartition(id_)).
bool GameServer::openApi(std::string *err)
{
    std::string xml = dds::loadConfig(configDir_, "tcp_client.xml",
                                      {{"HOST", apiHost_}, {"PORT", std::to_string(apiPort_)}}, "game-api");
    if (!dds::acquireDomain(kApiDomain, xml, err)) return false;
    apiDomain_ = true;
    apiPart_ = dds::createParticipant(kApiDomain, sec_, err);
    if (apiPart_ < 0) return false;
    dds_entity_t tBeat = dds_create_topic(apiPart_, &drifto_GsBeat_desc, kTopicGsBeat, nullptr, nullptr);
    dds_entity_t tRoom = dds_create_topic(apiPart_, &drifto_GsRoom_desc, kTopicGsRoom, nullptr, nullptr);
    dds_entity_t tStat = dds_create_topic(apiPart_, &drifto_GsRoomStatus_desc, kTopicGsStatus, nullptr, nullptr);
    dds_qos_t *bq = dds::qosReliable(1, false);
    beatW_ = dds_create_writer(apiPart_, tBeat, bq, nullptr);
    dds_delete_qos(bq);
    dds_qos_t *sq = dds::qosReliable(64, false);
    statusW_ = dds_create_writer(apiPart_, tStat, sq, nullptr);
    dds_delete_qos(sq);
    dds_entity_t sub = dds::createSubscriber(apiPart_, {gsPartition(id_)});
    dds_qos_t *rq = dds::qosReliable(1, true);
    roomR_ = dds_create_reader(sub, tRoom, rq, nullptr);
    dds_delete_qos(rq);
    if (beatW_ < 0 || statusW_ < 0 || roomR_ < 0)
    {
        *err = "failed to create API-side readers/writers";
        return false;
    }
    dds_set_status_mask(roomR_, DDS_DATA_AVAILABLE_STATUS);
    dds_waitset_attach(ws_, roomR_, roomR_);
    return true;
}

// Parses options (flags override DRIFTO_GAME_*_PORT env vars), opens the enabled game
// domains, then the API link. Returns false (after logging) on any failure.
bool GameServer::init(const srv::Args &a)
{
    sec_.enabled = !a.has("--insecure");
    sec_.certDir = a.str("--certs", "certs");
    sec_.identity = "server";
    configDir_ = a.str("--config", "net/config");
    const char *envTcp = getenv("DRIFTO_GAME_TCP_PORT");
    const char *envUdp = getenv("DRIFTO_GAME_UDP_PORT");
    d_[kUdp].port = a.num("--udp-port", envUdp ? atoi(envUdp) : kGameUdpPort);
    d_[kTcp].port = a.num("--tcp-port", envTcp ? atoi(envTcp) : kGameTcpPort);
    d_[kUdp].dom = (uint32_t)a.num("--udp-domain", kGameUdpDomain);
    d_[kTcp].dom = (uint32_t)a.num("--tcp-domain", kGameTcpDomain);
    capacity_ = (uint32_t)a.num("--capacity", 64);
    simulateUdpBlocked_ = a.has("--simulate-udp-blocked");
    srv::splitHostPort(a.str("--api", "127.0.0.1"), apiHost_, apiPort_);
    apiPort_ = a.num("--api-port", apiPort_);
    std::string pa = a.str("--public-addr", "");
    // public address clients use: loopback if the API is local, else this host's first IPv4
    if (pa.empty()) pa = srv::isLoopbackHost(apiHost_) ? "127.0.0.1" : srv::primaryIPv4();
    publicAddr_ = pa;
    char hn[64] = "gs";
    gethostname(hn, sizeof hn - 1);
    // default id <hostname>-<tcp port>-<udp port>, capped at 32 chars
    id_ = a.str("--id", std::string(hn) + "-" + std::to_string(d_[kTcp].port) + "-" + std::to_string(d_[kUdp].port));
    if (id_.size() > 32) id_ = id_.substr(0, 32);
    a.warnUnknown();
    // random per process start; lets the API server detect a restart
    incarnation_ = randomU64();

    ws_ = dds_create_waitset(DDS_CYCLONEDDS_HANDLE);
    std::string err;
    // DDS ExternalNetworkAddress (what this server advertises in discovery)
    // must be an IP: resolve --public-addr; loopback/unresolvable -> "auto".
    std::string extSave = publicAddr_;
    std::string ext = srv::resolveIPv4(publicAddr_);
    publicAddr_ = (ext.empty() || srv::isLoopbackHost(ext)) ? "auto" : ext;
    for (int i : {kUdp, kTcp})
    {
        DomLink &L = d_[i];
        // port 0 disables that transport
        if (L.port == 0) continue; // disabled
        if (i == kUdp && simulateUdpBlocked_)
        {
            srv::log("UDP service NOT started (--simulate-udp-blocked); still advertising udp:%d", L.port);
            continue;
        }
        if (!openGameDomain(i, &err))
        {
            srv::log("%s service on port %d failed: %s", i == kUdp ? "UDP" : "TCP", L.port, err.c_str());
            if (L.port < 1024)
                srv::log("binding port %d needs root, or once: sudo setcap cap_net_bind_service=+ep ./game_server "
                         "(or use --tcp-port / DRIFTO_GAME_TCP_PORT with a port >= 1024)",
                         L.port);
            return false;
        }
    }
    // restore the name/IP to advertise in GsBeat
    publicAddr_ = extSave;
    advertisedUdpPort_ = d_[kUdp].port;
    if (!openApi(&err))
    {
        srv::log("API link: %s", err.c_str());
        return false;
    }
    srv::log("game service '%s' up: public %s, UDP %s (domain %u), TCP %s (domain %u), API %s:%d, security %s", id_.c_str(),
             publicAddr_.c_str(),
             d_[kUdp].port ? (std::to_string(d_[kUdp].port) + "+" + std::to_string(d_[kUdp].port + 1)).c_str() : "off",
             d_[kUdp].dom, d_[kTcp].port ? std::to_string(d_[kTcp].port).c_str() : "off", d_[kTcp].dom, apiHost_.c_str(),
             apiPort_, sec_.enabled ? "ON" : "OFF (--insecure)");
    return true;
}

// Sends the registration/heartbeat sample. The API server expires this server after
// kGsTimeoutUs without one and closes its rooms.
void GameServer::beat()
{
    drifto_GsBeat b{};
    copyStr(b.gsId, sizeof b.gsId, id_);
    b.incarnation = incarnation_;
    copyStr(b.publicAddr, sizeof b.publicAddr, publicAddr_);
    b.udpPort = (uint16_t)advertisedUdpPort_;
    b.tcpPort = (uint16_t)d_[kTcp].port;
    b.udpDomain = d_[kUdp].dom;
    b.tcpDomain = d_[kTcp].dom;
    b.capacity = capacity_;
    b.load = (uint32_t)rooms_.size();
    dds_write(beatW_, &b);
}

// Lazily creates, per active transport, the room-partition writers (snapshots best
// effort, control reliable+transient-local). A stored control state is replayed to
// new writers.
void GameServer::ensureRoomPubs(Room &r)
{
    for (int d : {kUdp, kTcp})
    {
        if (!d_[d].on || r.rp[d].pub) continue;
        PubPair &p = r.rp[d];
        p.pub = dds::createPublisher(d_[d].part, roomPartition(r.id));
        dds_qos_t *sn = dds::qosBestEffort(1);
        dds_qos_t *ct = dds::qosReliable(1, true);
        p.w1 = dds_create_writer(p.pub, d_[d].tSnD, sn, nullptr);
        p.w2 = dds_create_writer(p.pub, d_[d].tCtD, ct, nullptr);
        dds_delete_qos(sn);
        dds_delete_qos(ct);
        if (r.hasCtl) dds_write(p.w2, &r.ctl);
    }
}

// Lazily creates the writers in one player's personal partition on transport d:
// InputDn (only used for the host) and PingDn.
void GameServer::ensurePersonal(Room &r, int32_t pid, Member &m, int d)
{
    if (m.pp[d].pub) return;
    PubPair &p = m.pp[d];
    p.pub = dds::createPublisher(d_[d].part, playerPartition(r.id, pid));
    dds_qos_t *in = dds::qosBestEffort(8);
    dds_qos_t *pi = dds::qosBestEffort(1);
    p.w1 = dds_create_writer(p.pub, d_[d].tInD, in, nullptr);
    p.w2 = dds_create_writer(p.pub, d_[d].tPiD, pi, nullptr);
    dds_delete_qos(in);
    dds_delete_qos(pi);
}

// Deletes the publishers (and their writers) and resets the slots.
static void deletePubs(PubPair *pp)
{
    for (int d = 0; d < 2; d++)
        if (pp[d].pub > 0)
        {
            dds_delete(pp[d].pub);
            pp[d] = PubPair{};
        }
}

// Releases every DDS publisher belonging to the room.
void GameServer::deleteRoom(Room &r)
{
    for (auto &m : r.members) deletePubs(m.second.pp);
    deletePubs(r.rp);
}

// The authorisation check for every Up packet: the room must exist, the player must be
// in its roster and the token must equal the one issued by the API server. Returns the
// member (and sets *room) or nullptr.
Member *GameServer::validate(uint32_t roomId, int32_t pid, uint64_t token, Room **room)
{
    auto it = rooms_.find(roomId);
    if (it == rooms_.end()) return nullptr;
    auto m = it->second.members.find(pid);
    if (m == it->second.members.end() || m->second.token != token) return nullptr;
    *room = &it->second;
    return &m->second;
}

// Drains the four Up readers of domain d and forwards. Samples from non-members, with a
// wrong token, or (for snapshots/control) not from the host are dropped and counted.
// While UDP is simulated-blocked, UDP samples are consumed but ignored.
void GameServer::pollDomain(int d, uint64_t now)
{
    DomLink &L = d_[d];
    if (!L.on) return;
    // fault injection for testing the TCP fallback
    bool drop = (d == kUdp) && udpBlocked();
    if (d == kUdp && drop != udpBlockedLogged_)
    {
        udpBlockedLogged_ = drop;
        srv::log("UDP traffic %s (SIGUSR1)", drop ? "BLOCKED" : "unblocked");
    }
    void *samples[64] = {nullptr};
    dds_sample_info_t infos[64];
    int n;
    Room *room;

    // Every accepted packet refreshes lastSeen[d] (transport liveness) and the room's
    // idle timer.
    // pings: answer to the sender, on the transport it came in on
    while ((n = dds_take(L.rPi, samples, infos, 64, 64)) > 0)
    {
        for (int i = 0; i < n && !drop; i++)
        {
            if (!infos[i].valid_data) continue;
            const auto &p = *(drifto_PingUp *)samples[i];
            Member *m = validate(p.roomId, p.playerId, p.token, &room);
            if (!m) { st_.rejected++; continue; }
            m->lastSeen[d] = now;
            room->lastTrafficUs = now;
            ensurePersonal(*room, p.playerId, *m, d);
            drifto_PingDn o{};
            o.playerId = p.playerId;
            o.roomId = p.roomId;
            o.seq = p.seq;
            o.clientTimeUs = p.clientTimeUs;
            dds_write(m->pp[d].w2, &o);
            st_.pings++;
        }
        dds_return_loan(L.rPi, samples, n);
    }
    // inputs: to the host only, on every transport the host was recently seen on
    while ((n = dds_take(L.rIn, samples, infos, 64, 64)) > 0)
    {
        for (int i = 0; i < n && !drop; i++)
        {
            if (!infos[i].valid_data) continue;
            const auto &u = *(drifto_InputUp *)samples[i];
            Member *m = validate(u.roomId, u.playerId, u.token, &room);
            if (!m) { st_.rejected++; continue; }
            m->lastSeen[d] = now;
            room->lastTrafficUs = now;
            auto h = room->members.find(room->hostId);
            // no host known, or the host's own inputs: nothing to forward
            if (h == room->members.end() || u.playerId == room->hostId) continue;
            drifto_InputDn o{};
            o.playerId = u.playerId;
            o.roomId = u.roomId;
            o.inputs = u.inputs; // shallow: dds_write serializes synchronously
            for (int e : {kUdp, kTcp})
            {
                if (!d_[e].on || (e == kUdp && udpBlocked())) continue;
                // send only on transports the host was recently heard on (clients may be on either)
                if (now - h->second.lastSeen[e] > kRecentUs) continue;
                ensurePersonal(*room, room->hostId, h->second, e);
                dds_write(h->second.pp[e].w1, &o);
            }
            st_.inputs++;
        }
        dds_return_loan(L.rIn, samples, n);
    }
    // snapshots: host -> whole room
    while ((n = dds_take(L.rSn, samples, infos, 64, 64)) > 0)
    {
        for (int i = 0; i < n && !drop; i++)
        {
            if (!infos[i].valid_data) continue;
            const auto &s = *(drifto_SnapUp *)samples[i];
            Member *m = validate(s.roomId, s.playerId, s.token, &room);
            if (!m || s.playerId != room->hostId) { st_.rejected++; continue; }
            m->lastSeen[d] = now;
            room->lastTrafficUs = now;
            drifto_SnapDn o{};
            o.roomId = s.roomId;
            o.hostTick = s.hostTick;
            o.hostTimeUs = s.hostTimeUs;
            o.phase = s.phase;
            o.phaseTimer = s.phaseTimer;
            o.eliminationTimer = s.eliminationTimer;
            o.eliminationStage = s.eliminationStage;
            o.lastPlacePlayerId = s.lastPlacePlayerId;
            o.winnerPlayerId = s.winnerPlayerId;
            o.cars = s.cars;
            ensureRoomPubs(*room);
            for (int e : {kUdp, kTcp})
            {
                if (!d_[e].on || (e == kUdp && udpBlocked())) continue;
                // skip the write if no non-host member was heard recently on this transport
                bool anyone = false;
                for (auto &mm : room->members)
                    if (mm.first != room->hostId && now - mm.second.lastSeen[e] <= kRecentUs) anyone = true;
                if (!anyone) continue;
                dds_write(room->rp[e].w1, &o);
                st_.snapFanout++;
            }
            st_.snaps++;
        }
        dds_return_loan(L.rSn, samples, n);
    }
    // match control: host -> whole room, reliable; seqs assigned here
    while ((n = dds_take(L.rCt, samples, infos, 64, 64)) > 0)
    {
        for (int i = 0; i < n && !drop; i++)
        {
            if (!infos[i].valid_data) continue;
            const auto &c = *(drifto_CtlUp *)samples[i];
            Member *m = validate(c.roomId, c.playerId, c.token, &room);
            if (!m || c.playerId != room->hostId) { st_.rejected++; continue; }
            // retransmit of an already-applied request
            if (room->lastCtlFrom == c.playerId && c.reqSeq <= room->lastCtlReq) continue; // duplicate
            room->lastCtlFrom = c.playerId;
            room->lastCtlReq = c.reqSeq;
            m->lastSeen[d] = now;
            room->lastTrafficUs = now;
            // the game server, not the client, assigns matchSeq/phaseSeq so clients can order and
            // de-duplicate control messages
            room->ctl.roomId = room->id;
            if (c.op == CtlStart)
            {
                room->ctl.matchSeq++;
                room->ctl.trackSeed = c.trackSeed;
                room->ctl.startTick = c.startTick;
            }
            else if (c.op == CtlPhase)
            {
                room->ctl.phaseSeq++;
                room->ctl.phase = c.phase;
            }
            room->hasCtl = true;
            ensureRoomPubs(*room);
            for (int e : {kUdp, kTcp})
                if (d_[e].on) dds_write(room->rp[e].w2, &room->ctl);
            st_.ctl++;
            srv::log("room %u: %s (match %u seed %u, phase %u)", room->id, c.op == CtlStart ? "match start" : "phase",
                     room->ctl.matchSeq, room->ctl.trackSeed, room->ctl.phase);
            // report the new phase to the API server (shown in the room list)
            drifto_GsRoomStatus rs{};
            rs.roomId = room->id;
            copyStr(rs.gsId, sizeof rs.gsId, id_);
            rs.phase = room->ctl.phase;
            dds_write(statusW_, &rs);
        }
        dds_return_loan(L.rCt, samples, n);
    }
}

// Applies GsRoom updates: open=false deletes the room; otherwise creates/updates it,
// sets the host, removes members no longer listed and refreshes tokens. The roster is
// capped at kMaxRoomPlayers.
void GameServer::pollApi(uint64_t now)
{
    void *samples[32] = {nullptr};
    dds_sample_info_t infos[32];
    int n;
    while ((n = dds_take(roomR_, samples, infos, 32, 32)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            if (!infos[i].valid_data) continue;
            const auto &g = *(drifto_GsRoom *)samples[i];
            auto it = rooms_.find(g.roomId);
            if (!g.open)
            {
                if (it != rooms_.end())
                {
                    deleteRoom(it->second);
                    rooms_.erase(it);
                    srv::log("room %u closed", g.roomId);
                }
                continue;
            }
            Room &r = rooms_[g.roomId];
            bool isNew = r.id == 0;
            r.id = g.roomId;
            if (!isNew && r.hostId != g.hostId) srv::log("room %u: host %d -> %d", r.id, r.hostId, g.hostId);
            r.hostId = g.hostId;
            r.lastTrafficUs = now;
            // Reconcile members: the API server's roster is authoritative.
            // sync members (keep existing writers)
            std::map<int32_t, uint64_t> want;
            for (uint32_t k = 0; k < g.members._length && want.size() < (size_t)kMaxRoomPlayers; k++)
                want[g.members._buffer[k].playerId] = g.members._buffer[k].token;
            if (g.members._length > (uint32_t)kMaxRoomPlayers)
                srv::log("room %u: roster of %u exceeds the %d-player cap, extra players ignored", g.roomId,
                         g.members._length, kMaxRoomPlayers);
            for (auto m = r.members.begin(); m != r.members.end();)
            {
                if (!want.count(m->first))
                {
                    deletePubs(m->second.pp);
                    m = r.members.erase(m);
                }
                else
                    ++m;
            }
            for (auto &w : want) r.members[w.first].token = w.second;
            ensureRoomPubs(r);
            if (isNew) srv::log("room %u assigned (host %d, %zu players)", r.id, r.hostId, r.members.size());
        }
        dds_return_loan(roomR_, samples, n);
    }
}

// Main loop (see header for the threading model).
void GameServer::run()
{
    uint64_t lastBeat = 0, lastStats = monoUs(), lastGc = 0, lastTune = 0;
    Stats prev;
    while (!srv::quitRequested())
    {
        dds_attach_t trig[16];
        dds_waitset_wait(ws_, trig, 16, DDS_MSECS(100));
        uint64_t now = monoUs();
        pollApi(now);
        for (int d : {kUdp, kTcp}) pollDomain(d, now);
        if (now - lastTune >= 250000)
        {
            dds::tuneTcpSockets();
            lastTune = now;
        }
        if (now - lastBeat >= kSec)
        {
            beat();
            lastBeat = now;
        }
        // drop rooms with no valid traffic for kRoomIdleUs
        if (now - lastGc >= 5 * kSec)
        {
            lastGc = now;
            for (auto it = rooms_.begin(); it != rooms_.end();)
            {
                if (now - it->second.lastTrafficUs > kRoomIdleUs)
                {
                    srv::log("room %u idle, dropped", it->first);
                    deleteRoom(it->second);
                    it = rooms_.erase(it);
                }
                else
                    ++it;
            }
        }
        // log per-second rates only when there was activity
        if (now - lastStats >= 10 * kSec)
        {
            double dt = (now - lastStats) / 1e6;
            if (st_.inputs != prev.inputs || st_.snaps != prev.snaps || st_.pings != prev.pings)
                srv::log("%zu rooms | per s: inputs %.0f, snapshots %.0f (fan-out %.0f), pings %.0f | rejected %llu",
                         rooms_.size(), (st_.inputs - prev.inputs) / dt, (st_.snaps - prev.snaps) / dt,
                         (st_.snapFanout - prev.snapFanout) / dt, (st_.pings - prev.pings) / dt,
                         (unsigned long long)st_.rejected);
            prev = st_;
            lastStats = now;
        }
    }
}

// Nothing to release explicitly.
void GameServer::shutdown()
{
    // No DDS teardown on purpose: CycloneDDS 0.10.4 can deadlock deleting a
    // TCP domain with live connections; process exit releases everything.
    srv::log("shutting down");
}

} // namespace

// Entry point: parse args, init, run until SIGINT/SIGTERM, then exit without DDS teardown.
int main(int argc, char **argv)
{
    srv::setLogTag("game_server");
    srv::Args args(argc, argv);
    if (args.has("--help") || args.has("-h"))
    {
        printf("usage: %s [options]\n"
               "  --api HOST[:PORT]       API server (default 127.0.0.1:7600)\n"
               "  --public-addr HOST      address clients use to reach this server (default: 127.0.0.1 if the\n"
               "                          API is local, else this machine's first IPv4)\n"
               "  --udp-port N            UDP game service port, also uses N+1 (default 7700, 0 = off)\n"
               "  --tcp-port N            TCP fallback port (default 443, env DRIFTO_GAME_TCP_PORT, 0 = off)\n"
               "  --udp-domain N          DDS domain id of the UDP service (default 2)\n"
               "  --tcp-domain N          DDS domain id of the TCP service (default 3)\n"
               "  --id NAME               game server id (default <hostname>-<tcp>-<udp>)\n"
               "  --capacity N            max rooms (default 64)\n"
               "  --insecure              no DDS Security (must match clients/API server)\n"
               "  --certs DIR             certificate dir (default certs)\n"
               "  --config DIR            CycloneDDS XML templates (default net/config)\n"
               "  --simulate-udp-blocked  advertise UDP but don't serve it (tests the TCP fallback);\n"
               "                          SIGUSR1 toggles dropping UDP traffic at runtime\n",
               argv[0]);
        return 0;
    }
    srv::installSignals();
    GameServer g;
    if (!g.init(args)) return 1;
    g.run();
    g.shutdown();
    fflush(stdout);
    _exit(0);
}
