// net_client.cpp -- NetClient: the client side of the Drifto API + game
// services. See net/include/net_client.h for the contract and
// docs/MULTIPLAYER.md for the architecture.
//
// One worker thread owns every DDS entity. Public methods only touch the
// mutex-guarded queues/mirrors below and poke a guard condition so the
// worker wakes up immediately (no polling latency on the send path).
//
// Responsibilities:
//   * OnlineCore (this file): the Online-mode backend. It talks to two
//     services: the API service (login, room list, create/join/leave, ready,
//     heartbeat; DDS over TCP, RELIABLE) and a game server (inputs, snapshots,
//     match control, ping; DDS over UDP with a TCP-443 fallback).
//   * NetClient (bottom of the file): the public facade. Every method either
//     queues a Cmd for the worker or reads a mirror under the mutex; it
//     dispatches to OnlineCore or LanCore (net_lan.cpp) depending on Config::mode.
//
// Threading / locking: the worker thread (ClientCore::run) is the only thread
// that touches DDS entities and the "worker-only state" fields of OnlineCore.
// Caller threads only touch ClientCore state guarded by ClientCore::m (cmds,
// outInputs, outSnap, events, snap, inInputs, the s* mirrors). The worker
// takes m only for short copies, never across a blocking DDS call.
//
// Design decisions:
//   * Gameplay topics (inputs, snapshots, pings) are BEST_EFFORT: a late
//     packet is worthless, retransmitting it only adds latency. Loss of
//     inputs is covered by sending the last kInputRedundancy inputs in every
//     packet; the host de-duplicates by (playerId, tick).
//   * Match control (start/phase) and the API service are RELIABLE; the
//     control reader is also TRANSIENT_LOCAL so a late joiner or a client that
//     switched transport still gets the current match state.
//   * Per-room filtering uses DDS partitions (see net_common.h) instead of
//     filtering in the application.
//   * The game link (UDP first, TCP-443 if UDP gets no ping answer) is a state
//     machine driven by tickLink(); see the timing constants below.
#include "../include/net_client.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "net_client_core.h"
#include "../../common/platform.h"

namespace net {

using namespace wire;
using namespace detail;

namespace {

// ---- timing (microseconds, compared against monoUs()) ----
constexpr uint64_t kApiHeartbeatUs = 1 * kSec;
constexpr uint64_t kApiTimeoutUs = 6 * kSec;      // no API reply at all -> Disconnected
constexpr uint64_t kApiUnreachableUs = 5 * kSec;  // request could not be delivered
constexpr uint64_t kUdpHandshakeUs = 3 * kSec;    // Auto: UDP gets this long before TCP fallback
constexpr uint64_t kTcpHandshakeUs = 6 * kSec;
constexpr uint64_t kLinkLossUs = 3 * kSec;        // no pong for this long -> link lost
// ping period while the link is handshaking / once it is up
constexpr uint64_t kPingHandshakeUs = 200000;
constexpr uint64_t kPingUpUs = 250000;

// An API request waiting for the request writer to match a reader (the API
// server may not be discovered yet). queuedUs drives the 'unreachable' timeout.
struct PendingReq
{
    drifto_ApiReq req;
    std::string strings[3];
    uint64_t queuedUs;
};

// Where and how to reach the room's game server; delivered by the API server
// in RepJoined. token authenticates every Up packet we send for this room.
struct GsInfo
{
    std::string host;
    int udpPort = 0, tcpPort = 0;
    uint32_t udpDomain = 0, tcpDomain = 0;
    uint64_t token = 0;
};

// Online-mode backend. run() is the single worker loop: it drains queued
// commands, polls both links, flushes outgoing data and ticks the link state
// machine and heartbeat.
struct OnlineCore : ClientCore
{
    // session: random per-run id; it names the reply partition ("s<hex>") so only
    // we receive our API replies. reqSeq numbers requests; lastRepSeq drops
    // replies re-delivered by the TRANSIENT_LOCAL history.
    // ---------------- worker-only state ----------------
    uint64_t session = 0;
    uint32_t reqSeq = 0, lastRepSeq = 0;
    // API link
    bool apiDomain = false;
    dds_entity_t apiPart = 0, apiReqW = 0, apiRepR = 0;
    std::deque<PendingReq> apiPending;
    uint64_t lastApiReplyUs = 0, lastHeartbeatUs = 0;
    bool apiLostReported = false;
    // room / game link
    uint32_t roomId = 0;
    int32_t pid = 0, hostId = 0;
    GsInfo gs;
    Transport linkTr = Transport::None; // transport of the current game link (handshaking or up)
    bool linkUp = false;
    uint32_t linkDomain = 0;
    dds_entity_t gPart = 0, gInW = 0, gSnapW = 0, gCtlW = 0, gPingW = 0;
    dds_entity_t gInR = 0, gSnapR = 0, gCtlR = 0, gPingR = 0;
    uint64_t linkStartUs = 0, lastPongUs = 0, lastPingUs = 0;
    uint32_t pingSeq = 0;
    std::deque<drifto_InputW> inputHistory;
    std::deque<drifto_CtlUp> pendingCtl;
    uint32_t ctlSeq = 0;
    uint32_t seenMatchSeq = 0, seenPhaseSeq = 0;
    uint64_t sentInputs = 0;
    uint64_t lastTuneUs = 0;

