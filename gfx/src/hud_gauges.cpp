// hud_gauges.cpp -- see gfx/include/hud_gauges.h.
//
// Everything is laid out in the "reference" pixel space of the original
// Initial-D-HUD CSS (1080p, origin = top-left of the dashboard background) and
// mapped to the screen with one uniform scale + offset. The procedural
// fallback theme uses the same layout so both look and anchor the same.
//
// Responsibilities:
//   - Tachometer (dial, needle, gear, 3-digit speed, km/h) and nitro gauge.
//   - Themes: key = value files (theme.txt) give layout rectangles, needle
//     pivots, angle ranges and texture names. type=layered selects sprite
//     rendering; otherwise (or if sprites are missing) a procedural version is
//     drawn from lines/arcs/stroke text so the HUD always works.
//   - Needle angles are smoothed per frame (exponential approach), and the
//     sprite/stroke-font paths share the same layout code.
// Everything is drawn through Renderer's 2D overlay API (drawSprite, drawLine,
// drawArc, drawRect, drawText). Angles are degrees, 0 = straight up, clockwise
// positive. Private state lives in HudGauges::Impl (pimpl) in this file.
#include "../include/hud_gauges.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include <filesystem>
#include <system_error>

namespace gfx {

namespace {

// Minimal 2D vector and rectangle used for layout data (reference pixels).
struct V2
{
    float x = 0, y = 0;
};
struct Rect
{
    float x = 0, y = 0, w = 0, h = 0;
};

// ---------------------------------------------------------------------------
// Stroke font: polylines in a 0.6 x 1 box (y down). Used for numerals, the
// gear digit and labels in the procedural theme and for NITRO / x2.0 / SLIP.
// ---------------------------------------------------------------------------
using Poly = std::vector<V2>;
// Glyph table: char -> list of polylines in a 0.6 wide x 1 tall box, y down.
// Built once on first use (function-local static).
const std::map<char, std::vector<Poly>> &strokeFont()
{
    static const std::map<char, std::vector<Poly>> f = {
        {'0', {{{0.1f, 0}, {0.5f, 0}, {0.6f, 0.12f}, {0.6f, 0.88f}, {0.5f, 1}, {0.1f, 1}, {0, 0.88f}, {0, 0.12f}, {0.1f, 0}}}},
        {'1', {{{0.12f, 0.18f}, {0.36f, 0}, {0.36f, 1}}}},
        {'2', {{{0, 0.14f}, {0.1f, 0}, {0.5f, 0}, {0.6f, 0.12f}, {0.6f, 0.38f}, {0, 1}, {0.62f, 1}}}},
        {'3', {{{0, 0.08f}, {0.1f, 0}, {0.5f, 0}, {0.6f, 0.12f}, {0.6f, 0.37f}, {0.5f, 0.48f}, {0.2f, 0.48f}},
               {{0.5f, 0.48f}, {0.6f, 0.6f}, {0.6f, 0.88f}, {0.5f, 1}, {0.1f, 1}, {0, 0.92f}}}},
        {'4', {{{0.46f, 1}, {0.46f, 0}, {0, 0.68f}, {0.62f, 0.68f}}}},
        {'5', {{{0.6f, 0}, {0.06f, 0}, {0.02f, 0.45f}, {0.48f, 0.42f}, {0.6f, 0.55f}, {0.6f, 0.88f}, {0.5f, 1}, {0.1f, 1}, {0, 0.9f}}}},
        {'6', {{{0.55f, 0.02f}, {0.15f, 0}, {0, 0.2f}, {0, 0.88f}, {0.1f, 1}, {0.5f, 1}, {0.6f, 0.88f}, {0.6f, 0.6f}, {0.5f, 0.48f}, {0.1f, 0.48f}, {0, 0.58f}}}},
        {'7', {{{0, 0}, {0.62f, 0}, {0.2f, 1}}}},
        {'8', {{{0.1f, 0}, {0.5f, 0}, {0.6f, 0.1f}, {0.6f, 0.37f}, {0.5f, 0.47f}, {0.1f, 0.47f}, {0, 0.37f}, {0, 0.1f}, {0.1f, 0}},
               {{0.1f, 0.47f}, {0, 0.57f}, {0, 0.9f}, {0.1f, 1}, {0.5f, 1}, {0.6f, 0.9f}, {0.6f, 0.57f}, {0.5f, 0.47f}}}},
        {'9', {{{0.05f, 0.98f}, {0.45f, 1}, {0.6f, 0.8f}, {0.6f, 0.12f}, {0.5f, 0}, {0.1f, 0}, {0, 0.12f}, {0, 0.4f}, {0.1f, 0.52f}, {0.5f, 0.52f}, {0.6f, 0.42f}}}},
        {'N', {{{0, 1}, {0, 0}, {0.6f, 1}, {0.6f, 0}}}},
        {'R', {{{0, 1}, {0, 0}, {0.5f, 0}, {0.6f, 0.12f}, {0.6f, 0.38f}, {0.5f, 0.5f}, {0, 0.5f}}, {{0.3f, 0.5f}, {0.62f, 1}}}},
        {'I', {{{0.3f, 0}, {0.3f, 1}}}},
        {'T', {{{0, 0}, {0.6f, 0}}, {{0.3f, 0}, {0.3f, 1}}}},
        {'O', {{{0.1f, 0}, {0.5f, 0}, {0.6f, 0.12f}, {0.6f, 0.88f}, {0.5f, 1}, {0.1f, 1}, {0, 0.88f}, {0, 0.12f}, {0.1f, 0}}}},
        {'S', {{{0.6f, 0.06f}, {0.5f, 0}, {0.1f, 0}, {0, 0.12f}, {0, 0.36f}, {0.1f, 0.47f}, {0.5f, 0.53f}, {0.6f, 0.64f}, {0.6f, 0.88f}, {0.5f, 1}, {0.1f, 1}, {0, 0.94f}}}},
        {'L', {{{0, 0}, {0, 1}, {0.58f, 1}}}},
        {'P', {{{0, 1}, {0, 0}, {0.5f, 0}, {0.6f, 0.12f}, {0.6f, 0.4f}, {0.5f, 0.52f}, {0, 0.52f}}}},
        {'M', {{{0, 1}, {0, 0}, {0.3f, 0.55f}, {0.6f, 0}, {0.6f, 1}}}},
        {'x', {{{0.06f, 0.45f}, {0.5f, 1}}, {{0.5f, 0.45f}, {0.06f, 1}}}},
        {'.', {{{0.12f, 0.93f}, {0.16f, 0.93f}}}},
    };
    return f;
}

// Horizontal advance per glyph, in units of glyph height (narrower for '.', 'I', '1', 'x').
float glyphAdvance(char c) { return c == '.' ? 0.35f : (c == 'I' || c == '1') ? 0.62f : (c == 'x' ? 0.68f : 0.8f); }

// Pixel width of string s at glyph height h (sum of advances).
float strokeWidth(const std::string &s, float h)
{
    float w = 0;
    for (char c : s) w += glyphAdvance(c) * h;
    return w;
}

// Draw text with its top-left at (x, y), glyph height h. italic = skew.
void strokeText(Renderer &r, const std::string &s, float x, float y, float h, const dm::Vec3 &col, float thick,
                float alpha = 1.0f, float italic = 0.18f, bool shadow = true, bool additive = false)
{
    const auto &font = strokeFont();
    // pass 0 = dark drop shadow (thicker, offset); pass 1 = the coloured text.
    // shadow = false skips pass 0.
    for (int pass = shadow ? 0 : 1; pass < 2; ++pass)
    {
        float cx = x;
        float off = pass == 0 ? std::max(1.0f, thick * 0.45f) : 0.0f;
        float t = pass == 0 ? thick * 1.9f : thick;
        dm::Vec3 c = pass == 0 ? dm::Vec3{0.02f, 0.02f, 0.03f} : col;
        float a = pass == 0 ? alpha * 0.85f : alpha;
        for (char ch : s)
        {
            auto it = font.find(ch);
            if (it != font.end())
                for (const Poly &p : it->second)
                    for (size_t i = 0; i + 1 < p.size(); ++i)
                    {
                        // map a glyph-space point to pixels; the italic shear moves points toward the
                        // top to the right, keeping the baseline fixed
                        auto P = [&](const V2 &v, float &ox, float &oy) {
                            ox = cx + (v.x + (1.0f - v.y) * italic) * h + off;
                            oy = y + v.y * h + off;
                        };
                        float x0, y0, x1, y1;
                        P(p[i], x0, y0);
                        P(p[i + 1], x1, y1);
                        // extend by half the thickness so joints close
                        float dx = x1 - x0, dy = y1 - y0, l = std::sqrt(dx * dx + dy * dy);
                        if (l > 1e-4f)
                        {
                            float e = t * 0.45f / l;
                            x0 -= dx * e;
                            y0 -= dy * e;
                            x1 += dx * e;
                            y1 += dy * e;
                        }
                        // additive blending applies only to the foreground pass
                        r.drawLine(x0, y0, x1, y1, t, c, a, additive && pass == 1);
                    }
            cx += glyphAdvance(ch) * h;
        }
    }
}

// Strip spaces, tabs and CRs from both ends.
std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Replace every occurrence of from with to (used for {c}/{scale} placeholders).
std::string replaceAll(std::string s, const std::string &from, const std::string &to)
{
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    return s;
}

// Cheap pseudo-random flicker in [0,1]: two sines at incommensurate frequencies
// (47 and 83 rad/s); seed decorrelates different callers.
float flicker(float t, float seed) { return 0.5f + 0.25f * std::sin(t * 47.0f + seed) + 0.25f * std::sin(t * 83.0f + 2.0f * seed); }

} // namespace

// Private state: parsed theme config, layout (reference pixels) and the smoothed
// needle angles.
struct HudGauges::Impl
{
    std::string theme = "drifto_arcade";
    std::string dir;
    bool sprites = false;
    std::map<std::string, std::string> kv;
    std::string scaleOverride;

