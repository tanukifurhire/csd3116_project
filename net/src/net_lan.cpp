// net_lan.cpp -- NetClient's LAN backend (Config::mode == Mode::Lan).
//
// No servers: every NetClient joins DDS domain 10 (UDP, multicast discovery,
// optional unicast peers). The room's host client does what api_server +
// game_server do in Online mode:
//   * announces the room (Drifto_Lan_Room, 1 Hz heartbeat, transient-local)
//   * admits joiners (Drifto_Lan_Req -> Drifto_Lan_JoinRep): cap 4, unique
//     names, player ids, session tokens
//   * publishes the roster (Drifto_Lan_Roster, reliable, transient-local)
//     and match control (Drifto_Lan_Ctl, same)
//   * answers pings (host liveness + RTT) and consumes the inputs
// Everyone else sends inputs/pings/requests to the room partition and reads
// snapshots, roster and match control from it. If the host goes silent for
// 3 s every member drops it and deterministically promotes the lowest
// remaining player id, which re-announces the same room and carries on.
// Public behaviour/Events are the same as Online (see net_client.h).
//
// Threading / locking: like OnlineCore, a single worker thread (run()) owns
// every DDS entity and all LanCore fields; caller threads only use the
// ClientCore queues and mirrors guarded by ClientCore::m. The worker locks m
// only for short copies into/out of those shared fields.
//
// Design decisions:
//   * Room-scoped traffic lives in partitions "lan.r<room>" (and
//     "lan.r<room>.s<session>" for a join reply addressed to one joiner), so a
//     member only receives its own room; discovery uses the shared "lan"
//     partition.
//   * Gameplay (inputs, snapshots, pings) is BEST_EFFORT with the last
//     kInputRedundancy inputs in each packet; control-plane topics (room
//     announcement, requests, join reply, roster, match control) are RELIABLE,
//     and the state-like ones TRANSIENT_LOCAL so late joiners and a newly
//     promoted host see current state.
//   * Authentication without a server: the host hands each player a random
//     64-bit token at admission; the roster carries only the first 64 bits of
//     SHA-256(token). Every member can therefore validate inputs/pings/
//     snapshots (including a future host), without ever seeing others' tokens.
//   * Host migration is deterministic: the lowest remaining player id takes
//     over with epoch+1; rosters/announcements with an older epoch are ignored.
//   * Join state machine: None -> Joining (request retried until JoinRep and a
//     roster containing us arrive, 5 s timeout) -> InRoom -> None.
#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "net_client_core.h"
#include "../../common/platform.h"