    // ---------------- API link ----------------
    // Creates the API participant (TCP, persistent domain), the request writer
    // and the reply reader. Called from open() on the caller's thread, before
    // the worker exists.
    bool openApi(std::string *err)
    {
        std::string host = cfg.apiHost;
        const char *envPort = getenv("DRIFTO_API_PORT");
        // port precedence: Config::apiPort, then DRIFTO_API_PORT, then the default;
        // an explicit "host:port" in apiHost (below) overrides all of them
        int port = cfg.apiPort > 0 ? cfg.apiPort : (envPort && atoi(envPort) > 0 ? atoi(envPort) : kApiPort);
        size_t colon = host.rfind(':');
        if (colon != std::string::npos && host.find(':') == colon) // "host:port"
        {
            port = atoi(host.c_str() + colon + 1);
            host = host.substr(0, colon);
        }
        std::string xml = dds::loadConfig(cfg.configDir, "tcp_client.xml",
                                          {{"HOST", host}, {"PORT", std::to_string(port)}}, "client-api");
        if (!dds::acquireDomain(kApiDomain, xml, err, /*persistent=*/true)) return false;
        apiDomain = true;
        apiPart = dds::createParticipant(kApiDomain, sec, err);
        if (apiPart < 0) return false;
        dds_entity_t tReq = dds_create_topic(apiPart, &drifto_ApiReq_desc, kTopicApiReq, nullptr, nullptr);
        dds_entity_t tRep = dds_create_topic(apiPart, &drifto_ApiRep_desc, kTopicApiRep, nullptr, nullptr);
        dds_qos_t *wq = dds::qosReliable(64, false);
        apiReqW = dds_create_writer(apiPart, tReq, wq, nullptr);
        dds_delete_qos(wq);
        dds_entity_t sub = dds::createSubscriber(apiPart, {sessionPartition(session)});
        // TRANSIENT_LOCAL: a reply sent before our reader finished matching the
        // partition is still delivered (avoids a join-time race)
        dds_qos_t *rq = dds::qosReliable(64, true);
        apiRepR = dds_create_reader(sub, tRep, rq, nullptr);
        dds_delete_qos(rq);
        if (tReq < 0 || tRep < 0 || apiReqW < 0 || apiRepR < 0 || sub < 0)
        {
            *err = "failed to create API topics/readers/writers (permissions?)";
            return false;
        }
        dds_set_status_mask(apiRepR, DDS_DATA_AVAILABLE_STATUS);
        // the worker wakes as soon as an API reply arrives
        dds_waitset_attach(waitset, apiRepR, apiRepR);
        return true;
    }

    // Builds an ApiReq and appends it to apiPending (worker thread only). It is
    // written by flushApi() once the server has been discovered.
    void queueApi(uint8_t op, const std::string &name = "", const std::string &pw = "",
                  const std::string &roomName = "", uint32_t room = 0, bool flag = false)
    {
        PendingReq p{};
        p.req.session = session;
        p.req.seq = ++reqSeq;
        p.req.op = op;
        copyStr(p.req.name, sizeof p.req.name, name);
        copyStr(p.req.password, sizeof p.req.password, pw);
        copyStr(p.req.roomName, sizeof p.req.roomName, roomName);
        p.req.roomId = room;
        p.req.flag = flag;
        p.queuedUs = monoUs();
        apiPending.push_back(p);
    }

    // Sends pending API requests when a reader is matched; otherwise fails
    // login/create/join requests older than kApiUnreachableUs so the UI gets an
    // answer instead of waiting forever. Other ops are dropped silently.
    void flushApi(uint64_t now)
    {
        if (apiPending.empty()) return;
        if (dds::matchedReaders(apiReqW) > 0)
        {
            while (!apiPending.empty())
            {
                dds_write(apiReqW, &apiPending.front().req);
                apiPending.pop_front();
            }
            return;
        }
        // not connected yet: fail requests that waited too long
        while (!apiPending.empty() && since(now, apiPending.front().queuedUs) > kApiUnreachableUs)
        {
            uint8_t op = apiPending.front().req.op;
            apiPending.pop_front();
            Event e;
            e.message = "API server unreachable (" + cfg.apiHost + ")";
            if (op == OpLogin) { e.type = EventType::LoginFailed; emit(e); }
            else if (op == OpCreateRoom || op == OpJoinRoom) { e.type = EventType::JoinFailed; emit(e); }
        }
    }

    // API session lost (server forgot us): drop the room locally, mark logged
    // out and report Disconnected so the game goes back to the login screen.
    void resetSession(const std::string &why)
    {
        leaveRoomLocal(false);
        {
            std::lock_guard<std::mutex> lk(m);
            sLoggedIn = false;
            sPid = 0;
        }
        pid = 0;
        Event e;
        e.type = EventType::Disconnected;
        e.message = why;
        emit(e);
    }

    // Wire roster -> public RosterEntry list.
    static std::vector<RosterEntry> toRoster(const drifto_ApiRep &r)
    {
        std::vector<RosterEntry> v;
        for (uint32_t i = 0; i < r.roster._length; i++)
        {
            const drifto_RosterW &w = r.roster._buffer[i];
            RosterEntry e;
            e.playerId = w.playerId;
            e.name = w.name;
            e.isHost = w.isHost;
            e.ready = w.ready;
            v.push_back(e);
        }
        return v;
    }

    // Stores a new roster mirror and emits RosterChanged. If the host id changed
    // (host migration: the API server promoted the next player in join order)
    // also emits HostChanged. A new host clears its input de-dup table because
    // other players' tick counters are unrelated to what it saw before.
    // 'initial' = roster that came with RepJoined (no HostChanged then).
    void applyRoster(const drifto_ApiRep &r, bool initial)
    {
        std::vector<RosterEntry> roster = toRoster(r);
        int32_t prevHost = hostId;
        hostId = r.hostId;
        bool me = (hostId == pid);
        {
            std::lock_guard<std::mutex> lk(m);
            sRoster = roster;
            sIsHost = me;
            if (me && prevHost != hostId) lastInputTick.clear();
        }
        Event e;
        e.type = EventType::RosterChanged;
        e.roster = roster;
        e.roomId = roomId;
        e.localPlayerId = pid;
        e.isHost = me;
        emit(e);
        if (!initial && prevHost != hostId)
        {
            Event h;
            h.type = EventType::HostChanged;
            h.isHost = me;
            h.roomId = roomId;
            h.localPlayerId = pid;
            h.roster = roster;
            emit(h);
        }
    }

