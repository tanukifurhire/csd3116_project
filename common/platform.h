// platform.h -- tiny header-only portability layer shared by every module
// (Linux/g++ and Windows/MSVC). Keeps <windows.h> out of everyone's includes.
//
//   platform::executableDir()        directory of the running executable (UTF-8)
//   platform::resolveRuntimePath(p)  p if it exists relative to the cwd, else
//                                    <exe dir>/p if that exists, else p
//   platform::configDir()            per-user settings dir: %APPDATA%\drifto on
//                                    Windows, $XDG_CONFIG_HOME/drifto or
//                                    ~/.config/drifto elsewhere ({} if unknown)
//   platform::strcasecmp(a, b)       case-insensitive compare (strcasecmp/_stricmp)
//   platform::strncasecmp(a, b, n)
//   platform::setEnv(name, value)    setenv / _putenv_s
//   platform::monotonicSeconds()     steady clock, seconds
//   DRIFTO_PRINTF_FORMAT(f, a)       __attribute__((format(printf, f, a))) or nothing
#pragma once

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <stdlib.h> // _get_pgmptr, _putenv_s
#include <string.h> // _stricmp
#else
#include <strings.h>
#include <unistd.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define DRIFTO_PRINTF_FORMAT(fmtIndex, firstArg) __attribute__((format(printf, fmtIndex, firstArg)))
#else
#define DRIFTO_PRINTF_FORMAT(fmtIndex, firstArg)
#endif

namespace platform {

inline std::string pathToUtf8(const std::filesystem::path &p)
{
#if defined(__cpp_lib_char8_t)
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
#else
    return p.u8string();
#endif
}

inline std::filesystem::path executablePath()
{
#ifdef _WIN32
    // _get_pgmptr, not _get_wpgmptr: the wide one is only initialised for
    // wmain() programs and otherwise fails fast (0xC0000409).
    char *a = nullptr;
    if (_get_pgmptr(&a) == 0 && a && *a) return std::filesystem::path(a);
    return {};
#else
    std::error_code ec;
    std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : p;
#endif
}

inline std::string executableDir()
{
    std::filesystem::path p = executablePath();
    return p.empty() ? std::string() : pathToUtf8(p.parent_path());
}

inline std::string resolveRuntimePath(const std::string &p)
{
    std::error_code ec;
    if (p.empty() || std::filesystem::path(p).is_absolute() || std::filesystem::exists(p, ec)) return p;
    std::string dir = executableDir();
    if (!dir.empty())
    {
        std::filesystem::path alt = std::filesystem::path(dir) / p;
        if (std::filesystem::exists(alt, ec)) return pathToUtf8(alt);
    }
    return p;
}

inline std::filesystem::path configDir()
{
#ifdef _WIN32
    const char *appdata = std::getenv("APPDATA");
    if (appdata && *appdata) return std::filesystem::path(appdata) / "drifto";
    return {};
#else
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    if (xdg && *xdg) return std::filesystem::path(xdg) / "drifto";
    if (home && *home) return std::filesystem::path(home) / ".config" / "drifto";
    return {};
#endif
}

inline int strcasecmp(const char *a, const char *b)
{
#ifdef _WIN32
    return ::_stricmp(a, b);
#else
    return ::strcasecmp(a, b);
#endif
}

inline int strncasecmp(const char *a, const char *b, size_t n)
{
#ifdef _WIN32
    return ::_strnicmp(a, b, n);
#else
    return ::strncasecmp(a, b, n);
#endif
}

inline void setEnv(const char *name, const char *value)
{
#ifdef _WIN32
    ::_putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

inline double monotonicSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace platform