namespace net {
namespace detail {
namespace {

using namespace wire;

// ---- timing (microseconds) ----
constexpr uint64_t kAnnounceUs = 1 * kSec;       // LanRoom heartbeat
constexpr uint64_t kRoomExpiryUs = 3 * kSec;     // room list drops silent rooms
constexpr uint64_t kPingUs = 250000;             // client -> host
constexpr uint64_t kHostLossUs = 3 * kSec;       // no sign of the host -> migrate
constexpr uint64_t kMemberTimeoutUs = 4 * kSec;  // host drops silent members
constexpr uint64_t kJoinRetryUs = 500000;
constexpr uint64_t kJoinTimeoutUs = 5 * kSec;

// Operations carried by Drifto_Lan_Req (member -> host).
enum LanOp : uint8_t { LanJoin = 1, LanReady = 2, LanLeave = 3 };

// First 8 bytes of SHA-256(token): the value published in the roster so that
// tokens can be verified by everyone but not recovered from it.
uint64_t tokenHash(uint64_t token)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(&token, sizeof token, md, &len, EVP_sha256(), nullptr);
    uint64_t h;
    memcpy(&h, md, sizeof h);
    return h;
}

// 16-digit hex of a session id (used in partition names).
std::string hex64(uint64_t v)
{
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

// One room member as known locally. token/lastSeenUs are only meaningful on
// the host; guests know just the hash (from the roster).
struct Member
{
    int32_t pid = 0;
    std::string name;
    bool ready = false;
    uint64_t session = 0;
    uint64_t tokenHash = 0;
    uint64_t token = 0;      // host only (from admission; 0 after migration)
    uint64_t lastSeenUs = 0; // host only
};

// A roster sample copied out of the DDS loan (kept while a join is pending).
struct RosterCopy
{
    uint32_t roomId = 0, epoch = 0;
    int32_t hostId = 0, nextPid = 0;
    uint64_t hostSession = 0;
    std::string roomName;
    std::vector<Member> members;
};

// A room announcement plus when we last heard it (for expiry).
struct RoomSeen
{
    drifto_LanRoom r;
    uint64_t lastSeenUs = 0;
};

// LAN backend. All methods run on the worker thread except open()/close(),
// which run on the caller's thread before the worker starts / after it joined.
struct LanCore : ClientCore
{
    uint64_t session = 0;
    bool domain = false;
    dds_entity_t part = 0;
    dds_entity_t tRoom = 0, tReq = 0, tRep = 0, tRoster = 0, tCtl = 0, tIn = 0, tSnap = 0, tPing = 0, tPong = 0;
    dds_entity_t roomW = 0, roomR = 0;
    // roomKey is a random 64-bit id; its low 32 bits are the public roomId.
    std::map<uint64_t, RoomSeen> rooms; // room list, by roomKey
    std::string name;
    bool loggedIn = false;

    // state: None = not in a room; Joining = request sent, waiting for JoinRep +
    // roster; InRoom = admitted (host or guest).
    // ---- current room ----
    enum State { None, Joining, InRoom } state = None;
    uint32_t roomId = 0;
    uint64_t roomKey = 0;
    std::string roomName;
    dds_entity_t pub = 0, sub = 0;
    dds_entity_t reqW = 0, rosterW = 0, ctlW = 0, inW = 0, snapW = 0, pingW = 0, pongW = 0;
    dds_entity_t reqR = 0, repR = 0, rosterR = 0, ctlR = 0, snapR = 0, pingR = 0, pongR = 0, inR = 0;
    // host only: one publisher/writer per joiner, in that joiner's private
    // partition, so a JoinRep reaches only the requester
    std::map<uint64_t, std::pair<dds_entity_t, dds_entity_t>> repPubs; // joiner session -> (publisher, writer)
    int32_t pid = 0, hostId = 0, nextPid = 1;
    uint64_t token = 0, hostSession = 0;
    uint32_t epoch = 0;
    bool isHost = false;
    bool announcedJoin = false; // JoinedRoom emitted
    std::vector<Member> members;
    std::set<int32_t> dead;
    uint32_t reqSeq = 0, joinSeq = 0;
    uint64_t joinStartUs = 0, lastJoinSendUs = 0;
    uint64_t lastHostAliveUs = 0, lastPingUs = 0, lastAnnounceUs = 0;
    uint32_t pingSeq = 0;
    bool haveCtl = false;
    drifto_LanCtl ctl{};
    uint32_t seenMatchSeq = 0, seenPhaseSeq = 0;
    std::deque<drifto_InputW> inputHistory;
    std::map<uint64_t, uint32_t> lastReqSeq; // host: dedup per session
    bool havePendingRoster = false;
    RosterCopy pendingRoster; // roster seen while our JoinRep was still in flight

    // =================================================================== setup
    // Creates the LAN participant in the shared domain and the discovery pair
    // (room announcements writer/reader). Room entities are created later.
    // Interface choice: DRIFTO_DDS_INTERFACE wins, else the default-route adapter
    // (avoids VPN/virtual adapters breaking multicast). DRIFTO_LAN_MULTICAST=0
    // disables multicast discovery (use lanPeers).
    bool open(std::string *err) override
    {
        session = randomU64();
        const char *mc = getenv("DRIFTO_LAN_MULTICAST");
        bool multicast = !(mc && strcmp(mc, "0") == 0);
        std::string peers;
        for (const std::string &p : cfg.lanPeers)
        {
            if (p.empty()) continue;
            peers += "<Peer address=\"" + p + "\"/>";
        }
        if (!peers.empty()) peers = "<Peers>" + peers + "</Peers>";
        // Pin the adapter facing the router unless DRIFTO_DDS_INTERFACE says otherwise.
        std::string autoIf;
        const char *pinned = getenv("DRIFTO_DDS_INTERFACE");
        if (!(pinned && *pinned))
        {
            autoIf = dds::defaultRouteIPv4();
            if (!autoIf.empty())
                fprintf(stderr, "[net] LAN: using interface %s (default route; override with DRIFTO_DDS_INTERFACE)\n",
                        autoIf.c_str());
        }
        std::string xml = dds::loadConfig(cfg.configDir, "lan.xml",
                                          {{"MULTICAST", multicast ? "default" : "false"}, {"PEERS", peers}}, "lan",
                                          autoIf);
        if (!dds::acquireDomain(kLanDomain, xml, err, /*persistent=*/true)) return false;
        domain = true;
        part = dds::createParticipant(kLanDomain, sec, err);
        if (part < 0)
        {
            part = 0;
            closeAll();
            return false;
        }
        tRoom = dds_create_topic(part, &drifto_LanRoom_desc, kTopicLanRoom, nullptr, nullptr);
        tReq = dds_create_topic(part, &drifto_LanReq_desc, kTopicLanReq, nullptr, nullptr);
        tRep = dds_create_topic(part, &drifto_LanJoinRep_desc, kTopicLanJoinRep, nullptr, nullptr);
        tRoster = dds_create_topic(part, &drifto_LanRoster_desc, kTopicLanRoster, nullptr, nullptr);
        tCtl = dds_create_topic(part, &drifto_LanCtl_desc, kTopicLanCtl, nullptr, nullptr);
        tIn = dds_create_topic(part, &drifto_InputUp_desc, kTopicLanInput, nullptr, nullptr);
        tSnap = dds_create_topic(part, &drifto_SnapUp_desc, kTopicLanSnap, nullptr, nullptr);
        tPing = dds_create_topic(part, &drifto_PingUp_desc, kTopicLanPing, nullptr, nullptr);
        tPong = dds_create_topic(part, &drifto_PingDn_desc, kTopicLanPong, nullptr, nullptr);
        dds_entity_t gpub = dds::createPublisher(part, "lan");
        dds_entity_t gsub = dds::createSubscriber(part, {"lan"});
        dds_qos_t *q = dds::qosReliable(1, true);
        roomW = dds_create_writer(gpub, tRoom, q, nullptr);
        dds_delete_qos(q);
        q = dds::qosReliable(4, true);
        roomR = dds_create_reader(gsub, tRoom, q, nullptr);
        dds_delete_qos(q);
        for (dds_entity_t e : {tRoom, tReq, tRep, tRoster, tCtl, tIn, tSnap, tPing, tPong, roomW, roomR})
            if (e < 0)
            {
                *err = std::string("LAN topic/reader/writer creation failed: ") + dds_strretcode(e) +
                       (sec.enabled ? " (permissions.xml needs the Drifto_Lan_* grant for domain 10; re-run make certs)" : "");
                closeAll();
                return false;
            }
        attach(roomR);
        return true;
    }

    // Wakes the worker when this reader has data.
    void attach(dds_entity_t r)
    {
        dds_set_status_mask(r, DDS_DATA_AVAILABLE_STATUS);
        dds_waitset_attach(waitset, r, r);
    }

    // Deletes the participant (bounded wait) and releases the domain reference.
    void closeAll()
    {
        if (part > 0) dds::deleteEntityBounded(part, 1000);
        part = 0;
        if (domain) dds::releaseDomain(kLanDomain);
        domain = false;
    }

    // After the worker joined: leave the current room politely (hand-over or
    // leave request), then tear down.
    void close() override
    {
        if (state != None) leaveRoom(false);
        closeAll();
    }

    // ============================================================ room entities
    // Creates the per-room publisher/subscriber and all writers/readers. Both
    // host and guests create them all; the input reader is host-only
    // (openInputReader). The subscriber also listens on our private
    // "lan.r<room>.s<session>" partition for the JoinRep.
    void openRoomEntities()
    {
        std::string rp = "lan.r" + std::to_string(roomId);
        pub = dds::createPublisher(part, rp);
        sub = dds::createSubscriber(part, {rp, rp + ".s" + hex64(session)});
        // reliable requests (retried join); transient-local roster/control/reply
        // (state, replayed to late readers); best-effort gameplay (newest wins)
        dds_qos_t *rel = dds::qosReliable(32, false);
        dds_qos_t *tl = dds::qosReliable(1, true);
        dds_qos_t *be1 = dds::qosBestEffort(1);
        reqW = dds_create_writer(pub, tReq, rel, nullptr);
        rosterW = dds_create_writer(pub, tRoster, tl, nullptr);
        ctlW = dds_create_writer(pub, tCtl, tl, nullptr);
        inW = dds_create_writer(pub, tIn, be1, nullptr);
        snapW = dds_create_writer(pub, tSnap, be1, nullptr);
        pingW = dds_create_writer(pub, tPing, be1, nullptr);
        pongW = dds_create_writer(pub, tPong, be1, nullptr);
        dds_delete_qos(be1);

        dds_qos_t *rr = dds::qosReliable(64, false);
        dds_qos_t *rep = dds::qosReliable(4, true);
        dds_qos_t *ros = dds::qosReliable(4, true);
        dds_qos_t *sn = dds::qosBestEffort(2);
        dds_qos_t *pi = dds::qosBestEffort(16);
        reqR = dds_create_reader(sub, tReq, rr, nullptr);
        repR = dds_create_reader(sub, tRep, rep, nullptr);
        rosterR = dds_create_reader(sub, tRoster, ros, nullptr);
        ctlR = dds_create_reader(sub, tCtl, ros, nullptr);
        snapR = dds_create_reader(sub, tSnap, sn, nullptr);
        pingR = dds_create_reader(sub, tPing, pi, nullptr);
        pongR = dds_create_reader(sub, tPong, pi, nullptr);
        for (dds_qos_t *x : {rel, tl, rr, rep, ros, sn, pi}) dds_delete_qos(x);
        for (dds_entity_t r : {reqR, repR, rosterR, ctlR, snapR, pingR, pongR})
        {
            if (r < 0) fprintf(stderr, "[net] LAN reader creation failed: %s\n", dds_strretcode(r));
            else attach(r);
        }
    }

    // Only the host reads inputs, so guests never receive each other's input.
    void openInputReader()
    {
        if (inR > 0 || sub <= 0) return;
        dds_qos_t *q = dds::qosBestEffort(32);
        inR = dds_create_reader(sub, tIn, q, nullptr);
        dds_delete_qos(q);
        if (inR > 0) attach(inR);
    }

    // Deleting the publisher/subscriber also deletes their writers and readers.
    void closeRoomEntities()
    {
        for (auto &kv : repPubs) dds_delete(kv.second.first);
        repPubs.clear();
        if (pub > 0) dds_delete(pub);
        if (sub > 0) dds_delete(sub);
        pub = sub = 0;
        reqW = rosterW = ctlW = inW = snapW = pingW = pongW = 0;
        reqR = repR = rosterR = ctlR = snapR = pingR = pongR = inR = 0;
    }

    // Back to the idle (logged-in, no room) state, including the shared mirrors.
    void resetRoomState()
    {
        state = None;
        roomId = 0;
        roomKey = 0;
        roomName.clear();
        pid = hostId = 0;
        nextPid = 1;
        token = hostSession = 0;
        epoch = 0;
        isHost = false;
        announcedJoin = false;
        members.clear();
        dead.clear();
        haveCtl = false;
        ctl = drifto_LanCtl{};
        seenMatchSeq = seenPhaseSeq = 0;
        inputHistory.clear();
        lastReqSeq.clear();
        havePendingRoster = false;
        std::lock_guard<std::mutex> lk(m);
        sInRoom = sIsHost = false;
        sRoom = 0;
        sPid = 0;
        sRoster.clear();
        sTransport = Transport::None;
        sRtt = -1;
        hasSnap = false;
        inInputs.clear();
        lastInputTick.clear();
    }

    // ============================================================ roster/state
    // Members -> public roster; the host flag follows hostId.
    std::vector<RosterEntry> rosterEntries() const
    {
        std::vector<RosterEntry> v;
        for (const Member &mm : members)
        {
            RosterEntry e;
            e.playerId = mm.pid;
            e.name = mm.name;
            e.isHost = mm.pid == hostId;
            e.ready = mm.ready;
            v.push_back(e);
        }
        return v;
    }

    // Copies room state into the mutex-guarded mirrors read by the public getters.
    // sInRoom only turns true once JoinedRoom has been announced.
    void syncMirrors()
    {
        std::lock_guard<std::mutex> lk(m);
        sInRoom = state == InRoom && announcedJoin;
        sRoom = roomId;
        sPid = pid;
        sIsHost = isHost;
        sRoster = rosterEntries();
        sTransport = sInRoom ? Transport::Udp : Transport::None;
        if (isHost) sRtt = 0; // the host is its own authority
    }

    void emitRoster()
    {
        Event e;
        e.type = EventType::RosterChanged;
        e.roster = rosterEntries();
        e.roomId = roomId;
        e.localPlayerId = pid;
        e.isHost = isHost;
        emit(e);
    }

    // JoinedRoom + first RosterChanged + TransportChanged(Udp): the same event
    // sequence Online produces when the game link comes up.
    void emitJoined()
    {
        announcedJoin = true;
        syncMirrors();
        Event e;
        e.type = EventType::JoinedRoom;
        e.roomId = roomId;
        e.localPlayerId = pid;
        e.isHost = isHost;
        e.roster = rosterEntries();
        emit(e);
        emitRoster();
        Event t;
        t.type = EventType::TransportChanged;
        t.transport = Transport::Udp;
        emit(t);
    }

    // Host: writes the full roster (transient-local, so late joiners get it).
    // epoch orders rosters across host migrations.
    void publishRoster()
    {
        std::vector<drifto_LanMember> buf;
        for (const Member &mm : members)
        {
            drifto_LanMember w{};
            w.playerId = mm.pid;
            copyStr(w.name, sizeof w.name, mm.name);
            w.ready = mm.ready;
            w.session = mm.session;
            w.tokenHash = mm.tokenHash;
            buf.push_back(w);
        }
        drifto_LanRoster r{};
        r.roomId = roomId;
        r.epoch = epoch;
        r.hostId = hostId;
        r.hostSession = session;
        r.nextPid = nextPid;
        copyStr(r.roomName, sizeof r.roomName, roomName);
        r.members._buffer = buf.data();
        r.members._length = r.members._maximum = (uint32_t)buf.size();
        dds_write(rosterW, &r);
    }

    // Host: (re)announces the room for the room list; called as a 1 Hz heartbeat
    // and on any change. closed=true also unregisters the instance so the room
    // vanishes from every list immediately.
    void announce(bool closed = false)
    {
        drifto_LanRoom a{};
        a.roomKey = roomKey;
        a.roomId = roomId;
        copyStr(a.name, sizeof a.name, roomName);
        a.hostId = hostId;
        copyStr(a.hostName, sizeof a.hostName, name);
        a.hostSession = session;
        a.epoch = epoch;
        a.players = (uint8_t)members.size();
        a.maxPlayers = kMaxRoomPlayers;
        a.inProgress = haveCtl && ctl.phase != (uint8_t)MatchPhase::Lobby;
        a.closed = closed;
        dds_write(roomW, &a);
        if (closed) dds_unregister_instance(roomW, &a);
        lastAnnounceUs = monoUs();
    }

    void unannounce() // stop announcing (handing the room to someone else)
    {
        drifto_LanRoom a{};
        a.roomKey = roomKey;
        dds_unregister_instance(roomW, &a);
    }

    Member *findMember(int32_t p)
    {
        for (Member &mm : members)
            if (mm.pid == p) return &mm;
        return nullptr;
    }

    // True if tok hashes to the value in the roster for player p.
    bool validToken(int32_t p, uint64_t tok)
    {
        Member *mm = findMember(p);
        return mm && mm->tokenHash == tokenHash(tok);
    }

    // Takes over (room creation, graceful hand-over, or failover): bumps the
    // epoch, resets liveness timers, opens the input reader, republishes the
    // roster, room announcement and current match state, and (for an
    // established member) emits RosterChanged + HostChanged(true).
    void becomeHost(uint32_t newEpoch)
    {
        bool was = isHost;
        isHost = true;
        hostId = pid;
        hostSession = session;
        epoch = newEpoch;
        uint64_t now = monoUs();
        for (Member &mm : members)
        {
            mm.lastSeenUs = now;
            if (mm.pid >= nextPid) nextPid = mm.pid + 1;
        }
        openInputReader();
        {
            std::lock_guard<std::mutex> lk(m);
            lastInputTick.clear();
            hasSnap = false;
        }
        publishRoster();
        announce();
        if (haveCtl)
        {
            ctl.hostId = pid;
            ctl.epoch = epoch;
            dds_write(ctlW, &ctl); // so late joiners get the running match from us
        }
        syncMirrors();
        if (!was && announcedJoin)
        {
            emitRoster();
            Event h;
            h.type = EventType::HostChanged;
            h.isHost = true;
            h.roomId = roomId;
            h.localPlayerId = pid;
            h.roster = rosterEntries();
            emit(h);
        }
    }

    // Stops acting as host (another host took over): withdraw the announcement
    // and drop the input reader.
    void demote()
    {
        if (!isHost) return;
        isHost = false;
        unannounce();
        if (inR > 0) dds_delete(inR);
        inR = 0;
    }

    // ============================================================ commands
    // Executes one queued public call. Login is local: it only validates the
    // name. RoomList is answered from the discovered announcements.
    void runCmd(const Cmd &c)
    {
        switch (c.kind)
        {
        case Cmd::Login:
        {
            Event e;
            if (!validPlayerName(c.a))
            {
                e.type = EventType::LoginFailed;
                e.message = "invalid name (1-8 chars: letters, digits, _ -)";
            }
            else
            {
                name = c.a;
                loggedIn = true;
                e.type = EventType::LoginOk;
                std::lock_guard<std::mutex> lk(m);
                sLoggedIn = true;
            }
            emit(e);
            break;
        }
        case Cmd::RoomList:
        {
            Event e;
            e.type = EventType::RoomList;
            uint64_t now = monoUs();
            for (auto &kv : rooms)
            {
                const drifto_LanRoom &r = kv.second.r;
                // skip closed rooms and rooms whose host stopped announcing
                if (r.closed || since(now, kv.second.lastSeenUs) > kRoomExpiryUs) continue;
                RoomInfo ri;
                ri.roomId = r.roomId;
                ri.name = r.name;
                ri.players = r.players;
                ri.maxPlayers = r.maxPlayers;
                ri.inProgress = r.inProgress;
                e.rooms.push_back(ri);
            }
            emit(e);
            break;
        }
        case Cmd::Create: createRoom(c.a); break;
        case Cmd::Join: joinRoom(c.u1); break;
        case Cmd::Leave:
            if (state != None) leaveRoom(true);
            break;
        case Cmd::Ready:
            if (state != InRoom) break;
            if (isHost)
            {
                if (Member *mm = findMember(pid)) mm->ready = c.flag;
                publishRoster();
                syncMirrors();
                emitRoster();
            }
            else
                sendReq(LanReady, c.flag);
            break;
        // match control: only the host may start/advance a match. Seq counters
        // make delivery idempotent for the receivers.
        case Cmd::Start:
        case Cmd::Phase:
            if (state != InRoom || !isHost) break; // non-host: ignored, as the game server does
            ctl.roomId = roomId;
            ctl.hostId = pid;
            ctl.epoch = epoch;
            if (c.kind == Cmd::Start)
            {
                ctl.matchSeq++;
                ctl.trackSeed = c.u1;
                ctl.startTick = c.u2;
            }
            else
            {
                ctl.phaseSeq++;
                ctl.phase = (uint8_t)c.phase;
            }
            haveCtl = true;
            dds_write(ctlW, &ctl); // our own reader emits the events (as everybody's)
            announce();
            break;
        }
    }

    // JoinFailed event helper.
    void joinFailed(const std::string &why)
    {
        Event e;
        e.type = EventType::JoinFailed;
        e.message = why;
        emit(e);
    }

    // Creates a room and becomes its host (pid 1, epoch 1). The random roomKey's
    // low 32 bits double as roomId (re-rolled if zero).
    void createRoom(const std::string &rn)
    {
        if (!loggedIn) return joinFailed("not logged in");
        if (state != None) leaveRoom(true);
        do roomKey = randomU64(); while ((uint32_t)roomKey == 0);
        roomId = (uint32_t)roomKey;
        roomName = rn.empty() ? name + "'s room" : rn.substr(0, 32);
        openRoomEntities();
        state = InRoom;
        pid = 1;
        nextPid = 2;
        token = randomU64();
        Member me;
        me.pid = pid;
        me.name = name;
        me.session = session;
        me.token = token;
        me.tokenHash = tokenHash(token);
        me.lastSeenUs = monoUs();
        members = {me};
        becomeHost(1);
        emitJoined();
    }

    // Starts joining a room seen in the room list: opens the room entities and
    // enters Joining; timers() then sends/retries the Join request.
    void joinRoom(uint32_t id)
    {
        if (!loggedIn) return joinFailed("not logged in");
        if (state == InRoom && id == roomId)
        {
            emitJoined(); // repeat, as Online does
            return;
        }
        uint64_t now = monoUs();
        const RoomSeen *found = nullptr;
        for (auto &kv : rooms)
            if (kv.second.r.roomId == id && !kv.second.r.closed && since(now, kv.second.lastSeenUs) <= kRoomExpiryUs)
                found = &kv.second;
        if (!found) return joinFailed("host gone");
        drifto_LanRoom r = found->r;
        if (state != None) leaveRoom(true);
        roomKey = r.roomKey;
        roomId = r.roomId;
        roomName = r.name;
        openRoomEntities();
        state = Joining;
        joinSeq = ++reqSeq;
        joinStartUs = now;
        lastJoinSendUs = 0;
    }

    // Guest -> host request. The join keeps one seq (joinSeq) across retries so
    // the host can match or de-duplicate them.
    void sendReq(uint8_t op, bool flag = false)
    {
        drifto_LanReq q{};
        q.session = session;
        q.roomId = roomId;
        q.seq = op == LanJoin ? joinSeq : ++reqSeq;
        q.op = op;
        copyStr(q.name, sizeof q.name, name);
        q.playerId = pid;
        q.token = token;
        q.flag = flag;
        dds_write(reqW, &q);
    }

    // Leaves the current room. A host with others remaining hands the room to the
    // lowest remaining player id (epoch+1) and a lone host closes the room; a
    // guest sends LanLeave. The short wait for acks lets the last message out
    // before the writers are deleted.
    void leaveRoom(bool emitEvent)
    {
        uint32_t old = roomId;
        if (state == InRoom)
        {
            if (isHost)
            {
                members.erase(std::remove_if(members.begin(), members.end(), [&](const Member &mm) { return mm.pid == pid; }),
                              members.end());
                if (members.empty())
                    announce(true); // room disappears from every list
                else
                {
                    // hand the room to the lowest remaining player id
                    unannounce();
                    int32_t next = members.front().pid;
                    for (const Member &mm : members) next = std::min(next, mm.pid);
                    hostId = next;
                    epoch++;
                    publishRoster();
                }
                dds_wait_for_acks(rosterW, DDS_MSECS(200));
            }
            else
            {
                sendReq(LanLeave);
                dds_wait_for_acks(reqW, DDS_MSECS(200));
            }
        }
        closeRoomEntities();
        bool wasIn = announcedJoin;
        resetRoomState();
        if (emitEvent && wasIn)
        {
            Event e;
            e.type = EventType::LeftRoom;
            e.roomId = old;
            emit(e);
        }
    }

    // ============================================================ polling
    // Takes every available sample of type T from reader r and calls fn on each
    // valid one; loans are returned after the batch.
    template <typename T, typename F> void drain(dds_entity_t r, F fn)
    {
        if (r <= 0) return;
        void *samples[32] = {nullptr};
        dds_sample_info_t infos[32];
        int n;
        while ((n = dds_take(r, samples, infos, 32, 32)) > 0)
        {
            for (int i = 0; i < n; i++)
                if (infos[i].valid_data) fn(*(const T *)samples[i]);
            dds_return_loan(r, samples, n);
        }
    }

    // Maintains the room list from announcements. A lower epoch than already
    // known (a deposed host) is ignored; rooms silent for 10 s are forgotten.
    void pollRooms(uint64_t now)
    {
        drain<drifto_LanRoom>(roomR, [&](const drifto_LanRoom &r) {
            if (r.closed)
            {
                rooms.erase(r.roomKey);
                return;
            }
            RoomSeen &s = rooms[r.roomKey];
            if (s.lastSeenUs != 0 && r.epoch < s.r.epoch && since(now, s.lastSeenUs) <= kRoomExpiryUs) return; // stale host
            s.r = r;
            s.lastSeenUs = now;
        });
        for (auto it = rooms.begin(); it != rooms.end();)
            if (since(now, it->second.lastSeenUs) > 10 * kSec) it = rooms.erase(it);
            else ++it;
    }

    // Copies the loaned roster sample into a RosterCopy before applying it.
    void handleRoster(const drifto_LanRoster &w, uint64_t now)
    {
        RosterCopy r;
        r.roomId = w.roomId;
        r.epoch = w.epoch;
        r.hostId = w.hostId;
        r.nextPid = w.nextPid;
        r.hostSession = w.hostSession;
        r.roomName = w.roomName;
        for (uint32_t i = 0; i < w.members._length; i++)
        {
            const drifto_LanMember &x = w.members._buffer[i];
            Member mm;
            mm.pid = x.playerId;
            mm.name = x.name;
            mm.ready = x.ready;
            mm.session = x.session;
            mm.tokenHash = x.tokenHash;
            r.members.push_back(mm);
        }
        applyRoster(r, now);
    }

    // Core of membership/migration handling on every roster sample. Ignored when:
    // it is our own echo, older than our epoch, a same-epoch rival with a higher
    // host id (concurrent promotion: lowest id wins), or names a host we gave up
    // on. While joining it is parked until JoinRep tells us our player id. A
    // roster without us means we were removed (or lost a split-brain).
    void applyRoster(const RosterCopy &r, uint64_t now)
    {
        if (r.roomId != roomId || state == None) return;
        bool own = r.hostSession == session;
        if (isHost && own && r.epoch == epoch) return; // our own echo
        if (r.epoch < epoch) return;                    // stale (an older host)
        if (r.epoch == epoch && state == InRoom && r.hostId != hostId && r.hostId > hostId) return;
        if (state == InRoom && dead.count(r.hostId) && r.epoch <= epoch) return; // the host we gave up on
        if (state == Joining && pid == 0)
        {
            pendingRoster = r; // apply once our JoinRep says who we are
            havePendingRoster = true;
            return;
        }
        int32_t prevHost = hostId;
        std::vector<Member> nm;
        bool meIn = false;
        for (Member mm : r.members)
        {
            mm.lastSeenUs = now;
            if (Member *old = findMember(mm.pid)) mm.token = old->token;
            if (mm.session == session) meIn = true;
            nm.push_back(mm);
        }
        if (state == Joining)
        {
            if (!meIn) return; // not admitted yet (the JoinRep will tell)
        }
        else if (!meIn)
        {
            if (isHost && !own) demote();
            closeRoomEntities();
            uint32_t old = roomId;
            bool wasIn = announcedJoin;
            resetRoomState();
            if (wasIn)
            {
                Event e;
                e.type = EventType::LeftRoom;
                e.roomId = old;
                e.message = "removed from room";
                emit(e);
            }
            return;
        }
        members = nm;
        epoch = r.epoch;
        hostId = r.hostId;
        hostSession = r.hostSession;
        nextPid = std::max(nextPid, (int32_t)r.nextPid);
        if (!r.roomName.empty()) roomName = r.roomName;
        dead.erase(hostId);
        if (hostId != pid) lastHostAliveUs = now;
        if (isHost && hostId != pid) demote();
        if (state == Joining)
        {
            state = InRoom;
            lastHostAliveUs = now;
            if (hostId == pid) becomeHost(epoch + 1); // (joined a room whose host just handed it to us)
            emitJoined();
            return;
        }
        if (hostId == pid && !isHost)
        {
            becomeHost(epoch + 1); // graceful hand-over from the old host; emits HostChanged
            return;
        }
        syncMirrors();
        emitRoster();
        if (prevHost != hostId)
        {
            Event h;
            h.type = EventType::HostChanged;
            h.isHost = isHost;
            h.roomId = roomId;
            h.localPlayerId = pid;
            h.roster = rosterEntries();
            emit(h);
        }
    }

    // Host-side handling of a member request: admission (with checks: room not
    // full, valid and unique name case-insensitively), ready toggle, leave.
    // Join retries are answered again with the same player id and token;
    // refused attempts are remembered by seq so they are not re-evaluated.
    // host: admit / ready / leave
    void handleReq(const drifto_LanReq &q, uint64_t now)
    {
        if (!isHost || q.roomId != roomId || q.session == session) return;
        if (q.op == LanJoin)
        {
            for (Member &mm : members)
                if (mm.session == q.session)
                {
                    sendJoinRep(q, true, "", mm.pid, mm.token); // retry of an admitted join
                    return;
                }
            auto it = lastReqSeq.find(q.session);
            if (it != lastReqSeq.end() && it->second == q.seq) return; // already refused this one
            lastReqSeq[q.session] = q.seq;
            if (members.size() >= (size_t)kMaxRoomPlayers) return sendJoinRep(q, false, "room full", 0, 0);
            if (!validPlayerName(q.name)) return sendJoinRep(q, false, "invalid name", 0, 0);
            for (const Member &mm : members)
                if (platform::strcasecmp(mm.name.c_str(), q.name) == 0) return sendJoinRep(q, false, "name taken", 0, 0);
            Member mm;
            mm.pid = nextPid++;
            mm.name = q.name;
            mm.session = q.session;
            mm.token = randomU64();
            mm.tokenHash = tokenHash(mm.token);
            mm.lastSeenUs = now;
            members.push_back(mm);
            sendJoinRep(q, true, "", mm.pid, mm.token);
            publishRoster();
            announce();
            syncMirrors();
            emitRoster();
            return;
        }
        // ready/leave must come from the player they claim to be
        if (!validToken(q.playerId, q.token)) return;
        if (q.op == LanReady)
        {
            findMember(q.playerId)->ready = q.flag;
            findMember(q.playerId)->lastSeenUs = now;
        }
        else if (q.op == LanLeave)
            removeMember(q.playerId);
        else
            return;
        publishRoster();
        announce();
        syncMirrors();
        emitRoster();
    }

    // Host: drops a member and the private reply writer made for it.
    void removeMember(int32_t p)
    {
        Member *mm = findMember(p);
        if (!mm) return;
        auto rp = repPubs.find(mm->session);
        if (rp != repPubs.end())
        {
            dds_delete(rp->second.first);
            repPubs.erase(rp);
        }
        members.erase(std::remove_if(members.begin(), members.end(), [&](const Member &x) { return x.pid == p; }),
                      members.end());
        {
            std::lock_guard<std::mutex> lk(m);
            lastInputTick.erase(p);
        }
    }

    // Host -> one joiner, in its private partition. Writers are created on first
    // use and pruned (non-members only) once more than 32 exist.
    void sendJoinRep(const drifto_LanReq &q, bool ok, const char *reason, int32_t p, uint64_t tok)
    {
        auto it = repPubs.find(q.session);
        if (it == repPubs.end())
        {
            dds_entity_t pb = dds::createPublisher(part, "lan.r" + std::to_string(roomId) + ".s" + hex64(q.session));
            dds_qos_t *qq = dds::qosReliable(4, true);
            dds_entity_t w = dds_create_writer(pb, tRep, qq, nullptr);
            dds_delete_qos(qq);
            it = repPubs.emplace(q.session, std::make_pair(pb, w)).first;
            if (repPubs.size() > 32) // forget refused joiners' writers
                for (auto x = repPubs.begin(); x != repPubs.end();)
                {
                    bool member = false;
                    for (const Member &mm : members) member |= mm.session == x->first;
                    if (!member && x->first != q.session)
                    {
                        dds_delete(x->second.first);
                        x = repPubs.erase(x);
                    }
                    else
                        ++x;
                }
        }
        drifto_LanJoinRep r{};
        r.session = q.session;
        r.roomId = roomId;
        r.seq = q.seq;
        r.ok = ok;
        copyStr(r.reason, sizeof r.reason, reason);
        r.playerId = p;
        r.token = tok;
        dds_write(it->second.second, &r);
    }

    // Guest: reply to our Join. Must match our session, room and join seq.
    // On success we remember pid/token; JoinedRoom waits for the roster.
    void handleJoinRep(const drifto_LanJoinRep &r, uint64_t now)
    {
        if (state != Joining || r.session != session || r.roomId != roomId || r.seq != joinSeq) return;
        if (!r.ok)
        {
            std::string reason = r.reason; // copy: closing the reader frees the loaned sample
            closeRoomEntities();
            resetRoomState();
            joinFailed(reason);
            return;
        }
        pid = r.playerId;
        token = r.token;
        lastHostAliveUs = now;
        // JoinedRoom is emitted once the roster with us in it arrives
        if (havePendingRoster)
        {
            havePendingRoster = false;
            RosterCopy pr = pendingRoster;
            applyRoster(pr, now);
        }
    }

    // Match control from the host (and our own echo when we are host). Events
    // fire when a sequence counter advances, so repeats and replays are harmless.
    void handleCtl(const drifto_LanCtl &c)
    {
        if (c.roomId != roomId || state != InRoom) return;
        if (c.epoch < epoch && c.hostId != hostId) return; // from a deposed host
        if (!isHost)
        {
            ctl = c;
            haveCtl = true;
        }
        if (c.hostId == hostId) lastHostAliveUs = monoUs();
        if (c.matchSeq > seenMatchSeq)
        {
            seenMatchSeq = c.matchSeq;
            {
                std::lock_guard<std::mutex> lk(m);
                lastInputTick.clear();
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

    // Drains all room readers in dependency order: JoinRep, roster, then (once
    // InRoom) requests, match control, pings/pongs, snapshots and inputs.
    void pollRoom(uint64_t now)
    {
        if (state == None) return;
        drain<drifto_LanJoinRep>(repR, [&](const drifto_LanJoinRep &r) { handleJoinRep(r, now); });
        if (state == None) return;
        drain<drifto_LanRoster>(rosterR, [&](const drifto_LanRoster &r) { handleRoster(r, now); });
        if (state != InRoom) return; // match control etc. stays queued in the readers until we're in
        drain<drifto_LanReq>(reqR, [&](const drifto_LanReq &q) { handleReq(q, now); });
        drain<drifto_LanCtl>(ctlR, [&](const drifto_LanCtl &c) { handleCtl(c); });
        // pings: host verifies the token, refreshes the member's liveness and echoes
        // clientTimeUs so the sender can compute RTT. pongs: any pong proves the host lives.
        // pings (host answers) / pongs (clients: RTT + host liveness)
        drain<drifto_PingUp>(pingR, [&](const drifto_PingUp &p) {
            if (!isHost || p.roomId != roomId || !validToken(p.playerId, p.token)) return;
            findMember(p.playerId)->lastSeenUs = now;
            drifto_PingDn o{};
            o.playerId = p.playerId;
            o.roomId = p.roomId;
            o.seq = p.seq;
            o.clientTimeUs = p.clientTimeUs;
            dds_write(pongW, &o);
        });
        drain<drifto_PingDn>(pongR, [&](const drifto_PingDn &p) {
            if (isHost || p.roomId != roomId || p.playerId != pid) return;
            lastHostAliveUs = now;
            float rtt = (float)(now - p.clientTimeUs) / 1000.f;
            std::lock_guard<std::mutex> lk(m);
            sRtt = sRtt < 0 ? rtt : sRtt * 0.8f + rtt * 0.2f;
        });
        // snapshots are accepted only from the current host with a valid token; the
        // newest in the batch wins
        // snapshots from the current host
        const drifto_SnapUp *best = nullptr;
        void *samples[32] = {nullptr};
        dds_sample_info_t infos[32];
        int n;
        if (snapR > 0 && (n = dds_take(snapR, samples, infos, 32, 32)) > 0)
        {
            for (int i = 0; i < n; i++)
            {
                if (!infos[i].valid_data) continue;
                const auto *s = (const drifto_SnapUp *)samples[i];
                if (s->roomId == roomId && s->playerId == hostId && !isHost && validToken(s->playerId, s->token)) best = s;
            }
            if (best)
            {
                lastHostAliveUs = now;
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
                snap = std::move(ws);
                hasSnap = true;
                snapHostTimeUs = best->hostTimeUs;
                snapRecvUs = now;
            }
            dds_return_loan(snapR, samples, n);
        }
        // inputs: token-checked, de-duplicated by acceptInputLocked (redundant copies)
        // inputs (host only has this reader)
        if (inR > 0 && (n = dds_take(inR, samples, infos, 32, 32)) > 0)
        {
            std::lock_guard<std::mutex> lk(m);
            for (int i = 0; i < n; i++)
            {
                if (!infos[i].valid_data || !isHost) continue;
                const auto &u = *(const drifto_InputUp *)samples[i];
                if (u.roomId != roomId || u.playerId == pid || !validToken(u.playerId, u.token)) continue;
                findMember(u.playerId)->lastSeenUs = now;
                for (uint32_t k = 0; k < u.inputs._length; k++) acceptInputLocked(u.playerId, unpackInput(u.inputs._buffer[k]));
            }
            dds_return_loan(inR, samples, n);
        }
    }

    // ============================================================ outgoing
    // Sends what the caller threads queued: a guest's inputs (with redundant
    // history) or the host's snapshot. Host inputs never leave the process
    // (sendInput short-circuits them).
    void flush(uint64_t now)
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
        if (state != InRoom) return;
        if (!ins.empty() && !isHost)
        {
            for (const CarInput &ci : ins)
            {
                drifto_InputW w;
                packInput(ci, w);
                // tick went backwards = the game restarted its counter; old copies would confuse the host
                if (!inputHistory.empty() && w.tick <= inputHistory.back().tick) inputHistory.clear();
                inputHistory.push_back(w);
            }
            while (inputHistory.size() > (size_t)kInputRedundancy) inputHistory.pop_front();
            drifto_InputW buf[kInputRedundancy];
            uint32_t k = 0;
            for (const auto &w : inputHistory) buf[k++] = w;
            drifto_InputUp u{};
            u.playerId = pid;
            u.roomId = roomId;
            u.token = token;
            u.inputs._buffer = buf;
            u.inputs._length = u.inputs._maximum = k;
            dds_write(inW, &u);
        }
        if (doSnap && isHost)
        {
            std::vector<drifto_CarW> cars(ws.cars.size());
            for (size_t i = 0; i < ws.cars.size(); i++) packCar(ws.cars[i], cars[i]);
            drifto_SnapUp s{};
            s.roomId = roomId;
            s.playerId = pid;
            s.token = token;
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
            dds_write(snapW, &s);
        }
        (void)now;
    }

    // ============================================================ timers
    // Host silent for kHostLossUs: mark it dead, drop it from the member list and
    // elect the lowest remaining player id. Every guest computes the same result;
    // the winner calls becomeHost, the others just point at it and wait.
    void hostLost(uint64_t now)
    {
        fprintf(stderr, "[net] LAN: host %d silent for %.1f s, migrating\n", hostId, since(now, lastHostAliveUs) / 1e6);
        dead.insert(hostId);
        members.erase(std::remove_if(members.begin(), members.end(), [&](const Member &mm) { return dead.count(mm.pid) != 0; }),
                      members.end());
        int32_t next = pid;
        for (const Member &mm : members) next = std::min(next, mm.pid);
        lastHostAliveUs = now; // grace period for the new host
        if (next == pid)
        {
            becomeHost(epoch + 1); // emits HostChanged(true)
            return;
        }
        hostId = next;
        {
            std::lock_guard<std::mutex> lk(m);
            hasSnap = false;
        }
        syncMirrors();
        emitRoster();
        Event h;
        h.type = EventType::HostChanged;
        h.isHost = false;
        h.roomId = roomId;
        h.localPlayerId = pid;
        h.roster = rosterEntries();
        emit(h);
    }

    // Time-driven behaviour. Joining: retry the request, fail after kJoinTimeoutUs.
    // Host: room heartbeat and eviction of silent members. Guest: ping the host and
    // start migration if it stays silent.
    void timers(uint64_t now)
    {
        if (state == Joining)
        {
            if (since(now, joinStartUs) > kJoinTimeoutUs)
            {
                closeRoomEntities();
                resetRoomState();
                joinFailed("host gone");
                return;
            }
            if (pid == 0 && since(now, lastJoinSendUs) >= kJoinRetryUs)
            {
                sendReq(LanJoin);
                lastJoinSendUs = now;
            }
            return;
        }
        if (state != InRoom) return;
        if (isHost)
        {
            if (since(now, lastAnnounceUs) >= kAnnounceUs) announce();
            std::vector<int32_t> gone;
            for (const Member &mm : members)
                if (mm.pid != pid && since(now, mm.lastSeenUs) > kMemberTimeoutUs) gone.push_back(mm.pid);
            if (!gone.empty())
            {
                for (int32_t p : gone)
                {
                    fprintf(stderr, "[net] LAN host: player %d timed out\n", p);
                    removeMember(p);
                }
                publishRoster();
                announce();
                syncMirrors();
                emitRoster();
            }
            return;
        }
        if (since(now, lastPingUs) >= kPingUs)
        {
            drifto_PingUp p{};
            p.playerId = pid;
            p.roomId = roomId;
            p.token = token;
            p.seq = ++pingSeq;
            p.clientTimeUs = now;
            dds_write(pingW, &p);
            lastPingUs = now;
        }
        if (since(now, lastHostAliveUs) > kHostLossUs) hostLost(now);
    }

    // Worker loop: short waits while joining so retries and the reply are prompt.
    void run() override
    {
        while (!quit.load())
        {
            waitForWork(DDS_MSECS(state == Joining ? 10 : 50));
            uint64_t now = monoUs();
            for (const Cmd &c : takeCmds()) runCmd(c);
            pollRooms(now);
            pollRoom(now);
            flush(now);
            timers(now);
        }
    }
};

} // namespace

std::unique_ptr<ClientCore> makeLanCore() { return std::unique_ptr<ClientCore>(new LanCore); }

} // namespace detail
} // namespace net