    // State machine for API replies, one case per ApiRepKind. Any reply counts as
    // proof the API server is alive (lastApiReplyUs).
    void handleApiReply(const drifto_ApiRep &r, uint64_t now)
    {
        // partitions already isolate sessions; this is a second line of defence
        if (r.session != session) return;
        // (TRANSIENT_LOCAL history can re-deliver old replies after a reconnect)
        if (r.repSeq != 0 && r.repSeq <= lastRepSeq) return; // re-delivered history
        lastRepSeq = r.repSeq;
        lastApiReplyUs = now;
        // the server is answering again: re-arm the one-shot loss report
        if (apiLostReported && r.kind != RepSessionExpired) apiLostReported = false;
        switch (r.kind)
        {
        case RepLoginOk:
        {
            pid = r.playerId;
            {
                std::lock_guard<std::mutex> lk(m);
                sLoggedIn = true;
                sPid = pid;
            }
            Event e;
            e.type = EventType::LoginOk;
            e.localPlayerId = pid;
            e.message = r.message;
            emit(e);
            break;
        }
        case RepLoginFailed:
        {
            Event e;
            e.type = EventType::LoginFailed;
            e.message = r.message;
            emit(e);
            break;
        }
        case RepRoomList:
        {
            Event e;
            e.type = EventType::RoomList;
            for (uint32_t i = 0; i < r.rooms._length; i++)
            {
                const drifto_RoomInfoW &w = r.rooms._buffer[i];
                RoomInfo ri;
                ri.roomId = w.roomId;
                ri.name = w.name;
                ri.players = w.players;
                ri.maxPlayers = w.maxPlayers;
                ri.inProgress = w.inProgress;
                e.rooms.push_back(ri);
            }
            emit(e);
            break;
        }
        case RepJoined:
        {
            // joined a different room without a LeftRoom in between: tear down the old link first
            if (roomId != 0 && roomId != r.roomId) leaveRoomLocal(true);
            bool sameRoom = (roomId == r.roomId);
            roomId = r.roomId;
            pid = r.playerId;
            gs.host = r.gsHost;
            gs.udpPort = r.gsUdpPort;
            gs.tcpPort = r.gsTcpPort;
            gs.udpDomain = r.gsUdpDomain;
            gs.tcpDomain = r.gsTcpDomain;
            gs.token = r.token;
            hostId = r.hostId;
            bool me = hostId == pid;
            {
                std::lock_guard<std::mutex> lk(m);
                sInRoom = true;
                sRoom = roomId;
                sPid = pid;
                sIsHost = me;
                if (!sameRoom)
                {
                    lastInputTick.clear();
                    inInputs.clear();
                    hasSnap = false;
                }
            }
            if (!sameRoom)
            {
                seenMatchSeq = seenPhaseSeq = 0;
                Event e;
                e.type = EventType::JoinedRoom;
                e.roomId = roomId;
                e.localPlayerId = pid;
                e.isHost = me;
                e.roster = toRoster(r);
                emit(e);
            }
            applyRoster(r, true);
            // RepJoined can also repeat for the room we are already in; only (re)start
            // the game link when needed.
            if (!sameRoom || linkTr == Transport::None) startLink(firstTransport());
            break;
        }
        case RepJoinFailed:
        {
            Event e;
            e.type = EventType::JoinFailed;
            e.message = r.message;
            emit(e);
            break;
        }
        case RepLeft:
            if (roomId != 0 && (r.roomId == 0 || r.roomId == roomId)) leaveRoomLocal(true, r.message);
            break;
        case RepRoster:
            if (r.roomId == roomId && roomId != 0) applyRoster(r, false);
            break;
        case RepHeartbeatAck:
            break;
        case RepSessionExpired:
            if (pid != 0 || roomId != 0) resetSession(std::string("session expired: ") + r.message);
            break;
        case RepError:
        default:
            fprintf(stderr, "[net] API error: %s\n", r.message);
            break;
        }
    }

    // Drains the reply reader (loaned samples must be returned).
    void pollApi(uint64_t now)
    {
        void *samples[16] = {nullptr};
        dds_sample_info_t infos[16];
        int n = dds_take(apiRepR, samples, infos, 16, 16);
        for (int i = 0; i < n; i++)
            if (infos[i].valid_data) handleApiReply(*(drifto_ApiRep *)samples[i], now);
        if (n > 0) dds_return_loan(apiRepR, samples, n);
    }

    // ---------------- game link ----------------
    // Transport to try first for a new room: honours the user's preference and
    // the server advertising UDP port 0 (= UDP disabled -> straight to TCP).
    Transport firstTransport() const
    {
        if (cfg.transport == TransportPref::Tcp443Only) return Transport::Tcp443;
        if (gs.udpPort == 0) return cfg.transport == TransportPref::UdpOnly ? Transport::Udp : Transport::Tcp443;
        return Transport::Udp;
    }

    // Tears down the game participant (bounded: a TCP delete can hang in
    // connect()) and releases its domain. Resets the transport/RTT mirrors.
    void closeLink()
    {
        if (gPart > 0)
        {
            dds::deleteEntityBounded(gPart, 500); // readers/writers too (auto-detached from the waitset)
            gPart = 0;
        }
        gInW = gSnapW = gCtlW = gPingW = gInR = gSnapR = gCtlR = gPingR = 0;
        if (linkDomain != 0) dds::releaseDomain(linkDomain);
        linkDomain = 0;
        bool wasUp = linkUp;
        linkUp = false;
        linkTr = Transport::None;
        {
            std::lock_guard<std::mutex> lk(m);
            sTransport = Transport::None;
            sRtt = -1;
        }
        (void)wasUp;
    }