    // layout (reference px)
    float refHeight = 1080;
    // bounds: extents of the whole cluster (boost dial sits above y = 0); used to
    // anchor the cluster to the screen corner
    Rect bounds{0, -152, 454.4f, 402};
    Rect dash{0, 0, 450, 250};
    std::vector<std::pair<std::string, float>> scales;
    V2 labelsSize{240, 180}, labelsCentre{119.6f, 114.6f}, rpmCentre{313.7f, 136.2f};
    V2 needleSize{26, 169}, needlePivot{0.5f, 0.75f};
    // needle sweep ends in degrees (0 = up, clockwise positive)
    float rpmA0 = -120, rpmA1 = 120;
    Rect gearRect{80, 50, 50, 57};
    char gearReverse = 'R';
    Rect speedRect{5, 119, 52, 57};
    float speedAdvance = 56.4f;
    Rect unitRect{115, 177, 64, 18};
    Rect boostRect{260, -152, 194.4f, 194.4f};
    V2 boostCentre{356.7f, -52.8f};
    V2 boostNeedleSize{25, 95}, boostNeedlePivot{0.5f, 0.93f};
    V2 boostLabelsSize{194.4f, 181}, boostLabelsCentre{96.9f, 97.0f};
    float boostA0 = -133, boostA1 = 133;
    bool nitroLabels = false;

