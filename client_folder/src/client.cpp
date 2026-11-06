// Application layer of the drifto client: command line, window and main loop,
// screen state machine (menus, lobby, races), input, networking glue, HUD and
// sound glue. Gameplay rules live in game/, rendering in gfx/, networking in
// net/; this file wires them together.
// Main loop (Client::Run, single thread): poll window events, poll network
// events, then run the simulation at a fixed 60 Hz (kFixedDt) with an
// accumulator, and render once per iteration. fixedTick() advances the active
// MultiplayerSession (prediction / host simulation); frame() draws using
// alpha = accumulator / kFixedDt, the fraction of a tick elapsed, so the
// picture is interpolated between the last two ticks. Hitches are clamped to
// 0.25 s and at most 8 ticks run per frame (spiral-of-death guard).
// Modes: offline races run a local host (LoopbackTransport + bots) through the
// same MultiplayerSession as online/LAN races, so there is one code path.
// Headless mode (--headless) skips window, GL and rendering (bots, soak tests)
// but still runs the same simulation, network and audio glue.
// Screens form a state machine (Screen): MainMenu, Login, RoomList, CreateRoom,
// Lobby, OnlineRace, OfflineRace, Message. The menu background is an
// "attract" run: a bot driving a random track.
// Everything GL-related goes through gfx::Renderer; GLFW callbacks only
// forward to onChar/onKey.
// client.cpp -- window, fixed-step loop, menus, offline/online races, HUD.
#include "client_folder/include/client.h"

#define GLFW_INCLUDE_NONE // nothing outside gfx/ touches GL
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_easy_font.h" // only for measuring text (gfx draws it)
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "common/platform.h"
#include "game/include/autopilot.h"
#include "game/include/rng.h"

namespace client {

namespace {

// HUD palette (linear RGB).
const dm::Vec3 kWhite{1, 1, 1};
const dm::Vec3 kGrey{0.7f, 0.72f, 0.76f};
const dm::Vec3 kDim{0.5f, 0.52f, 0.56f};
const dm::Vec3 kYellow{1.0f, 0.82f, 0.25f};
const dm::Vec3 kRed{1.0f, 0.35f, 0.3f};
const dm::Vec3 kGreen{0.45f, 0.95f, 0.5f};
const dm::Vec3 kAccent{0.95f, 0.45f, 0.25f};

#if defined(__GNUC__)
// printf-style formatting into a std::string (messages up to 255 chars).
std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
#endif
std::string fmt(const char *f, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// Wall clock in seconds (works without GLFW, e.g. --headless).
double nowSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Non-deterministic track seed: random_device mixed with the clock.
uint32_t randomSeed()
{
    std::random_device rd;
    uint64_t t = static_cast<uint64_t>(nowSeconds() * 1e6);
    return static_cast<uint32_t>(game::splitmix64((static_cast<uint64_t>(rd()) << 32) ^ t));
}

// Render view of a car from two consecutive sim states (offline attract run and
// menus, which have no MultiplayerSession); alpha blends them.
game::CarView viewOf(const game::CarSimState &a, const game::CarSimState &b, float alpha, int32_t id)
{
    game::CarView v;
    v.playerId = id;
    v.local = true;
    v.position = dm::lerp(a.position, b.position, alpha);
    v.rotation = dm::slerp(a.rotation, b.rotation, alpha);
    v.velocity = dm::lerp(a.velocity, b.velocity, alpha);
    v.section = b.section;
    v.distance = b.distance;
    v.driftAngle = b.driftAngle;
    v.nitro = b.nitro;
    v.alive = b.alive;
    v.drifting = b.drifting;
    v.nitroActive = b.nitroActive;
    return v;
}

// Label for the network panel.
const char *transportName(net::Transport t)
{
    switch (t)
    {
    case net::Transport::Udp: return "UDP";
    case net::Transport::Tcp443: return "TCP/443";
    case net::Transport::None: break;
    }
    return "--";
}

} // namespace

// ===========================================================================
// command line
// ===========================================================================

// Help text printed for --help or a bad command line.
const char *usageText()
{
    return "usage: drifto [--offline] [--api HOST[:PORT]] [--cert N] [--transport auto|udp|tcp443]\n"
           "              [--skybox NAME] [--assets DIR] [--insecure] [--seed N] [--bots N] [--size WxH]\n"
           "              [--low] [--demo] [--screenshot FILE.png] [--shot-delay S]\n"
           "              [--lan] [--lan-peer IP[:PORT]]... [--login NAME[:PASS]] [--room NAME] [--autostart N]\n"
           "              [--headless] [--fast] [--races N] [--duration S] [--fps N] [--mute] [--volume V]\n"
           "  --offline      start an offline elimination race against bots straight away\n"
           "  --bots N       offline opponents, 1..3 (default 3)\n"
           "  --api          online API service address (default: last used, else 127.0.0.1)\n"
           "  --lan          LAN mode: no servers, games are discovered on the local network\n"
           "  --lan-peer     extra LAN peer address when multicast is blocked (repeatable)\n"
           "  --cert N       use certs/client<N>.pem/.key (default 1)\n"
           "  --transport    game-service transport preference (default auto)\n"
           "  --skybox       e.g. SkyAfterNoon (default), SkyMidnight, SkySunset, ...\n"
           "  --assets       asset root produced by `make assets` (default assets)\n"
           "  --insecure     plain DDS without DDS Security (must match the servers)\n"
           "  --seed N       offline track seed (default random)\n"
           "  --low          cheaper rendering (no shadows / tree outlines, fewer trees)\n"
           "  --demo         our car is driven by the bot AI (offline, or online with --login)\n"
           "  --screenshot   save a PNG after ~7 s and quit (menu, or a race with --demo/--offline)\n"
           "  --login/--room/--autostart  automation (online or --lan): log in, join or create the room, ready up;\n"
           "                 the host starts once N players are in (use with --demo for bots)\n"
           "  --headless     no window / GL: bot client or soak run (implies --demo); --fast runs\n"
           "                 offline races as fast as the CPU allows; --races N / --duration S end it\n"
           "  --mute / --volume V   sound off / master volume 0..1 (remembered)\n"
           "controls: auto-throttle, A/D or Left/Right steer, Space nitro (charges while drifting), Esc back\n";
}

// Fill Options from argv. Returns false on a bad argument (error set) or on
// --help (error left empty). Some flags imply others (--demo => --offline,
// --headless => --demo). `next` fetches an option's value or reports it missing.
bool parseArgs(int argc, char **argv, Options &o, std::string &error)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&](const char *what) -> const char * {
            if (i + 1 >= argc) { error = std::string(a) + " needs " + what; return nullptr; }
            return argv[++i];
        };
        if (a == "--offline") o.offline = true;
        else if (a == "--insecure") o.insecure = true;
        else if (a == "--demo") { o.demo = true; o.offline = true; }
        else if (a == "--low") o.lowQuality = true;
        else if (a == "--headless") { o.headless = true; o.demo = true; }
        else if (a == "--fast") o.fast = true;
        else if (a == "--mute") o.mute = true;
        else if (a == "--volume")
        {
            const char *v = next("0..1");
            if (!v) return false;
            o.volume = dm::clampf(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
        }
        else if (a == "--fps") { const char *v = next("a frame cap"); if (!v) return false; o.fpsCap = std::atoi(v); }
        else if (a == "--races") { const char *v = next("a count"); if (!v) return false; o.races = std::atoi(v); }
        else if (a == "--duration") { const char *v = next("seconds"); if (!v) return false; o.duration = static_cast<float>(std::atof(v)); }
        else if (a == "--help" || a == "-h") { error.clear(); return false; }
        else if (a == "--api") { const char *v = next("a host"); if (!v) return false; o.apiHost = v; o.apiGiven = true; }
        else if (a == "--lan") o.lan = true;
        else if (a == "--lan-peer") { const char *v = next("IP[:PORT]"); if (!v) return false; o.lanPeers.push_back(v); }
        else if (a == "--skybox") { const char *v = next("a name"); if (!v) return false; o.skybox = v; }
        else if (a == "--assets") { const char *v = next("a directory"); if (!v) return false; o.assetDir = v; }
        else if (a == "--screenshot")
        {
            const char *v = next("a file name");
            if (!v) return false;
            o.screenshot = v;
        }
        else if (a == "--login")
        {
            const char *v = next("NAME:PASS");
            if (!v) return false;
            o.autoLogin = v; // NAME:PASS online, NAME alone (LAN, or an empty password)
            if (o.autoLogin.empty() || o.autoLogin[0] == ':') { error = "--login expects NAME[:PASS]"; return false; }
        }
        else if (a == "--room") { const char *v = next("a room name"); if (!v) return false; o.autoRoom = v; }
        else if (a == "--autostart") { const char *v = next("a player count"); if (!v) return false; o.autoStart = std::atoi(v); }
        else if (a == "--shot-delay") { const char *v = next("seconds"); if (!v) return false; o.shotDelay = static_cast<float>(std::atof(v)); }
        else if (a == "--bots")
        {
            const char *v = next("1..3");
            if (!v) return false;
            o.bots = std::atoi(v);
            if (o.bots < 1 || o.bots > 3) { error = "--bots must be 1..3"; return false; }
        }
        else if (a == "--cert")
        {
            const char *v = next("a number");
            if (!v) return false;
            o.certIndex = std::atoi(v);
            if (o.certIndex < 1) { error = "--cert must be >= 1"; return false; }
        }
        else if (a == "--seed")
        {
            const char *v = next("a number");
            if (!v) return false;
            o.seed = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
        }
        else if (a == "--size")
        {
            const char *v = next("WxH");
            if (!v || std::sscanf(v, "%dx%d", &o.width, &o.height) != 2 || o.width < 320 || o.height < 240)
            {
                error = "--size expects WxH, e.g. 1280x720";
                return false;
            }
        }
        else if (a == "--transport")
        {
            const char *v = next("auto|udp|tcp443");
            if (!v) return false;
            std::string t = v;
            if (t == "auto") o.transport = net::TransportPref::Auto;
            else if (t == "udp") o.transport = net::TransportPref::UdpOnly;
            else if (t == "tcp443" || t == "tcp") o.transport = net::TransportPref::Tcp443Only;
            else { error = "--transport must be auto, udp or tcp443"; return false; }
        }
        else
        {
            error = "unknown option " + a;
            return false;
        }
    }
    return true;
}

// ===========================================================================
// lifecycle
// ===========================================================================