    // (Re)creates the game link over transport t: domain, participant, the eight
    // topics, writers and readers. Failures are only logged; the handshake
    // timeout in tickLink() then moves on to the next transport.
    void startLink(Transport t)
    {
        closeLink();
        if (roomId == 0) return;
        bool udp = (t == Transport::Udp);
        uint32_t dom = udp ? gs.udpDomain : gs.tcpDomain;
        int port = udp ? gs.udpPort : gs.tcpPort;
        std::string err;
        std::string xml = dds::loadConfig(cfg.configDir, udp ? "udp_client.xml" : "tcp_client.xml",
                                          {{"HOST", gs.host}, {"PORT", std::to_string(port)}},
                                          udp ? "client-game-udp" : "client-game-tcp");
        linkTr = t;
        linkStartUs = monoUs();
        lastPingUs = 0;
        if (port == 0 || !dds::acquireDomain(dom, xml, &err, /*persistent=*/true))
        {
            fprintf(stderr, "[net] game link %s: %s\n", udp ? "UDP" : "TCP", port == 0 ? "disabled on server" : err.c_str());
            return; // the handshake timeout moves us along
        }
        linkDomain = dom;
        gPart = dds::createParticipant(dom, sec, &err);
        if (gPart < 0)
        {
            fprintf(stderr, "[net] game participant: %s\n", err.c_str());
            gPart = 0;
            return;
        }
        dds_entity_t tInU = dds_create_topic(gPart, &drifto_InputUp_desc, kTopicInputUp, nullptr, nullptr);
        dds_entity_t tInD = dds_create_topic(gPart, &drifto_InputDn_desc, kTopicInputDn, nullptr, nullptr);
        dds_entity_t tSnU = dds_create_topic(gPart, &drifto_SnapUp_desc, kTopicSnapUp, nullptr, nullptr);
        dds_entity_t tSnD = dds_create_topic(gPart, &drifto_SnapDn_desc, kTopicSnapDn, nullptr, nullptr);
        dds_entity_t tCtU = dds_create_topic(gPart, &drifto_CtlUp_desc, kTopicCtlUp, nullptr, nullptr);
        dds_entity_t tCtD = dds_create_topic(gPart, &drifto_CtlDn_desc, kTopicCtlDn, nullptr, nullptr);
        dds_entity_t tPiU = dds_create_topic(gPart, &drifto_PingUp_desc, kTopicPingUp, nullptr, nullptr);
        dds_entity_t tPiD = dds_create_topic(gPart, &drifto_PingDn_desc, kTopicPingDn, nullptr, nullptr);

        // Up (writer) side: gameplay best-effort with depth 1 (only the newest
        // matters); match control reliable.
        dds_qos_t *be1 = dds::qosBestEffort(1);
        dds_qos_t *rel = dds::qosReliable(32, false);
        gInW = dds_create_writer(gPart, tInU, be1, nullptr);
        gSnapW = dds_create_writer(gPart, tSnU, be1, nullptr);
        gCtlW = dds_create_writer(gPart, tCtU, rel, nullptr);
        gPingW = dds_create_writer(gPart, tPiU, be1, nullptr);
        dds_delete_qos(be1);
        dds_delete_qos(rel);

        // Down (reader) side: r<room> carries snapshots + match control;
        // r<room>.p<pid> carries inputs routed to the host and our own ping answers.
        dds_entity_t sub = dds::createSubscriber(gPart, {roomPartition(roomId), playerPartition(roomId, pid)});
        dds_qos_t *in = dds::qosBestEffort(32);
        dds_qos_t *sn = dds::qosBestEffort(2);
        dds_qos_t *ct = dds::qosReliable(8, true);
        dds_qos_t *pi = dds::qosBestEffort(8);
        gInR = dds_create_reader(sub, tInD, in, nullptr);
        gSnapR = dds_create_reader(sub, tSnD, sn, nullptr);
        gCtlR = dds_create_reader(sub, tCtD, ct, nullptr);
        gPingR = dds_create_reader(sub, tPiD, pi, nullptr);
        dds_delete_qos(in);
        dds_delete_qos(sn);
        dds_delete_qos(ct);
        dds_delete_qos(pi);
        // wake the worker on new data for every game reader
        for (dds_entity_t r : {gInR, gSnapR, gCtlR, gPingR})
        {
            if (r < 0) { fprintf(stderr, "[net] game reader creation failed: %s\n", dds_strretcode(r)); continue; }
            dds_set_status_mask(r, DDS_DATA_AVAILABLE_STATUS);
            dds_waitset_attach(waitset, r, r);
        }
    }

    // Ping doubles as handshake probe (first answer = link up), heartbeat
    // (liveness) and RTT measurement (clientTimeUs is echoed back).
    void sendPing(uint64_t now)
    {
        if (gPingW <= 0) return;
        drifto_PingUp p{};
        p.playerId = pid;
        p.roomId = roomId;
        p.token = gs.token;
        p.seq = ++pingSeq;
        p.clientTimeUs = now;
        dds_write(gPingW, &p);
        lastPingUs = now;
    }

    // First ping answer on this link: publish the transport and tell the game.
    void setLinkUp(uint64_t now)
    {
        linkUp = true;
        lastPongUs = now;
        {
            std::lock_guard<std::mutex> lk(m);
            sTransport = linkTr;
        }
        Event e;
        e.type = EventType::TransportChanged;
        e.transport = linkTr;
        emit(e);
    }