    // smoothed needles
    // current (smoothed) needle angles in degrees and the last timestamp used for smoothing
    float rpmAngle = -120, boostAngle = -133, lastRpmT = -1, lastBoostT = -1;

    // ------------------------------------------------------------------
    // Theme value for key k, or def when absent.
    std::string get(const std::string &k, const std::string &def = "") const
    {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    }
    // Typed getters: leave the output unchanged when the key is missing, so the
    // built-in defaults survive a theme.txt that overrides only some keys.
    void getF(const std::string &k, float &v) const
    {
        std::string s = get(k);
        if (!s.empty()) v = std::strtof(s.c_str(), nullptr);
    }
    void getV2(const std::string &k, V2 &v) const
    {
        std::string s = get(k);
        if (!s.empty()) std::sscanf(s.c_str(), "%f %f", &v.x, &v.y);
    }
    void getRect(const std::string &k, Rect &v) const
    {
        std::string s = get(k);
        if (!s.empty()) std::sscanf(s.c_str(), "%f %f %f %f", &v.x, &v.y, &v.w, &v.h);
    }

    // Load the texture for theme key. At night `<key>_night` is preferred, falling
    // back to `<key>`. Placeholders {c} (e.g. digit/gear char) and {scale} (rpm
    // scale label) are substituted. Returns 0 if the key is unset.
    TextureHandle tex(Renderer &r, const std::string &key, bool night, const std::string &c = "",
                      const std::string &scale = "")
    {
        std::string rel = night ? get(key + "_night") : "";
        if (rel.empty()) rel = get(key);
        if (rel.empty()) return 0;
        rel = replaceAll(replaceAll(rel, "{c}", c), "{scale}", scale);
        return r.loadTexture(dir + "/" + rel);
    }

    // Choose the dial face: explicit override if set; else the smallest scale whose
    // max covers maxRpm (1 rpm tolerance), ignoring "86" unless requested; else the
    // largest scale. nullptr if the theme defines none.
    const std::pair<std::string, float> *pickScale(float maxRpm) const
    {
        if (scales.empty()) return nullptr;
        if (!scaleOverride.empty())
            for (const auto &s : scales)
                if (s.first == scaleOverride) return &s;
        const std::pair<std::string, float> *best = nullptr;
        for (const auto &s : scales)
        {
            if (s.first == "86") continue; // only on request
            if (s.second + 1.0f >= maxRpm && (!best || s.second < best->second)) best = &s;
        }
        if (!best)
            for (const auto &s : scales)
                if (!best || s.second > best->second) best = &s;
        return best;
    }

    // Frame-rate independent exponential smoothing of a needle angle (deg): moves
    // the fraction 1 - exp(-14 dt) toward target. dt is clamped to 0.25 s; on the
    // first call (lastT < 0) dt = 1 so the needle effectively snaps. Returns cur.
    float smooth(float &cur, float target, float t, float &lastT)
    {
        float dt = lastT < 0 ? 1.0f : dm::clampf(t - lastT, 0.0f, 0.25f);
        lastT = t;
        cur += (target - cur) * (1.0f - std::exp(-dt * 14.0f));
        return cur;
    }

    // ------------------------------------------------------------------
    // mapping ref -> screen
    // Reference -> screen transform: uniform scale k plus offset (ox, oy).
    struct Xf
    {
        float ox, oy, k;
        float X(float x) const { return ox + x * k; }
        float Y(float y) const { return oy + y * k; }
    };

    // Draw a sprite given in reference px (x, y, w, h) through f; skipped for a null texture.
    void img(Renderer &r, const Xf &f, TextureHandle t, float x, float y, float w, float h, float alpha = 1.0f,
             dm::Vec3 tint = {1, 1, 1}, bool additive = false)
    {
        if (!t) return;
        SpriteParams p;
        p.alpha = alpha;
        p.tint = tint;
        p.additive = additive;
        r.drawSprite(t, f.X(x), f.Y(y), w * f.k, h * f.k, p);
    }

    // Draw a rotating needle sprite: its pivot (fraction of sprite size) is placed on
    // `centre` (reference px) and the sprite is rotated by angleDeg about it. Used
    // for the needle and, with additive/widthMul, for its glow halo.
    void needleSprite(Renderer &r, const Xf &f, TextureHandle t, V2 centre, V2 size, V2 pivot, float angleDeg,
                      float alpha = 1.0f, bool additive = false, dm::Vec3 tint = {1, 1, 1}, float widthMul = 1.0f)
    {
        if (!t) return;
        SpriteParams p;
        p.rotation = dm::radians(angleDeg);
        p.pivot = {pivot.x, pivot.y};
        p.alpha = alpha;
        p.additive = additive;
        p.tint = tint;
        float w = size.x * widthMul * f.k, h = size.y * f.k;
        r.drawSprite(t, f.X(centre.x) - pivot.x * w, f.Y(centre.y) - pivot.y * h, w, h, p);
    }

