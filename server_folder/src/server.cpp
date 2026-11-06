// server.cpp -- see server_folder/include/server.h.
#include "../include/server.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace srv {

// Skips argv[0]; remaining tokens are kept verbatim and start out "unused".
Args::Args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) args_.push_back(argv[i]);
    used_.assign(args_.size(), false);
}

// True if the exact switch is present; marks it used.
bool Args::has(const std::string &flag) const
{
    for (size_t i = 0; i < args_.size(); i++)
        if (args_[i] == flag)
        {
            used_[i] = true;
            return true;
        }
    return false;
}

// Value of "--flag value" or "--flag=value"; 'def' when absent. Marks tokens used.
std::string Args::str(const std::string &flag, const std::string &def) const
{
    // form 1: "--flag value" (needs a following token)
    for (size_t i = 0; i + 1 < args_.size(); i++)
        if (args_[i] == flag)
        {
            used_[i] = used_[i + 1] = true;
            return args_[i + 1];
        }
    for (size_t i = 0; i < args_.size(); i++) // "--flag=value"
        if (args_[i].rfind(flag + "=", 0) == 0)
        {
            used_[i] = true;
            return args_[i].substr(flag.size() + 1);
        }
    return def;
}

// Integer flag; 'def' when the flag is missing or empty.
int Args::num(const std::string &flag, int def) const
{
    std::string s = str(flag, "");
    return s.empty() ? def : atoi(s.c_str());
}

// Call after all has()/str()/num() lookups: prints every argument nobody asked for.
void Args::warnUnknown() const
{
    for (size_t i = 0; i < args_.size(); i++)
        if (!used_[i]) fprintf(stderr, "warning: ignoring unknown argument '%s'\n", args_[i].c_str());
}

// Flags written by the signal handler and polled by the main loops; atomics
// make this safe without locks.
static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_usr1{false};

// Signal handler: SIGUSR1 flips g_usr1 (game_server uses it to simulate a UDP
// block at runtime); any other handled signal requests shutdown.
static void onSignal(int sig)
{
    if (sig == SIGUSR1)
        g_usr1 = !g_usr1;
    else
        g_quit = true;
}

void installSignals()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

bool quitRequested() { return g_quit.load(); }
bool usr1Toggle() { return g_usr1.load(); }

// Program tag shown in every log line (set once at startup, before any logging).
static const char *g_tag = "server";
void setLogTag(const char *tag) { g_tag = tag; }

// printf-style log line: "HH:MM:SS.mmm [tag] message" on stdout, flushed per line.
// Messages longer than 1023 bytes are truncated.
void log(const char *fmt, ...)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stdout, "%s.%03d [%s] %s\n", ts, (int)(tv.tv_usec / 1000), g_tag, msg);
    fflush(stdout);
}

// First non-loopback IPv4 of any interface; falls back to 127.0.0.1 if enumeration
// fails or none exists. Used as the default public address of a remote game server.
std::string primaryIPv4()
{
    std::string out = "127.0.0.1";
    struct ifaddrs *ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return out;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next)
    {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        auto *sin = (struct sockaddr_in *)i->ifa_addr;
        // skip 127.0.0.0/8
        if ((ntohl(sin->sin_addr.s_addr) >> 24) == 127) continue;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf);
        out = buf;
        break;
    }
    freeifaddrs(ifs);
    return out;
}

// True for "localhost", any 127.x.x.x address or "::1" (string match, no resolution).
bool isLoopbackHost(const std::string &host)
{
    return host == "localhost" || host.rfind("127.", 0) == 0 || host == "::1";
}

// Resolves a name or dotted quad to a dotted-quad IPv4 string; "" on failure.
std::string resolveIPv4(const std::string &host)
{
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return "";
    char buf[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, buf, sizeof buf);
    freeaddrinfo(res);
    return buf;
}

// "host[:port]" -> host/port. Port is left untouched when absent; a string with more
// than one ':' is treated as a bare host (IPv6 literal).
void splitHostPort(const std::string &in, std::string &host, int &port)
{
    size_t c = in.rfind(':');
    if (c != std::string::npos && in.find(':') == c)
    {
        host = in.substr(0, c);
        port = atoi(in.c_str() + c + 1);
    }
    else
        host = in;
}

} // namespace srv
