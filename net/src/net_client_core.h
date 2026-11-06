// net_client_core.h -- private to net/src. The state every NetClient backend
// shares (queues the public methods fill, mirrors they read, the Event list)
// plus the backend interface. Two backends implement it:
//   OnlineCore (net_client.cpp) -- api_server + game_server
//   LanCore    (net_lan.cpp)    -- serverless LAN rooms over DDS multicast
// NetClient (net_client.cpp) only dispatches to the active backend, so the
// public API and Events behave the same in both modes.
#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net_common.h"

namespace net {
namespace detail {

// ---- shared constants ----
constexpr uint64_t kSec = 1000000;
// host input queue cap; on overflow the oldest half is dropped (a stalled
// game thread must not grow memory without bound)
constexpr size_t kMaxQueuedInputs = 8192;
constexpr uint32_t kTickResetWindow = 120; // 2 s at 60 Hz

// now - t, clamped (t may be stamped a little after 'now' was sampled)
inline uint64_t since(uint64_t now, uint64_t t) { return now > t ? now - t : 0; }

// A public call queued for the worker thread. a/b = strings (name/password or
// room name), u1/u2 = ints (room id; track seed/start tick), flag = ready,
// phase = target match phase.
struct Cmd
{
    enum Kind { Login, RoomList, Create, Join, Leave, Ready, Start, Phase } kind;
    explicit Cmd(Kind k) : kind(k) {}
    std::string a, b;
    uint32_t u1 = 0, u2 = 0;
    bool flag = false;
    MatchPhase phase = MatchPhase::Lobby;
};

// State shared by both backends. Concurrency contract: fields under 'shared
// state' are guarded by m; thr/quit/started/waitset/guard are owned by
// NetClient::start/stop; everything else lives in the derived classes and is
// worker-thread-only.
struct ClientCore
{
    virtual ~ClientCore() = default;

    NetClient::Config cfg;
    dds::Security sec;
    std::thread thr;
    std::atomic<bool> quit{false};
    bool started = false;
    dds_entity_t waitset = 0, guard = 0; // owned by NetClient::start/stop

    // Naming: s* are mirrors for the public getters; out* are caller -> worker
    // mailboxes; snap/inInputs are worker -> caller mailboxes.
    // ---------------- shared state (guarded by m) ----------------
    mutable std::mutex m;
    std::deque<Cmd> cmds;
    std::vector<CarInput> outInputs;
    bool hasOutSnap = false;
    WorldSnapshot outSnap;
    uint64_t outSnapTimeUs = 0;
    std::vector<Event> events;
    bool hasSnap = false;
    WorldSnapshot snap;
    uint64_t snapHostTimeUs = 0, snapRecvUs = 0; // diagnostics
    std::vector<NetClient::PlayerInput> inInputs;
    std::map<int32_t, uint32_t> lastInputTick; // host-side dedup
    bool sLoggedIn = false, sInRoom = false, sIsHost = false;
    int32_t sPid = 0;
    uint32_t sRoom = 0;
    std::vector<RosterEntry> sRoster;
    Transport sTransport = Transport::None;
    float sRtt = -1;

    // ---------------- backend interface ----------------
    // open(): create the DDS entities (caller thread, before the worker starts)
    virtual bool open(std::string *err) { (void)err; return true; }
    // run(): the worker loop, until 'quit'
    virtual void run() {}
    // close(): after the worker joined; say goodbye + delete DDS entities
    virtual void close() {}

    // ---------------- helpers ----------------
    // Queues an Event for pollEvents(); callable from the worker thread.
    void emit(Event e)
    {
        std::lock_guard<std::mutex> lk(m);
        events.push_back(std::move(e));
    }
    // Interrupts waitForWork() so a freshly queued call is handled immediately.
    void wake()
    {
        if (guard > 0) dds_set_guardcondition(guard, true);
    }
    // Called by the public API: queue the command, then wake the worker.
    void push(Cmd c)
    {
        {
            std::lock_guard<std::mutex> lk(m);
            cmds.push_back(std::move(c));
        }
        wake();
    }
    // host-side dedup by (playerId, tick); caller holds m
    // Accepts an input only if its tick is newer than the last one seen for that
    // player, which drops the redundant copies carried by later packets and
    // reordered stragglers. A tick far below the last one (more than
    // kTickResetWindow behind) means the sender restarted its counter, so it is
    // accepted and becomes the new baseline.
    void acceptInputLocked(int32_t player, const CarInput &in)
    {
        auto it = lastInputTick.find(player);
        if (it != lastInputTick.end())
        {
            uint32_t last = it->second;
            bool newer = in.tick > last;
            // far behind = the sender restarted its tick counter (new match);
            // merely old = redundant copy / reordering -> drop
            bool reset = in.tick + kTickResetWindow < last;
            if (!newer && !reset) return;
        }
        lastInputTick[player] = in.tick;
        inInputs.push_back({player, in});
        if (inInputs.size() > kMaxQueuedInputs) inInputs.erase(inInputs.begin(), inInputs.begin() + inInputs.size() / 2);
    }
    // Waits on the waitset (guard condition = a public call queued work).
    void waitForWork(dds_duration_t timeout)
    {
        dds_attach_t trig[8];
        dds_waitset_wait(waitset, trig, 8, timeout);
        dds_set_guardcondition(guard, false);
    }
    // Worker: grabs all queued commands in one lock acquisition.
    std::deque<Cmd> takeCmds()
    {
        std::deque<Cmd> local;
        std::lock_guard<std::mutex> lk(m);
        local.swap(cmds);
        return local;
    }
};

// Backend factories (defined in net_client.cpp and net_lan.cpp).
std::unique_ptr<ClientCore> makeOnlineCore();
std::unique_ptr<ClientCore> makeLanCore();

// Valid player name (Online: the API server checks the same rule).
bool validPlayerName(const std::string &n);

} // namespace detail
} // namespace net