    // ------------------------------------------------------------------
    // Tachometer (dashboard panel + rpm dial + gear + speed)
    // Draw the tachometer: dial face, needle, gear, speed and unit. Uses sprites when
    // the theme has them, else the procedural dial. Needle angle comes from s.rpm.
    void tach(Renderer &r, const Xf &f, const TachState &s)
    {
        const auto *sc = pickScale(s.maxRpm);
        // full-scale rpm of the dial: themed scale, or maxRpm rounded up to whole thousands
        float scaleMax = sc ? sc->second : std::max(1000.0f, std::ceil(s.maxRpm / 1000.0f) * 1000.0f);
        // fraction of the sweep; may overshoot the end stop by 5%
        float frac = dm::clampf(s.rpm / scaleMax, 0.0f, 1.05f);
        // linear rpm -> angle map between the sweep ends (deg)
        float target = rpmA0 + frac * (rpmA1 - rpmA0);
        float ang = smooth(rpmAngle, target, s.time, lastRpmT);
        // limiter shake: two fast sines (95 and 151 rad/s), about +-2.4 deg
        if (s.revLimiter) ang += 1.6f * std::sin(s.time * 95.0f) + 0.8f * std::sin(s.time * 151.0f);

        if (sprites)
        {
            img(r, f, tex(r, "dash_tex", s.night), dash.x, dash.y, dash.w, dash.h);
            TextureHandle lab = tex(r, "rpm_labels_tex", s.night, "", sc ? sc->first : "8k");
            img(r, f, lab, rpmCentre.x - labelsCentre.x, rpmCentre.y - labelsCentre.y, labelsSize.x, labelsSize.y);
            TextureHandle nd = tex(r, "rpm_needle_tex", s.night);
            if (s.night) // the night needle is a glowing bar: add a soft halo
                needleSprite(r, f, nd, rpmCentre, needleSize, needlePivot, ang, 0.35f, true, {1, 0.6f, 0.3f}, 2.2f);
            needleSprite(r, f, nd, rpmCentre, needleSize, needlePivot, ang);
        }
        else
            proceduralTach(r, f, s, scaleMax, ang);

        // gear
        // gear glyph: reverse char for gear < 0, 'N' for neutral, else the digit (capped at 9)
        char g = s.gear < 0 ? gearReverse : (s.gear == 0 ? 'N' : (char)('0' + std::min(9, s.gear)));
        TextureHandle gt = sprites ? tex(r, "gear_tex", s.night, std::string(1, g)) : 0;
        if (gt)
            img(r, f, gt, gearRect.x, gearRect.y, gearRect.w, gearRect.h);
        else
            strokeText(r, std::string(1, g == '0' ? 'R' : g), f.X(gearRect.x + 8), f.Y(gearRect.y + 4),
                       gearRect.h * 0.85f * f.k, {1.0f, 0.45f, 0.05f}, 6.0f * f.k);

        // speed: 3 digits, leading zeros hidden (as script.js)
        int sp = std::max(0, std::min(999, (int)std::lround(s.speedKmh)));
        char buf[4];
        std::snprintf(buf, sizeof buf, "%03d", sp);
        for (int i = 0; i < 3; ++i)
        {
            // units digit always shown; tens if nonzero or hundreds shown; hundreds if nonzero
            bool vis = i == 2 || buf[i] != '0' || (i == 1 && buf[0] != '0');
            if (!vis) continue;
            float x = speedRect.x + i * speedAdvance;
            TextureHandle dt = sprites ? tex(r, "speed_tex", s.night, std::string(1, buf[i])) : 0;
            if (dt)
                img(r, f, dt, x, speedRect.y, speedRect.w, speedRect.h);
            else
                strokeText(r, std::string(1, buf[i]), f.X(x + 6), f.Y(speedRect.y + 3), speedRect.h * 0.88f * f.k,
                           {1, 1, 1}, 5.5f * f.k);
        }
        // unit label: sprite, or plain "km/h" with a 1 px drop shadow
        TextureHandle ut = sprites ? tex(r, "unit_tex", s.night) : 0;
        if (ut)
            img(r, f, ut, unitRect.x, unitRect.y, unitRect.w, unitRect.h);
        else
        {
            float sc2 = 1.6f * f.k;
            r.drawText(f.X(unitRect.x + 8) + 1, f.Y(unitRect.y) + 1, sc2, "km/h", {0, 0, 0});
            r.drawText(f.X(unitRect.x + 8), f.Y(unitRect.y), sc2, "km/h", {1, 1, 1});
        }
    }

