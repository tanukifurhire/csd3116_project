// client.h -- the `drifto` application: GLFW window, fixed 60 Hz loop with
// render interpolation, menu state machine, offline and online races, HUD.
//
// Screens:
//   MainMenu -> Offline: elimination race against 1-3 bots (we host it
//               locally) -> results -> (again | main menu)
//            -> Online: Login -> Room list (refresh/create/join) -> Lobby
//               (roster, ready, host starts) -> Race (countdown/racing/
//               results, driven by the host) -> back to Lobby
// Controls: auto-throttle, A/D or Left/Right steer, Space nitro, Esc back.
// Eliminated players spectate (Left/Right switches car). Menu, room list and
// lobby: A/D or Left/Right picks the car (AE86 / RX-7, cosmetic, remembered).
#pragma once

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "client_folder/include/net_transport.h"
#include "client_folder/include/world_view.h"
#include "game/include/car_physics.h"
#include "game/include/chase_camera.h"
#include "game/include/gearbox.h"
#include "game/include/multiplayer_session.h"
#include "game/include/race.h"
#include "game/include/track.h"
#include "audio/include/audio.h"
#include "gfx/include/hud_gauges.h"
#include "gfx/include/renderer.h"
#include "net/include/net_client.h"

struct GLFWwindow;

namespace client {

struct Options
{
    bool offline = false;           // skip the main menu, straight into an offline run
    bool lan = false;               // start in LAN mode (serverless)
    std::vector<std::string> lanPeers; // LAN unicast peers when multicast is blocked
    std::string apiHost = "127.0.0.1";
    bool apiGiven = false;          // --api on the command line (else the remembered one)
    int certIndex = 1;
    net::TransportPref transport = net::TransportPref::Auto;
    bool insecure = false;
    std::string skybox = "SkyAfterNoon"; // the original day scene
    std::string assetDir = "assets";
    int width = 1280, height = 720;
    uint32_t seed = 0;               // offline track seed, 0 = random
    int bots = 3;                    // offline opponents (1..3)
    bool demo = false;               // autopilot drives (offline)
    std::string screenshot;          // save a PNG after shotDelay seconds, then quit
    float shotDelay = 7.0f;
    bool lowQuality = false;
    int fpsCap = 60;                 // frame limiter (vsync doesn't throttle llvmpipe on XWayland)
    bool mute = false;               // --mute: no sound at all
    float volume = -1.0f;            // --volume 0..1 (< 0 = remembered setting, else 0.8)
    int carModel = -1;               // --car ae86|rx7 (< 0 = remembered setting, else AE86)
    // Automation (smoke tests / demos): log in, join-or-create a room, ready
    // up, and (host) start once `autoStart` players are in the room.
    std::string autoLogin;           // "NAME:PASS" (online) or "NAME" (LAN)
    std::string autoRoom;
    int autoStart = 0;
    // Headless (no window, no GL; implies --demo): bots and soak runs.
    bool headless = false;
    bool fast = false;               // headless offline: simulate as fast as possible
    int races = 0;                   // offline: quit after this many races (0 = never)
    float duration = 0;              // quit after this many seconds (0 = never)
};

// Returns false on bad arguments (error set) or --help (error empty).
bool parseArgs(int argc, char **argv, Options &out, std::string &error);
const char *usageText();

class Client
{
public:
    explicit Client(const Options &options = {});
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    bool Init();
    int Run();
    void Shutdown();

    // GLFW callbacks (forwarded through the window user pointer).
    void onChar(unsigned codepoint);
    void onKey(int key, int action, int mods);

private:
    enum class Screen
    {
        MainMenu,
        OfflineRace,
        Login,
        RoomList,
        CreateRoom,
        Lobby,
        OnlineRace,
        Message,
    };
    struct TextField
    {
        std::string text;
        size_t maxLen = 16;
        bool password = false;
    };
    // Menu background: one bot lapping a random track.
    struct LocalRun
    {
        std::unique_ptr<game::Track> track;
        game::CarSimState car, prev;
        game::BotDriver bot;
        uint32_t tick = 0;
    };

    // --- frame / tick ---------------------------------------------------------
    void frame(float dt, float alpha);
    void fixedTick();
    int runHeadless();
    void onRaceFinished(game::MultiplayerSession &s);
    net::CarInput readDriveInput(uint32_t tick, const game::CarSimState *car, const game::Track *track);
    void autoStep();
    void stepAttract();
    void startOfflineRun();
    void startAttract();
    // The session of the race on screen (offline or online), or nullptr.
    game::MultiplayerSession *raceSession();
    void raceTick(game::MultiplayerSession &s);
    void cycleSpectate(int dir);
    const game::CarView *cameraTarget(const std::vector<game::CarView> &cars);

    // --- networking -----------------------------------------------------------
    // Starts (or restarts, if the mode / server changed) the NetClient.
    bool ensureNetStarted();
    void loadSettings();
    void saveSettings(const char *mode) const;
    void submitLogin();
    // Sound: engines / squeal / nitro per car, one-shots from contact events.
    void updateAudio(game::MultiplayerSession *s, const std::vector<game::CarView> &cars,
                     const game::CarView *target, float dt);
    void playContacts(game::MultiplayerSession &s);
    void pollNet();
    std::vector<int32_t> rosterIds() const;
    std::string playerName(int32_t id) const;
    void goToMessage(const std::string &text, Screen back);
    void enterOnlineRace(uint32_t seed);