// Fields are limited to what the server and UI accept. Remembered settings
// are loaded first; explicit command-line options win over them.
Client::Client(const Options &options) : opt_(options)
{
    nameField_.maxLen = net::kMaxNameLen;
    passField_.maxLen = 32;
    passField_.password = true;
    roomField_.maxLen = 16;
    apiField_.maxLen = 64;
    loadSettings();
    if (opt_.volume >= 0) volume_ = opt_.volume;
    if (opt_.lan) lanMode_ = true;
    apiField_.text = opt_.apiHost;
    // Relative asset dirs resolve against the cwd, else next to the executable
    // (Windows players often launch drifto.exe from Explorer or another dir).
    opt_.assetDir = platform::resolveRuntimePath(opt_.assetDir);
}

// ---------------------------------------------------------------------------
// remembered settings: <platform::configDir()>/settings.txt (mode, name, api, volume).
// Best effort: every failure is silent.
// ---------------------------------------------------------------------------

namespace {
// Per-user settings file, or an empty path if there is no config directory.
std::filesystem::path settingsPath()
{
    std::filesystem::path dir = platform::configDir(); // %APPDATA%\drifto or ~/.config/drifto
    return dir.empty() ? std::filesystem::path() : dir / "settings.txt";
}
} // namespace

// Read key=value lines; unknown keys are ignored and command-line values win
// (name only fills an empty field, api only without --api, volume only without --volume).
void Client::loadSettings()
{
    std::filesystem::path p = settingsPath();
    if (p.empty()) return;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line))
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "name" && nameField_.text.empty()) nameField_.text = v.substr(0, net::kMaxNameLen);
        else if (k == "api" && !opt_.apiGiven && !v.empty()) opt_.apiHost = v;
        else if (k == "volume" && opt_.volume < 0) volume_ = dm::clampf(static_cast<float>(std::atof(v.c_str())), 0.0f, 1.0f);
        else if (k == "mode")
            menuIndex_ = v == "online" ? 1 : (v == "lan" ? 2 : 0); // preselect only; --lan / the menu decide
    }
}

// Write the remembered settings. Automation runs never overwrite them.
void Client::saveSettings(const char *mode) const
{
    if (opt_.headless || !opt_.autoLogin.empty()) return; // bots must not overwrite the player's settings
    std::filesystem::path p = settingsPath();
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::trunc);
    if (!out) return;
    out << "mode=" << mode << "\n" << "name=" << nameField_.text << "\n" << "api=" << opt_.apiHost << "\n"
        << "volume=" << volume_ << "\n";
}

Client::~Client() { Shutdown(); }