    // Procedural dial used without sprites: panel slabs, disc and rim, redline arc,
    // ticks every 250 rpm (numerals every 1000), needle and hub. scaleMax in rpm,
    // ang is the smoothed needle angle in degrees.
    void proceduralTach(Renderer &r, const Xf &f, const TachState &s, float scaleMax, float ang)
    {
        // dashboard panel: translucent dark slab with the MT tag (as the art)
        float k = f.k;
        dm::Vec3 dark{0.05f, 0.07f, 0.06f};
        r.drawRect(f.X(0), f.Y(110), 200 * k, 90 * k, dark, 0.55f);
        r.drawRect(f.X(110), f.Y(30), 110 * k, 80 * k, dark, 0.45f);
        r.drawLine(f.X(45), f.Y(112), f.X(165), f.Y(112), 1.5f * k, {0.7f, 0.75f, 0.7f}, 0.6f);
        strokeText(r, "MT", f.X(145), f.Y(42), 16 * k, {1, 1, 1}, 3.2f * k);
        // dial
        float cx = f.X(rpmCentre.x), cy = f.Y(rpmCentre.y);
        float R = 124 * k;
        // disc and thin rim
        r.drawArc(cx, cy, 0, R, 0, 2 * dm::kPi, {0.06f, 0.08f, 0.07f}, 0.62f);
        r.drawArc(cx, cy, R - 2.0f * k, R, 0, 2 * dm::kPi, {0.75f, 0.8f, 0.78f}, 0.45f);
        dm::Vec3 ink = s.night ? dm::Vec3{1.0f, 0.62f, 0.12f} : dm::Vec3{1, 1, 1};
        float a0 = dm::radians(rpmA0), a1 = dm::radians(rpmA1);
        float ro = 114 * k;
        // redline position as a fraction of the sweep
        float red = dm::clampf(s.redlineRpm / scaleMax, 0.0f, 1.0f);
        // red arc from the redline to the end; coloured arc for the normal range
        r.drawArc(cx, cy, ro - 7 * k, ro, a0 + (a1 - a0) * red, a1, {0.9f, 0.08f, 0.05f}, 0.95f);
        r.drawArc(cx, cy, ro - 1.6f * k, ro, a0, a0 + (a1 - a0) * red, ink, 0.95f);
        // one tick per 250 rpm: every 4th is a 1000 rpm major tick (with numeral),
        // every 2nd is a medium tick
        int steps = (int)std::lround(scaleMax / 250.0f);
        for (int i = 0; i <= steps; ++i)
        {
            float t = (float)i / steps, a = a0 + (a1 - a0) * t;
            bool major = i % 4 == 0, half = i % 2 == 0;
            float len = (major ? 16 : half ? 10 : 6) * k;
            // angle 0 points up and grows clockwise: x = sin(a), y = -cos(a) in y-down screen space
            float sn = std::sin(a), cs = -std::cos(a);
            dm::Vec3 c = t > red + 1e-3f ? dm::Vec3{0.95f, 0.1f, 0.06f} : ink;
            // 1 px offset dark shadow under each tick
            r.drawLine(cx + sn * (ro - len) + 1, cy + cs * (ro - len) + 1, cx + sn * ro + 1, cy + cs * ro + 1,
                       (major ? 3.2f : 1.8f) * k + 1.0f, {0, 0, 0}, 0.6f);
            r.drawLine(cx + sn * (ro - len), cy + cs * (ro - len), cx + sn * ro, cy + cs * ro, (major ? 3.2f : 1.8f) * k, c);
            if (major)
            {
                // numeral = thousands (i / 4), centred on a point inside the ticks
                std::string num = std::to_string(i / 4);
                float h = 17 * k, w = strokeWidth(num, h);
                float rr = ro - 34 * k;
                strokeText(r, num, cx + sn * rr - w * 0.5f, cy + cs * rr - h * 0.5f, h, ink, 2.6f * k);
            }
        }
        float lw = r.textWidth("rpm", 1.6f * k);
        r.drawText(cx - lw * 0.5f, cy - 52 * k, 1.6f * k, "rpm", ink);
        lw = r.textWidth("x1000", 1.4f * k);
        r.drawText(cx - lw * 0.5f, cy - 36 * k, 1.4f * k, "x1000", ink);
        // needle: red bar with a dark outline, hub
        // needle spans from a tail (T behind the centre) to the tip (L) along the same direction
        float na = dm::radians(ang), sn = std::sin(na), cs = -std::cos(na);
        float L = 118 * k, T = 30 * k;
        if (s.night)
            r.drawLine(cx - sn * T, cy - cs * T, cx + sn * L, cy + cs * L, 14 * k, {1, 0.35f, 0.1f}, 0.25f, true);
        r.drawLine(cx - sn * T, cy - cs * T, cx + sn * L, cy + cs * L, 7 * k, {0.1f, 0.02f, 0.02f}, 0.8f);
        r.drawLine(cx - sn * T, cy - cs * T, cx + sn * L, cy + cs * L, 4.5f * k, {1.0f, 0.18f, 0.06f});
        r.drawArc(cx, cy, 0, 14 * k, 0, 2 * dm::kPi, {0.31f, 0.31f, 0.31f}, 1.0f);
    }