    // --- input handling per screen ------------------------------------------
    void handleKey(int key, int mods);
    void handleText(unsigned codepoint);
    void menuKey(int key);
    void loginKey(int key);
    void roomListKey(int key);
    void lobbyKey(int key);

    // --- drawing ----------------------------------------------------------------
    void drawWorld(const game::Track &track, const std::vector<game::CarView> &cars, const game::CarView *target,
                   float dt, bool racing);
    void drawMenus();
    void drawRaceHud(game::MultiplayerSession &s, const std::vector<game::CarView> &cars, const game::CarView *target,
                     bool online);
    void drawPanel(float x, float y, float w, float h);
    void text(float x, float y, float scale, const std::string &s, const dm::Vec3 &c = {1, 1, 1});
    void textCentred(float cx, float y, float scale, const std::string &s, const dm::Vec3 &c = {1, 1, 1});
    float textWidth(const std::string &s, float scale) const;
    float rechargeMultiplierFor(game::MultiplayerSession &s, const std::vector<game::CarView> &cars, int32_t id);
    gfx::Lighting lighting() const;

    Options opt_;
    GLFWwindow *window_ = nullptr;
    std::unique_ptr<gfx::Renderer> renderer_;
    std::unique_ptr<WorldView> world_;
    std::unique_ptr<net::NetClient> net_;
    std::unique_ptr<NetClientTransport> transport_;
    std::unique_ptr<game::MultiplayerSession> session_;
    bool netStarted_ = false;

    Screen screen_ = Screen::MainMenu;
    Screen messageBack_ = Screen::MainMenu;
    std::string message_;
    std::string status_;
    int menuIndex_ = 0;
    int carModel_ = 0;               // our car: 0 = AE86, 1 = RX-7 (cosmetic, remembered)
    void setCarModel(int model);
    TextField nameField_, passField_, roomField_, apiField_;
    bool lanMode_ = false;           // the multiplayer mode picked in the menu
    bool netLan_ = false;            // ...and the one the NetClient was started with
    std::string netApi_;
    int loginFocus_ = 0;
    bool loginPending_ = false;
    std::vector<net::RoomInfo> rooms_;
    int roomIndex_ = 0;
    double lastRoomRefresh_ = -100;
    std::vector<net::RosterEntry> roster_;
    std::map<int32_t, std::string> knownNames_; // names outlive leaving the room (results)
    bool ready_ = false;
    net::Transport transportKind_ = net::Transport::None;

    // Offline: we host a local race (loopback transport) with bots.
    std::unique_ptr<game::LoopbackTransport> offlineNet_;
    std::unique_ptr<game::MultiplayerSession> offline_;
    game::BotDriver demoBot_; // --demo: drives our car
    int offlineWins_ = 0, offlineRaces_ = 0;
    bool offlineCounted_ = false;
    LocalRun attract_;

    std::unique_ptr<gfx::HudGauges> gauges_;
    // Per-car presentation state: cosmetic gearbox (tach + engine sound),
    // nitro edge detection, sound voice bookkeeping.
    struct CarSound
    {
        game::Gearbox gearbox;
        int lastGear = 1;
        bool wasNitro = false;
        float nitro = 0;
        bool removed = false;
        bool seen = false;
        float level = 1;       // 1 while racing; fades to 0 once the race is over
    };
    std::map<int32_t, CarSound> carSounds_;
    CarSound &carSound(int32_t id);
    std::unique_ptr<audio::Engine> snd_;
    struct RecentHit
    {
        dm::Vec3 position;
        double time;
    };
    std::vector<RecentHit> recentHits_;
    dm::Vec3 lastListenerPos_;
    dm::Vec3 listenerVel_;
    bool windowFocused_ = true;
    float volume_ = 0.8f;
    int32_t spectateId_ = 0;
    int32_t cameraCarId_ = -1;
    dm::Vec3 cameraCarPos_;
    float fov_ = dm::radians(70.0f);
    float fxNitro_ = 0, fxSlip_ = 0;
    float takedownFlash_ = 0; // > 0: "TAKEDOWN!" on the HUD (seconds left)
    float shake_ = 0;         // camera shake amount 0..1 (takedowns / wrecks)

    game::ChaseCamera camera_;
    uint32_t cameraSeed_ = 0xffffffffu;
    double time_ = 0;
    double accumulator_ = 0;
    float fps_ = 60;
    float raceTime_ = 0; // seconds since the current race screen started
    float frameDt_ = 0;
    bool quit_ = false;
    double startTime_ = 0;
    bool autoRoomRequested_ = false, autoReadySent_ = false;
    // LAN rooms take ~0.3-1 s to be discovered: --room waits this long for the
    // named room to show up before creating it itself.
    double autoRoomCreateAfter_ = 0;
};

} // namespace client