    // Link declared dead: report TransportChanged(None) and, in Auto mode, try the
    // other transport (UDP -> TCP, TCP -> UDP; UDP is skipped if disabled).
    void linkLost(const char *why)
    {
        Transport was = linkTr;
        fprintf(stderr, "[net] game link (%s) lost: %s\n", was == Transport::Udp ? "UDP" : "TCP", why);
        Event e;
        e.type = EventType::TransportChanged;
        e.transport = Transport::None;
        e.message = why;
        emit(e);
        Transport next = was;
        if (cfg.transport == TransportPref::Auto) next = (was == Transport::Udp) ? Transport::Tcp443 : Transport::Udp;
        if (next == Transport::Udp && gs.udpPort == 0) next = Transport::Tcp443;
        startLink(next);
    }

    // Game-link state machine, run every loop iteration.
    //   handshaking: ping every kPingHandshakeUs; no answer within the
    //     per-transport limit -> switch transport (Auto) or retry (fixed).
    //   up: ping every kPingUpUs; no answer for kLinkLossUs -> linkLost().
    void tickLink(uint64_t now)
    {
        if (roomId == 0 || linkTr == Transport::None) return;
        if (!linkUp)
        {
            if (since(now, lastPingUs) >= kPingHandshakeUs) sendPing(now);
            uint64_t limit = (linkTr == Transport::Udp) ? kUdpHandshakeUs : kTcpHandshakeUs;
            if (since(now, linkStartUs) > limit)
            {
                if (cfg.transport == TransportPref::Auto)
                {
                    Transport next = (linkTr == Transport::Udp) ? Transport::Tcp443 : Transport::Udp;
                    if (next == Transport::Udp && gs.udpPort == 0) next = Transport::Tcp443;
                    fprintf(stderr, "[net] no game-server ack over %s after %.1fs, trying %s\n",
                            linkTr == Transport::Udp ? "UDP" : "TCP", since(now, linkStartUs) / 1e6,
                            next == Transport::Udp ? "UDP" : "TCP");
                    startLink(next);
                }
                else
                    linkStartUs = now; // fixed transport: keep trying
            }
            return;
        }
        if (since(now, lastPingUs) >= kPingUpUs) sendPing(now);
        if (since(now, lastPongUs) > kLinkLossUs) linkLost("no heartbeat ack from game server");
    }

    // Drops all room state on the worker and in the mirrors (no API traffic).
    // emitEvent=false is used by resetSession(), which reports Disconnected instead.
    void leaveRoomLocal(bool emitEvent, const std::string &msg = "")
    {
        if (roomId == 0) return;
        closeLink();
        uint32_t old = roomId;
        roomId = 0;
        hostId = 0;
        gs = GsInfo{};
        pendingCtl.clear();
        inputHistory.clear();
        {
            std::lock_guard<std::mutex> lk(m);
            sInRoom = false;
            sIsHost = false;
            sRoom = 0;
            sRoster.clear();
            hasSnap = false;
            inInputs.clear();
            lastInputTick.clear();
        }
        if (emitEvent)
        {
            Event e;
            e.type = EventType::LeftRoom;
            e.roomId = old;
            e.message = msg;
            emit(e);
        }
    }

    // Drains the four game readers: pongs (link state, smoothed RTT), match
    // control, snapshots and relayed inputs. Samples are loaned; each take is
    // paired with dds_return_loan.
    void pollGame(uint64_t now)
    {
        void *samples[32] = {nullptr};
        dds_sample_info_t infos[32];
        int n;
        // pongs: ignore answers for other players/rooms (the reader is shared per partition)
        // pongs
        if (gPingR > 0 && (n = dds_take(gPingR, samples, infos, 32, 32)) > 0)
        {
            for (int i = 0; i < n; i++)
            {
                if (!infos[i].valid_data) continue;
                const drifto_PingDn &p = *(drifto_PingDn *)samples[i];
                if (p.playerId != pid || p.roomId != roomId) continue;
                if (!linkUp) setLinkUp(now);
                lastPongUs = now;
                // exponential moving average (alpha 0.2) smooths RTT jitter
                float rtt = (float)(now - p.clientTimeUs) / 1000.f;
                std::lock_guard<std::mutex> lk(m);
                sRtt = sRtt < 0 ? rtt : sRtt * 0.8f + rtt * 0.2f;
            }
            dds_return_loan(gPingR, samples, n);
        }
        // match control: the *Seq counters make delivery idempotent; transient-local
        // history and transport switches may repeat the same state.
        // match control
        if (gCtlR > 0 && (n = dds_take(gCtlR, samples, infos, 32, 32)) > 0)
        {
            for (int i = 0; i < n; i++)
            {
                if (!infos[i].valid_data) continue;
                const drifto_CtlDn &c = *(drifto_CtlDn *)samples[i];
                if (c.roomId != roomId) continue;
                if (c.matchSeq > seenMatchSeq)
                {
                    seenMatchSeq = c.matchSeq;
                    {
                        std::lock_guard<std::mutex> lk(m);
                        lastInputTick.clear(); // tick counters may restart with the match
                    }
                    Event e;
                    e.type = EventType::MatchStarted;
                    e.trackSeed = c.trackSeed;
                    e.startTick = c.startTick;
                    e.roomId = roomId;
                    emit(e);
                }
                if (c.phaseSeq > seenPhaseSeq)
                {
                    seenPhaseSeq = c.phaseSeq;
                    Event e;
                    e.type = EventType::PhaseChanged;
                    e.phase = (MatchPhase)c.phase;
                    e.roomId = roomId;
                    emit(e);
                }
            }
            dds_return_loan(gCtlR, samples, n);
        }
        // snapshots: only the newest one in the batch is kept (older ones are stale)
        // snapshots (non-hosts; the host just drains its own echo)
        if (gSnapR > 0 && (n = dds_take(gSnapR, samples, infos, 32, 32)) > 0)
        {
            const drifto_SnapDn *best = nullptr;
            for (int i = 0; i < n; i++)
                if (infos[i].valid_data && ((drifto_SnapDn *)samples[i])->roomId == roomId) best = (drifto_SnapDn *)samples[i];
            if (best)
            {
                WorldSnapshot ws;
                ws.hostTick = best->hostTick;
                ws.phase = (MatchPhase)best->phase;
                ws.phaseTimer = best->phaseTimer;
                ws.eliminationTimer = best->eliminationTimer;
                ws.eliminationStage = best->eliminationStage;
                ws.lastPlacePlayerId = best->lastPlacePlayerId;
                ws.winnerPlayerId = best->winnerPlayerId;
                ws.cars.reserve(best->cars._length);
                for (uint32_t k = 0; k < best->cars._length; k++) ws.cars.push_back(unpackCar(best->cars._buffer[k]));
                std::lock_guard<std::mutex> lk(m);
                if (!sIsHost)
                {
                    snap = std::move(ws);
                    hasSnap = true;
                    snapHostTimeUs = best->hostTimeUs;
                    snapRecvUs = now;
                }
            }
            dds_return_loan(gSnapR, samples, n);
        }
        // relayed inputs: the game server only routes these to the current host;
        // acceptInputLocked() de-duplicates the redundant copies.
        // relayed inputs (only routed to us while we're host)
        if (gInR > 0 && (n = dds_take(gInR, samples, infos, 32, 32)) > 0)
        {
            std::lock_guard<std::mutex> lk(m);
            for (int i = 0; i < n; i++)
            {
                if (!infos[i].valid_data || !sIsHost) continue;
                const drifto_InputDn &d = *(drifto_InputDn *)samples[i];
                if (d.roomId != roomId) continue;
                for (uint32_t k = 0; k < d.inputs._length; k++) acceptInputLocked(d.playerId, unpackInput(d.inputs._buffer[k]));
            }
            dds_return_loan(gInR, samples, n);
        }
    }

