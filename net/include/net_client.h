// net_client.h -- the seam between game/ (gameplay, host simulation) and
// net/ (CycloneDDS transport, API + game services).
//
// Owned by net/. game/ and client_folder/ only include this header; nothing
// outside net/ includes dds/dds.h or the idlc-generated headers.
// Additive changes are fine; changing/removing anything here needs game/
// updated in the same change.
//
// Architecture (see docs/MULTIPLAYER.md):
//   * API service  -- CycloneDDS over TCP. Login (auth.c), room list,
//                     create/join/leave room. Its room allocator assigns
//                     each room to a game server and tells the client where.
//   * Game service -- CycloneDDS over UDP, falling back to TCP port 443 when
//                     UDP is blocked. A pure router: it forwards a room's
//                     packets between that room's clients and does NO game
//                     simulation.
//   * Host authority -- one client per room is the host. It runs the
//                     authoritative simulation for every car from the
//                     inputs the others send, and publishes snapshots.
//                     Non-hosts predict their own car locally and reconcile
//                     against host snapshots; remote cars are interpolated.
//
// Threading: NetClient runs its own worker thread(s). Every public method is
// thread-safe and non-blocking (requests are queued; results come back as
// Events from pollEvents()).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../common/dmath.h"

namespace net {

// ---- limits and shared enums ----
constexpr int kMaxRoomPlayers = 4;
constexpr int kMaxNameLen = 8; // matches the existing JoinRequest.str_name[9]

enum class TransportPref : uint8_t { Auto, UdpOnly, Tcp443Only };
enum class Transport : uint8_t { None, Udp, Tcp443 };

// Pre-race lobby -> countdown -> race -> results, then back to Lobby.
enum class MatchPhase : uint8_t { Lobby, Countdown, Racing, Results };

// One fixed simulation tick of player input. Ticks are the sender's local
// fixed-step counter (60 Hz, see game/).
struct CarInput
{
    uint32_t tick = 0;
    float steer = 0;    // -1 (left) .. +1 (right)
    float throttle = 0; // 0..1
    float brake = 0;    // 0..1
    bool handbrake = false;
    bool nitro = false; // held: burn nitro gauge
    // Cosmetic car choice (0 = AE86, 1 = RX-7): same physics, the host
    // copies it into CarState::flags (kCarModelRx7) so every peer draws and
    // hears the right car. Rides in a spare bit of the existing input flags
    // octet, so the wire format (and the relaying servers) are unchanged.
    uint8_t carModel = 0;
};

// Authoritative state of one car, as simulated by the host.
struct CarState
{
    int32_t playerId = 0;
    uint32_t lastInputTick = 0; // newest input of this player the host has applied (for reconciliation)
    dm::Vec3 position;
    dm::Quat rotation;
    dm::Vec3 velocity;
    dm::Vec3 angularVelocity;
    float driftScore = 0;   // accumulated drift points
    float distance = 0;     // metres travelled along the track
    uint32_t trackSection = 0;
    uint8_t flags = 0;      // see CarFlag
    float nitro = 0;        // gauge 0..1
    uint8_t place = 0;      // 1 = leading, among cars still in the race; 0 = eliminated
    uint8_t eliminatedOrder = 0; // 0 = still racing, 1 = first car eliminated, 2 = second, ...
};

// Bit flags for CarState::flags (packed into one byte on the wire).
enum CarFlag : uint8_t
{
    kCarAlive = 1 << 0,
    kCarDrifting = 1 << 1,
    kCarCrashed = 1 << 2,    // wrecked / respawning
    kCarNitro = 1 << 3,      // nitro burning this tick
    kCarSlipstream = 1 << 4, // in another car's wake
    kCarEliminated = 1 << 5,
    kCarGhost = 1 << 6,      // just respawned: no car-car contact, drawn translucent
    kCarModelRx7 = 1 << 7,   // cosmetic: the player picked the RX-7 (else AE86); see CarInput::carModel
};

// What the host publishes ~20-30 times per second: match phase + every car.
struct WorldSnapshot
{
    uint32_t hostTick = 0;
    MatchPhase phase = MatchPhase::Lobby;
    float phaseTimer = 0; // seconds remaining in Countdown/Results
    // Elimination race (Racing phase): the car in last place is eliminated when
    // eliminationTimer reaches 0. Each time last place changes hands the timer
    // restarts from the next, shorter stage (32,16,8,4,2,1 s); an elimination
    // resets it to stage 0 (32 s).
    float eliminationTimer = 0;     // seconds left for the current last-place car
    uint8_t eliminationStage = 0;   // index into 32,16,8,4,2,1
    int32_t lastPlacePlayerId = 0;  // car currently on the clock
    int32_t winnerPlayerId = 0;     // last car standing (Results), 0 until decided
    std::vector<CarState> cars;
};

struct RosterEntry
{
    int32_t playerId = 0;
    std::string name;
    bool isHost = false;
    bool ready = false;
};

struct RoomInfo
{
    uint32_t roomId = 0;
    std::string name;
    int players = 0;
    int maxPlayers = kMaxRoomPlayers;
    bool inProgress = false;
};

// Everything the net layer reports to the game; see Event for which fields
// are filled per type.
enum class EventType : uint8_t
{
    LoginOk,
    LoginFailed,     // message = reason
    RoomList,        // rooms
    JoinedRoom,      // roomId, localPlayerId, isHost
    JoinFailed,      // message = reason
    LeftRoom,
    RosterChanged,   // roster
    HostChanged,     // isHost = whether *we* are now host
    MatchStarted,    // trackSeed, startTick: everyone (re)builds the track from trackSeed
    PhaseChanged,    // phase
    TransportChanged,// transport (e.g. UDP failed -> Tcp443); None = game link lost, re-handshaking
    Disconnected,    // message = reason
};

struct Event
{
    EventType type;
    std::string message;
    std::vector<RoomInfo> rooms;
    std::vector<RosterEntry> roster;
    uint32_t roomId = 0;
    int32_t localPlayerId = 0;
    bool isHost = false;
    uint32_t trackSeed = 0;
    uint32_t startTick = 0;
    MatchPhase phase = MatchPhase::Lobby;
    Transport transport = Transport::None;
};

// Thread-safe facade; see the threading note at the top of this file.
class NetClient
{
public:
    enum class Mode : uint8_t { Online, Lan };

