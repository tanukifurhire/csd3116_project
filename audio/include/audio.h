// audio.h -- Drifto sound: procedural engines (AE86 4A-GE / RX7 13B rotary),
// tyre squeal, nitro, impacts, 3D positioned around the camera.
//
// Portable C++17 (Linux + Windows/MSVC). Backend: miniaudio (third_party/,
// public domain / MIT-0); this header pulls in no platform or miniaudio
// headers. Every call is cheap and thread-safe w.r.t. the audio thread; call
// them from the game thread. If there is no audio device (headless CI, no
// PulseAudio, ...) init() returns false and everything else is a silent no-op.
//
//   audio::Engine snd;
//   snd.init("assets");                         // once at startup
//   // every frame:
//   snd.setListener(cam.position, camForward, cam.up, cameraVelocity);
//   audio::CarAudioState c;
//   c.model = audio::CarModel::AE86; c.isLocal = true;
//   c.position = carPos; c.velocity = carVel; c.rpm = rpm; c.throttle = throttle;
//   c.shiftEvent = gearChangedThisFrame; c.skid01 = slip; c.nitro01 = nitroBurn;
//   c.limiter = atLimiter;
//   snd.updateCar(playerId, c);                 // same for every other car
//   if (hitCar)     snd.playBump(contactPoint, impulse);
//   if (hitBarrier) snd.playCrash(contactPoint, impulse);
//   if (nitroStarted) snd.playNitroIgnite(exhaustPos);
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "../../common/dmath.h"

namespace audio {

enum class CarModel
{
    AE86, // Toyota 4A-GE 1.6 inline-4, NA, ITB intake rasp, 7600 rpm redline
    RX7   // Mazda 13B two-rotor rotary: smooth buzzy "brap", lumpy idle, 9000 rpm
};

struct CarAudioState
{
    CarModel model = CarModel::AE86;
    dm::Vec3 position{0, 0, 0}; // world, metres
    dm::Vec3 velocity{0, 0, 0}; // m/s (doppler)
    float rpm = 900.0f;
    float throttle = 0.0f;      // 0..1 pedal / engine load (0 = overrun: softer, pops)
    bool shiftEvent = false;    // true on the frame a gear change happens (brief cut + pop)
    float skid01 = 0.0f;        // tyre slip 0..1 -> squeal
    float nitro01 = 0.0f;       // nitro burn 0..1 -> hiss / roar
    bool limiter = false;       // on the rev limiter -> fuel-cut bounce
    bool isLocal = false;       // the player's car: close-miked, louder, little panning
    float gain = 1.0f;          // 0..1 voice level (e.g. the race-over fade); ramped click-free
};

struct Config
{
    bool noDevice = false;      // offline rendering only (tools, tests): use render()
    uint32_t sampleRate = 48000;
    uint32_t periodMs = 10;     // device buffer period (latency)
};

class Engine
{
public:
    Engine();
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    // assetDir/audio/* (from `make assets`) supplies recorded layers (tyre
    // squeal loop, impact hits); everything has a procedural fallback, so a
    // missing folder only changes the timbre. Returns false if no device
    // could be opened (the engine then stays silent; all calls stay safe).
    bool init(const std::string &assetDir = "assets", const Config &config = {});
    void shutdown();
    bool deviceActive() const;

    void setListener(const dm::Vec3 &position, const dm::Vec3 &forward, const dm::Vec3 &up,
                     const dm::Vec3 &velocity = {0, 0, 0});

    // Up to 4 cars are voiced (extra ones are ignored; the local car always
    // gets a voice). A car that isn't updated for 1 s fades out by itself.
    void updateCar(int id, const CarAudioState &state);
    void removeCar(int id);

    // impulse = |delta momentum| of the contact in kg*m/s (~ mass * relative
    // speed): ~500 tap, ~3000 solid knock, 10000+ big hit.
    void playBump(const dm::Vec3 &position, float impulse);
    void playCrash(const dm::Vec3 &position, float impulse);
    void playNitroIgnite(const dm::Vec3 &position);

    void setMasterVolume(float v); // 0..1 (default 0.8)
    void setEngineVolume(float v); // engines (default 1)
    void setSfxVolume(float v);    // squeal, nitro, impacts (default 1)
    void setMuted(bool muted);     // e.g. on window focus loss (smooth fade)

    // Offline mixing (Config::noDevice): renders `frames` stereo frames
    // (interleaved float, -1..1) advancing the mixer clock. Also usable for
    // tests with a device-less engine.
    void render(float *interleavedStereo, uint32_t frames);
    uint32_t sampleRate() const;
    // Mixer time spent per second of audio on the audio thread (0..1), for
    // profiling.
    float cpuLoad() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace audio