    // ---------------- commands ----------------
    // Executes one queued public call on the worker thread. API-style commands
    // become requests; Start/Phase become reliable CtlUp messages that wait in
    // pendingCtl until the link is up (see flushGame).
    void runCmd(const Cmd &c)
    {
        switch (c.kind)
        {
        case Cmd::Login: queueApi(OpLogin, c.a, c.b); break;
        case Cmd::RoomList: queueApi(OpRoomList); break;
        case Cmd::Create: queueApi(OpCreateRoom, "", "", c.a); break;
        case Cmd::Join: queueApi(OpJoinRoom, "", "", "", c.u1); break;
        case Cmd::Leave:
            queueApi(OpLeaveRoom, "", "", "", roomId);
            leaveRoomLocal(true);
            break;
        case Cmd::Ready: queueApi(OpSetReady, "", "", "", roomId, c.flag); break;
        case Cmd::Start:
        case Cmd::Phase:
        {
            if (roomId == 0) break;
            drifto_CtlUp u{};
            u.playerId = pid;
            u.roomId = roomId;
            u.token = gs.token;
            // reqSeq lets the game server drop duplicates of this reliable request
            u.reqSeq = ++ctlSeq;
            u.op = c.kind == Cmd::Start ? CtlStart : CtlPhase;
            u.trackSeed = c.u1;
            u.startTick = c.u2;
            u.phase = (uint8_t)c.phase;
            pendingCtl.push_back(u);
            break;
        }
        }
    }

    // Sends everything the caller threads queued: inputs (with redundancy), the
    // host's latest snapshot and pending match-control messages.
    void flushGame(uint64_t now)
    {
        std::vector<CarInput> ins;
        bool doSnap = false;
        WorldSnapshot ws;
        uint64_t snapT = 0;
        {
            std::lock_guard<std::mutex> lk(m);
            ins.swap(outInputs);
            if (hasOutSnap)
            {
                doSnap = true;
                ws = std::move(outSnap);
                snapT = outSnapTimeUs;
                hasOutSnap = false;
            }
        }
        if (roomId == 0 || gPart <= 0) return;
        if (!ins.empty() && gInW > 0)
        {
            for (const CarInput &ci : ins)
            {
                drifto_InputW w;
                packInput(ci, w);
                // tick went backwards: the game restarted its counter, so the
                // redundant copies of old ticks would only confuse the host
                if (!inputHistory.empty() && w.tick <= inputHistory.back().tick) inputHistory.clear();
                inputHistory.push_back(w);
            }
            // each packet carries the newest kInputRedundancy inputs (oldest first), so a
            // lost packet is recovered from the next one
            while (inputHistory.size() > (size_t)kInputRedundancy) inputHistory.pop_front();
            drifto_InputW buf[kInputRedundancy];
            uint32_t k = 0;
            for (const auto &w : inputHistory) buf[k++] = w;
            drifto_InputUp u{};
            u.playerId = pid;
            u.roomId = roomId;
            u.token = gs.token;
            u.inputs._buffer = buf;
            u.inputs._length = u.inputs._maximum = k;
            u.inputs._release = false;
            dds_write(gInW, &u);
            sentInputs++;
        }
        if (doSnap && gSnapW > 0)
        {
            std::vector<drifto_CarW> cars(ws.cars.size());
            for (size_t i = 0; i < ws.cars.size(); i++) packCar(ws.cars[i], cars[i]);
            drifto_SnapUp s{};
            s.roomId = roomId;
            s.playerId = pid;
            s.token = gs.token;
            s.hostTick = ws.hostTick;
            s.hostTimeUs = snapT;
            s.phase = (uint8_t)ws.phase;
            s.phaseTimer = ws.phaseTimer;
            s.eliminationTimer = ws.eliminationTimer;
            s.eliminationStage = ws.eliminationStage;
            s.lastPlacePlayerId = ws.lastPlacePlayerId;
            s.winnerPlayerId = ws.winnerPlayerId;
            s.cars._buffer = cars.data();
            s.cars._length = s.cars._maximum = (uint32_t)cars.size();
            s.cars._release = false;
            dds_write(gSnapW, &s);
        }
        // reliable match control: only once the link is up and matched
        if (!pendingCtl.empty() && linkUp && gCtlW > 0 && dds::matchedReaders(gCtlW) > 0)
        {
            while (!pendingCtl.empty())
            {
                dds_write(gCtlW, &pendingCtl.front());
                pendingCtl.pop_front();
            }
        }
        (void)now;
    }

