// hud_gauges.h -- Initial D arcade style tachometer + nitro gauge for the HUD.
//
// Drawn with Renderer's 2D overlay API (call between beginFrame/endFrame).
// Themes are data driven: <assetDir>/hud/<theme>/theme.txt. `make assets`
// installs "initiald" (the Initial D arcade HUD sprites from
// github.com/xmatheusgc/Initial-D-HUD, layout recovered from its CSS/JS);
// without it (or for theme "drifto_arcade") a procedural look-alike is drawn,
// so the HUD always works.
//
//   gfx::HudGauges hud;
//   hud.init(renderer);                        // once, after Renderer::init
//   ...
//   gfx::TachState t;  t.rpm = ...; t.maxRpm = 8000; t.gear = 3; t.speedKmh = ...;
//   gfx::NitroGaugeState n;  n.level01 = ...; n.burning = ...;
//   t.night = n.night = skyIsDark; t.time = n.time = now;
//   hud.drawCluster(renderer, t, n);           // bottom-right, scales with fb height
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "renderer.h"

namespace gfx {

struct TachState
{
    float rpm = 0.0f;
    float maxRpm = 8000.0f;     // picks the dial scale (8k, 9k, 10k ... 20k)
    float redlineRpm = 7000.0f; // procedural theme only (sprite labels have their own)
    int gear = 0;               // -1 = reverse, 0 = neutral, 1..9
    float speedKmh = 0.0f;
    bool revLimiter = false;    // bouncing off the limiter: needle shake
    bool night = false;         // night sprite variants (orange labels, glowing needle)
    float time = 0.0f;          // seconds, for smoothing / flicker
};

struct NitroGaugeState
{
    float level01 = 0.0f;              // tank level -> needle sweep
    bool burning = false;              // glow + flicker
    float rechargeMultiplier = 1.0f;   // rubber-band recharge rate, shown as "x2.0" (hidden near 1.0)
    bool slipstream = false;           // "SLIP" lamp
    bool night = false;
    float time = 0.0f;
};

class HudGauges
{
public:
    HudGauges();
    ~HudGauges();
    HudGauges(const HudGauges &) = delete;
    HudGauges &operator=(const HudGauges &) = delete;

    // Loads <assetDir>/hud/<theme>/theme.txt. Returns false (and uses the
    // procedural "drifto_arcade" look) if the theme or its sprites are missing.
    bool init(Renderer &r, const std::string &theme = "initiald");
    const std::string &theme() const;
    bool usingSprites() const;
    // Every theme folder under <assetDir>/hud plus "drifto_arcade".
    static std::vector<std::string> availableThemes(const Renderer &r);
    // Force an rpm label sheet ("8k", "9k", ..., "86" = AE86 yellow/red zone);
    // "" = pick from TachState::maxRpm.
    void setRpmScale(const std::string &scale);

    // The whole arcade cluster (dashboard panel with speed + gear, rpm dial,
    // nitro dial above it) anchored to the bottom-right corner. scale 1 = the
    // original size at 1080p, and it scales with the framebuffer height.
    void drawCluster(Renderer &r, const TachState &tach, const NitroGaugeState &nitro, float scale = 1.0f,
                     float marginPx = 12.0f);
    // Pieces on their own: (cx, cy) = dial centre in pixels, size = dial
    // diameter in pixels. drawTachometer also draws the dashboard panel
    // (speed + gear) that belongs to it, to the left of the dial.
    void drawTachometer(Renderer &r, const TachState &s, float cx, float cy, float size);
    void drawNitroGauge(Renderer &r, const NitroGaugeState &s, float cx, float cy, float size);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gfx