// Create everything the client needs. Headless: network + session + silent
// audio only. Otherwise: GLFW window (OpenGL 3.3 core), input callbacks,
// renderer, skybox, world view, HUD gauges, audio, network objects, then the
// menu attract run and any automatic start (--offline, --login, --lan).
bool Client::Init()
{
    if (opt_.headless)
    {
        // No window, no renderer, no world view: simulation + network only.
        net_ = std::make_unique<net::NetClient>();
        transport_ = std::make_unique<NetClientTransport>(*net_);
        session_ = std::make_unique<game::MultiplayerSession>(*transport_);
        if (opt_.autoLogin.empty()) startOfflineRun();
        else
        {
            size_t c = opt_.autoLogin.find(':');
            nameField_.text = opt_.autoLogin.substr(0, std::min(c, static_cast<size_t>(net::kMaxNameLen)));
            passField_.text = c == std::string::npos ? std::string() : opt_.autoLogin.substr(c + 1);
            screen_ = Screen::Login;
            submitLogin();
            if (!netStarted_) { std::fprintf(stderr, "drifto: could not start networking\n"); return false; }
        }
        // Sound glue runs too (device-less engine: silent, but exercised).
        snd_ = std::make_unique<audio::Engine>();
        audio::Config acfg;
        acfg.noDevice = true;
        snd_->init(opt_.assetDir, acfg);
        startTime_ = nowSeconds();
        return true;
    }
    if (!glfwInit())
    {
        std::fprintf(stderr, "drifto: glfwInit failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    window_ = glfwCreateWindow(opt_.width, opt_.height, "Drifto", nullptr, nullptr);
    if (!window_)
    {
        std::fprintf(stderr, "drifto: could not create a GL 3.3 core window\n");
        glfwTerminate();
        return false;
    }
    // The callbacks are captureless lambdas (C API); they find the Client
    // through the window user pointer.
    glfwSetWindowUserPointer(window_, this);
    glfwSetCharCallback(window_, [](GLFWwindow *w, unsigned cp) {
        static_cast<Client *>(glfwGetWindowUserPointer(w))->onChar(cp);
    });
    glfwSetKeyCallback(window_, [](GLFWwindow *w, int key, int, int action, int mods) {
        static_cast<Client *>(glfwGetWindowUserPointer(w))->onKey(key, action, mods);
    });
    glfwSetWindowFocusCallback(window_, [](GLFWwindow *w, int focused) {
        Client *c = static_cast<Client *>(glfwGetWindowUserPointer(w));
        c->windowFocused_ = focused != 0;
        if (c->snd_) c->snd_->setMuted(c->opt_.mute || !c->windowFocused_);
    });

    renderer_ = std::make_unique<gfx::Renderer>();
    if (!renderer_->init(window_, opt_.assetDir))
    {
        std::fprintf(stderr, "drifto: renderer init failed\n");
        return false;
    }
    // Vsync on (the frame cap in Run() covers drivers that ignore it).
    glfwSwapInterval(1);
    if (!renderer_->setSkybox(opt_.skybox))
    {
        std::vector<std::string> skies = renderer_->availableSkyboxes();
        std::fprintf(stderr, "drifto: skybox '%s' not found (%zu available)\n", opt_.skybox.c_str(), skies.size());
        if (!skies.empty()) { opt_.skybox = skies.front(); renderer_->setSkybox(opt_.skybox); }
    }
    if (renderer_->availableModels().empty())
        std::fprintf(stderr, "drifto: no models under %s/models (run `make assets`); using fallback meshes\n",
                     opt_.assetDir.c_str());

    world_ = std::make_unique<WorldView>(*renderer_);
    world_->initAssets();
    gauges_ = std::make_unique<gfx::HudGauges>();
    if (!gauges_->init(*renderer_)) std::fprintf(stderr, "drifto: HUD theme missing, using the procedural gauges\n");
    // Sound: a missing device just means silence (every call stays safe).
    snd_ = std::make_unique<audio::Engine>();
    if (!snd_->init(opt_.assetDir)) std::fprintf(stderr, "drifto: no audio device, running silent\n");
    snd_->setMasterVolume(volume_);
    snd_->setMuted(opt_.mute);
    if (opt_.lowQuality)
    {
        renderer_->setShadowsEnabled(false);
        world_->treeOutlines = false;
        world_->treeAhead = 100.0f;
        world_->viewAhead = 220.0f;
        gfx::FxQuality q = renderer_->fxQuality();
        q.postFx = false;
        renderer_->setFxQuality(q);
    }

    net_ = std::make_unique<net::NetClient>();
    transport_ = std::make_unique<NetClientTransport>(*net_);
    session_ = std::make_unique<game::MultiplayerSession>(*transport_);

    startAttract();
    if (opt_.offline && opt_.autoLogin.empty()) startOfflineRun();
    if (!opt_.autoLogin.empty())
    {
        size_t c = opt_.autoLogin.find(':');
        nameField_.text = opt_.autoLogin.substr(0, std::min(c, static_cast<size_t>(net::kMaxNameLen)));
        passField_.text = c == std::string::npos ? std::string() : opt_.autoLogin.substr(c + 1);
        screen_ = Screen::Login;
        submitLogin();
    }
    else if (opt_.lan)
    {
        screen_ = Screen::Login; // --lan: straight to the LAN name prompt
        loginFocus_ = 0;
    }
    startTime_ = nowSeconds();
    return true;
}

// Tear down in dependency order: audio, network session, then GL objects and
// finally the window. Safe to call twice (the destructor calls it as well).
void Client::Shutdown()
{
    // Hide the window first: tearing the network down can take a moment
    // (DDS TCP domains), and a mapped window that stops repainting looks
    // exactly like a frozen game.
    if (window_) glfwHideWindow(window_);
    if (snd_) snd_->shutdown();
    snd_.reset();
    if (session_) session_->end();
    double t0 = nowSeconds();
    if (net_ && netStarted_) net_->stop();
    if (netStarted_ && nowSeconds() - t0 > 0.5)
        std::fprintf(stderr, "drifto: network shutdown took %.1f s\n", nowSeconds() - t0);
    netStarted_ = false;
    if (world_) world_->shutdown();
    world_.reset();
    gauges_.reset();
    if (renderer_) renderer_->shutdown();
    renderer_.reset();
    if (window_)
    {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }
}

#ifdef _WIN32
// winmm's timer resolution, declared here to keep <windows.h> out.
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int);
#pragma comment(lib, "winmm.lib")
#endif

// Main loop; returns the process exit code. See the file header for the
// fixed-step / render split.
int Client::Run()
{
    if (opt_.headless) return runHeadless();
#ifdef _WIN32
    // Windows sleeps in 15.6 ms ticks by default, so the 60 fps cap below
    // overslept every frame to ~31 ms: the game ran at 30 fps (judder against
    // the display, doubled motion blur). 1 ms ticks for the lifetime of the loop.
    timeBeginPeriod(1);
    struct TimerPeriod
    {
        ~TimerPeriod() { timeEndPeriod(1); }
    } timerPeriod;
#endif
    double last = nowSeconds();
    while (!quit_ && !glfwWindowShouldClose(window_))
    {
        glfwPollEvents();
        double now = nowSeconds();
        if (opt_.duration > 0 && now - startTime_ > opt_.duration) quit_ = true;
        // Frame time in seconds, clamped so a stall does not cause a burst of ticks.
        double dt = std::min(now - last, 0.25); // clamp hitches (debugger, window drag)
        last = now;
        time_ += dt;
        // Exponentially smoothed frames per second.
        if (dt > 0) fps_ = dm::lerpf(fps_, static_cast<float>(1.0 / dt), 0.05f);

        pollNet();
        autoStep();

        // Fixed 60 Hz simulation; render interpolates between the last two ticks.
        accumulator_ += dt;
        int steps = 0;
        while (accumulator_ >= game::kFixedDt && steps < 8)
        {
            fixedTick();
            accumulator_ -= game::kFixedDt;
            ++steps;
        }
        if (steps == 8) accumulator_ = 0; // spiral-of-death guard
        frame(static_cast<float>(dt), static_cast<float>(accumulator_ / game::kFixedDt));

        bool shotNow = !opt_.screenshot.empty() && now - startTime_ > opt_.shotDelay;
        if (shotNow && std::getenv("DRIFTO_SHOT_WHEN") && now - startTime_ < opt_.shotDelay + 40.0)
        {
            // Dev aid for HUD/VFX screenshots: wait for the camera car to burn
            // nitro with another car close by.
            shotNow = false;
            if (game::MultiplayerSession *rs = raceSession())
            {
                std::vector<game::CarView> cars = rs->carViews(0.0f);
                const game::CarView *t = cameraTarget(cars);
                for (const game::CarView &c : cars)
                    if (t && t->nitroActive && &c != t && !c.eliminated && dm::length(c.position - t->position) < 25.0f)
                        shotNow = true;
            }
        }
        if (shotNow)
        {
            bool ok = renderer_->saveScreenshot(opt_.screenshot);
            std::fprintf(stderr, "drifto: screenshot %s %s (%.0f fps)\n", opt_.screenshot.c_str(),
                         ok ? "saved" : "FAILED", fps_);
            quit_ = true;
        }
        glfwSwapBuffers(window_);
        // Frame cap: with software GL on XWayland swaps don't block on vsync,
        // and several uncapped windows flood the display server.
        // Sleep in 1 ms steps and yield for the last 2 ms so frames stay
        // evenly spaced (one long sleep can overshoot by a whole OS tick).
        if (opt_.fpsCap > 0)
        {
            double until = now + 1.0 / opt_.fpsCap;
            for (double t = nowSeconds(); t < until; t = nowSeconds())
            {
                if (until - t > 0.002) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                else std::this_thread::yield();
            }
        }
    }
    return 0;
}

// ===========================================================================
// simulation
// ===========================================================================

// Input for one simulation tick: the bot in --demo mode, otherwise keyboard
// (A/D or arrows steer, Space nitro; throttle is automatic). Sampled once per
// fixed tick so it is what the session records, sends and replays.
net::CarInput Client::readDriveInput(uint32_t tick, const game::CarSimState *car, const game::Track *track)
{
    if (opt_.demo && car && track) return demoBot_.drive(*car, *track, game::defaultCarParams(), tick);
    auto down = [this](int k) { return glfwGetKey(window_, k) == GLFW_PRESS; };
    float left = (down(GLFW_KEY_A) || down(GLFW_KEY_LEFT)) ? 1.0f : 0.0f;
    float right = (down(GLFW_KEY_D) || down(GLFW_KEY_RIGHT)) ? 1.0f : 0.0f;
    // drifto controls: constant throttle, steer, plus nitro. Steering is
    // smoothed inside the (deterministic) simulation.
    return game::driveInput(tick, right - left, down(GLFW_KEY_SPACE));
}

// Start a local race: a loopback "network" with us as host (player 1) and 1-3
// bots (ids 2..4). The results screen stays until the player chooses.
void Client::startOfflineRun()
{
    uint32_t seed = opt_.seed ? opt_.seed : randomSeed();
    game::SessionConfig cfg;
    cfg.resultsSeconds = 1e6f; // results stay up until the player chooses
    offlineNet_ = std::make_unique<game::LoopbackTransport>(1);
    offline_ = std::make_unique<game::MultiplayerSession>(*offlineNet_, cfg);
    std::vector<int32_t> bots;
    for (int i = 0; i < std::clamp(opt_.bots, 1, 3); ++i) bots.push_back(2 + i);
    offline_->hostStartMatch(seed, {1}, bots);
    // A mix of bot personalities, rotated by the track seed.
    const game::BotPreset mix[4] = {game::BotPreset::Ace, game::BotPreset::Bully, game::BotPreset::Racer,
                                    game::BotPreset::Rookie};
    for (size_t i = 0; i < bots.size(); ++i) offline_->setBotProfile(bots[i], game::botPreset(mix[(seed + i) % 4]));
    demoBot_ = game::BotDriver{};
    offlineCounted_ = false;
    if (!opt_.headless && !opt_.demo) saveSettings("offline");
    spectateId_ = 0;
    screen_ = Screen::OfflineRace;
    raceTime_ = 0;
    std::fprintf(stderr, "drifto: offline elimination race vs %zu bots, track seed %u\n", bots.size(), seed);
}

// Menu background: a bot drives a fresh random track, with no multiplayer state.
void Client::startAttract()
{
    attract_.track = std::make_unique<game::Track>(randomSeed());
    attract_.car = attract_.prev = game::spawnCar(*attract_.track, 0, 26.0f, 0.0f, 0.0f);
    attract_.bot = game::BotDriver{};
    attract_.tick = 0;
}

// One fixed tick of the attract run; restarts it a few seconds after the bot dies.
void Client::stepAttract()
{
    LocalRun &run = attract_;
    if (!run.track) return; // headless: no menu background
    run.prev = run.car;
    run.track->update(run.car.section);
    run.track->ensureGenerated(run.car.section + 30);
    ++run.tick;
    game::stepCar(run.car, run.bot.drive(run.car, *run.track, game::defaultCarParams(), run.tick), *run.track);
    if (world_) world_->onSimTick(*run.track, {viewOf(run.prev, run.car, 1.0f, 0)});
    if (!run.car.alive && run.car.deadTimer > 3.0f) startAttract();
}

// The session of the screen being shown (offline or online), else nullptr.
game::MultiplayerSession *Client::raceSession()
{
    if (screen_ == Screen::OfflineRace) return offline_.get();
    if (screen_ == Screen::OnlineRace) return session_.get();
    return nullptr;
}

// One fixed tick of a race session: sample input for the next local tick, step
// the session, then let the world view emit skid marks and smoke.
void Client::raceTick(game::MultiplayerSession &s)
{
    net::CarInput in = readDriveInput(s.localTick() + 1, s.localCar(), &s.track());
    s.tick(in);
    if (world_) world_->onSimTick(s.track(), s.carViews(1.0f));
}

// Log the result of a finished offline race (headless/soak runs read this).
void Client::onRaceFinished(game::MultiplayerSession &s)
{
    const game::RaceInfo &ri = s.raceInfo();
    std::string order, wrecks;
    uint32_t rescues = 0;
    for (int32_t id : ri.finishingOrder)
    {
        order += " " + playerName(id);
        if (const game::CarSimState *c = s.hostCar(id))
        {
            wrecks += fmt(" %s:%u", playerName(id).c_str(), c->deaths);
            rescues += c->rescues;
        }
    }
    std::fprintf(stderr, "drifto: race %d (seed %u) finished after %.1f s: order%s | wrecks%s | rescues %u\n",
                 offlineRaces_, s.trackSeed(), s.localTick() / 60.0f, order.c_str(), wrecks.c_str(), rescues);
}

// Main loop without window or renderer: polls the network, runs fixed ticks
// (optionally 60 per iteration with --fast, i.e. faster than real time) and
// prints a status line every 5 s while online.
int Client::runHeadless()
{
    double last = nowSeconds(), acc = 0, lastStatus = 0;
    while (!quit_)
    {
        double now = nowSeconds();
        if (opt_.duration > 0 && now - startTime_ > opt_.duration) break;
        if (screen_ == Screen::OnlineRace && now - lastStatus > 5.0)
        {
            // Bot / soak visibility: are we still simulating, who hosts, how stale is the host?
            lastStatus = now;
            const game::RaceInfo &ri = session_->raceInfo();
            const game::CarSimState *c = session_->localCar();
            const char *ph[4] = {"lobby", "countdown", "racing", "results"};
            uint8_t place = 0;
            for (const game::CarView &v : session_->carViews(0)) if (v.local) place = v.place;
            std::fprintf(stderr, "drifto[%s] t=%.0f %s host=%d tick=%u place=%d/%d clock=%.1f(st%d) last=%d snapAge=%.1fs %.0fm wrecks=%u\n",
                         nameField_.text.c_str(), now - startTime_, ph[static_cast<int>(session_->phase()) & 3],
                         session_->isHost(), session_->localTick(), place, ri.racing, ri.timer, ri.stage, ri.lastPlaceId,
                         session_->secondsSinceSnapshot(), c ? c->distance : 0.0f, c ? c->deaths : 0u);
        }
        pollNet();
        autoStep();
        if (opt_.fast && screen_ == Screen::OfflineRace)
        {
            for (int i = 0; i < 60 && !quit_; ++i) { fixedTick(); time_ += game::kFixedDt; }
            continue;
        }
        double dt = std::min(now - last, 0.25);
        last = now;
        acc += dt;
        time_ += dt;
        while (acc >= game::kFixedDt) { fixedTick(); acc -= game::kFixedDt; }
        if (game::MultiplayerSession *rs = raceSession())
        {
            std::vector<game::CarView> cars = rs->carViews(0.0f);
            updateAudio(rs, cars, cameraTarget(cars), static_cast<float>(dt));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return 0;
}

// Advance whichever screen is active by one 60 Hz tick: a race session if
// racing, otherwise the menu attract run. Also counts finished offline races.
void Client::fixedTick()
{
    if (opt_.headless) raceTime_ += game::kFixedDt; // frame() (which advances it) never runs headless
    switch (screen_)
    {
    case Screen::OfflineRace:
        if (offline_ && offline_->active()) raceTick(*offline_);
        if (offline_ && offline_->phase() == net::MatchPhase::Results && !offlineCounted_)
        {
            offlineCounted_ = true;
            ++offlineRaces_;
            offlineWins_ += offline_->raceInfo().winnerId == 1;
            onRaceFinished(*offline_);
            if (opt_.races > 0 && offlineRaces_ >= opt_.races) quit_ = true;
            else if (opt_.demo && opt_.screenshot.empty()) startOfflineRun(); // demo loops
        }
        break;
    case Screen::OnlineRace:
        if (session_->active()) raceTick(*session_);
        else if (raceTime_ > 1.0f)
        {
            // Host returned to the lobby (or the match ended).
            screen_ = net_->inRoom() ? Screen::Lobby : Screen::RoomList;
            ready_ = false;
        }
        break;
    default:
        stepAttract();
        break;
    }
}

// ===========================================================================
// networking
// ===========================================================================

// Start the network if needed, restarting it when the mode (LAN/online) or
// server address changed since the last start. Returns false on failure.
bool Client::ensureNetStarted()
{
    // Restart when the mode or the server changed since the last start.
    if (netStarted_ && netLan_ == lanMode_ && (lanMode_ || netApi_ == opt_.apiHost)) return true;
    if (netStarted_)
    {
        session_->end();
        net_->stop();
        netStarted_ = false;
        roster_.clear();
        rooms_.clear();
    }
    net::NetClient::Config cfg;
    cfg.mode = lanMode_ ? net::NetClient::Mode::Lan : net::NetClient::Mode::Online;
    cfg.lanPeers = opt_.lanPeers;
    cfg.apiHost = opt_.apiHost;
    cfg.certIndex = opt_.certIndex;
    cfg.transport = opt_.transport;
    cfg.insecure = opt_.insecure;
    netStarted_ = net_->start(cfg);
    netLan_ = lanMode_;
    netApi_ = opt_.apiHost;
    return netStarted_;
}

// Login screen submit (and the --login automation): start the network in the
// chosen mode, then log in. LAN logs in locally with just the name.
// Validate the name, apply the API address, start the network and log in.
// The result arrives later as a LoginOk/LoginFailed event in pollNet().
void Client::submitLogin()
{
    if (nameField_.text.empty()) { status_ = "Enter a name (max 8 characters)"; loginFocus_ = 0; return; }
    if (!lanMode_)
    {
        std::string api = apiField_.text;
        while (!api.empty() && api.back() == ' ') api.pop_back();
        if (!api.empty()) opt_.apiHost = api;
        apiField_.text = opt_.apiHost;
    }
    if (!ensureNetStarted())
    {
        status_ = lanMode_ ? std::string("Could not start LAN networking") : "Could not reach the network (API " + opt_.apiHost + ")";
        return;
    }
    net_->login(nameField_.text, lanMode_ ? std::string() : passField_.text);
    loginPending_ = true;
    status_ = lanMode_ ? "Starting LAN..." : "Logging in to " + opt_.apiHost + "...";
}

// Sorted, de-duplicated player ids of the room, including ourselves.
std::vector<int32_t> Client::rosterIds() const
{
    std::vector<int32_t> ids;
    for (const net::RosterEntry &r : roster_) ids.push_back(r.playerId);
    if (net_ && netStarted_) ids.push_back(net_->localPlayerId());
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// Display name for an id: fixed names offline, roster / remembered names
// online, "P<id>" as a last resort.
std::string Client::playerName(int32_t id) const
{
    if (screen_ == Screen::OfflineRace) return id == 1 ? std::string("YOU") : fmt("BOT%d", id - 1);
    for (const net::RosterEntry &r : roster_)
        if (r.playerId == id) return r.name;
    auto it = knownNames_.find(id);
    return it != knownNames_.end() ? it->second : fmt("P%d", id);
}

// Show a message screen that returns to `back` when dismissed.
void Client::goToMessage(const std::string &text, Screen back)
{
    message_ = text;
    messageBack_ = back;
    screen_ = Screen::Message;
}

// Switch to the race screen and start the session (unless the host path
// already did) with the room roster and our host flag.
void Client::enterOnlineRace(uint32_t seed)
{
    if (!session_->active() || session_->trackSeed() != seed)
        session_->beginMatch(seed, rosterIds(), net_->isHost());
    screen_ = Screen::OnlineRace;
    raceTime_ = 0;
    spectateId_ = 0;
}

// Drain network events and update the UI state machine accordingly: login
// result, room list, join/leave, roster and host changes (forwarded to the
// session for host migration), match and phase announcements, transport
// kind, disconnect. Also refreshes the room list periodically.
void Client::pollNet()
{
    if (!netStarted_) return;
    for (const net::Event &ev : net_->pollEvents())
    {
        switch (ev.type)
        {
        case net::EventType::LoginOk:
            loginPending_ = false;
            status_.clear();
            saveSettings(lanMode_ ? "lan" : "online");
            screen_ = Screen::RoomList;
            net_->requestRoomList();
            lastRoomRefresh_ = time_;
            autoRoomCreateAfter_ = time_ + (lanMode_ ? 2.5 : 0.0);
            break;
        case net::EventType::LoginFailed:
            loginPending_ = false;
            status_ = "Login failed: " + ev.message;
            break;
        case net::EventType::RoomList:
            rooms_ = ev.rooms;
            roomIndex_ = rooms_.empty() ? 0 : std::min(roomIndex_, static_cast<int>(rooms_.size()) - 1);
            if (!opt_.autoRoom.empty() && !autoRoomRequested_ && screen_ == Screen::RoomList)
            {
                // Automation: join the named room if it exists, otherwise create it
                // (in LAN mode only once discovery has had time to find it).
                auto it = std::find_if(rooms_.begin(), rooms_.end(),
                                       [&](const net::RoomInfo &r) { return r.name == opt_.autoRoom; });
                if (it != rooms_.end())
                {
                    autoRoomRequested_ = true;
                    net_->joinRoom(it->roomId);
                }
                else if (time_ >= autoRoomCreateAfter_)
                {
                    autoRoomRequested_ = true;
                    net_->createRoom(opt_.autoRoom);
                }
            }
            break;
        case net::EventType::JoinedRoom:
            screen_ = Screen::Lobby;
            ready_ = false;
            roster_ = net_->roster();
            for (const net::RosterEntry &r : roster_) knownNames_[r.playerId] = r.name;
            status_.clear();
            break;
        case net::EventType::JoinFailed:
            status_ = "Join failed: " + ev.message;
            if (screen_ == Screen::CreateRoom) screen_ = Screen::RoomList;
            break;
        case net::EventType::LeftRoom:
            session_->end();
            roster_.clear();
            if (screen_ == Screen::Lobby || screen_ == Screen::OnlineRace) screen_ = Screen::RoomList;
            net_->requestRoomList();
            break;
        case net::EventType::RosterChanged:
            roster_ = ev.roster;
            for (const net::RosterEntry &r : roster_) knownNames_[r.playerId] = r.name;
            if (std::getenv("DRIFTO_DEBUG"))
            {
                std::fprintf(stderr, "drifto: roster (%zu):", roster_.size());
                for (const net::RosterEntry &r : roster_) std::fprintf(stderr, " %d=%s", r.playerId, r.name.c_str());
                std::fprintf(stderr, "\n");
            }
            if (session_->active()) session_->onRosterChanged(rosterIds());
            break;
        case net::EventType::HostChanged:
            session_->onHostChanged(ev.isHost);
            if (ev.isHost) status_ = "You are now the host";
            break;
        case net::EventType::MatchStarted:
            enterOnlineRace(ev.trackSeed);
            break;
        case net::EventType::PhaseChanged:
            session_->onPhaseChanged(ev.phase);
            break;
        case net::EventType::TransportChanged:
            transportKind_ = ev.transport;
            break;
        case net::EventType::Disconnected:
            session_->end();
            net_->stop();
            netStarted_ = false;
            loginPending_ = false;
            goToMessage("Disconnected: " + ev.message, Screen::MainMenu);
            break;
        }
    }
    if (screen_ == Screen::RoomList && time_ - lastRoomRefresh_ > (lanMode_ ? 1.0 : 3.0))
    {
        net_->requestRoomList();
        lastRoomRefresh_ = time_;
    }
}

// Automation (--login/--room/--autostart): ready up in the lobby and, as host,
// start the match once enough players are ready.
void Client::autoStep()
{
    if (opt_.autoLogin.empty() || !netStarted_) return;
    if (screen_ == Screen::Lobby)
    {
        if (!autoReadySent_) { ready_ = true; net_->setReady(true); autoReadySent_ = true; }
        int readyCount = 0;
        for (const net::RosterEntry &r : roster_) readyCount += (r.ready || r.isHost) ? 1 : 0;
        if (net_->isHost() && opt_.autoStart > 0 && static_cast<int>(roster_.size()) >= opt_.autoStart &&
            readyCount >= opt_.autoStart && !session_->active())
        {
            uint32_t seed = randomSeed();
            std::fprintf(stderr, "drifto: auto-starting match, seed %u, %zu players\n", seed, roster_.size());
            session_->hostStartMatch(seed, rosterIds());
            enterOnlineRace(seed);
        }
    }
    else
        autoReadySent_ = false;
}

// ===========================================================================
// input
// ===========================================================================

// Text input (character callback): ASCII only, into the focused field.
void Client::onChar(unsigned cp) { handleText(cp); }

// Key callback: presses and auto-repeats go to handleKey().
void Client::onKey(int key, int action, int mods)
{
    if (action == GLFW_PRESS || action == GLFW_REPEAT) handleKey(key, mods);
}

// Append a printable ASCII character to the active text field.
void Client::handleText(unsigned cp)
{
    TextField *f = nullptr;
    if (screen_ == Screen::Login && !loginPending_)
    {
        int fi = lanMode_ ? 0 : loginFocus_ % 3;
        f = fi == 0 ? &nameField_ : (fi == 1 ? &passField_ : &apiField_);
    }
    else if (screen_ == Screen::CreateRoom) f = &roomField_;
    if (!f || cp < 32 || cp > 126 || f->text.size() >= f->maxLen) return;
    if (f == &nameField_ && cp == ' ') return; // names are single words
    f->text.push_back(static_cast<char>(cp));
}

// Route a key press to the handler of the current screen.
void Client::handleKey(int key, int mods)
{
    (void)mods;
    switch (screen_)
    {
    case Screen::MainMenu: menuKey(key); break;
    case Screen::Login: loginKey(key); break;
    case Screen::RoomList: roomListKey(key); break;
    case Screen::Lobby: lobbyKey(key); break;
    case Screen::CreateRoom:
        if (key == GLFW_KEY_BACKSPACE && !roomField_.text.empty()) roomField_.text.pop_back();
        else if (key == GLFW_KEY_ESCAPE) screen_ = Screen::RoomList;
        else if ((key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) && !roomField_.text.empty())
        {
            net_->createRoom(roomField_.text);
            status_ = "Creating room...";
        }
        break;
    case Screen::OfflineRace:
        if (key == GLFW_KEY_ESCAPE) screen_ = Screen::MainMenu;
        else if (offline_ && offline_->phase() == net::MatchPhase::Results &&
                 (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_R))
            startOfflineRun();
        else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A) cycleSpectate(-1);
        else if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_D) cycleSpectate(+1);
        break;
    case Screen::OnlineRace:
        if (key == GLFW_KEY_ESCAPE)
        {
            session_->end();
            net_->leaveRoom();
            screen_ = Screen::RoomList;
        }
        else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A) cycleSpectate(-1);
        else if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_D) cycleSpectate(+1);
        break;
    case Screen::Message:
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_ESCAPE || key == GLFW_KEY_SPACE)
            screen_ = messageBack_;
        break;
    }
}

