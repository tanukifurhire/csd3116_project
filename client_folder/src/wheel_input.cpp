// wheel_input.cpp -- DirectInput 8 wheel / pedals (see wheel_input.h).
// Non-Windows builds get the settings parser and a stub that never connects.
#include "client_folder/include/wheel_input.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define DIRECTINPUT_VERSION 0x0800
#define GLFW_EXPOSE_NATIVE_WIN32
#include <windows.h>
#include <dinput.h>
#include <GLFW/glfw3native.h>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#endif

namespace client {

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------

namespace {
const char *const kAxisNames[kWheelAxisCount + 1] = {"x", "y", "z", "rx", "ry", "rz", "slider0", "slider1", "none"};

bool parseAxis(const std::string &v, WheelAxis &out)
{
    for (int i = 0; i <= kWheelAxisCount; ++i)
        if (v == kAxisNames[i]) { out = static_cast<WheelAxis>(i); return true; }
    return false;
}
bool parseBool(const std::string &v, bool &out)
{
    if (v == "1" || v == "on" || v == "true" || v == "yes") { out = true; return true; }
    if (v == "0" || v == "off" || v == "false" || v == "no") { out = false; return true; }
    return false;
}
bool parseFloat(const std::string &v, float lo, float hi, float &out)
{
    char *end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    if (end == v.c_str() || f < lo || f > hi) return false;
    out = f;
    return true;
}
bool parseInt(const std::string &v, int &out)
{
    char *end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || n < -1 || n > 127) return false;
    out = static_cast<int>(n);
    return true;
}
} // namespace

const char *wheelAxisName(WheelAxis a)
{
    int i = static_cast<int>(a);
    return i >= 0 && i <= kWheelAxisCount ? kAxisNames[i] : "?";
}

bool applyWheelSetting(WheelConfig &c, const std::string &k, const std::string &v)
{
    if (k == "wheel") return parseBool(v, c.enabled);
    if (k == "wheel_device") { c.deviceMatch = v; return true; }
    if (k == "wheel_steer_axis") return parseAxis(v, c.steerAxis);
    if (k == "wheel_throttle_axis") return parseAxis(v, c.throttleAxis);
    if (k == "wheel_brake_axis") return parseAxis(v, c.brakeAxis);
    if (k == "wheel_combined_pedals") return parseBool(v, c.combinedPedals);
    if (k == "wheel_invert_steer") return parseBool(v, c.invertSteer);
    if (k == "wheel_steer_gain") return parseFloat(v, 0.1f, 20.0f, c.steerGain);
    if (k == "wheel_steer_deadzone") return parseFloat(v, 0.0f, 0.5f, c.steerDeadzone);
    if (k == "wheel_pedal_deadzone") return parseFloat(v, 0.0f, 0.5f, c.pedalDeadzone);
    if (k == "wheel_pedals") return parseBool(v, c.usePedals);
    if (k == "wheel_accept_button") return parseInt(v, c.acceptButton);
    if (k == "wheel_back_button") return parseInt(v, c.backButton);
    if (k == "wheel_nitro_buttons") // comma separated, e.g. "4,5" (empty = none)
    {
        std::vector<int> b;
        size_t i = 0;
        while (i < v.size())
        {
            size_t j = v.find(',', i);
            if (j == std::string::npos) j = v.size();
            int n = -1;
            if (!parseInt(v.substr(i, j - i), n) || n < 0) return false;
            b.push_back(n);
            i = j + 1;
        }
        c.nitroButtons = b;
        return true;
    }
    return false;
}

#ifdef _WIN32
// ---------------------------------------------------------------------------
// DirectInput
// ---------------------------------------------------------------------------