    struct Config
    {
        std::string apiHost = "127.0.0.1";   // also accepts "host:port"
        int apiPort = 0;                      // [net: added] 0 = env DRIFTO_API_PORT or 7600
        std::string configDir = "net/config"; // CycloneDDS XML templates
        std::string certDir = "certs";
        int certIndex = 1;                    // certs/client<N>.pem/.key
        TransportPref transport = TransportPref::Auto;
        // [net: added] true = plain DDS without DDS Security (no certs needed;
        // must match the servers' --insecure). Env DRIFTO_INSECURE=1 also works.
        bool insecure = false;

        // Online = api_server + game_server (everything above).
        // Lan    = serverless: rooms are discovered over DDS multicast and the
        //          host's client admits players and publishes the roster
        //          itself; inputs/snapshots go peer to peer. apiHost/apiPort/
        //          transport are ignored. login() succeeds locally with just
        //          the name (no password check, no server); requestRoomList()
        //          lists rooms announced on the LAN; createRoom() makes us the
        //          host. Every other call and Event behaves as in Online mode,
        //          except: LoginOk.localPlayerId is 0 (LAN ids are per room
        //          and arrive with JoinedRoom); rttMs() is guest->host (0 on
        //          the host); transport() is Udp in a room; joinRoom() only
        //          works for a room already seen in the room list (discovery
        //          takes ~0.3-1 s after start), else JoinFailed "host gone".
        Mode mode = Mode::Online;
        // LAN fallback when multicast is blocked: unicast peer addresses
        // ("192.168.1.20" or "host:port") added to discovery.
        std::vector<std::string> lanPeers;
    };

    NetClient();
    ~NetClient();
    NetClient(const NetClient &) = delete;
    NetClient &operator=(const NetClient &) = delete;

    // Returns false (reason on stderr) if the DDS entities could not be created,
    // e.g. missing certificates. Calls made before start() are kept and run after it.
    bool start(const Config &config); // connects to the API service
    // Blocks briefly while the worker leaves cleanly (Logout / room hand-over).
    void stop();

    // --- API service (queued; answers arrive as Events) ------------------
    void login(const std::string &name, const std::string &password);
    void requestRoomList();
    void createRoom(const std::string &roomName);
    void joinRoom(uint32_t roomId);
    void leaveRoom();
    void setReady(bool ready);

    // Call from the game thread each frame; returns and clears the event queue.
    std::vector<Event> pollEvents();

    // --- current state -----------------------------------------------------
    bool loggedIn() const;
    bool inRoom() const;
    bool isHost() const;
    int32_t localPlayerId() const;
    std::vector<RosterEntry> roster() const;
    Transport transport() const;
    float rttMs() const; // round trip to the game server, -1 if unknown

    // --- game service: every client --------------------------------------
    // Our input for one tick. The host's own input is short-circuited
    // locally (never leaves the process). Best-effort; the net layer adds
    // redundancy (recent inputs ride along) so single drops don't matter.
    void sendInput(const CarInput &input);
    // Newest snapshot from the host since the last call; false if none.
    bool takeLatestSnapshot(WorldSnapshot &out);

    // --- game service: host only -----------------------------------------
    // Inputs received from all players (including our own), oldest first,
    // de-duplicated by (playerId, tick).
    struct PlayerInput
    {
        int32_t playerId;
        CarInput input;
    };
    std::vector<PlayerInput> takeInputs();
    void publishSnapshot(const WorldSnapshot &snapshot); // ~20-30 Hz, best-effort
    // Reliable: starts a match; every client gets EventType::MatchStarted.
    void startMatch(uint32_t trackSeed, uint32_t startTick);
    // Reliable: phase transitions (Countdown/Racing/Results/Lobby).
    void setPhase(MatchPhase phase);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace net