// Main menu navigation: offline / online / LAN / quit.
void Client::menuKey(int key)
{
    const int items = 4; // offline, online, LAN, quit
    if (key == GLFW_KEY_UP || key == GLFW_KEY_W) menuIndex_ = (menuIndex_ + items - 1) % items;
    else if (key == GLFW_KEY_DOWN || key == GLFW_KEY_S) menuIndex_ = (menuIndex_ + 1) % items;
    else if (key == GLFW_KEY_ESCAPE) quit_ = true;
    else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_SPACE)
    {
        if (menuIndex_ == 0) { startOfflineRun(); return; }
        if (menuIndex_ == 3) { quit_ = true; return; }
        lanMode_ = menuIndex_ == 2;
        status_.clear();
        apiField_.text = opt_.apiHost;
        bool sameNet = netStarted_ && netLan_ == lanMode_;
        if (sameNet && net_->loggedIn())
        {
            screen_ = Screen::RoomList;
            net_->requestRoomList();
            lastRoomRefresh_ = time_;
        }
        else
        {
            screen_ = Screen::Login;
            loginFocus_ = (lanMode_ || nameField_.text.empty()) ? 0 : 1;
        }
    }
}

// Login form editing; Tab and arrows move focus, Enter submits.
void Client::loginKey(int key)
{
    if (key == GLFW_KEY_ESCAPE) { screen_ = Screen::MainMenu; loginPending_ = false; return; }
    if (loginPending_) return;
    // Online: name, password, server. LAN: name only.
    const int fields = lanMode_ ? 1 : 3;
    TextField *f[3] = {&nameField_, &passField_, &apiField_};
    TextField &cur = *f[loginFocus_ % fields];
    if (key == GLFW_KEY_BACKSPACE && !cur.text.empty()) cur.text.pop_back();
    else if (key == GLFW_KEY_TAB || key == GLFW_KEY_DOWN) loginFocus_ = (loginFocus_ + 1) % fields;
    else if (key == GLFW_KEY_UP) loginFocus_ = (loginFocus_ + fields - 1) % fields;
    else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)
    {
        if (!lanMode_ && loginFocus_ == 0) { loginFocus_ = 1; return; }
        submitLogin();
    }
}