    // ---------------- worker ----------------
    // Worker loop. Wakes on the guard condition (a public call queued work), a
    // DATA_AVAILABLE reader, or the timeout (short while something is pending).
    void run() override
    {
        while (!quit.load())
        {
            uint64_t now = monoUs();
            // timeout: short while handshaking/when things are pending, else a heartbeat-ish tick
            dds_duration_t timeout = DDS_MSECS(50);
            if (!apiPending.empty() || (roomId != 0 && !linkUp) || !pendingCtl.empty()) timeout = DDS_MSECS(10);
            waitForWork(timeout);
            now = monoUs();
            for (const Cmd &c : takeCmds()) runCmd(c);

            pollApi(now);
            pollGame(now);
            flushApi(now);
            flushGame(now);
            tickLink(now);

            if (since(now, lastTuneUs) >= 250000)
            {
                dds::tuneTcpSockets(); // TCP_NODELAY (see net_common.h)
                lastTuneUs = now;
            }

            // API heartbeat (1 Hz) and loss detection: no reply for kApiTimeoutUs ->
            // report Disconnected once; keeps retrying in the background.
            // API heartbeat / loss detection
            if (pid != 0)
            {
                if (since(now, lastHeartbeatUs) >= kApiHeartbeatUs)
                {
                    queueApi(OpHeartbeat);
                    lastHeartbeatUs = now;
                }
                if (!apiLostReported && since(now, lastApiReplyUs) > kApiTimeoutUs)
                {
                    apiLostReported = true;
                    Event e;
                    e.type = EventType::Disconnected;
                    e.message = "lost connection to API server (retrying)";
                    emit(e);
                }
            }
        }
    }

    // Called from NetClient::start() before the worker thread exists.
    bool open(std::string *err) override
    {
        session = randomU64();
        if (!openApi(err))
        {
            if (apiPart > 0) dds_delete(apiPart);
            apiPart = 0;
            if (apiDomain) dds::releaseDomain(kApiDomain);
            apiDomain = false;
            return false;
        }
        lastApiReplyUs = monoUs();
        return true;
    }

