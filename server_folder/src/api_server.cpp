// api_server.cpp -- the Drifto API service (CycloneDDS over TCP).
//
//   * login (auth.c: PBKDF2, players.auth), sessions + heartbeats/timeouts
//   * room list / create / join / leave, ready flags, host election
//   * room allocator: game servers register + heartbeat (GsBeat); each new
//     room is assigned to the least-loaded live game server, which gets the
//     room's roster + per-player tokens (GsRoom) so it can route and
//     validate that room's packets.
//
// Usage: ./api_server [--port N] [--insecure] [--auth FILE] [--certs DIR]
//                     [--config DIR] [--public-addr HOST]
// See docs/MULTIPLAYER.md.
//
// Why a separate service: the API server owns everything stateful and
// security-relevant (accounts, sessions, rooms, host election, the per-player
// room tokens). Game servers hold no accounts; they only learn rooms from
// GsRoom and can be added or lost at any time (rooms on a lost game server are
// closed). The data path (UDP/TCP game traffic) therefore never touches this
// process.
//
// Transport/security: one DDS domain (kApiDomain) over TCP, configured from
// net/config/tcp_server.xml, so it works through NAT/firewalls. DDS Security
// (certs/ + governance/permissions) is on unless --insecure. Because every
// client ships the same cert material, the application-layer login (PBKDF2,
// see auth.c) is what identifies a player. The client picks a random non-zero
// session id; replies go out on a per-session partition ("s<hex>") so only
// that client sees them. Every op except login requires a logged-in session
// (RepSessionExpired otherwise).
//
// Room allocation/capacity: a new room goes to the live game server with the
// lowest rooms/capacity ratio (pickGs); servers at capacity or silent for
// kGsTimeoutUs are skipped. A room holds at most kMaxRoomPlayers. Each member
// gets a random 64-bit token on join; the client uses it on the game server
// and the game server checks it against the GsRoom roster.
//
// Threading: single thread. run() waits on a DDS waitset (100 ms cap), drains
// the request / game-server heartbeat / room-status readers and calls timers()
// every 200 ms. All maps (sessions_, rooms_, gss_) are touched only here, so no
// locking is needed; auth.c takes its own mutex internally.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include "../../net/src/net_common.h"
#include "../include/server.h"

extern "C" {
#include "../../auth.h"
}

using namespace net;
using namespace net::wire;

namespace {

// Liveness timeouts, all measured against monoUs() of the last packet seen.
constexpr uint64_t kSec = 1000000;
constexpr uint64_t kSessionTimeoutUs = 6 * kSec;  // logged-in client silent this long -> dropped
constexpr uint64_t kAnonTimeoutUs = 15 * kSec;    // never logged in -> forget the session
constexpr uint64_t kGsTimeoutUs = 5 * kSec;       // game server silent -> its rooms are closed

// One API client, keyed by the client-chosen session id. Created lazily by session()
// on the first request; pub/w is the reply writer in this session's own partition.
struct Session
{
    uint64_t id = 0;
    bool loggedIn = false;
    std::string name;
    int32_t pid = 0;
    uint32_t room = 0;
    uint64_t lastSeenUs = 0;
    dds_entity_t pub = 0, w = 0;
    uint32_t repSeq = 0;
};

// A player inside a room. 'token' is the credential the game server checks on
// every packet from this player; 'session' links back to the Session for roster names.
struct Member
{
    int32_t pid;
    uint64_t session;
    uint64_t token;
    bool ready;
};

// Lobby/match room, allocated to one game server (gsId). 'phase' mirrors the
// MatchPhase reported back by the game server (GsRoomStatus).
struct Room
{
    uint32_t id = 0;
    std::string name;
    std::string gsId;
    std::vector<Member> members; // join order; members[0] is next in line for host
    int32_t hostId = 0;
    uint8_t phase = 0;
};

// A registered game server, created/refreshed by its GsBeat heartbeat. 'rooms' is the
// number of rooms assigned here (drives pickGs); roomW publishes GsRoom to it.
struct GameSrv
{
    std::string id, addr;
    uint64_t incarnation = 0;
    int udpPort = 0, tcpPort = 0;
    uint32_t udpDomain = 0, tcpDomain = 0;
    uint32_t capacity = 0, load = 0;
    uint64_t lastSeenUs = 0;
    dds_entity_t pub = 0, roomW = 0;
    int rooms = 0;
};

class ApiServer
{
public:
    bool init(const srv::Args &a);
    void run();
    void shutdown();

private:
    dds::Security sec_;
    std::string configDir_;
    dds_entity_t part_ = 0, ws_ = 0;
    dds_entity_t tRep_ = 0, tGsRoom_ = 0;
    dds_entity_t reqR_ = 0, beatR_ = 0, statusR_ = 0;
    std::map<uint64_t, Session> sessions_;
    std::map<uint32_t, Room> rooms_;
    std::map<std::string, GameSrv> gss_;
    uint32_t nextRoom_ = 1;
    int32_t nextPid_ = 1;

