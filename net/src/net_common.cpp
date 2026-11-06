// net_common.cpp -- see net_common.h.
//
// Contents: (1) wire helpers: partition names, quantization of inputs/cars to
// the compact IDL types, monotonic clock, random ids; (2) DDS helpers: config
// templates, domain ref-counting, bounded teardown, secure participant
// creation, QoS presets, TCP_NODELAY tuning.
//
// Threading: everything here is callable from any thread. The only shared
// mutable state is the domain table (g_domMu), the wedge flag (atomic) and the
// random generator (its own mutex). Entities returned to callers are owned by
// them; the helpers never keep references to them.
//
// Key decisions:
//   * Floats are quantized for the wire (positions stay float, rotation as 4x
//     int16, velocities as centi-units, ...) to keep snapshots small enough to
//     send at 20-30 Hz with several cars.
//   * Security is all-or-nothing: with DDS Security enabled a missing cert or
//     plugin is an error, never a silent fallback to an unauthenticated
//     participant.
//   * CycloneDDS 0.10.4 teardown can hang, so deletes that may block run on a
//     helper thread with a time limit.
#include "net_common.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>
#include <random>
#include <sstream>
#include <filesystem>
#include <future>
#include <set>
#include <system_error>
#ifdef _WIN32
#include <winsock2.h> // (CycloneDDS already did WSAStartup for us)
#include <ws2tcpip.h>
#else
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace net {
namespace wire {

// ---- partition names: see net_common.h for who uses which ----
static std::string hex64(uint64_t v)
{
    char buf[24];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return buf;
}

std::string sessionPartition(uint64_t session) { return "s" + hex64(session); }
std::string gsPartition(const std::string &gsId) { return "gs." + gsId; }
std::string roomPartition(uint32_t roomId) { return "r" + std::to_string(roomId); }
std::string playerPartition(uint32_t roomId, int32_t pid)
{
    return "r" + std::to_string(roomId) + ".p" + std::to_string(pid);
}

// Quantization helpers. q16: scale then round into the symmetric int16 range
// (NaN -> 0, clamped to +-32767 so -32768 is never produced). qu8: [0,1] -> 0..255.
static int16_t q16(float v, float scale)
{
    float s = std::round(v * scale);
    if (!(s == s)) s = 0; // NaN
    if (s > 32767.f) s = 32767.f;
    if (s < -32767.f) s = -32767.f;
    return (int16_t)s;
}
static uint8_t qu8(float v)
{
    float s = std::round(dm::clampf(v, 0.f, 1.f) * 255.f);
    return (uint8_t)s;
}

// CarInput <-> wire (InputW): steer as signed 8 bit (/127), throttle and
// brake as unsigned 8 bit (/255), handbrake/nitro as flag bits 0/1. Quantization step is
// 1/127 for steering and 1/255 for the pedals.
void packInput(const CarInput &in, drifto_InputW &out)
{
    out.tick = in.tick;
    float s = std::round(dm::clampf(in.steer, -1.f, 1.f) * 127.f);
    out.steer = (uint8_t)(int8_t)s;
    out.throttle = qu8(in.throttle);
    out.brake = qu8(in.brake);
    out.flags = (in.handbrake ? 1 : 0) | (in.nitro ? 2 : 0) | (in.carModel ? 4 : 0); // bit2: car choice
}

CarInput unpackInput(const drifto_InputW &in)
{
    CarInput o;
    o.tick = in.tick;
    o.steer = (float)(int8_t)in.steer / 127.f;
    o.throttle = in.throttle / 255.f;
    o.brake = in.brake / 255.f;
    o.handbrake = (in.flags & 1) != 0;
    o.nitro = (in.flags & 2) != 0;
    o.carModel = (in.flags & 4) ? 1 : 0;
    return o;
}

// CarState -> wire (CarW). Scales: position raw float; rotation unit
// quaternion *32767; velocity 1 cm/s steps; angular velocity 0.001 rad/s
// steps; nitro 0..255. Rotation is normalised and sign-flipped so w >= 0
// (q and -q are the same rotation), which makes the encoding unique.
void packCar(const CarState &in, drifto_CarW &out)
{
    out.playerId = in.playerId;
    out.lastInputTick = in.lastInputTick;
    out.px = in.position.x;
    out.py = in.position.y;
    out.pz = in.position.z;
    // normalise and keep w >= 0 (q and -q are the same rotation)
    float x = in.rotation.x, y = in.rotation.y, z = in.rotation.z, w = in.rotation.w;
    float l = std::sqrt(x * x + y * y + z * z + w * w);
    if (l < 1e-6f) { x = y = z = 0; w = 1; l = 1; }
    float sgn = w < 0 ? -1.f : 1.f;
    out.qx = q16(sgn * x / l, 32767.f);
    out.qy = q16(sgn * y / l, 32767.f);
    out.qz = q16(sgn * z / l, 32767.f);
    out.qw = q16(sgn * w / l, 32767.f);
    out.vx = q16(in.velocity.x, 100.f);
    out.vy = q16(in.velocity.y, 100.f);
    out.vz = q16(in.velocity.z, 100.f);
    out.wx = q16(in.angularVelocity.x, 1000.f);
    out.wy = q16(in.angularVelocity.y, 1000.f);
    out.wz = q16(in.angularVelocity.z, 1000.f);
    out.driftScore = in.driftScore;
    out.distance = in.distance;
    out.trackSection = in.trackSection;
    out.flags = in.flags;
    out.nitro = qu8(in.nitro);
    out.place = in.place;
    out.eliminatedOrder = in.eliminatedOrder;
}

// Inverse of packCar; the quaternion is re-normalised to remove quantization drift.
CarState unpackCar(const drifto_CarW &in)
{
    CarState o;
    o.playerId = in.playerId;
    o.lastInputTick = in.lastInputTick;
    o.position = {in.px, in.py, in.pz};
    float x = in.qx / 32767.f, y = in.qy / 32767.f, z = in.qz / 32767.f, w = in.qw / 32767.f;
    float l = std::sqrt(x * x + y * y + z * z + w * w);
    if (l < 1e-6f) { x = y = z = 0; w = 1; l = 1; }
    o.rotation.x = x / l;
    o.rotation.y = y / l;
    o.rotation.z = z / l;
    o.rotation.w = w / l;
    o.velocity = {in.vx / 100.f, in.vy / 100.f, in.vz / 100.f};
    o.angularVelocity = {in.wx / 1000.f, in.wy / 1000.f, in.wz / 1000.f};
    o.driftScore = in.driftScore;
    o.distance = in.distance;
    o.trackSection = in.trackSection;
    o.flags = in.flags;
    o.nitro = in.nitro / 255.f;
    o.place = in.place;
    o.eliminatedOrder = in.eliminatedOrder;
    return o;
}

// Steady (never goes backwards) clock; only differences are meaningful.
uint64_t monoUs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Process-wide seeded generator (guarded by a mutex); zero is excluded so 0
// can mean 'unset' for sessions and tokens. mt19937_64 (not a CSPRNG).
uint64_t randomU64()
{
    static std::mutex mu;
    static std::mt19937_64 rng = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seq);
    }();
    std::lock_guard<std::mutex> lk(mu);
    uint64_t v;
    do v = rng(); while (v == 0);
    return v;
}