// Room list: select, refresh, host (H), create named (C), join (Enter).
void Client::roomListKey(int key)
{
    int n = static_cast<int>(rooms_.size());
    if (key == GLFW_KEY_ESCAPE) screen_ = Screen::MainMenu;
    else if ((key == GLFW_KEY_UP || key == GLFW_KEY_W) && n) roomIndex_ = (roomIndex_ + n - 1) % n;
    else if ((key == GLFW_KEY_DOWN || key == GLFW_KEY_S) && n) roomIndex_ = (roomIndex_ + 1) % n;
    else if (key == GLFW_KEY_R || key == GLFW_KEY_F5)
    {
        net_->requestRoomList();
        lastRoomRefresh_ = time_;
    }
    else if (key == GLFW_KEY_H)
    {
        // Host a game named after the player, no questions asked.
        std::string name = (nameField_.text.empty() ? std::string("drift") : nameField_.text) + "'s game";
        if (name.size() > roomField_.maxLen) name.resize(roomField_.maxLen);
        net_->createRoom(name);
        status_ = "Hosting...";
    }
    else if (key == GLFW_KEY_C || key == GLFW_KEY_N)
    {
        roomField_.text = (nameField_.text.empty() ? std::string("drift") : nameField_.text) + "'s room";
        if (roomField_.text.size() > roomField_.maxLen) roomField_.text.resize(roomField_.maxLen);
        status_.clear();
        screen_ = Screen::CreateRoom;
    }
    else if ((key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) && n)
    {
        const net::RoomInfo &r = rooms_[roomIndex_];
        if (r.inProgress) status_ = "That race is already in progress";
        else if (r.players >= r.maxPlayers) status_ = "Room is full";
        else { net_->joinRoom(r.roomId); status_ = "Joining..."; }
    }
}

// Lobby: toggle ready; the host starts with Enter (everyone ready) or F (force).
void Client::lobbyKey(int key)
{
    if (key == GLFW_KEY_ESCAPE) { net_->leaveRoom(); screen_ = Screen::RoomList; return; }
    if (key == GLFW_KEY_SPACE || key == GLFW_KEY_R)
    {
        ready_ = !ready_;
        net_->setReady(ready_);
    }
    else if ((key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_F) && net_->isHost())
    {
        bool allReady = true;
        for (const net::RosterEntry &r : roster_) allReady = allReady && (r.ready || r.isHost);
        if (!allReady && key != GLFW_KEY_F) { status_ = "Not everyone is ready (F = start anyway)"; return; }
        uint32_t seed = randomSeed();
        session_->hostStartMatch(seed, rosterIds());
        enterOnlineRace(seed);
    }
}

// ===========================================================================
// drawing
// ===========================================================================

// Sun and ambient light chosen from the skybox name, plus distance fog tinted
// with the sky's horizon colour.
gfx::Lighting Client::lighting() const
{
    gfx::Lighting l;
    const std::string &s = opt_.skybox;
    if (s.find("Night") != std::string::npos || s.find("Midnight") != std::string::npos)
    {
        // Dim moonlight for the dark skies.
        l.sunDirection = dm::normalize(dm::Vec3{0.3f, -1.0f, 0.4f});
        l.sunColor = {0.32f, 0.38f, 0.55f};
        l.ambient = {0.12f, 0.14f, 0.22f};
    }
    else if (s.find("Sunset") != std::string::npos || s.find("Dusk") != std::string::npos)
    {
        l.sunDirection = dm::normalize(dm::Vec3{-0.7f, -0.35f, -0.4f});
        l.sunColor = {1.0f, 0.72f, 0.48f};
        l.ambient = {0.32f, 0.28f, 0.34f};
    }
    else if (s.find("Morning") != std::string::npos)
    {
        l.sunDirection = dm::normalize(dm::Vec3{0.6f, -0.5f, -0.3f});
        l.sunColor = {1.0f, 0.92f, 0.8f};
    }
    l.fogColor = renderer_->skyboxHorizonColor();
    l.fogStart = 110.0f;
    l.fogEnd = 480.0f;
    return l;
}

// Pixel width of a string at `scale`, measured with stb_easy_font.
float Client::textWidth(const std::string &s, float scale) const
{
    std::string t = s;
    return static_cast<float>(stb_easy_font_width(&t[0])) * scale;
}

void Client::text(float x, float y, float scale, const std::string &s, const dm::Vec3 &c)
{
    // Cheap drop shadow for legibility over the 3D scene.
    renderer_->drawText(x + scale, y + scale, scale, s, {0.02f, 0.02f, 0.03f});
    renderer_->drawText(x, y, scale, s, c);
}

void Client::textCentred(float cx, float y, float scale, const std::string &s, const dm::Vec3 &c)
{
    text(cx - textWidth(s, scale) * 0.5f, y, scale, s, c);
}

void Client::drawPanel(float x, float y, float w, float h) { renderer_->drawRect(x, y, w, h, {0.03f, 0.04f, 0.06f}, 0.62f); }

// Choose the car the camera follows: our own while alive; once eliminated, the
// spectated car (Left/Right cycles), defaulting to the race leader.
const game::CarView *Client::cameraTarget(const std::vector<game::CarView> &cars)
{
    const game::CarView *me = nullptr, *spec = nullptr, *leader = nullptr;
    for (const game::CarView &c : cars)
    {
        if (c.local) me = &c;
        if (c.playerId == spectateId_ && !c.eliminated) spec = &c;
        if (!c.eliminated && (!leader || (c.place != 0 && (leader->place == 0 || c.place < leader->place))))
            leader = &c;
    }
    if (me && !me->eliminated) return me;
    // Eliminated: spectate (Left/Right cycles), default the leader.
    if (!spec) spec = leader;
    if (spec) spectateId_ = spec->playerId;
    return spec ? spec : me;
}

// Move the spectate target to the next/previous non-eliminated car by id.
void Client::cycleSpectate(int dir)
{
    game::MultiplayerSession *s = raceSession();
    if (!s) return;
    std::vector<int32_t> ids;
    for (const game::CarView &c : s->carViews(0.0f))
        if (!c.eliminated) ids.push_back(c.playerId);
    if (ids.empty()) return;
    std::sort(ids.begin(), ids.end());
    auto it = std::find(ids.begin(), ids.end(), spectateId_);
    int i = it == ids.end() ? 0 : static_cast<int>(it - ids.begin());
    spectateId_ = ids[(i + dir + static_cast<int>(ids.size())) % ids.size()];
}

// Position the chase camera, stream track sections around the cars, derive
// speed/nitro/slipstream effect strengths (smoothed with exp(-k*dt)) and FOV,
// then begin the frame and draw the 3D scene. The HUD is drawn afterwards.
void Client::drawWorld(const game::Track &track, const std::vector<game::CarView> &cars, const game::CarView *me,
                       float dt, bool racing)
{
    // Menus get a wider, more cinematic follow.
    // Follow distance scales with the car (kCarScale) so the car keeps its
    // size on screen and the road reads wider.
    camera_.distance = (racing ? 7.6f : 11.0f) * game::kCarScale;
    camera_.height = (racing ? 2.8f : 4.4f) * game::kCarScale;
    camera_.lookHeight = 0.9f * game::kCarScale;
    camera_.lookAhead = 2.0f * game::kCarScale;
    float speed01 = 0, nitroTarget = 0, slipTarget = 0;
    if (me)
    {
        // Cut (don't whip across the map) on a new track, a new target car,
        // or a teleport such as a respawn.
        // Hard cut (snap) when the camera should jump instead of glide.
        if (cameraSeed_ != track.seed() || !camera_.initialised || me->playerId != cameraCarId_ ||
            dm::length(me->position - cameraCarPos_) > 12.0f)
        {
            camera_.snap(me->position, me->rotation, me->velocity);
            cameraSeed_ = track.seed();
        }
        else
            camera_.update(me->position, me->rotation, me->velocity, dt);
        cameraCarId_ = me->playerId;
        cameraCarPos_ = me->position;
        uint32_t lo = me->section, hi = me->section;
        for (const game::CarView &c : cars)
            if (!c.eliminated) { lo = std::min(lo, c.section); hi = std::max(hi, c.section); }
        world_->streamSections(track, me->section, lo, hi);
        const game::CarParams &p = game::defaultCarParams();
        // speed01: speed relative to the drag-limited top speed sqrt(driveForce/drag).
        speed01 = dm::length(me->velocity) / std::sqrt(p.driveForce / p.drag);
        nitroTarget = me->nitroActive ? 1.0f : 0.0f;
        slipTarget = me->slipstream ? 1.0f : 0.0f;
    }
    // Speed / nitro driven FOV and screen effects (gfx), smoothed.
    fxNitro_ += (nitroTarget - fxNitro_) * (1.0f - std::exp(-8.0f * dt));
    fxSlip_ += (slipTarget - fxSlip_) * (1.0f - std::exp(-5.0f * dt));
    float fovTarget = racing ? gfx::speedFovY(speed01, fxNitro_) : dm::radians(55.0f);
    fov_ += (fovTarget - fov_) * (1.0f - std::exp(-6.0f * dt));

    // Takedown / wreck camera shake: decays in ~0.5 s, render-only.
    takedownFlash_ = std::fmax(0.0f, takedownFlash_ - dt);
    shake_ = std::fmax(0.0f, shake_ - 2.0f * dt);
    dm::Vec3 shake{0, 0, 0};
    if (shake_ > 0 && racing)
    {
        float a = 0.35f * shake_ * shake_, t = static_cast<float>(time_);
        shake = {a * std::sin(t * 61.0f), a * 0.6f * std::sin(t * 47.0f + 1.3f), a * std::sin(t * 53.0f + 2.1f)};
    }
    gfx::Camera cam;
    cam.position = camera_.position + shake;
    cam.target = camera_.target + shake * 0.5f;
    cam.up = camera_.up;
    cam.fovY = fov_;
    cam.zFar = 900.0f;
    renderer_->beginFrame(cam, lighting());
    gfx::FrameFx fx;
    fx.speed01 = racing ? speed01 : 0.0f;
    fx.nitro01 = racing ? fxNitro_ : 0.0f;
    fx.slipstream01 = racing ? fxSlip_ : 0.0f;
    fx.time = static_cast<float>(time_);
    renderer_->setFrameFx(fx);
    world_->draw(cars, dt, static_cast<float>(time_), me ? me->playerId : -1);
}