namespace {
constexpr double kRescanInterval = 3.0; // s between EnumDevices while no wheel is attached

struct EnumCtx
{
    const WheelConfig *cfg;
    GUID guid;
    bool found = false;
    bool foundIsWheel = false;
    std::string name;
};

std::string narrow(const wchar_t *w)
{
    char buf[MAX_PATH * 2];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string();
}

std::string lower(std::string s)
{
    for (char &ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// First driving-class device, or (wheel_device set) the first game controller
// whose name contains that text -- some wheels enumerate as joysticks.
BOOL CALLBACK enumDevice(LPCDIDEVICEINSTANCEW inst, LPVOID user)
{
    EnumCtx &ctx = *static_cast<EnumCtx *>(user);
    std::string name = narrow(inst->tszProductName);
    bool wheel = GET_DIDEVICE_TYPE(inst->dwDevType) == DI8DEVTYPE_DRIVING;
    bool match = ctx.cfg->deviceMatch.empty() ? wheel
                                              : lower(name).find(lower(ctx.cfg->deviceMatch)) != std::string::npos;
    if (!match) return DIENUM_CONTINUE;
    ctx.guid = inst->guidInstance;
    ctx.found = true;
    ctx.foundIsWheel = wheel;
    ctx.name = name;
    return DIENUM_STOP;
}

float axisValue(const DIJOYSTATE2 &js, WheelAxis a)
{
    LONG v = 0;
    switch (a)
    {
    case WheelAxis::X: v = js.lX; break;
    case WheelAxis::Y: v = js.lY; break;
    case WheelAxis::Z: v = js.lZ; break;
    case WheelAxis::RX: v = js.lRx; break;
    case WheelAxis::RY: v = js.lRy; break;
    case WheelAxis::RZ: v = js.lRz; break;
    case WheelAxis::Slider0: v = js.rglSlider[0]; break;
    case WheelAxis::Slider1: v = js.rglSlider[1]; break;
    default: return 0.0f;
    }
    float f = static_cast<float>(v) / 1000.0f; // range set to -1000..1000 below
    return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
}

// POV hat (hundredths of a degree, 0 = up) -> GLFW arrow key, or 0 centred.
int povKey(DWORD pov)
{
    if (LOWORD(pov) == 0xFFFF) return 0;
    DWORD d = pov / 100;
    if (d >= 315 || d < 45) return GLFW_KEY_UP;
    if (d < 135) return GLFW_KEY_RIGHT;
    if (d < 225) return GLFW_KEY_DOWN;
    return GLFW_KEY_LEFT;
}
} // namespace

struct WheelInput::Impl
{
    WheelConfig cfg;
    HWND hwnd = nullptr;
    IDirectInput8W *di = nullptr;
    IDirectInputDevice8W *dev = nullptr;
    double nextScan = 0.0;
    double nextDebug = 0.0;
    int failures = 0;
    PedalCalibration throttleCal, brakeCal;
    BYTE prevButtons[128] = {};
    int prevPov = 0;
    std::vector<int> keys;

    void release()
    {
        if (dev)
        {
            dev->Unacquire();
            dev->Release();
            dev = nullptr;
        }
    }

    bool open(WheelState &st)
    {
        EnumCtx ctx;
        ctx.cfg = &cfg;
        if (FAILED(di->EnumDevices(DI8DEVCLASS_GAMECTRL, enumDevice, &ctx, DIEDFL_ATTACHEDONLY)) || !ctx.found)
            return false;
        if (FAILED(di->CreateDevice(ctx.guid, &dev, nullptr))) { dev = nullptr; return false; }
        // Non-exclusive + background: no fight with the wheel's own software,
        // and no re-acquire dance on focus changes (the client gates on focus).
        if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2)) ||
            FAILED(dev->SetCooperativeLevel(hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND)))
        {
            release();
            return false;
        }
        DIPROPRANGE range = {};
        range.diph.dwSize = sizeof range;
        range.diph.dwHeaderSize = sizeof range.diph;
        range.diph.dwHow = DIPH_DEVICE; // every axis
        range.lMin = -1000;
        range.lMax = 1000;
        dev->SetProperty(DIPROP_RANGE, &range.diph);
        DIPROPDWORD dz = {}; // our own deadzones; the driver's off
        dz.diph.dwSize = sizeof dz;
        dz.diph.dwHeaderSize = sizeof dz.diph;
        dz.diph.dwHow = DIPH_DEVICE;
        dz.dwData = 0;
        dev->SetProperty(DIPROP_DEADZONE, &dz.diph);
        dev->Acquire();

