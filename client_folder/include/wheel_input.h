// wheel_input.h -- racing wheel + pedals through DirectInput 8 (Windows).
//
// The game itself only knows net::CarInput (steer -1..1, throttle/brake 0..1,
// nitro). WheelInput turns a DirectInput driving device (Logitech G29/G920,
// Thrustmaster T150/T300, Fanatec, ...) into those values once per frame:
//   steer     wheel axis * steerGain (clamped), so the car reaches full lock
//             well before the wheel does (a 900 deg wheel at gain 2.5 = 180 deg)
//   throttle  pedal 0..1; replaces the auto-throttle once it has been pressed
//   brake     pedal 0..1 (sent on the wire; the arcade physics ignores it)
//   nitro     any of the nitro buttons (default: the shift paddles)
// plus menu keys: POV hat -> arrow keys, accept/back buttons -> Enter/Esc.
//
// Pedal axes are auto-calibrated: a released pedal rests at one end of its
// axis (+1 on Logitech, -1 on some others); the first reading near an end
// fixes that as "released" and the other end as "floored".
//
// Everything is configurable through settings.txt (wheel_* keys, see
// applyWheelSetting) and --no-wheel / --wheel-debug. On other platforms the
// class is a stub that never reports a device.
#pragma once

#include <cmath>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace client {

// DirectInput joystick axes, in DIJOYSTATE2 order.
enum class WheelAxis
{
    X,
    Y,
    Z,
    RX,
    RY,
    RZ,
    Slider0,
    Slider1,
    None,
};
constexpr int kWheelAxisCount = 8;

struct WheelConfig
{
    bool enabled = true;
    std::string deviceMatch;            // pick the device whose name contains this (any class); empty = first wheel
    WheelAxis steerAxis = WheelAxis::X;
    WheelAxis throttleAxis = WheelAxis::Y;
    WheelAxis brakeAxis = WheelAxis::RZ;
    bool combinedPedals = false;        // one axis: throttle towards -1, brake towards +1 (throttleAxis)
    bool invertSteer = false;
    float steerGain = 2.5f;             // game steer = axis * gain; 1 = full wheel lock for full steer
    float steerDeadzone = 0.01f;
    float pedalDeadzone = 0.05f;
    bool usePedals = true;              // throttle pedal drives the throttle (else auto-throttle)
    std::vector<int> nitroButtons{4, 5}; // 0-based DirectInput buttons (G29/G920: 4/5 = paddles)
    int acceptButton = 0;               // -> Enter (G29: Cross / G920: A); -1 = none
    int backButton = 2;                 // -> Esc   (G29: Circle / G920: B); -1 = none
    bool debug = false;                 // print raw axes / buttons once a second
};

// Apply one "wheel_*" settings.txt key. Returns false if the key or value is
// not understood (the line is then kept verbatim but has no effect).
bool applyWheelSetting(WheelConfig &cfg, const std::string &key, const std::string &value);
const char *wheelAxisName(WheelAxis a);

struct WheelState
{
    bool connected = false;
    std::string name;
    float steer = 0.0f;       // -1 (left) .. +1 (right), gain applied
    float throttle = 0.0f;    // 0..1
    float brake = 0.0f;       // 0..1
    bool throttleSeen = false; // throttle pedal pressed at least once since the device appeared
    bool nitro = false;
};

// --- mapping helpers (pure; shared by the DirectInput code and tests) -------

// One pedal axis. `restSign` is 0 until the pedal has been seen resting at an
// end of its axis; until then the pedal reads 0 (a never-touched or absent
// pedal must not floor the throttle).
struct PedalCalibration
{
    int restSign = 0;
};

inline float applyDeadzone(float v, float dz)
{
    float a = std::fabs(v);
    if (a <= dz) return 0.0f;
    float out = (a - dz) / (1.0f - dz);
    return v < 0 ? -out : out;
}

// raw in -1..1 -> pedal 0..1 (0 = released).
inline float pedalFromAxis(PedalCalibration &cal, float raw, float deadzone)
{
    if (cal.restSign == 0)
    {
        if (raw > 0.8f) cal.restSign = 1;
        else if (raw < -0.8f) cal.restSign = -1;
        else return 0.0f;
    }
    float p = (1.0f - static_cast<float>(cal.restSign) * raw) * 0.5f; // rest end -> 0, far end -> 1
    p = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
    return p <= deadzone ? 0.0f : (p - deadzone) / (1.0f - deadzone);
}

// raw in -1..1 -> game steer -1..1.
inline float steerFromAxis(float raw, const WheelConfig &cfg)
{
    float v = applyDeadzone(cfg.invertSteer ? -raw : raw, cfg.steerDeadzone) * cfg.steerGain;
    return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

class WheelInput
{
public:
    WheelInput();
    ~WheelInput();
    WheelInput(const WheelInput &) = delete;
    WheelInput &operator=(const WheelInput &) = delete;

    // Returns false when DirectInput is unavailable (or not Windows); safe to
    // keep calling update() either way.
    bool init(GLFWwindow *window, const WheelConfig &cfg);
    void shutdown();
    // Poll once per frame. While no device is attached it rescans every few
    // seconds, so a wheel plugged in mid-game is picked up.
    void update(double nowSeconds);
    const WheelState &state() const { return state_; }
    // GLFW key codes synthesized since the last call (hat / accept / back).
    std::vector<int> takeKeyPresses();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    WheelState state_;
};

} // namespace client