// Render one frame: 3D world, audio update, HUD (race) or menus. alpha is the
// fraction of a fixed tick elapsed, used to interpolate car poses.
void Client::frame(float dt, float alpha)
{
    raceTime_ += dt;
    frameDt_ = dt;
    game::MultiplayerSession *s = raceSession();
    if (s)
    {
        std::vector<game::CarView> cars = s->carViews(alpha);
        const game::CarView *target = cameraTarget(cars);
        drawWorld(s->track(), cars, target, dt, true);
        updateAudio(s, cars, target, dt); // before the HUD: it reads the per-car gearbox
        drawRaceHud(*s, cars, target, screen_ == Screen::OnlineRace);
    }
    else
    {
        std::vector<game::CarView> cars{viewOf(attract_.prev, attract_.car, alpha, 0)};
        drawWorld(*attract_.track, cars, &cars[0], dt, false);
        updateAudio(nullptr, cars, nullptr, dt); // menus are silent
        drawMenus();
    }
    renderer_->endFrame();
}

// ===========================================================================
// sound
// ===========================================================================

// Per-car sound state, created on first use; the gearbox is tuned so that top
// speed cruises in 5th a little under the redline (never on the limiter).
Client::CarSound &Client::carSound(int32_t id)
{
    auto it = carSounds_.find(id);
    if (it != carSounds_.end()) return it->second;
    CarSound &cs = carSounds_[id];
    const game::CarParams &p = game::defaultCarParams();
    cs.gearbox.configure(std::sqrt(p.driveForce / p.drag)); // top speed cruises in 5th under the redline
    return cs;
}

// Drive the audio engine: place the listener at the camera (velocity for
// Doppler, ignoring camera cuts), then per car update the gearbox and send
// engine, skid and nitro parameters. Voices of eliminated or departed cars
// are removed. With s == nullptr (menus) all voices are silenced.
void Client::updateAudio(game::MultiplayerSession *s, const std::vector<game::CarView> &cars,
                         const game::CarView *target, float dt)
{
    if (dt <= 0) dt = game::kFixedDt;
    if (!s)
    {
        // No race on screen: silence every voice.
        if (snd_)
            for (auto &kv : carSounds_) snd_->removeCar(kv.first);
        carSounds_.clear();
        return;
    }

    // Listener: the camera (headless: the followed car).
    dm::Vec3 lp = camera_.initialised ? camera_.position : (target ? target->position : dm::Vec3{});
    dm::Vec3 fwd = camera_.initialised ? dm::normalize(camera_.target - camera_.position)
                                       : (target ? dm::forwardOf(target->rotation) : dm::Vec3{0, 0, -1});
    dm::Vec3 up = camera_.initialised ? camera_.up : dm::Vec3{0, 1, 0};
    dm::Vec3 v = (lp - lastListenerPos_) / dt;
    if (dm::length(v) > 80.0f) v = {0, 0, 0}; // a cut, not motion
    listenerVel_ = dm::lerp(listenerVel_, v, 1.0f - std::exp(-8.0f * dt));
    lastListenerPos_ = lp;
    if (snd_) snd_->setListener(lp, fwd, up, listenerVel_);

    const net::MatchPhase phase = s->phase();
    // Race over: the host stops stepping the cars on the podium, so their
    // views keep the last racing state (top speed, nitro, slide). Rather
    // than hold that as a loud sustained tone, every voice fades out.
    const bool raceOver = phase == net::MatchPhase::Results || phase == net::MatchPhase::Lobby;
    const float kRaceOverFade = 0.5f; // seconds
    const game::CarParams &p = game::defaultCarParams();
    for (auto &kv : carSounds_) kv.second.seen = false;
    for (const game::CarView &c : cars)
    {
        CarSound &cs = carSound(c.playerId);
        cs.seen = true;
        if (phase == net::MatchPhase::Countdown)
        {
            // On the grid (also a new race on the same ids: race again, demo
            // loop, next online match): start the sound afresh from idle.
            cs.gearbox.reset();
            cs.lastGear = 1;
            cs.nitro = 0;
            cs.wasNitro = false;
            cs.level = 1.0f;
            cs.removed = false;
        }
        if (c.eliminated)
        {
            if (!cs.removed && snd_) snd_->removeCar(c.playerId); // fades out
            cs.removed = true;
            continue;
        }
        if (raceOver)
        {
            if (cs.removed) continue; // already faded out
            cs.level = std::fmax(0.0f, cs.level - dt / kRaceOverFade);
            if (cs.level <= 0.0f)
            {
                if (snd_) snd_->removeCar(c.playerId);
                cs.removed = true;
                continue;
            }
        }
        else
            cs.removed = false;
        float speed = dm::length(c.velocity);
        bool burning = c.nitroActive && c.alive && !raceOver;
        if (!raceOver) cs.gearbox.update(speed, burning, c.driftAngle, dt); // frozen car: keep the last rpm
        bool shift = cs.gearbox.gear() != cs.lastGear;
        cs.lastGear = cs.gearbox.gear();
        cs.nitro += ((burning ? 1.0f : 0.0f) - cs.nitro) * (1.0f - std::exp(-8.0f * dt));
        dm::Vec3 exhaust = c.position + dm::rotate(c.rotation, dm::Vec3{0.4f * game::kCarScale, -p.rideHeight + 0.3f * game::kCarScale,
                                                                       p.halfLength});
        if (burning && !cs.wasNitro && snd_) snd_->playNitroIgnite(exhaust);
        cs.wasNitro = burning;
        if (!snd_) continue;

        audio::CarAudioState st;
        st.model = c.look % 2 == 0 ? audio::CarModel::AE86 : audio::CarModel::RX7; // matches the drawn car
        st.isLocal = target && c.playerId == target->playerId; // spectated car is close-miked too
        st.position = c.position;
        st.velocity = c.velocity;
        // While fading out after the race the revs also sink toward idle.
        st.rpm = cs.gearbox.idleRpm + (cs.gearbox.rpm() - cs.gearbox.idleRpm) * cs.level;
        // Auto-throttle is on while racing; lift-off when airborne, wrecked /
        // respawning, on the grid (idle) and on the podium.
        bool onPower = phase == net::MatchPhase::Racing && c.alive && !c.airborne;
        st.throttle = onPower ? 1.0f : 0.0f;
        if (raceOver) st.throttle = 0.2f; // a light load: no overrun pops while fading out
        st.shiftEvent = shift;
        // The cosmetic gearbox keeps the revs under the redline, so the
        // synth's fuel-cut limiter bounce is never used in game.
        st.limiter = false;
        float slip = (std::fabs(c.driftAngle) - p.driftAngleThreshold) / 35.0f;
        st.skid01 = c.alive && !c.airborne ? dm::clampf(slip, 0.0f, 1.0f) * dm::clampf(speed / 10.0f, 0.0f, 1.0f) : 0.0f;
        st.skid01 *= cs.level;
        st.nitro01 = cs.nitro;
        st.gain = cs.level;
        snd_->updateCar(c.playerId, st);
    }
    for (auto it = carSounds_.begin(); it != carSounds_.end();)
    {
        if (it->second.seen) { ++it; continue; }
        if (snd_) snd_->removeCar(it->first); // left the room
        it = carSounds_.erase(it);
    }
    playContacts(*s);
}

// One-shots from contact events. Each event is reported once by the
// session; this merges the remaining legitimate duplicates (both cars of a
// bump, a predicted bump and the same bump seen in a snapshot) by time and
// place.
void Client::playContacts(game::MultiplayerSession &s)
{
    const float carMass = 1000.0f; // kg, for the impulse scale the mixer expects
    for (const game::ContactEvent &e : s.takeContactEvents())
    {
        // Takedown feedback for OUR car (presentation only, never the sim).
        if (e.playerId == s.localId())
        {
            if (e.kind == game::ContactEvent::Takedown) { takedownFlash_ = 1.4f; shake_ = 1.0f; }
            else if (e.kind == game::ContactEvent::Wreck) shake_ = std::fmax(shake_, 0.7f);
        }
        bool dup = false;
        for (const RecentHit &h : recentHits_)
            dup = dup || (time_ - h.time < 0.3 && dm::length(h.position - e.position) < 6.0f);
        if (dup) continue;
        recentHits_.push_back({e.position, time_});
        if (!snd_) continue;
        float impulse = carMass * e.speed;
        switch (e.kind)
        {
        case game::ContactEvent::Bump: snd_->playBump(e.position, impulse); break;
        case game::ContactEvent::Barrier: snd_->playCrash(e.position, impulse); break;
        case game::ContactEvent::Wreck: snd_->playCrash(e.position, std::fmax(impulse, 12000.0f)); break;
        case game::ContactEvent::Takedown: snd_->playCrash(e.position, 16000.0f); break;
        }
    }
    recentHits_.erase(std::remove_if(recentHits_.begin(), recentHits_.end(),
                                     [this](const RecentHit &h) { return time_ - h.time > 1.0; }),
                      recentHits_.end());
}

// The rubber-band nitro recharge multiplier a car currently gets (same rule
// as the host's EliminationRace: 0.5x leader .. 2.0x last, by progress).
float Client::rechargeMultiplierFor(game::MultiplayerSession &s, const std::vector<game::CarView> &cars, int32_t id)
{
    float leader = -1e30f, last = 1e30f, mine = 0;
    int racing = 0;
    for (const game::CarView &c : cars)
    {
        if (c.eliminated) continue;
        ++racing;
        leader = std::max(leader, c.distance);
        last = std::min(last, c.distance);
        if (c.playerId == id) mine = c.distance;
    }
    if (racing < 2 || s.phase() != net::MatchPhase::Racing) return 1.0f;
    const game::RaceParams &rp = game::defaultRaceParams();
    if (mine >= leader) return rp.rechargeLeader;
    if (mine <= last) return rp.rechargeLast;
    return game::rechargeMultiplier(mine, leader, last, rp);
}

