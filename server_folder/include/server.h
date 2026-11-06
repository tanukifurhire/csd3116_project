// server.h -- small helpers shared by api_server and game_server
// (server_folder/src/api_server.cpp, game_server.cpp). The DDS plumbing they
// share with NetClient lives in net/src/net_common.h.
// Contents: command-line parsing (Args), signal-driven shutdown flags, a
// timestamped logger and a few IPv4/host helpers. Implemented in server.cpp.
// Both servers are single-threaded event loops; the only state shared with
// another context is the pair of std::atomic flags set by the signal handler.
// Nothing here depends on DDS.
#pragma once

#include <atomic>
#include <cstdarg>
#include <string>
#include <vector>

namespace srv {

// Minimal "--flag value" / "--switch" command line parser.
// Command-line view. Lookups are const but mark the matching argv entries as
// used (used_ is mutable) so warnUnknown() can report leftovers after init().
// Accepted forms: "--flag value", "--flag=value" (str/num) and bare "--switch" (has).
class Args
{
public:
    Args(int argc, char **argv);
    bool has(const std::string &flag) const;
    std::string str(const std::string &flag, const std::string &def) const;
    // atoi() of the flag's value; 'def' if absent or empty (junk parses as 0).
    int num(const std::string &flag, int def) const;
    // Flags that were given but never queried (typos) -> printed by warnUnknown().
    void warnUnknown() const;

private:
    std::vector<std::string> args_;
    mutable std::vector<bool> used_;
};

// SIGINT/SIGTERM set quitRequested(); SIGUSR1 toggles usr1Toggle().
// Installs handlers for SIGINT/SIGTERM (quit), SIGUSR1 (toggle) and ignores
// SIGPIPE so a broken TCP peer cannot kill the server.
void installSignals();
bool quitRequested();
bool usr1Toggle();

// printf-style log line with a timestamp and the program tag.
void setLogTag(const char *tag);
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// First non-loopback IPv4 address of this machine, or "127.0.0.1".
std::string primaryIPv4();
bool isLoopbackHost(const std::string &host);
// DNS name or dotted quad -> dotted quad ("" if it can't be resolved).
std::string resolveIPv4(const std::string &host);

// Splits "host:port" (port optional).
void splitHostPort(const std::string &in, std::string &host, int &port);

} // namespace srv