// strncpy-like but always NUL-terminated and truncating safely (fixed IDL char arrays).
void copyStr(char *dst, size_t cap, const std::string &src)
{
    if (cap == 0) return;
    size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

} // namespace wire

namespace dds {

// Built-in copies of net/config/*.xml, used when the file can't be found
// (e.g. the binary is started from another directory). Keep in sync.
// Placeholders: @HOST@/@PORT@ (peer to connect to), @EXTERNAL@ (address the
// server advertises), @BASE@ (UDP port base), @MULTICAST@ and @PEERS@ (LAN).
static const std::map<std::string, std::string> kBuiltinTemplates = {
    {"tcp_client.xml",
     "<CycloneDDS><Domain id=\"any\">"
     "<General><Transport>tcp</Transport><AllowMulticast>false</AllowMulticast></General>"
     "<TCP><Port>-1</Port><AlwaysUsePeeraddrForUnicast>true</AlwaysUsePeeraddrForUnicast></TCP>"
     "<Discovery><ParticipantIndex>none</ParticipantIndex><SPDPInterval>2s</SPDPInterval>"
     "<Peers><Peer address=\"@HOST@:@PORT@\"/></Peers></Discovery>"
     "</Domain></CycloneDDS>"},
    {"tcp_server.xml",
     "<CycloneDDS><Domain id=\"any\">"
     "<General><Transport>tcp</Transport><AllowMulticast>false</AllowMulticast>"
     "<ExternalNetworkAddress>@EXTERNAL@</ExternalNetworkAddress></General>"
     "<TCP><Port>@PORT@</Port><AlwaysUsePeeraddrForUnicast>true</AlwaysUsePeeraddrForUnicast></TCP>"
     "<Discovery><ParticipantIndex>none</ParticipantIndex></Discovery>"
     "</Domain></CycloneDDS>"},
    {"udp_client.xml",
     "<CycloneDDS><Domain id=\"any\">"
     "<General><Transport>udp</Transport><AllowMulticast>false</AllowMulticast></General>"
     "<Discovery><ParticipantIndex>none</ParticipantIndex><SPDPInterval>1s</SPDPInterval>"
     "<Peers><Peer address=\"@HOST@:@PORT@\"/></Peers></Discovery>"
     "</Domain></CycloneDDS>"},
    {"lan.xml",
     "<CycloneDDS><Domain id=\"any\">"
     "<General><Transport>udp</Transport><AllowMulticast>@MULTICAST@</AllowMulticast></General>"
     "<Discovery><ParticipantIndex>auto</ParticipantIndex><MaxAutoParticipantIndex>20</MaxAutoParticipantIndex>"
     "<SPDPInterval>1s</SPDPInterval>@PEERS@</Discovery>"
     "</Domain></CycloneDDS>"},
    {"udp_server.xml",
     "<CycloneDDS><Domain id=\"any\">"
     "<General><Transport>udp</Transport><AllowMulticast>false</AllowMulticast>"
     "<ExternalNetworkAddress>@EXTERNAL@</ExternalNetworkAddress></General>"
     "<Discovery><ParticipantIndex>0</ParticipantIndex><Ports><Base>@BASE@</Base></Ports></Discovery>"
     "</Domain></CycloneDDS>"},
};

// Asks the OS which local address would be used to reach a public host (no
// packet is sent). Loopback, link-local and 0.0.0.0 are treated as 'none'.
std::string defaultRouteIPv4()
{
    // connect() on a UDP socket sends nothing; it only makes the OS pick the
    // route (and so the local address) it would use to reach that host.
    std::string ip;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return ip;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    bool ok = s != INVALID_SOCKET;
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    bool ok = s >= 0;
#endif
    if (ok)
    {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
        sockaddr_in me{};
        socklen_t len = sizeof me;
        char buf[INET_ADDRSTRLEN] = {};
        if (connect(s, (const sockaddr *)&dst, sizeof dst) == 0 && getsockname(s, (sockaddr *)&me, &len) == 0 &&
            inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf))
            ip = buf;
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
    }
#ifdef _WIN32
    WSACleanup();
#endif
    // no route (offline) or a useless address: let CycloneDDS choose
    if (ip == "0.0.0.0" || ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0) ip.clear();
    return ip;
}