// Draw the race HUD in 720p-relative units (scaled by `sc`): place, elimination
// clock with stage pips, countdown/GO, gauges, standings, event feed, network
// panel, spectator and wreck banners, and the results table.
void Client::drawRaceHud(game::MultiplayerSession &s, const std::vector<game::CarView> &cars,
                         const game::CarView *target, bool online)
{
    const float W = static_cast<float>(renderer_->framebufferWidth());
    const float H = static_cast<float>(renderer_->framebufferHeight());
    const float sc = H / 720.0f;
    const game::RaceInfo &ri = s.raceInfo();
    const net::MatchPhase phase = s.phase();
    const game::CarView *me = nullptr;
    for (const game::CarView &c : cars)
        if (c.local) me = &c;
    const bool spectating = me && me->eliminated;
    const bool amLast = me && !me->eliminated && ri.clockRunning && ri.lastPlaceId == me->playerId;
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(time_) * 10.0f);

    // --- place (top-left) -------------------------------------------------------
    if (me && phase != net::MatchPhase::Results)
    {
        drawPanel(16 * sc, 16 * sc, 220 * sc, 80 * sc);
        if (spectating) text(30 * sc, 28 * sc, 5.0f * sc, "OUT", kRed);
        else if (me->place > 0)
        {
            std::string pl = fmt("%d", me->place);
            text(30 * sc, 24 * sc, 6.0f * sc, pl, amLast ? kRed : kWhite);
            text(30 * sc + textWidth(pl, 6.0f * sc) + 6 * sc, 52 * sc, 2.4f * sc, fmt("/%d", ri.racing), kGrey);
        }
        else
            text(30 * sc, 28 * sc, 5.0f * sc, fmt("-/%d", ri.total), kGrey);
    }

    // --- elimination clock (top-centre) -------------------------------------
    if (phase == net::MatchPhase::Racing && ri.clockRunning)
    {
        float t = ri.timer;
        std::string clock = fmt("%d.%d", static_cast<int>(t), static_cast<int>(t * 10.0f) % 10);
        dm::Vec3 col = amLast ? dm::lerp(kRed, kWhite, 0.35f * pulse) : (t < 4.0f ? kYellow : kWhite);
        float scale = (amLast ? 7.0f + 0.6f * pulse : 6.0f) * sc;
        if (amLast) renderer_->drawRect(W * 0.5f - 170 * sc, 14 * sc, 340 * sc, 118 * sc, {0.5f, 0.02f, 0.02f}, 0.35f + 0.25f * pulse);
        textCentred(W * 0.5f, 22 * sc, scale, clock, col);
        std::string who = amLast ? std::string("YOU ARE LAST!") : "LAST: " + playerName(ri.lastPlaceId);
        textCentred(W * 0.5f, 84 * sc, 2.2f * sc, who, amLast ? kRed : kGrey);
        // Stage pips: 32 16 8 4 2 1, current highlighted.
        const char *stages[6] = {"32", "16", "8", "4", "2", "1"};
        float x = W * 0.5f - 90 * sc;
        for (int i = 0; i < 6; ++i)
        {
            dm::Vec3 c = i == ri.stage ? kYellow : (i < ri.stage ? kDim * 0.6f : kDim);
            text(x, 110 * sc, 1.8f * sc, stages[i], c);
            x += 30 * sc;
        }
    }
    else if (phase == net::MatchPhase::Racing && ri.racing <= 1 && ri.total <= 1)
        textCentred(W * 0.5f, 24 * sc, 2.4f * sc, "SOLO RUN - no elimination clock", kGrey);

    // --- countdown / GO ------------------------------------------------------------
    if (phase == net::MatchPhase::Countdown && s.phaseTimer() > 0)
        textCentred(W * 0.5f, H * 0.3f, 10.0f * sc, fmt("%d", static_cast<int>(std::ceil(s.phaseTimer()))), kYellow);
    else if (phase == net::MatchPhase::Racing && ri.clockRunning && ri.stage == 0 && ri.timer > 31.0f && ri.racing == ri.total)
        textCentred(W * 0.5f, H * 0.3f, 10.0f * sc, "GO!", kGreen);

    // --- arcade cluster (bottom-right): tach + speed + gear + nitro dial -------
    // Shows the camera car: ours, or the one we spectate.
    const game::CarView *gaugeCar = target ? target : me;
    if (gaugeCar && gauges_ && phase != net::MatchPhase::Results)
    {
        float speed = dm::length(gaugeCar->velocity);
        const game::Gearbox &gearbox_ = carSound(gaugeCar->playerId).gearbox; // updated by updateAudio()
        dm::Vec3 hz = renderer_->skyboxHorizonColor();
        bool night = 0.2126f * hz.x + 0.7152f * hz.y + 0.0722f * hz.z < 0.25f;
        gfx::TachState tach;
        tach.rpm = gearbox_.rpm();
        tach.maxRpm = gearbox_.redlineRpm;
        tach.redlineRpm = 7000.0f;
        tach.gear = gearbox_.gear();
        tach.speedKmh = speed * 3.6f;
        tach.revLimiter = gearbox_.revLimiter();
        tach.night = night;
        tach.time = static_cast<float>(time_);
        gfx::NitroGaugeState ng;
        ng.level01 = dm::clampf(gaugeCar->nitro, 0.0f, 1.0f);
        ng.burning = gaugeCar->nitroActive;
        ng.rechargeMultiplier = rechargeMultiplierFor(s, cars, gaugeCar->playerId);
        ng.slipstream = gaugeCar->slipstream;
        ng.night = night;
        ng.time = tach.time;
        // AE86 looks get the AE86 dial (yellow/red zone), the RX-7 the 8k one.
        gauges_->setRpmScale(gaugeCar->look % 2 == 0 ? "86" : "");
        gauges_->drawCluster(*renderer_, tach, ng);
        // Drift feedback (it's what charges the nitro), centred above the hint line.
        if (gaugeCar->drifting)
            textCentred(W * 0.5f, H - 64 * sc, 2.2f * sc, fmt("DRIFT %.0f deg  +NITRO", std::fabs(gaugeCar->driftAngle)),
                        kGreen);
    }

    // --- standings (left) ------------------------------------------------------------
    if (phase != net::MatchPhase::Results)
    {
        std::vector<const game::CarView *> order;
        for (const game::CarView &c : cars) order.push_back(&c);
        std::sort(order.begin(), order.end(), [](const game::CarView *a, const game::CarView *b) {
            if (a->eliminated != b->eliminated) return !a->eliminated;
            if (a->eliminated) return a->eliminatedOrder > b->eliminatedOrder;
            return a->distance > b->distance;
        });
        float y = 108 * sc;
        drawPanel(16 * sc, y, 250 * sc, (16 + 26 * order.size()) * sc);
        float leadDist = order.empty() ? 0 : order.front()->distance;
        for (size_t i = 0; i < order.size(); ++i)
        {
            const game::CarView &c = *order[i];
            float ry = y + (10 + 26 * i) * sc;
            renderer_->drawRect(26 * sc, ry + 2 * sc, 10 * sc, 14 * sc, lookColor(c.look), c.eliminated ? 0.3f : 1.0f);
            dm::Vec3 col = c.eliminated ? kDim : (c.local ? kYellow : kWhite);
            if (!c.eliminated && ri.clockRunning && c.playerId == ri.lastPlaceId) col = kRed;
            text(44 * sc, ry, 2.0f * sc,
                 fmt("%s %-8s", c.eliminated ? "-" : fmt("%zu", i + 1).c_str(), playerName(c.playerId).c_str()), col);
            std::string gap = c.eliminated ? std::string("OUT") : (i == 0 ? std::string("") : fmt("-%.0fm", leadDist - c.distance));
            text(200 * sc, ry, 1.8f * sc, gap, c.eliminated ? kDim : kGrey);
        }
    }

    // --- elimination feed (right) -------------------------------------------------
    {
        float y = (online ? 120 : 86) * sc;
        for (auto it = s.feed().rbegin(); it != s.feed().rend(); ++it)
        {
            if (it->age > 5.0f) continue;
            std::string line;
            dm::Vec3 col = kGrey;
            if (it->kind == game::FeedEvent::Eliminated) { line = playerName(it->playerId) + " ELIMINATED"; col = kRed; }
            else if (it->kind == game::FeedEvent::Winner) { line = playerName(it->playerId) + " WINS!"; col = kYellow; }
            else line = fmt("%s is last  (%.0fs)", playerName(it->playerId).c_str(), ri.stageTime);
            text(W - 20 * sc - textWidth(line, 2.2f * sc), y, 2.2f * sc, line, col);
            y += 26 * sc;
        }
    }

    // --- network panel (top-right) --------------------------------------------------
    if (online)
    {
        drawPanel(W - 236 * sc, 16 * sc, 220 * sc, 92 * sc);
        float rx = W - 222 * sc;
        float rtt = net_->rttMs();
        net::Transport t = net_->transport();
        text(rx, 26 * sc, 2.0f * sc, rtt >= 0 ? fmt("PING %.0f ms", rtt) : std::string("PING --"));
        text(rx, 48 * sc, 2.0f * sc,
             netLan_ ? std::string("NET  LAN") : std::string("NET  ") + transportName(t != net::Transport::None ? t : transportKind_),
             kGrey);
        if (s.isHost())
        {
            renderer_->drawRect(rx, 70 * sc, 60 * sc, 18 * sc, kAccent, 0.9f);
            text(rx + 8 * sc, 73 * sc, 1.8f * sc, "HOST");
        }
    }
    else
    {
        drawPanel(W - 236 * sc, 16 * sc, 220 * sc, 56 * sc);
        text(W - 222 * sc, 26 * sc, 2.0f * sc, fmt("OFFLINE %d BOTS", ri.total - 1), kGrey);
        text(W - 222 * sc, 48 * sc, 1.8f * sc, fmt("wins %d / %d", offlineWins_, offlineRaces_), kDim);
    }

    // --- spectator banner -----------------------------------------------------------
    if (spectating && phase == net::MatchPhase::Racing && target)
    {
        renderer_->drawRect(W * 0.5f - 330 * sc, 138 * sc, 660 * sc, 34 * sc, {0.05f, 0.05f, 0.08f}, 0.7f);
        textCentred(W * 0.5f, 146 * sc, 2.2f * sc,
                    "ELIMINATED - spectating " + playerName(target->playerId) + "  (LEFT/RIGHT)", kWhite);
    }
    if (me && !me->alive && !spectating && phase == net::MatchPhase::Racing)
        textCentred(W * 0.5f, H * 0.42f, 5.0f * sc, "WRECKED!", kRed);
    if (takedownFlash_ > 0 && phase == net::MatchPhase::Racing)
    {
        float k = std::fmin(1.0f, takedownFlash_ / 0.4f); // fades over the last 0.4 s
        float grow = 1.0f + 0.25f * std::fmax(0.0f, takedownFlash_ - 1.1f) / 0.3f; // punch in
        textCentred(W * 0.5f, H * 0.3f, 7.0f * sc * grow, "TAKEDOWN!", dm::lerp(kWhite, kYellow, k));
    }
    // No snapshots: the host stalled or left. Keep driving (prediction) and
    // say so; NET promotes a new host after its session timeout.
    if (online && s.secondsSinceSnapshot() > 1.0f)
        textCentred(W * 0.5f, H * 0.36f, 3.0f * sc,
                    fmt("WAITING FOR HOST... %.0f s", s.secondsSinceSnapshot()), dm::lerp(kYellow, kWhite, pulse));

    // --- results ----------------------------------------------------------------------
    if (phase == net::MatchPhase::Results)
    {
        const std::vector<int32_t> &order = ri.finishingOrder;
        float pw = 560 * sc, ph = (130 + 40 * order.size()) * sc;
        float px = (W - pw) * 0.5f, py = H * 0.2f;
        drawPanel(px, py, pw, ph);
        bool won = me && ri.winnerId == me->playerId;
        textCentred(W * 0.5f, py + 18 * sc, 4.5f * sc, won ? "YOU WIN!" : "RESULTS", won ? kGreen : kYellow);
        const char *ord[4] = {"1st", "2nd", "3rd", "4th"};
        for (size_t i = 0; i < order.size() && i < 4; ++i)
        {
            int32_t id = order[i];
            const game::CarView *c = nullptr;
            for (const game::CarView &v : cars)
                if (v.playerId == id) c = &v;
            float y = py + (80 + 40 * i) * sc;
            dm::Vec3 col = c && c->local ? kYellow : kWhite;
            if (c) renderer_->drawRect(px + 30 * sc, y + 2 * sc, 14 * sc, 20 * sc, lookColor(c->look), 1.0f);
            text(px + 56 * sc, y, 2.8f * sc, fmt("%s  %s", ord[i], playerName(id).c_str()), col);
            if (c) text(px + 330 * sc, y + 4 * sc, 2.0f * sc, i == 0 ? std::string("WINNER") : fmt("out #%d", c->eliminatedOrder), i == 0 ? kGreen : kGrey);
        }
        std::string foot = online ? fmt("back to lobby in %.0f s", std::ceil(s.phaseTimer()))
                                  : std::string("ENTER race again     ESC menu");
        textCentred(W * 0.5f, py + ph - 30 * sc, 2.0f * sc, foot, kGrey);
    }

    if (raceTime_ < 8.0f && !opt_.demo && phase != net::MatchPhase::Results)
        text(20 * sc, H - 30 * sc, 1.6f * sc, "A/D or LEFT/RIGHT steer   SPACE nitro (drift to charge)   ESC menu", kGrey);
    if (opt_.demo) text(20 * sc, H - 30 * sc, 1.6f * sc, fmt("%.0f fps", fps_), kDim);
}