        st = WheelState{};
        st.connected = true;
        st.name = ctx.name;
        throttleCal = brakeCal = PedalCalibration{};
        std::memset(prevButtons, 0, sizeof prevButtons);
        prevPov = 0;
        failures = 0;
        std::fprintf(stderr, "drifto: wheel \"%s\"%s: steer %s, throttle %s, brake %s%s\n", ctx.name.c_str(),
                     ctx.foundIsWheel ? "" : " (not a driving device)", wheelAxisName(cfg.steerAxis),
                     wheelAxisName(cfg.throttleAxis),
                     cfg.combinedPedals ? "(combined)" : wheelAxisName(cfg.brakeAxis),
                     cfg.usePedals ? "" : ", pedals off (auto-throttle)");
        return true;
    }

    void lost(WheelState &st, double now)
    {
        std::fprintf(stderr, "drifto: wheel \"%s\" disconnected\n", st.name.c_str());
        release();
        st = WheelState{};
        nextScan = now + kRescanInterval;
    }

    void poll(WheelState &st, double now)
    {
        HRESULT hr = dev->Poll();
        if (FAILED(hr))
        {
            hr = dev->Acquire();
            // Unplugged, or still failing after ~1 s of frames: drop it and rescan.
            if (hr == DIERR_UNPLUGGED || ++failures > 60) lost(st, now);
            return;
        }
        DIJOYSTATE2 js;
        hr = dev->GetDeviceState(sizeof js, &js);
        if (FAILED(hr))
        {
            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) dev->Acquire();
            if (hr == DIERR_UNPLUGGED || ++failures > 60) lost(st, now);
            return;
        }
        failures = 0;

        st.steer = steerFromAxis(axisValue(js, cfg.steerAxis), cfg);
        if (cfg.combinedPedals)
        {
            float v = axisValue(js, cfg.throttleAxis);
            st.throttle = applyDeadzone(v < 0 ? -v : 0.0f, cfg.pedalDeadzone);
            st.brake = applyDeadzone(v > 0 ? v : 0.0f, cfg.pedalDeadzone);
        }
        else
        {
            st.throttle = cfg.throttleAxis == WheelAxis::None
                              ? 0.0f
                              : pedalFromAxis(throttleCal, axisValue(js, cfg.throttleAxis), cfg.pedalDeadzone);
            st.brake = cfg.brakeAxis == WheelAxis::None
                           ? 0.0f
                           : pedalFromAxis(brakeCal, axisValue(js, cfg.brakeAxis), cfg.pedalDeadzone);
        }
        if (st.throttle > 0.3f) st.throttleSeen = true;

        st.nitro = false;
        for (int b : cfg.nitroButtons)
            if (b >= 0 && b < 128 && (js.rgbButtons[b] & 0x80)) st.nitro = true;
        auto pressed = [&](int b) { return b >= 0 && b < 128 && (js.rgbButtons[b] & 0x80) && !(prevButtons[b] & 0x80); };
        if (pressed(cfg.acceptButton)) keys.push_back(GLFW_KEY_ENTER);
        if (pressed(cfg.backButton)) keys.push_back(GLFW_KEY_ESCAPE);
        std::memcpy(prevButtons, js.rgbButtons, sizeof prevButtons);
        int pk = povKey(js.rgdwPOV[0]);
        if (pk && pk != prevPov) keys.push_back(pk);
        prevPov = pk;

        if (cfg.debug && now >= nextDebug)
        {
            nextDebug = now + 1.0;
            const float ax[kWheelAxisCount] = {js.lX / 1000.f, js.lY / 1000.f, js.lZ / 1000.f, js.lRx / 1000.f,
                                               js.lRy / 1000.f, js.lRz / 1000.f, js.rglSlider[0] / 1000.f,
                                               js.rglSlider[1] / 1000.f};
            std::string held;
            for (int b = 0; b < 128; ++b)
                if (js.rgbButtons[b] & 0x80) held += (held.empty() ? "" : ",") + std::to_string(b);
            std::fprintf(stderr,
                         "wheel: x %+.2f y %+.2f z %+.2f rx %+.2f ry %+.2f rz %+.2f s0 %+.2f s1 %+.2f pov %lu "
                         "buttons [%s] -> steer %+.2f thr %.2f brk %.2f%s\n",
                         ax[0], ax[1], ax[2], ax[3], ax[4], ax[5], ax[6], ax[7],
                         static_cast<unsigned long>(js.rgdwPOV[0]), held.c_str(), st.steer, st.throttle, st.brake,
                         st.nitro ? " NITRO" : "");
        }
    }
};

WheelInput::WheelInput() = default;
WheelInput::~WheelInput() { shutdown(); }

bool WheelInput::init(GLFWwindow *window, const WheelConfig &cfg)
{
    shutdown();
    if (!cfg.enabled || !window) return false;
    impl_ = std::make_unique<Impl>();
    impl_->cfg = cfg;
    impl_->hwnd = glfwGetWin32Window(window);
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                  reinterpret_cast<void **>(&impl_->di), nullptr)))
    {
        std::fprintf(stderr, "drifto: DirectInput unavailable, no wheel support\n");
        impl_.reset();
        return false;
    }
    return true;
}

void WheelInput::shutdown()
{
    if (!impl_) return;
    impl_->release();
    if (impl_->di) impl_->di->Release();
    impl_.reset();
    state_ = WheelState{};
}

void WheelInput::update(double now)
{
    if (!impl_) return;
    if (!impl_->dev)
    {
        if (now < impl_->nextScan) return;
        impl_->nextScan = now + kRescanInterval;
        if (!impl_->open(state_)) return;
    }
    impl_->poll(state_, now);
}

std::vector<int> WheelInput::takeKeyPresses()
{
    std::vector<int> k;
    if (impl_) k.swap(impl_->keys);
    return k;
}

#else // !_WIN32: no DirectInput

struct WheelInput::Impl
{
};
WheelInput::WheelInput() = default;
WheelInput::~WheelInput() = default;
bool WheelInput::init(GLFWwindow *, const WheelConfig &) { return false; }
void WheelInput::shutdown() {}
void WheelInput::update(double) {}
std::vector<int> WheelInput::takeKeyPresses() { return {}; }

#endif

} // namespace client