    // ------------------------------------------------------------------
    // Nitro gauge (the boost dial)
    // Draw the nitro (boost) gauge: face, glow while burning, scale and level cells,
    // title, recharge multiplier, SLIP lamp, needle, hot tip (back to front).
    // The needle follows s.level01 (smoothed); flicker only applies while burning.
    void nitro(Renderer &r, const Xf &f, const NitroGaugeState &s)
    {
        float k = f.k;
        float lvl = dm::clampf(s.level01, 0.0f, 1.0f);
        float target = boostA0 + lvl * (boostA1 - boostA0);
        float ang = smooth(boostAngle, target, s.time, lastBoostT);
        float cx = f.X(boostCentre.x), cy = f.Y(boostCentre.y);
        float fl = s.burning ? flicker(s.time, 1.3f) : 0.0f;
        dm::Vec3 ink = s.night ? dm::Vec3{1.0f, 0.62f, 0.12f} : dm::Vec3{1, 1, 1};
        dm::Vec3 cyan{0.25f, 0.8f, 1.0f};

        TextureHandle bg = sprites ? tex(r, "boost_bg_tex", s.night) : 0;
        if (bg)
            img(r, f, bg, boostRect.x, boostRect.y, boostRect.w, boostRect.h);
        else
        {
            r.drawArc(cx, cy, 0, 92 * k, 0, 2 * dm::kPi, {0.06f, 0.08f, 0.07f}, 0.62f);
            r.drawArc(cx, cy, 90 * k, 92 * k, 0, 2 * dm::kPi, {0.75f, 0.8f, 0.78f}, 0.45f);
        }
        if (s.burning) // hot glow behind the face
            r.drawSprite(r.loadTexture("builtin:glow"), cx - 110 * k, cy - 110 * k, 220 * k, 220 * k,
                         SpriteParams{{0, 0}, {1, 1}, cyan, 0.30f + 0.2f * fl, 0, {0.5f, 0.5f}, true});

        if (nitroLabels && sprites)
        {
            TextureHandle lt = tex(r, "boost_labels_tex", s.night);
            img(r, f, lt, boostCentre.x - boostLabelsCentre.x, boostCentre.y - boostLabelsCentre.y, boostLabelsSize.x,
                boostLabelsSize.y);
        }
        else
        {
            // scale in the style of the art: white (night: orange) arc + ticks
            // with dark drop shadow, the "full" end in nitro blue
            float a0 = dm::radians(boostA0), a1 = dm::radians(boostA1);
            float ro = 83.5f * k;
            // first 80% of the sweep is the normal range; the rest is drawn in nitro blue
            float full = 0.8f;
            r.drawArc(cx + 1, cy + 1, ro - 2.4f * k, ro + 0.5f, a0, a0 + (a1 - a0) * full, {0, 0, 0}, 0.5f);
            r.drawArc(cx, cy, ro - 2.0f * k, ro, a0, a0 + (a1 - a0) * full, ink, 0.95f);
            r.drawArc(cx, cy, ro - 7.0f * k, ro, a0 + (a1 - a0) * full, a1, cyan, 0.95f);
            // 16 intervals; every 4th tick is a major tick
            for (int i = 0; i <= 16; ++i)
            {
                float t = i / 16.0f, a = a0 + (a1 - a0) * t;
                bool major = i % 4 == 0;
                float len = (major ? 13 : 7) * k;
                float sn = std::sin(a), cs = -std::cos(a);
                dm::Vec3 c = t > full + 1e-3f ? cyan : ink;
                r.drawLine(cx + sn * (ro - len) + 1, cy + cs * (ro - len) + 1, cx + sn * ro + 1, cy + cs * ro + 1,
                           (major ? 3.0f : 1.8f) * k + 1, {0, 0, 0}, 0.55f);
                r.drawLine(cx + sn * (ro - len), cy + cs * (ro - len), cx + sn * ro, cy + cs * ro,
                           (major ? 3.0f : 1.8f) * k, c);
            }
            // level band inside the scale: lit cells up to the needle
            // 24 cells with a gap (each spans 0.78 of its pitch); `on` is the 0..1 fill of a
            // cell from the tank level
            int cells = 24;
            for (int i = 0; i < cells; ++i)
            {
                float t0 = (float)i / cells, t1 = (i + 0.78f) / cells;
                float on = dm::clampf((lvl - t0) * cells, 0.0f, 1.0f);
                float aa0 = a0 + (a1 - a0) * t0, aa1 = a0 + (a1 - a0) * t1;
                r.drawArc(cx, cy, ro - 20 * k, ro - 13 * k, aa0, aa1, {0.1f, 0.16f, 0.2f}, 0.55f);
                if (on > 0)
                {
                    float b = s.burning ? 0.75f + 0.25f * fl : 0.85f;
                    r.drawArc(cx, cy, ro - 20 * k, ro - 13 * k, aa0, aa1,
                              s.burning ? dm::Vec3{0.75f, 0.95f, 1.0f} : cyan, on * b, s.burning);
                }
            }
        }
        // NITRO title in the place of the original "TURBO"
        {
            float h = 13 * k, w = strokeWidth("NITRO", h);
            dm::Vec3 c = s.burning ? dm::lerp(ink, dm::Vec3{0.75f, 0.95f, 1.0f}, 0.6f + 0.4f * fl) : ink;
            strokeText(r, "NITRO", cx - w * 0.5f, cy - 40 * k, h, c, 2.4f * k);
        }
        // recharge multiplier "x2.0": same digit sprites as the speedometer,
        // shown when the rubber band is active
        float m = s.rechargeMultiplier;
        if (std::fabs(m - 1.0f) > 0.05f)
        {
            char mb[8];
            std::snprintf(mb, sizeof mb, "%.1f", dm::clampf(m, 0.0f, 9.9f));
            dm::Vec3 tint = m > 1.0f ? dm::Vec3{0.65f, 1.0f, 0.7f} : dm::Vec3{1.0f, 0.72f, 0.45f};
            // digit sprites keep the speedometer aspect (52:57); advance is 82% of its pitch
            float dh = 21 * k, dw = dh * (52.0f / 57.0f), adv = dh * (56.4f / 57.0f) * 0.82f;
            // total width = 'x' (0.6 dh) + two digits + '.' (0.3 dh), for centring
            float total = 0.6f * dh + adv * 2 + 0.3f * dh;
            float x = cx - total * 0.5f, y = cy + 18 * k;
            strokeText(r, "x", x, y + dh * 0.25f, dh * 0.75f, tint, 2.4f * k);
            x += 0.6f * dh;
            for (char c : std::string(mb))
            {
                if (c == '.')
                {
                    strokeText(r, ".", x - 0.05f * dh, y, dh, tint, 3.0f * k);
                    x += 0.3f * dh;
                    continue;
                }
                TextureHandle dt = sprites ? tex(r, "speed_tex", s.night, std::string(1, c)) : 0;
                if (dt)
                {
                    SpriteParams p;
                    p.tint = tint;
                    r.drawSprite(dt, x, y, dw, dh, p);
                }
                else
                    strokeText(r, std::string(1, c), x + 0.1f * dh, y, dh * 0.9f, tint, 2.4f * k);
                x += adv;
            }
        }
        // SLIP lamp: soft glow quad, then the text; brightness flickers
        if (s.slipstream)
        {
            float h = 9 * k, w = strokeWidth("SLIP", h);
            float a = 0.65f + 0.35f * flicker(s.time * 0.5f, 4.0f);
            r.drawSprite(r.loadTexture("builtin:glow"), cx - w, cy + 60 * k - h, w * 2, h * 3,
                         SpriteParams{{0, 0}, {1, 1}, {0.3f, 0.85f, 1.0f}, 0.35f * a, 0, {0.5f, 0.5f}, true});
            strokeText(r, "SLIP", cx - w * 0.5f, cy + 60 * k, h, {0.55f, 0.92f, 1.0f}, 1.8f * k, a);
        }
        // needle
        TextureHandle nd = sprites ? tex(r, "boost_needle_tex", s.night) : 0;
        if (nd)
        {
            // sprite needle: additive halo layer when burning or at night, then the needle itself
            if (s.burning || s.night)
                needleSprite(r, f, nd, boostCentre, boostNeedleSize, boostNeedlePivot, ang,
                             s.burning ? 0.45f + 0.3f * fl : 0.3f, true,
                             s.burning ? dm::Vec3{0.5f, 0.85f, 1.0f} : dm::Vec3{1, 0.6f, 0.3f}, 2.4f);
            needleSprite(r, f, nd, boostCentre, boostNeedleSize, boostNeedlePivot, ang);
        }
        else
        {
            float na = dm::radians(ang), sn = std::sin(na), cs = -std::cos(na);
            float L = 82 * k, T = 8 * k;
            r.drawLine(cx - sn * T, cy - cs * T, cx + sn * L, cy + cs * L, 5.5f * k, {0.1f, 0.02f, 0.02f}, 0.8f);
            r.drawLine(cx - sn * T, cy - cs * T, cx + sn * L, cy + cs * L, 3.5f * k, {1.0f, 0.45f, 0.15f});
            r.drawArc(cx, cy, 0, 11 * k, 0, 2 * dm::kPi, {0.31f, 0.31f, 0.31f}, 1.0f);
        }
        if (s.burning) // hot tip
        {
            float na = dm::radians(ang), sn = std::sin(na), cs = -std::cos(na);
            float L = 70 * k, gs = (26 + 10 * fl) * k;
            r.drawSprite(r.loadTexture("builtin:glow"), cx + sn * L - gs * 0.5f, cy + cs * L - gs * 0.5f, gs, gs,
                         SpriteParams{{0, 0}, {1, 1}, {0.7f, 0.95f, 1.0f}, 0.8f, 0, {0.5f, 0.5f}, true});
        }
    }
};

// Pimpl construction; Impl is defined above.
HudGauges::HudGauges() : impl_(new Impl) {}
HudGauges::~HudGauges() = default;

// Load <assetDir>/hud/<theme>/theme.txt (empty theme -> "initiald") over the
// procedural defaults. Falls back to the built-in "drifto_arcade" when the file
// is missing. Returns true if the requested theme is usable (sprites present, or
// the procedural theme was asked for). Call once after Renderer::init.
bool HudGauges::init(Renderer &r, const std::string &theme)
{
    Impl &I = *impl_;
    I.theme = theme.empty() ? "initiald" : theme;
    I.kv.clear();
    I.sprites = false;
    I.dir = r.assetDir() + "/hud/" + I.theme;
    // procedural defaults (also used if theme.txt only overrides some keys)
    I.scales = {{"8k", 8000.f}, {"9k", 9000.f}, {"10k", 10000.f}, {"13k", 13000.f}, {"16k", 16000.f}, {"18k", 18000.f}, {"20k", 20000.f}};
    std::ifstream f(I.dir + "/theme.txt");
    if (!f)
    {
        if (I.theme != "drifto_arcade")
            std::fprintf(stderr, "[gfx] HUD theme '%s' not found (%s/theme.txt) -- using built-in 'drifto_arcade'%s\n",
                         I.theme.c_str(), I.dir.c_str(), I.theme == "initiald" ? " (run `make assets`)" : "");
        I.theme = "drifto_arcade";
        return theme == "drifto_arcade";
    }
    // parse `key = value` lines; '#' starts a comment line
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        I.kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    I.getF("ref_height", I.refHeight);
    I.getRect("bounds", I.bounds);
    I.getRect("dash_rect", I.dash);
    // rpm_scales: whitespace separated `label:maxRpm` tokens, e.g. 8k:8000
    std::string sc = I.get("rpm_scales");
    if (!sc.empty())
    {
        I.scales.clear();
        std::istringstream ss(sc);
        std::string tok;
        while (ss >> tok)
        {
            size_t c = tok.find(':');
            if (c != std::string::npos) I.scales.push_back({tok.substr(0, c), std::strtof(tok.c_str() + c + 1, nullptr)});
        }
    }
    I.getV2("rpm_labels_size", I.labelsSize);
    I.getV2("rpm_labels_centre", I.labelsCentre);
    I.getV2("rpm_centre", I.rpmCentre);
    I.getV2("rpm_needle_size", I.needleSize);
    I.getV2("rpm_needle_pivot", I.needlePivot);
    // angle keys hold `start end` in degrees
    V2 ra{I.rpmA0, I.rpmA1};
    I.getV2("rpm_angle", ra);
    I.rpmA0 = ra.x;
    I.rpmA1 = ra.y;
    I.getRect("gear_rect", I.gearRect);
    std::string gr = I.get("gear_reverse");
    if (!gr.empty()) I.gearReverse = gr[0];
    I.getRect("speed_rect", I.speedRect);
    I.getF("speed_advance", I.speedAdvance);
    I.getRect("unit_rect", I.unitRect);
    I.getRect("boost_rect", I.boostRect);
    I.getV2("boost_centre", I.boostCentre);
    I.getV2("boost_needle_size", I.boostNeedleSize);
    I.getV2("boost_needle_pivot", I.boostNeedlePivot);
    I.getV2("boost_labels_size", I.boostLabelsSize);
    I.getV2("boost_labels_centre", I.boostLabelsCentre);
    V2 ba{I.boostA0, I.boostA1};
    I.getV2("boost_angle", ba);
    I.boostA0 = ba.x;
    I.boostA1 = ba.y;
    I.nitroLabels = I.get("nitro_labels", "0") == "1";
    I.rpmAngle = I.rpmA0;
    I.boostAngle = I.boostA0;
    // type = layered selects sprites; verify the dash texture actually loads
    I.sprites = I.get("type") == "layered";
    if (I.sprites && !r.loadTexture(I.dir + "/" + I.get("dash_tex")))
    {
        std::fprintf(stderr, "[gfx] HUD theme '%s': sprites missing -- procedural look\n", I.theme.c_str());
        I.sprites = false;
    }
    return I.sprites || I.theme == "drifto_arcade";
}

// Accessors. setRpmScale forces a dial scale label (empty = automatic choice).
const std::string &HudGauges::theme() const { return impl_->theme; }
bool HudGauges::usingSprites() const { return impl_->sprites; }
void HudGauges::setRpmScale(const std::string &scale) { impl_->scaleOverride = scale; }

// Procedural theme first, then other theme directories under <assetDir>/hud that
// contain a theme.txt, sorted.
std::vector<std::string> HudGauges::availableThemes(const Renderer &r)
{
    std::vector<std::string> out{"drifto_arcade"};
    std::string d = r.assetDir() + "/hud";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
    {
        std::string n = it->path().filename().string();
        std::error_code ec2;
        if (!n.empty() && n[0] != '.' && n != "drifto_arcade" &&
            std::filesystem::exists(it->path() / "theme.txt", ec2))
            out.push_back(n);
    }
    std::sort(out.begin() + 1, out.end());
    return out;
}

// Draw both gauges bottom-right. k = scale * framebufferHeight / refHeight so
// the size tracks resolution; the offset puts the bottom-right corner of the
// cluster bounds marginPx from the framebuffer corner.
void HudGauges::drawCluster(Renderer &r, const TachState &tach, const NitroGaugeState &nitro, float scale,
                            float marginPx)
{
    Impl &I = *impl_;
    float fbW = (float)r.framebufferWidth(), fbH = (float)r.framebufferHeight();
    if (fbW <= 0 || fbH <= 0) return;
    float k = scale * fbH / I.refHeight;
    Impl::Xf f;
    f.k = k;
    f.ox = fbW - marginPx - (I.bounds.x + I.bounds.w) * k;
    f.oy = fbH - marginPx - (I.bounds.y + I.bounds.h) * k;
    I.nitro(r, f, nitro); // behind the dashboard, as in the original DOM order
    I.tach(r, f, tach);
}

// Stand-alone tachometer centred at (cx, cy) px; size = on-screen width of the
// dial labels in px.
void HudGauges::drawTachometer(Renderer &r, const TachState &s, float cx, float cy, float size)
{
    Impl &I = *impl_;
    Impl::Xf f;
    f.k = size / I.labelsSize.x;
    f.ox = cx - I.rpmCentre.x * f.k;
    f.oy = cy - I.rpmCentre.y * f.k;
    I.tach(r, f, s);
}

// Stand-alone nitro gauge centred at (cx, cy) px; size = on-screen width of the
// boost face in px.
void HudGauges::drawNitroGauge(Renderer &r, const NitroGaugeState &s, float cx, float cy, float size)
{
    Impl &I = *impl_;
    Impl::Xf f;
    f.k = size / I.boostRect.w;
    f.ox = cx - I.boostCentre.x * f.k;
    f.oy = cy - I.boostCentre.y * f.k;
    I.nitro(r, f, s);
}

} // namespace gfx