    // Called from NetClient::stop() after the worker has been joined.
    void close() override
    {
        // best effort: tell the API server we're gone so rosters update at once
        // (otherwise the session times out after a few seconds)
        if (pid != 0 && apiReqW > 0 && dds::matchedReaders(apiReqW) > 0)
        {
            drifto_ApiReq r{};
            r.session = session;
            r.seq = ++reqSeq;
            r.op = OpLogout;
            dds_write(apiReqW, &r);
            dds_wait_for_acks(apiReqW, DDS_MSECS(200));
        }
        closeLink();
        if (apiPart > 0) dds::deleteEntityBounded(apiPart, 1000);
        apiPart = 0;
        if (apiDomain) dds::releaseDomain(kApiDomain);
        apiDomain = false;
    }
};

} // namespace

namespace detail {

std::unique_ptr<ClientCore> makeOnlineCore() { return std::unique_ptr<ClientCore>(new OnlineCore); }

// Names are 1..kMaxNameLen chars of [A-Za-z0-9_-] (also enforced server-side).
bool validPlayerName(const std::string &n)
{
    if (n.empty() || n.size() > (size_t)kMaxNameLen) return false;
    for (char c : n)
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
    return true;
}

} // namespace detail

// ============================================================================
// Impl holds the active backend. Before start() it is an inert ClientCore so
// the public methods are safe to call (they just queue or read defaults).
// NetClient: dispatches to the active backend.
struct NetClient::Impl
{
    std::unique_ptr<ClientCore> c{new ClientCore}; // inert until start()
};

NetClient::NetClient() : impl_(new Impl) {}
NetClient::~NetClient() { stop(); }

// Picks the backend, creates the waitset + guard condition, opens the backend
// (DDS entities) and starts the worker. Returns false on failure with the
// reason on stderr. Calls queued before start() are carried over.
bool NetClient::start(const Config &config)
{
    if (impl_->c->started) return true;
    std::unique_ptr<ClientCore> core = config.mode == Mode::Lan ? makeLanCore() : makeOnlineCore();
    {
        // calls made before start() (e.g. login) are kept
        std::lock_guard<std::mutex> lk(impl_->c->m);
        core->cmds.swap(impl_->c->cmds);
    }
    ClientCore &I = *core;
    I.cfg = config;
    const char *ins = getenv("DRIFTO_INSECURE");
    // security is on unless Config::insecure or DRIFTO_INSECURE=1 (never an
    // automatic downgrade; createParticipant fails if certs are missing)
    I.sec.enabled = !(config.insecure || (ins && *ins && strcmp(ins, "0") != 0));
    // relative paths: the cwd, else next to the executable (Windows double-click)
    I.cfg.certDir = platform::resolveRuntimePath(config.certDir);
    I.cfg.configDir = platform::resolveRuntimePath(config.configDir);
    I.sec.certDir = I.cfg.certDir;
    I.sec.identity = "client" + std::to_string(config.certIndex);
    I.waitset = dds_create_waitset(DDS_CYCLONEDDS_HANDLE);
    I.guard = dds_create_guardcondition(DDS_CYCLONEDDS_HANDLE);
    if (I.waitset < 0 || I.guard < 0)
    {
        fprintf(stderr, "[net] failed to create waitset/guard condition\n");
        return false;
    }
    dds_waitset_attach(I.waitset, I.guard, 0);
    std::string err;
    if (!I.open(&err))
    {
        fprintf(stderr, "[net] NetClient::start: %s\n", err.c_str());
        dds_delete(I.guard);
        dds_delete(I.waitset);
        return false;
    }
    I.quit = false;
    I.started = true;
    impl_->c = std::move(core);
    ClientCore *raw = impl_->c.get();
    raw->thr = std::thread([raw] { raw->run(); });
    return true;
}

// Joins the worker, lets the backend say goodbye (Logout), then frees the
// waitset. Idempotent. Skips deleting DDS objects if Cyclone teardown wedged.
void NetClient::stop()
{
    ClientCore &I = *impl_->c;
    if (!I.started) return;
    I.quit = true;
    I.wake();
    if (I.thr.joinable()) I.thr.join();
    I.close();
    if (!dds::wedged()) // (else Cyclone may hold locks: leave it to process exit)
    {
        dds_delete(I.guard);
        dds_delete(I.waitset);
    }
    I.guard = I.waitset = 0;
    I.started = false;
    std::lock_guard<std::mutex> lk(I.m);
    I.sLoggedIn = I.sInRoom = I.sIsHost = false;
    I.sTransport = Transport::None;
}

void NetClient::login(const std::string &name, const std::string &password)
{
    Cmd c{Cmd::Login};
    c.a = name;
    c.b = password;
    impl_->c->push(c);
}
void NetClient::requestRoomList() { impl_->c->push(Cmd{Cmd::RoomList}); }
void NetClient::createRoom(const std::string &roomName)
{
    Cmd c{Cmd::Create};
    c.a = roomName;
    impl_->c->push(c);
}
void NetClient::joinRoom(uint32_t roomId)
{
    Cmd c{Cmd::Join};
    c.u1 = roomId;
    impl_->c->push(c);
}
void NetClient::leaveRoom() { impl_->c->push(Cmd{Cmd::Leave}); }
void NetClient::setReady(bool ready)
{
    Cmd c{Cmd::Ready};
    c.flag = ready;
    impl_->c->push(c);
}

std::vector<Event> NetClient::pollEvents()
{
    std::vector<Event> out;
    std::lock_guard<std::mutex> lk(impl_->c->m);
    out.swap(impl_->c->events);
    return out;
}

bool NetClient::loggedIn() const { std::lock_guard<std::mutex> lk(impl_->c->m); return impl_->c->sLoggedIn; }
bool NetClient::inRoom() const { std::lock_guard<std::mutex> lk(impl_->c->m); return impl_->c->sInRoom; }
bool NetClient::isHost() const { std::lock_guard<std::mutex> lk(impl_->c->m); return impl_->c->sIsHost; }
int32_t NetClient::localPlayerId() const { std::lock_guard<std::mutex> lk(impl_->c->m); return impl_->c->sPid; }
std::vector<RosterEntry> NetClient::roster() const { std::lock_guard<std::mutex> lk(impl_->c->m); return impl_->c->sRoster; }
Transport NetClient::transport() const { std::lock_guard<std::mutex> lk(impl_->c->m); return impl_->c->sTransport; }
float NetClient::rttMs() const { std::lock_guard<std::mutex> lk(impl_->c->m); return impl_->c->sRtt; }

// Host: input goes straight into its own de-dup queue (no network).
// Guest: queued for the worker, which packs it with redundant history.
void NetClient::sendInput(const CarInput &input)
{
    ClientCore &I = *impl_->c;
    {
        std::lock_guard<std::mutex> lk(I.m);
        if (!I.sInRoom) return;
        if (I.sIsHost)
        {
            I.acceptInputLocked(I.sPid, input); // host's own input never leaves the process
            return;
        }
        I.outInputs.push_back(input);
    }
    I.wake();
}

// Latest-wins mailbox: the worker overwrites older snapshots.
bool NetClient::takeLatestSnapshot(WorldSnapshot &out)
{
    std::lock_guard<std::mutex> lk(impl_->c->m);
    if (!impl_->c->hasSnap) return false;
    out = std::move(impl_->c->snap);
    impl_->c->hasSnap = false;
    return true;
}

// Host: hands over all de-duplicated inputs received since the last call.
std::vector<NetClient::PlayerInput> NetClient::takeInputs()
{
    std::vector<PlayerInput> out;
    std::lock_guard<std::mutex> lk(impl_->c->m);
    out.swap(impl_->c->inInputs);
    return out;
}

// Host only: stores the snapshot (latest wins) for the worker to send.
void NetClient::publishSnapshot(const WorldSnapshot &snapshot)
{
    ClientCore &I = *impl_->c;
    {
        std::lock_guard<std::mutex> lk(I.m);
        if (!I.sInRoom || !I.sIsHost) return;
        I.outSnap = snapshot;
        I.outSnapTimeUs = wire::monoUs();
        I.hasOutSnap = true;
    }
    I.wake();
}

void NetClient::startMatch(uint32_t trackSeed, uint32_t startTick)
{
    Cmd c{Cmd::Start};
    c.u1 = trackSeed;
    c.u2 = startTick;
    impl_->c->push(c);
}

void NetClient::setPhase(MatchPhase phase)
{
    Cmd c{Cmd::Phase};
    c.phase = phase;
    impl_->c->push(c);
}

} // namespace net