// Draw the menu screens. Each Screen case lays out its panel with the small
// helpers below (title, text field, status line, hint line).
void Client::drawMenus()
{
    const float W = static_cast<float>(renderer_->framebufferWidth());
    const float H = static_cast<float>(renderer_->framebufferHeight());
    const float s = H / 720.0f;
    const float cx = W * 0.5f;

    auto title = [&](const std::string &sub) {
        textCentred(cx, H * 0.12f, 9.0f * s, "DRIFTO", kWhite);
        textCentred(cx, H * 0.12f + 70 * s, 2.0f * s, sub, kGrey);
    };
    auto field = [&](float y, const char *label, const TextField &f, bool focus) {
        float w = 360 * s, x = cx - w * 0.5f;
        text(x, y - 26 * s, 2.0f * s, label, kGrey);
        renderer_->drawRect(x, y, w, 40 * s, focus ? dm::Vec3{0.18f, 0.2f, 0.26f} : dm::Vec3{0.08f, 0.09f, 0.12f}, 0.85f);
        std::string shown = f.password ? std::string(f.text.size(), '*') : f.text;
        bool caret = focus && std::fmod(time_, 1.0) < 0.5;
        text(x + 12 * s, y + 10 * s, 2.6f * s, shown + (caret ? "_" : ""), kWhite);
    };
    auto statusLine = [&](float y) {
        if (!status_.empty()) textCentred(cx, y, 2.0f * s, status_, kAccent);
    };
    auto hint = [&](const std::string &h) { textCentred(cx, H - 40 * s, 1.8f * s, h, kGrey); };

    switch (screen_)
    {
    case Screen::MainMenu:
    {
        title("elimination touge - last place has a clock");
        const char *items[4] = {"OFFLINE  -  race the bots", "ONLINE  -  race on a server", "LAN  -  race on this network",
                                "QUIT"};
        drawPanel(cx - 230 * s, H * 0.42f - 20 * s, 460 * s, 220 * s);
        for (int i = 0; i < 4; ++i)
        {
            float y = H * 0.42f + i * 50 * s;
            bool sel = i == menuIndex_;
            if (sel) renderer_->drawRect(cx - 210 * s, y - 8 * s, 420 * s, 38 * s, kAccent, 0.85f);
            textCentred(cx, y, 2.8f * s, items[i], sel ? kWhite : kGrey);
        }
        hint("UP/DOWN select   ENTER confirm   ESC quit");
        break;
    }
    case Screen::Login:
        if (lanMode_)
        {
            title("LAN  -  no server needed");
            drawPanel(cx - 230 * s, H * 0.38f, 460 * s, 140 * s);
            field(H * 0.38f + 50 * s, "NAME (max 8)", nameField_, true);
            statusLine(H * 0.38f + 160 * s);
            hint("ENTER find LAN games   ESC back");
        }
        else
        {
            title("online  -  log in");
            drawPanel(cx - 230 * s, H * 0.33f, 460 * s, 310 * s);
            field(H * 0.33f + 50 * s, "NAME (max 8)", nameField_, loginFocus_ % 3 == 0);
            field(H * 0.33f + 140 * s, "PASSWORD", passField_, loginFocus_ % 3 == 1);
            field(H * 0.33f + 230 * s, "SERVER (API address)", apiField_, loginFocus_ % 3 == 2);
            statusLine(H * 0.33f + 330 * s);
            hint("TAB switch field   ENTER log in   ESC back");
        }
        break;
    case Screen::RoomList:
    {
        title(lanMode_ ? fmt("LAN games  -  you are %s", nameField_.text.c_str())
                       : fmt("rooms on %s  -  logged in as %s", opt_.apiHost.c_str(), nameField_.text.c_str()));
        float pw = 620 * s, px = cx - pw * 0.5f, py = H * 0.33f;
        int shown = std::min<int>(static_cast<int>(rooms_.size()), 8);
        drawPanel(px, py, pw, (60 + 40 * std::max(shown, 1)) * s);
        text(px + 20 * s, py + 16 * s, 2.0f * s, "ROOM", kDim);
        text(px + 380 * s, py + 16 * s, 2.0f * s, "PLAYERS", kDim);
        if (rooms_.empty())
            textCentred(cx, py + 60 * s, 2.2f * s,
                        lanMode_ ? "no games found on the LAN yet - press H to host one" : "no rooms yet - press H to host one",
                        kGrey);
        for (int i = 0; i < shown; ++i)
        {
            const net::RoomInfo &r = rooms_[i];
            float y = py + (52 + 40 * i) * s;
            bool sel = i == roomIndex_;
            if (sel) renderer_->drawRect(px + 8 * s, y - 6 * s, pw - 16 * s, 34 * s, kAccent, 0.8f);
            dm::Vec3 c = r.inProgress ? kDim : kWhite;
            text(px + 20 * s, y, 2.4f * s, r.name, c);
            text(px + 380 * s, y, 2.4f * s, fmt("%d/%d%s", r.players, r.maxPlayers, r.inProgress ? "  racing" : ""), c);
        }
        statusLine(py + (80 + 40 * std::max(shown, 1)) * s);
        hint(lanMode_ ? "UP/DOWN select   ENTER join   H host game   ESC back"
                      : "UP/DOWN select   ENTER join   H host   C create named   R refresh   ESC back");
        break;
    }
    case Screen::CreateRoom:
        title("create a room");
        drawPanel(cx - 230 * s, H * 0.38f, 460 * s, 130 * s);
        field(H * 0.38f + 50 * s, "ROOM NAME", roomField_, true);
        statusLine(H * 0.38f + 150 * s);
        hint("ENTER create   ESC back");
        break;
    case Screen::Lobby:
    {
        title(lanMode_ ? "lobby  (LAN)" : "lobby");
        float pw = 520 * s, px = cx - pw * 0.5f, py = H * 0.33f;
        drawPanel(px, py, pw, (70 + 38 * std::max<size_t>(roster_.size(), 1)) * s);
        text(px + 20 * s, py + 16 * s, 2.0f * s, "PLAYER", kDim);
        text(px + 340 * s, py + 16 * s, 2.0f * s, "STATUS", kDim);
        int32_t me = net_->localPlayerId();
        for (size_t i = 0; i < roster_.size(); ++i)
        {
            const net::RosterEntry &r = roster_[i];
            float y = py + (54 + 38 * i) * s;
            dm::Vec3 c = r.playerId == me ? kYellow : kWhite;
            text(px + 20 * s, y, 2.6f * s, r.name, c);
            if (r.isHost)
            {
                renderer_->drawRect(px + 200 * s, y - 2 * s, 60 * s, 22 * s, kAccent, 0.9f);
                text(px + 208 * s, y + 2 * s, 1.8f * s, "HOST");
            }
            text(px + 340 * s, y, 2.4f * s, r.ready ? "READY" : "...", r.ready ? kGreen : kDim);
        }
        statusLine(py + (90 + 38 * std::max<size_t>(roster_.size(), 1)) * s);
        hint(net_->isHost() ? "SPACE ready   ENTER start race   ESC leave"
                            : "SPACE ready   (the host starts the race)   ESC leave");
        break;
    }
    case Screen::Message:
    {
        float pw = 640 * s, px = cx - pw * 0.5f, py = H * 0.38f;
        drawPanel(px, py, pw, 130 * s);
        textCentred(cx, py + 30 * s, 2.4f * s, message_, kAccent);
        textCentred(cx, py + 86 * s, 2.0f * s, "ENTER ok", kGrey);
        break;
    }
    default: break;
    }
}

} // namespace client