    Session &session(uint64_t id, uint64_t now);
    void dropSession(uint64_t id, const char *why);
    void send(Session &s, drifto_ApiRep &r);
    void sendSimple(Session &s, uint32_t reqSeq, uint8_t kind, const std::string &msg);
    void handle(const drifto_ApiReq &q, uint64_t now);
    void doLogin(Session &s, const drifto_ApiReq &q);
    void doCreate(Session &s, const drifto_ApiReq &q);
    void doJoin(Session &s, const drifto_ApiReq &q, uint32_t roomId);
    void sendJoined(Session &s, uint32_t reqSeq, Room &r, const Member &m);
    void removeFromRoom(Session &s, const char *why);
    void pushRoster(Room &r);
    void fillRoster(const Room &r, std::vector<drifto_RosterW> &buf, drifto_ApiRep &rep);
    void publishRoom(Room &r, bool open);
    void closeRoom(uint32_t roomId, const std::string &why);
    GameSrv *pickGs(uint64_t now);
    void handleBeat(const drifto_GsBeat &b, uint64_t now);
    void timers(uint64_t now);
};

// 1..kMaxNameLen chars of [A-Za-z0-9_-]. The restricted set also keeps names safe for
// the ':'-separated credentials file and for log lines.
bool validName(const std::string &n)
{
    if (n.empty() || n.size() > (size_t)kMaxNameLen) return false;
    for (char c : n)
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
    return true;
}

// Parses options, opens the credentials file, joins the API DDS domain and creates
// topics/readers. Returns false (after logging) on any failure; main() then exits 1.
bool ApiServer::init(const srv::Args &a)
{
    const char *envPort = getenv("DRIFTO_API_PORT");
    int port = a.num("--port", envPort ? atoi(envPort) : kApiPort);
    sec_.enabled = !a.has("--insecure");
    sec_.certDir = a.str("--certs", "certs");
    sec_.identity = "server";
    configDir_ = a.str("--config", "net/config");
    std::string authFile = a.str("--auth", "players.auth");
    // CycloneDDS needs an IP for ExternalNetworkAddress: resolve the host name; loopback or
    // unresolvable -> "auto".
    std::string external = a.str("--public-addr", "auto");
    if (external != "auto")
    {
        std::string ip = srv::resolveIPv4(external); // Cyclone wants an IP here
        external = (ip.empty() || srv::isLoopbackHost(ip)) ? "auto" : ip;
    }
    a.warnUnknown();

    // Credentials must be available before accepting any login.
    if (!auth_init(authFile.c_str()))
    {
        srv::log("cannot open credentials file %s", authFile.c_str());
        return false;
    }
    std::string err;
    std::string xml = dds::loadConfig(configDir_, "tcp_server.xml",
                                      {{"PORT", std::to_string(port)}, {"EXTERNAL", external}}, "api-server");
    // Binds the TCP listener for the API domain (port < 1024 needs privileges).
    if (!dds::acquireDomain(kApiDomain, xml, &err))
    {
        srv::log("%s", err.c_str());
        if (port < 1024) srv::log("ports < 1024 need root or: sudo setcap cap_net_bind_service=+ep ./api_server");
        return false;
    }
    part_ = dds::createParticipant(kApiDomain, sec_, &err);
    if (part_ < 0)
    {
        srv::log("%s", err.c_str());
        return false;
    }
    dds_entity_t tReq = dds_create_topic(part_, &drifto_ApiReq_desc, kTopicApiReq, nullptr, nullptr);
    tRep_ = dds_create_topic(part_, &drifto_ApiRep_desc, kTopicApiRep, nullptr, nullptr);
    dds_entity_t tBeat = dds_create_topic(part_, &drifto_GsBeat_desc, kTopicGsBeat, nullptr, nullptr);
    tGsRoom_ = dds_create_topic(part_, &drifto_GsRoom_desc, kTopicGsRoom, nullptr, nullptr);
    dds_entity_t tStatus = dds_create_topic(part_, &drifto_GsRoomStatus_desc, kTopicGsStatus, nullptr, nullptr);
    // Requests and room status: reliable with a deep history so bursts are not lost.
    // Heartbeats only need the latest sample (depth 1).
    dds_qos_t *rq = dds::qosReliable(4096, false);
    reqR_ = dds_create_reader(part_, tReq, rq, nullptr);
    statusR_ = dds_create_reader(part_, tStatus, rq, nullptr);
    dds_delete_qos(rq);
    dds_qos_t *bq = dds::qosReliable(1, false);
    beatR_ = dds_create_reader(part_, tBeat, bq, nullptr);
    dds_delete_qos(bq);
    if (tReq < 0 || tRep_ < 0 || tBeat < 0 || tGsRoom_ < 0 || tStatus < 0 || reqR_ < 0 || beatR_ < 0 || statusR_ < 0)
    {
        srv::log("failed to create topics/readers");
        return false;
    }
    // Wake the main loop whenever any reader gets data.
    ws_ = dds_create_waitset(DDS_CYCLONEDDS_HANDLE);
    for (dds_entity_t r : {reqR_, beatR_, statusR_})
    {
        dds_set_status_mask(r, DDS_DATA_AVAILABLE_STATUS);
        dds_waitset_attach(ws_, r, r);
    }
    srv::log("API service up: TCP port %d, DDS domain %u, security %s, auth file %s", port, kApiDomain,
             sec_.enabled ? "ON" : "OFF (--insecure)", authFile.c_str());
    return true;
}

// Find-or-create the session for 'id' and refresh its liveness timestamp. Any request
// (including heartbeats) counts as activity.
Session &ApiServer::session(uint64_t id, uint64_t now)
{
    Session &s = sessions_[id];
    if (s.id == 0)
    {
        s.id = id;
        s.pub = dds::createPublisher(part_, sessionPartition(id));
        dds_qos_t *q = dds::qosReliable(32, true);
        s.w = dds_create_writer(s.pub, tRep_, q, nullptr);
        dds_delete_qos(q);
    }
    s.lastSeenUs = now;
    return s;
}

// Stamps session id + per-session reply sequence and writes to the session's writer.
void ApiServer::send(Session &s, drifto_ApiRep &r)
{
    r.session = s.id;
    r.repSeq = ++s.repSeq;
    if (s.w > 0) dds_write(s.w, &r);
}

// Reply carrying only kind + message (+ current player/room ids) for the given request.
void ApiServer::sendSimple(Session &s, uint32_t reqSeq, uint8_t kind, const std::string &msg)
{
    drifto_ApiRep r{};
    r.reqSeq = reqSeq;
    r.kind = kind;
    r.playerId = s.pid;
    r.roomId = s.room;
    copyStr(r.message, sizeof r.message, msg);
    send(s, r);
}

// Forget a session: leave its room (re-electing host / closing the room if needed),
// delete its reply publisher. No-op for unknown ids.
void ApiServer::dropSession(uint64_t id, const char *why)
{
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    Session &s = it->second;
    if (s.loggedIn) srv::log("player %d '%s' logged out (%s)", s.pid, s.name.c_str(), why);
    removeFromRoom(s, why);
    if (s.pub > 0) dds_delete(s.pub);
    sessions_.erase(it);
}

// Dispatch one client request (called from run()). Session 0 is ignored; logout drops
// the session; everything but login needs a logged-in session.
void ApiServer::handle(const drifto_ApiReq &q, uint64_t now)
{
    // 0 is the "unset" id; never create a session for it
    if (q.session == 0) return;
    if (q.op == OpLogout)
    {
        dropSession(q.session, "logout");
        return;
    }
    Session &s = session(q.session, now);
    // auth gate: the client reacts to RepSessionExpired by logging in again
    if (q.op != OpLogin && !s.loggedIn)
    {
        sendSimple(s, q.seq, RepSessionExpired, "not logged in");
        return;
    }
    switch (q.op)
    {
    case OpLogin: doLogin(s, q); break;
    // keep-alive: session() above already refreshed lastSeenUs, so just ack
    case OpHeartbeat: sendSimple(s, q.seq, RepHeartbeatAck, ""); break;
    // snapshot of all rooms; inProgress = anything past the Lobby phase
    case OpRoomList:
    {
        std::vector<drifto_RoomInfoW> buf;
        for (auto &kv : rooms_)
        {
            drifto_RoomInfoW w{};
            w.roomId = kv.first;
            copyStr(w.name, sizeof w.name, kv.second.name);
            w.players = (uint8_t)kv.second.members.size();
            w.maxPlayers = kMaxRoomPlayers;
            w.inProgress = kv.second.phase != (uint8_t)MatchPhase::Lobby;
            buf.push_back(w);
        }
        drifto_ApiRep r{};
        r.reqSeq = q.seq;
        r.kind = RepRoomList;
        r.playerId = s.pid;
        r.rooms._buffer = buf.data();
        r.rooms._length = r.rooms._maximum = (uint32_t)buf.size();
        send(s, r);
        break;
    }
    case OpCreateRoom: doCreate(s, q); break;
    case OpJoinRoom: doJoin(s, q, q.roomId); break;
    // remember the room id so the reply can say which room was left
    case OpLeaveRoom:
    {
        uint32_t old = s.room;
        removeFromRoom(s, "left");
        drifto_ApiRep r{};
        r.reqSeq = q.seq;
        r.kind = RepLeft;
        r.roomId = old;
        r.playerId = s.pid;
        send(s, r);
        break;
    }
    // toggle this player's ready flag, then broadcast the new roster to the room
    case OpSetReady:
    {
        auto it = rooms_.find(s.room);
        if (it == rooms_.end()) break;
        for (auto &m : it->second.members)
            if (m.pid == s.pid) m.ready = q.flag;
        pushRoster(it->second);
        break;
    }
    default: sendSimple(s, q.seq, RepError, "unknown op"); break;
    }
}

// Login: validates name/password, authenticates (register-on-first-use) and rejects a
// second concurrent session for the same name. On success assigns a fresh player id
// (pid). Failures are reported as RepLoginFailed with a reason.
void ApiServer::doLogin(Session &s, const drifto_ApiReq &q)
{
    std::string name = q.name, pw = q.password;
    // idempotent: repeated login of the same account on the same session just re-acks
    if (s.loggedIn && strcasecmp(s.name.c_str(), name.c_str()) == 0)
    {
        drifto_ApiRep r{};
        r.reqSeq = q.seq;
        r.kind = RepLoginOk;
        r.playerId = s.pid;
        copyStr(r.message, sizeof r.message, "already logged in");
        send(s, r);
        return;
    }
    if (!validName(name))
    {
        sendSimple(s, q.seq, RepLoginFailed, "invalid name (1-8 chars: letters, digits, _ -)");
        return;
    }
    if (pw.empty())
    {
        sendSimple(s, q.seq, RepLoginFailed, "empty password");
        return;
    }
    // PBKDF2 check (or first-time registration) in auth.c. Players pick their own
    // password: the first login with a new name creates the account, later logins
    // must match it. The reply text tells the client which case happened.
    bool registered = false;
    AuthResult ar = auth_authenticate_ex(name.c_str(), pw.c_str(), &registered);
    if (ar == AUTH_BAD_PASSWORD)
    {
        srv::log("login '%s': bad password", name.c_str());
        sendSimple(s, q.seq, RepLoginFailed, "wrong password - this name is already registered");
        return;
    }
    if (ar != AUTH_OK)
    {
        sendSimple(s, q.seq, RepLoginFailed, "authentication error (server)");
        return;
    }
    // one live login per account (case-insensitive)
    for (auto &kv : sessions_)
        if (kv.first != s.id && kv.second.loggedIn && strcasecmp(kv.second.name.c_str(), name.c_str()) == 0)
        {
            sendSimple(s, q.seq, RepLoginFailed, "already logged in elsewhere");
            return;
        }
    // switching account on an existing session: leave the old identity's room first
    if (s.loggedIn) removeFromRoom(s, "relogin");
    s.loggedIn = true;
    s.name = name;
    s.pid = nextPid_++;
    srv::log("player %d '%s' logged in%s", s.pid, name.c_str(), registered ? " (new account)" : "");
    drifto_ApiRep r{};
    r.reqSeq = q.seq;
    r.kind = RepLoginOk;
    r.playerId = s.pid;
    copyStr(r.message, sizeof r.message, registered ? "account created" : "welcome back");
    send(s, r);
}

// Room allocator: least-loaded live game server (rooms/capacity; capacity 0 is treated
// as unlimited and ranked by room count). Returns nullptr when none is alive or all are full.
GameSrv *ApiServer::pickGs(uint64_t now)
{
    GameSrv *best = nullptr;
    double bestLoad = 1e9;
    for (auto &kv : gss_)
    {
        GameSrv &g = kv.second;
        if (now - g.lastSeenUs > kGsTimeoutUs) continue;
        if (g.capacity > 0 && (uint32_t)g.rooms >= g.capacity) continue;
        double load = g.capacity ? (double)g.rooms / g.capacity : g.rooms;
        if (load < bestLoad)
        {
            bestLoad = load;
            best = &g;
        }
    }
    return best;
}

// Create a room on the best game server; the creator becomes host and first member.
// Fails with "no game server available" before leaving the current room.
void ApiServer::doCreate(Session &s, const drifto_ApiReq &q)
{
    GameSrv *g = pickGs(monoUs());
    if (!g)
    {
        sendSimple(s, q.seq, RepJoinFailed, "no game server available");
        return;
    }
    removeFromRoom(s, "switched room");
    Room r;
    r.id = nextRoom_++;
    r.name = q.roomName[0] ? std::string(q.roomName) : "Room " + std::to_string(r.id);
    r.gsId = g->id;
    r.hostId = s.pid;
    // fresh random token for this player in this room
    r.members.push_back({s.pid, s.id, randomU64(), false});
    g->rooms++;
    Room &room = rooms_[r.id] = r;
    s.room = room.id;
    srv::log("room %u '%s' created by %d on game server %s", room.id, room.name.c_str(), s.pid, g->id.c_str());
    // tell the game server the roster so it accepts this player's packets
    publishRoom(room, true);
    sendJoined(s, q.seq, room, room.members.back());
}

// Join an existing room. Re-joining a room you are in repeats the answer (idempotent);
// full rooms are refused. Switching rooms leaves the old one first.
void ApiServer::doJoin(Session &s, const drifto_ApiReq &q, uint32_t roomId)
{
    auto it = rooms_.find(roomId);
    if (it == rooms_.end())
    {
        sendSimple(s, q.seq, RepJoinFailed, "no such room");
        return;
    }
    Room &room = it->second;
    for (auto &m : room.members)
        if (m.pid == s.pid)
        {
            sendJoined(s, q.seq, room, m); // already in: repeat the answer
            return;
        }
    // capacity check
    if (room.members.size() >= (size_t)kMaxRoomPlayers)
    {
        sendSimple(s, q.seq, RepJoinFailed, "room full");
        return;
    }
    removeFromRoom(s, "switched room");
    room.members.push_back({s.pid, s.id, randomU64(), false});
    s.room = room.id;
    srv::log("player %d joined room %u (%zu players)", s.pid, room.id, room.members.size());
    publishRoom(room, true);
    sendJoined(s, q.seq, room, room.members.back());
    pushRoster(room);
}

// Builds the RepRoster payload (name, host flag, ready flag per member) into 'buf', which
// must outlive the write since 'rep' only points at it.
void ApiServer::fillRoster(const Room &r, std::vector<drifto_RosterW> &buf, drifto_ApiRep &rep)
{
    buf.clear();
    for (const auto &m : r.members)
    {
        drifto_RosterW w{};
        w.playerId = m.pid;
        auto it = sessions_.find(m.session);
        copyStr(w.name, sizeof w.name, it != sessions_.end() ? it->second.name : "?");
        w.isHost = m.pid == r.hostId;
        w.ready = m.ready;
        buf.push_back(w);
    }
    rep.roster._buffer = buf.data();
    rep.roster._length = rep.roster._maximum = (uint32_t)buf.size();
    rep.roomId = r.id;
    rep.hostId = r.hostId;
}

// RepJoined: roster plus everything the client needs to reach its game server (address,
// UDP/TCP ports and DDS domains) and the player's room token.
void ApiServer::sendJoined(Session &s, uint32_t reqSeq, Room &r, const Member &m)
{
    auto git = gss_.find(r.gsId);
    std::vector<drifto_RosterW> buf;
    drifto_ApiRep rep{};
    rep.reqSeq = reqSeq;
    rep.kind = RepJoined;
    rep.playerId = s.pid;
    fillRoster(r, buf, rep);
    if (git != gss_.end())
    {
        copyStr(rep.gsHost, sizeof rep.gsHost, git->second.addr);
        rep.gsUdpPort = (uint16_t)git->second.udpPort;
        rep.gsTcpPort = (uint16_t)git->second.tcpPort;
        rep.gsUdpDomain = git->second.udpDomain;
        rep.gsTcpDomain = git->second.tcpDomain;
    }
    rep.token = m.token;
    send(s, rep);
}

// Send the current roster to every member that still has a session.
void ApiServer::pushRoster(Room &r)
{
    std::vector<drifto_RosterW> buf;
    for (const auto &m : r.members)
    {
        auto it = sessions_.find(m.session);
        if (it == sessions_.end()) continue;
        drifto_ApiRep rep{};
        rep.kind = RepRoster;
        rep.playerId = m.pid;
        fillRoster(r, buf, rep);
        send(it->second, rep);
    }
}

// Sync a room to its game server as GsRoom (roster + tokens + host). open=false
// unregisters the instance, which tells the game server to delete the room.
void ApiServer::publishRoom(Room &r, bool open)
{
    auto git = gss_.find(r.gsId);
    if (git == gss_.end() || git->second.roomW <= 0) return;
    std::vector<drifto_GsMember> mem;
    for (const auto &m : r.members) mem.push_back({m.pid, m.token});
    drifto_GsRoom g{};
    g.roomId = r.id;
    g.open = open;
    g.hostId = r.hostId;
    g.members._buffer = mem.data();
    g.members._length = g.members._maximum = (uint32_t)mem.size();
    dds_write(git->second.roomW, &g);
    if (!open) dds_unregister_instance(git->second.roomW, &g);
}

// Remove the session's player from its room. Empty room -> closed. If the host left,
// the next member in join order is promoted. Remaining members get a new roster.
void ApiServer::removeFromRoom(Session &s, const char *why)
{
    if (s.room == 0) return;
    auto it = rooms_.find(s.room);
    s.room = 0;
    if (it == rooms_.end()) return;
    Room &r = it->second;
    r.members.erase(std::remove_if(r.members.begin(), r.members.end(), [&](const Member &m) { return m.pid == s.pid; }),
                    r.members.end());
    srv::log("player %d left room %u (%s)", s.pid, r.id, why);
    if (r.members.empty())
    {
        closeRoom(r.id, "empty");
        return;
    }
    if (r.hostId == s.pid)
    {
        r.hostId = r.members.front().pid; // next in join order
        srv::log("room %u: host %d gone, promoting %d", r.id, s.pid, r.hostId);
    }
    publishRoom(r, true);
    pushRoster(r);
}

// Close a room: notify members with RepLeft, tell the game server, release the game
// server's room slot and erase the room.
void ApiServer::closeRoom(uint32_t roomId, const std::string &why)
{
    auto it = rooms_.find(roomId);
    if (it == rooms_.end()) return;
    Room &r = it->second;
    for (const auto &m : r.members)
    {
        auto s = sessions_.find(m.session);
        if (s == sessions_.end()) continue;
        s->second.room = 0;
        drifto_ApiRep rep{};
        rep.kind = RepLeft;
        rep.roomId = roomId;
        rep.playerId = m.pid;
        copyStr(rep.message, sizeof rep.message, "room closed: " + why);
        send(s->second, rep);
    }
    publishRoom(r, false);
    auto g = gss_.find(r.gsId);
    if (g != gss_.end() && g->second.rooms > 0) g->second.rooms--;
    srv::log("room %u closed (%s)", roomId, why.c_str());
    rooms_.erase(it);
}

// Register/refresh a game server from its heartbeat (address, ports, domains, capacity).
// A changed 'incarnation' means the process restarted and is logged again.
void ApiServer::handleBeat(const drifto_GsBeat &b, uint64_t now)
{
    GameSrv &g = gss_[b.gsId];
    bool isNew = g.id.empty();
    if (isNew)
    {
        g.id = b.gsId;
        g.pub = dds::createPublisher(part_, gsPartition(g.id));
        dds_qos_t *q = dds::qosReliable(1, true);
        g.roomW = dds_create_writer(g.pub, tGsRoom_, q, nullptr);
        dds_delete_qos(q);
    }
    if (isNew || g.incarnation != b.incarnation)
        srv::log("game server '%s' registered: %s udp:%u tcp:%u (domains %u/%u), capacity %u", b.gsId, b.publicAddr,
                 b.udpPort, b.tcpPort, b.udpDomain, b.tcpDomain, b.capacity);
    g.incarnation = b.incarnation;
    g.addr = b.publicAddr;
    g.udpPort = b.udpPort;
    g.tcpPort = b.tcpPort;
    g.udpDomain = b.udpDomain;
    g.tcpDomain = b.tcpDomain;
    g.capacity = b.capacity;
    g.load = b.load;
    g.lastSeenUs = now;
}

// Housekeeping (every ~200 ms): drop silent sessions (logged-in after kSessionTimeoutUs,
// never-logged-in after kAnonTimeoutUs) and expire game servers silent for
// kGsTimeoutUs, closing all of their rooms.
void ApiServer::timers(uint64_t now)
{
    std::vector<uint64_t> dead;
    for (auto &kv : sessions_)
    {
        uint64_t idle = now - kv.second.lastSeenUs;
        if ((kv.second.loggedIn && idle > kSessionTimeoutUs) || (!kv.second.loggedIn && idle > kAnonTimeoutUs))
            dead.push_back(kv.first);
    }
    for (uint64_t id : dead) dropSession(id, "timed out");

    std::vector<std::string> deadGs;
    for (auto &kv : gss_)
        if (now - kv.second.lastSeenUs > kGsTimeoutUs) deadGs.push_back(kv.first);
    for (const auto &id : deadGs)
    {
        srv::log("game server '%s' timed out", id.c_str());
        std::vector<uint32_t> victims;
        for (auto &r : rooms_)
            if (r.second.gsId == id) victims.push_back(r.first);
        for (uint32_t r : victims) closeRoom(r, "game server lost");
        if (gss_[id].pub > 0) dds_delete(gss_[id].pub);
        gss_.erase(id);
    }
}

// Main loop (see header for the threading model).
void ApiServer::run()
{
    uint64_t lastTimers = 0;
    dds::tuneTcpSockets();
    while (!srv::quitRequested())
    {
        dds_attach_t trig[4];
        dds_waitset_wait(ws_, trig, 4, DDS_MSECS(100));
        uint64_t now = monoUs();
        void *samples[64] = {nullptr};
        dds_sample_info_t infos[64];
        int n;
        while ((n = dds_take(reqR_, samples, infos, 64, 64)) > 0)
        {
            for (int i = 0; i < n; i++)
                // requests
                if (infos[i].valid_data) handle(*(drifto_ApiReq *)samples[i], now);
            dds_return_loan(reqR_, samples, n);
        }
        if ((n = dds_take(beatR_, samples, infos, 64, 64)) > 0)
        {
            for (int i = 0; i < n; i++)
                // game-server heartbeats
                if (infos[i].valid_data) handleBeat(*(drifto_GsBeat *)samples[i], now);
            dds_return_loan(beatR_, samples, n);
        }
        if ((n = dds_take(statusR_, samples, infos, 64, 64)) > 0)
        {
            for (int i = 0; i < n; i++)
            {
                if (!infos[i].valid_data) continue;
                const auto &st = *(drifto_GsRoomStatus *)samples[i];
                // room phase reports; accept only from the game server that owns the room
                auto it = rooms_.find(st.roomId);
                if (it != rooms_.end() && it->second.gsId == st.gsId) it->second.phase = st.phase;
            }
            dds_return_loan(statusR_, samples, n);
        }
        if (now - lastTimers > 200000)
        {
            timers(now);
            dds::tuneTcpSockets();
            lastTimers = now;
        }
    }
}

// Close all rooms so clients get RepLeft before the process exits.
void ApiServer::shutdown()
{
    srv::log("shutting down");
    std::vector<uint32_t> ids;
    for (auto &r : rooms_) ids.push_back(r.first);
    for (uint32_t id : ids) closeRoom(id, "API server shutting down");
    dds_sleepfor(DDS_MSECS(200)); // let the RepLeft pushes go out
    // No DDS teardown on purpose: CycloneDDS 0.10.4 can deadlock in
    // dds_delete(domain) on a TCP domain with live connections (observed
    // here); process exit releases everything.
}

} // namespace

// Entry point: parse args, init, run until SIGINT/SIGTERM, then exit without DDS teardown.
int main(int argc, char **argv)
{
    srv::setLogTag("api_server");
    srv::Args args(argc, argv);
    if (args.has("--help") || args.has("-h"))
    {
        printf("usage: %s [--port N(7600)] [--insecure] [--auth FILE(players.auth)] [--certs DIR(certs)]\n"
               "          [--config DIR(net/config)] [--public-addr HOST(auto)]\n",
               argv[0]);
        return 0;
    }
    srv::installSignals();
    ApiServer s;
    if (!s.init(args)) return 1;
    s.run();
    s.shutdown();
    fflush(stdout);
    _exit(0);
}
