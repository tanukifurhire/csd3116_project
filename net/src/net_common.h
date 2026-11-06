// net_common.h -- internals shared by NetClient (net/src) and the two servers
// (server_folder/). Not part of the public contract (that is
// net/include/net_client.h). Everything DDS-specific lives behind this header.
//
// Contents: wire constants (domains, ports, topic names), the API/control
// opcodes, partition-name builders, quantization functions between the public
// structs and the compact IDL types, and the DDS helper layer (config XML,
// domain ref-counting, secure participants, QoS presets).
// Threading: all functions are callable from any thread (see net_common.cpp
// for the few that share state).
// Why partitions: DDS delivers a sample only to readers whose partition
// overlaps the writer's, so per-room/per-player routing happens in the
// middleware instead of filtering every packet in the application.
#pragma once

#include <dds/dds.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "drifto_net.h" // idlc output, build/net/gen/
#include "../include/net_client.h"

namespace net {
namespace wire {

// ---- defaults (all overridable, see docs/MULTIPLAYER.md) -------------------
// Separate DDS domain per service/transport.
constexpr uint32_t kApiDomain = 1;      // DDS domain of the API service (TCP)
constexpr uint32_t kGameUdpDomain = 2;  // game service, UDP flavour
constexpr uint32_t kGameTcpDomain = 3;  // game service, TCP(-443) flavour
constexpr int kApiPort = 7600;          // API service TCP port
constexpr int kGameUdpPort = 7700;      // game service UDP port (and +1)
constexpr int kGameTcpPort = 443;       // game service TCP fallback port

// LAN mode (serverless): one UDP domain with multicast discovery (+ optional
// unicast peers). Default ports: 7400 + 250*10 = 9900 (multicast), 9910+2i /
// 9911+2i (unicast, participant index i).
constexpr uint32_t kLanDomain = 10;
constexpr const char *kTopicLanRoom = "Drifto_Lan_Room";
constexpr const char *kTopicLanReq = "Drifto_Lan_Req";
constexpr const char *kTopicLanJoinRep = "Drifto_Lan_JoinRep";
constexpr const char *kTopicLanRoster = "Drifto_Lan_Roster";
constexpr const char *kTopicLanCtl = "Drifto_Lan_Ctl";
constexpr const char *kTopicLanInput = "Drifto_Lan_Input";
constexpr const char *kTopicLanSnap = "Drifto_Lan_Snap";
constexpr const char *kTopicLanPing = "Drifto_Lan_Ping";
constexpr const char *kTopicLanPong = "Drifto_Lan_Pong";

// ---- topic names -------------------------------------------------------------
// *_Up: written by clients, read by servers. *_Dn: written by servers, read by
// clients. permissions.xml grants clients publish "Drifto_*_Up" and subscribe
// "Drifto_*_Dn" only.
constexpr const char *kTopicApiReq = "Drifto_ApiReq_Up";
constexpr const char *kTopicApiRep = "Drifto_ApiRep_Dn";
constexpr const char *kTopicGsBeat = "Drifto_GsBeat";
constexpr const char *kTopicGsRoom = "Drifto_GsRoom";
constexpr const char *kTopicGsStatus = "Drifto_GsStatus";
constexpr const char *kTopicInputUp = "Drifto_Input_Up";
constexpr const char *kTopicInputDn = "Drifto_Input_Dn";
constexpr const char *kTopicSnapUp = "Drifto_Snap_Up";
constexpr const char *kTopicSnapDn = "Drifto_Snap_Dn";
constexpr const char *kTopicCtlUp = "Drifto_Ctl_Up";
constexpr const char *kTopicCtlDn = "Drifto_Ctl_Dn";
constexpr const char *kTopicPingUp = "Drifto_Ping_Up";
constexpr const char *kTopicPingDn = "Drifto_Ping_Dn";

// API request opcodes (client -> API server, ApiReq.op).
enum ApiOp : uint8_t
{
    OpLogin = 1,
    OpRoomList,
    OpCreateRoom,
    OpJoinRoom,
    OpLeaveRoom,
    OpSetReady,
    OpHeartbeat,
    OpLogout,
};

// API reply kinds (API server -> client, ApiRep.kind).
enum ApiRepKind : uint8_t
{
    RepLoginOk = 1,
    RepLoginFailed,
    RepRoomList,
    RepJoined,
    RepJoinFailed,
    RepLeft,
    RepRoster,
    RepHeartbeatAck,
    RepError,
    RepSessionExpired,
};

// Match-control opcodes (host -> game server / LAN room).
enum CtlOp : uint8_t
{
    CtlStart = 1,
    CtlPhase = 2,
};

// Each InputUp carries the newest 5 inputs (about 83 ms at 60 Hz) so that up
// to 4 consecutive lost packets still deliver every tick; the receiver
// de-duplicates by tick.
constexpr int kInputRedundancy = 5; // inputs carried by each InputUp

// ---- partitions ----------------------------------------------------------------
std::string sessionPartition(uint64_t session);         // "s<hex>"   API replies
std::string gsPartition(const std::string &gsId);       // "gs.<id>"  room assignments
std::string roomPartition(uint32_t roomId);             // "r<id>"    snapshots, match control
std::string playerPartition(uint32_t roomId, int32_t pid); // "r<id>.p<pid>" inputs (host), pongs

// ---- quantization ------------------------------------------------------------
// Lossy but bounded: steer 1/127, pedals 1/255, velocity 1 cm/s, angular
// velocity 0.001 rad/s, rotation 1/32767 per quaternion component.
void packInput(const CarInput &in, drifto_InputW &out);
CarInput unpackInput(const drifto_InputW &in);
void packCar(const CarState &in, drifto_CarW &out);
CarState unpackCar(const drifto_CarW &in);

// ---- misc ------------------------------------------------------------------------
uint64_t monoUs();           // steady clock, microseconds
uint64_t randomU64();        // non-zero, from std::random_device
void copyStr(char *dst, size_t cap, const std::string &src); // cap = sizeof(dst)

} // namespace wire

namespace dds {

// Reads <configDir>/<file> (falls back to the built-in copy of the template
// if the file is missing) and substitutes @KEY@ placeholders.
// DRIFTO_DDS_INTERFACE=<name or IPv4>[,...] pins the network interface(s);
// when it is unset, autoInterface (same syntax) is used if non-empty.
std::string loadConfig(const std::string &configDir, const std::string &file,
                       const std::map<std::string, std::string> &vars, const std::string &traceTag,
                       const std::string &autoInterface = "");

// Local IPv4 of the default route (the adapter facing the router), or "" if
// there is none. LAN mode pins CycloneDDS to it: left alone, Cyclone often
// picks a VPN/virtual adapter (Radmin, Tailscale, WireGuard, Hyper-V).
std::string defaultRouteIPv4();

// Process-wide, ref-counted dds_create_domain(): several NetClients in one
// process (net_tests) share a domain; the first caller's XML wins.
// persistent = never dds_delete() it when the last user releases it (it is
// reused while the XML is unchanged): LAN mode uses this, because deleting a
// multicast UDP domain hangs when multicast doesn't actually work (WSL2 NAT).
bool acquireDomain(uint32_t domainId, const std::string &xml, std::string *err, bool persistent = false);
void releaseDomain(uint32_t domainId);

// dds_delete() on a helper thread, waiting at most timeoutMs. CycloneDDS
// blocks deleting a TCP participant while its event thread sits in a
// blocking connect() to an unreachable peer (up to ~20 s on Windows, ~2 min
// on Linux); callers must not hang on that. Returns false if it's still
// running (it completes in the background, or dies with the process).
bool deleteEntityBounded(dds_entity_t e, int timeoutMs);
// True once any bounded delete timed out: Cyclone may hold internal locks,
// so skip further non-essential teardown (process exit cleans up).
bool wedged();

struct Security
{
    bool enabled = true;
    std::string certDir = "certs";
    std::string identity = "client1"; // certs/<identity>.pem / .key
};
// Creates a participant in an already-acquired domain. With security enabled
// it uses certs/ca.pem, <identity>.pem/.key, governance.p7s, permissions.p7s
// and fails (with a readable *err) if any file is missing or the plugins
// can't be loaded -- it never silently falls back to an insecure participant.
dds_entity_t createParticipant(uint32_t domainId, const Security &sec, std::string *err);

// QoS helpers (caller deletes with dds_delete_qos).
dds_qos_t *qosBestEffort(int depth);
dds_qos_t *qosReliable(int depth, bool transientLocal);

dds_entity_t createPublisher(dds_entity_t participant, const std::string &partition);
dds_entity_t createSubscriber(dds_entity_t participant, const std::vector<std::string> &partitions);

// CycloneDDS 0.10.4 never sets TCP_NODELAY on its TCP sockets (despite
// <TCP><NoDelay>), so small packets sit in Nagle + delayed-ACK for up to
// ~40 ms. This sets TCP_NODELAY on every TCP socket of this process (Linux:
// accepted sockets inherit it from the listener). Cheap; call periodically.
// Returns how many sockets were changed.
int tuneTcpSockets();

// Count of currently matched readers of 'writer' (0 on error).
int matchedReaders(dds_entity_t writer);

} // namespace dds
} // namespace net