// Returns the final CycloneDDS XML: file from configDir (else the built-in
// template), comments stripped, optional <Interfaces> pin injected into
// <General>, then @KEY@ placeholders substituted from vars.
std::string loadConfig(const std::string &configDir, const std::string &file,
                       const std::map<std::string, std::string> &vars, const std::string &traceTag,
                       const std::string &autoInterface)
{
    (void)traceTag; // (CycloneDDS 0.10.4's own <Tracing> aborts with a fortify
                    // "buffer overflow" in this Debian build, so no trace hook)
    std::string text;
    std::ifstream f(configDir + "/" + file);
    if (f)
    {
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    else
    {
        auto it = kBuiltinTemplates.find(file);
        if (it != kBuiltinTemplates.end()) text = it->second;
    }
    // strip XML comments (keeps the templates readable, the parser happy)
    for (size_t a; (a = text.find("<!--")) != std::string::npos;)
    {
        size_t b = text.find("-->", a);
        text.erase(a, b == std::string::npos ? std::string::npos : b + 3 - a);
    }
    // DRIFTO_DDS_INTERFACE=<name or IPv4>[,...]: pin the network interface(s)
    // (machines with VPN adapters -- Tailscale, Radmin, Hyper-V -- may get
    // the wrong one picked automatically, which breaks LAN multicast).
    // Without it, autoInterface (if given) is used instead.
    const char *ifs = getenv("DRIFTO_DDS_INTERFACE");
    if (!(ifs && *ifs) && !autoInterface.empty()) ifs = autoInterface.c_str();
    if (ifs)
    {
        std::string list = ifs, xml;
        size_t pos = 0;
        while (pos <= list.size())
        {
            size_t comma = list.find(',', pos);
            std::string one = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? list.size() + 1 : comma + 1;
            if (one.empty()) continue;
            bool ip = one.find_first_not_of("0123456789.") == std::string::npos;
            xml += std::string("<NetworkInterface ") + (ip ? "address" : "name") + "=\"" + one + "\"/>";
        }
        size_t g = text.find("<General>");
        if (!xml.empty() && g != std::string::npos) text.insert(g + 9, "<Interfaces>" + xml + "</Interfaces>");
    }
    for (const auto &kv : vars)
    {
        std::string key = "@" + kv.first + "@";
        for (size_t p; (p = text.find(key)) != std::string::npos;) text.replace(p, key.size(), kv.second);
    }
    return text;
}

namespace {
// Table entry per DDS domain id. refs counts NetClients using it; xml is the
// config it was created with.
struct DomainRef
{
    dds_entity_t handle = 0;
    int refs = 0;
    bool persistent = false;
    std::string xml;
};
std::mutex g_domMu;
std::map<uint32_t, DomainRef> g_domains;

std::atomic<bool> g_wedged{false};

// dds_delete on a detached helper thread, giving up after 3 s and setting the
// wedge flag.
void deleteDomainBounded(uint32_t domainId, dds_entity_t h)
{
    // CycloneDDS 0.10.4 can hang tearing a domain down (TCP: self-deadlock in
    // rtps_fini -> remove_from_addrset; UDP with non-working multicast: the
    // multicast receive thread never wakes up). Delete on a helper thread
    // and stop waiting after 3 s.
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> f = done->get_future();
    std::thread([h, done] {
        dds_delete(h);
        done->set_value();
    }).detach();
    if (f.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
    {
        g_wedged = true;
        fprintf(stderr, "[net] warning: CycloneDDS did not finish deleting domain %u (known 0.10.4 teardown bug); "
                        "continuing without it\n",
                domainId);
    }
}
} // namespace

// Creates the domain on first use, then just bumps the reference count. A
// persistent domain left over from earlier is recreated if the XML changed
// (e.g. a different peer list). Thread-safe via g_domMu.
bool acquireDomain(uint32_t domainId, const std::string &xml, std::string *err, bool persistent)
{
    std::lock_guard<std::mutex> lk(g_domMu);
    DomainRef &d = g_domains[domainId];
    if (d.refs == 0 && d.handle != 0 && d.xml != xml)
    {
        deleteDomainBounded(domainId, d.handle); // persistent domain kept from earlier, but config changed
        d.handle = 0;
    }
    if (d.handle == 0)
    {
        dds_entity_t h = dds_create_domain(domainId, xml.c_str());
        if (h < 0)
        {
            if (err)
                *err = "dds_create_domain(" + std::to_string(domainId) + ") failed: " + dds_strretcode(h) +
                       " (bad config XML? port in use / needs privileges?)";
            g_domains.erase(domainId);
            return false;
        }
        d.handle = h;
        d.xml = xml;
        d.persistent = persistent;
    }
    d.refs++;
    return true;
}

// Drops one reference; the last user deletes the domain (outside the lock,
// bounded) unless it is persistent.
void releaseDomain(uint32_t domainId)
{
    dds_entity_t h = 0;
    {
        std::lock_guard<std::mutex> lk(g_domMu);
        auto it = g_domains.find(domainId);
        if (it == g_domains.end() || it->second.refs <= 0) return;
        if (--it->second.refs > 0) return;
        if (it->second.persistent) return; // kept until process exit (see acquireDomain)
        h = it->second.handle;
        g_domains.erase(it);
    }
    deleteDomainBounded(domainId, h);
}

// Same technique as deleteDomainBounded for arbitrary entities.
bool deleteEntityBounded(dds_entity_t e, int timeoutMs)
{
    if (e <= 0) return true;
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> f = done->get_future();
    std::thread([e, done] {
        dds_delete(e);
        done->set_value();
    }).detach();
    if (f.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready) return true;
    g_wedged = true;
    return false;
}

bool wedged() { return g_wedged.load(); }

// Non-throwing existence check.
static bool fileExists(const std::string &p)
{
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

// With security: verifies all five files exist, then sets the dds.sec.*
// properties (authentication, access control, crypto plugins loaded by name).
// Returns the participant or a negative DDS code with *err filled in.
dds_entity_t createParticipant(uint32_t domainId, const Security &sec, std::string *err)
{
    dds_qos_t *qos = dds_create_qos();
    if (sec.enabled)
    {
        std::string dir = sec.certDir.empty() ? "." : sec.certDir;
        const std::string files[] = {dir + "/ca.pem", dir + "/" + sec.identity + ".pem", dir + "/" + sec.identity + ".key",
                                     dir + "/governance.p7s", dir + "/permissions.p7s"};
        for (const auto &f : files)
            if (!fileExists(f))
            {
                if (err) *err = "DDS Security file missing: " + f + " (run 'make certs', or run insecure)";
                dds_delete_qos(qos);
                return -1;
            }
        auto uri = [](const std::string &p) { return "file:" + p; };
        dds_qset_prop(qos, "dds.sec.auth.identity_ca", uri(files[0]).c_str());
        dds_qset_prop(qos, "dds.sec.auth.identity_certificate", uri(files[1]).c_str());
        dds_qset_prop(qos, "dds.sec.auth.private_key", uri(files[2]).c_str());
        dds_qset_prop(qos, "dds.sec.auth.library.path", "dds_security_auth");
        dds_qset_prop(qos, "dds.sec.auth.library.init", "init_authentication");
        dds_qset_prop(qos, "dds.sec.auth.library.finalize", "finalize_authentication");
        dds_qset_prop(qos, "dds.sec.access.permissions_ca", uri(files[0]).c_str());
        dds_qset_prop(qos, "dds.sec.access.governance", uri(files[3]).c_str());
        dds_qset_prop(qos, "dds.sec.access.permissions", uri(files[4]).c_str());
        dds_qset_prop(qos, "dds.sec.access.library.path", "dds_security_ac");
        dds_qset_prop(qos, "dds.sec.access.library.init", "init_access_control");
        dds_qset_prop(qos, "dds.sec.access.library.finalize", "finalize_access_control");
        dds_qset_prop(qos, "dds.sec.crypto.library.path", "dds_security_crypto");
        dds_qset_prop(qos, "dds.sec.crypto.library.init", "init_crypto");
        dds_qset_prop(qos, "dds.sec.crypto.library.finalize", "finalize_crypto");
    }
    dds_entity_t p = dds_create_participant(domainId, qos, nullptr);
    // CycloneDDS 0.10.4 + security: creating a second secure participant in a
    // domain while another one in the same process is mid-handshake can fail
    // spuriously ("get_identity_token: Invalid handle"). Only happens with
    // several NetClients per process (tests); a short retry fixes it.
    for (int attempt = 0; p < 0 && sec.enabled && attempt < 10; attempt++)
    {
        dds_sleepfor(DDS_MSECS(100 + 50 * attempt));
        p = dds_create_participant(domainId, qos, nullptr);
    }
    dds_delete_qos(qos);
    if (p < 0 && err)
    {
        *err = std::string("dds_create_participant(domain ") + std::to_string(domainId) + ") failed: " + dds_strretcode(p);
        if (sec.enabled)
            *err += " -- DDS Security could not initialise (plugins libdds_security_*.so, certs, "
                    "governance/permissions signatures); see the CycloneDDS log above, or run insecure";
    }
    return p;
}

// Gameplay QoS: no retransmission, keep only the newest 'depth' samples.
dds_qos_t *qosBestEffort(int depth)
{
    dds_qos_t *q = dds_create_qos();
    dds_qset_reliability(q, DDS_RELIABILITY_BEST_EFFORT, 0);
    dds_qset_history(q, DDS_HISTORY_KEEP_LAST, depth);
    dds_qset_durability(q, DDS_DURABILITY_VOLATILE);
    return q;
}

// Control QoS: retransmit until acknowledged (blocking up to 100 ms on a full
// history). transientLocal keeps the history for readers that join later.
dds_qos_t *qosReliable(int depth, bool transientLocal)
{
    dds_qos_t *q = dds_create_qos();
    dds_qset_reliability(q, DDS_RELIABILITY_RELIABLE, DDS_MSECS(100));
    dds_qset_history(q, DDS_HISTORY_KEEP_LAST, depth);
    dds_qset_durability(q, transientLocal ? DDS_DURABILITY_TRANSIENT_LOCAL : DDS_DURABILITY_VOLATILE);
    return q;
}

// Partition = the room/session filter: writers and readers only match when
// their partitions overlap.
dds_entity_t createPublisher(dds_entity_t participant, const std::string &partition)
{
    dds_qos_t *q = dds_create_qos();
    const char *parts[1] = {partition.c_str()};
    dds_qset_partition(q, 1, parts);
    dds_entity_t p = dds_create_publisher(participant, q, nullptr);
    dds_delete_qos(q);
    return p;
}

dds_entity_t createSubscriber(dds_entity_t participant, const std::vector<std::string> &partitions)
{
    dds_qos_t *q = dds_create_qos();
    std::vector<const char *> parts;
    for (const auto &s : partitions) parts.push_back(s.c_str());
    dds_qset_partition(q, (uint32_t)parts.size(), parts.data());
    dds_entity_t s = dds_create_subscriber(participant, q, nullptr);
    dds_delete_qos(q);
    return s;
}

#ifdef _WIN32
// Windows variant: no /proc, so probe the low socket-handle range and set
// TCP_NODELAY (Nagle off) on every TCP socket that lacks it.
int tuneTcpSockets()
{
    // No /proc on Windows: socket handles are small multiples of 4, so probe
    // the low handle range (getsockopt on a non-socket fails fast with
    // WSAENOTSOCK). Rate-limited: at most once a second.
    static uint64_t lastUs = 0;
    uint64_t now = wire::monoUs();
    if (now - lastUs < 1000000) return 0;
    lastUs = now;
    int changed = 0;
    for (uintptr_t h = 4; h < 0x4000; h += 4)
    {
        SOCKET s = (SOCKET)h;
        int type = 0;
        int len = sizeof type;
        if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &len) != 0 || type != SOCK_STREAM) continue;
        struct sockaddr_storage ss;
        len = sizeof ss;
        if (getsockname(s, (struct sockaddr *)&ss, &len) != 0) continue;
        if (ss.ss_family != AF_INET && ss.ss_family != AF_INET6) continue;
        BOOL cur = FALSE;
        len = sizeof cur;
        if (getsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&cur, &len) == 0 && cur) continue;
        BOOL one = TRUE;
        if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one) == 0) changed++;
    }
    return changed;
}
#else
// POSIX variant: walks /proc/self/fd, applying TCP_NODELAY to every TCP
// (IPv4/IPv6 stream) socket that lacks it. Returns the number changed.
int tuneTcpSockets()
{
    int changed = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return 0;
    while (struct dirent *e = readdir(d))
    {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int fd = atoi(e->d_name);
        if (fd == dirfd(d)) continue;
        int type = 0;
        socklen_t len = sizeof type;
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) != 0 || type != SOCK_STREAM) continue;
        struct sockaddr_storage ss;
        len = sizeof ss;
        if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) continue;
        if (ss.ss_family != AF_INET && ss.ss_family != AF_INET6) continue;
        int cur = 0;
        len = sizeof cur;
        if (getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &cur, &len) == 0 && cur) continue;
        int one = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0) changed++;
    }
    closedir(d);
    return changed;
}
#endif

// Current number of matched readers; used to hold back reliable sends until
// the peer has been discovered.
int matchedReaders(dds_entity_t writer)
{
    dds_publication_matched_status_t st;
    if (dds_get_publication_matched_status(writer, &st) < 0) return 0;
    return (int)st.current_count;
}

} // namespace dds
} // namespace net
