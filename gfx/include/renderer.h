// renderer.h -- the seam between game/ (what to draw) and gfx/ (how to draw).
//
// Owned by gfx/. game/ and client_folder/ only ever include this header;
// nothing outside gfx/ calls gl* directly or includes a GL header.
// Additive changes (new methods/fields) are fine; changing or removing
// anything here needs the other modules updated in the same change.
//
// Threading: every method must be called from the thread that owns the GL
// context (the GLFW main thread).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../common/dmath.h"

struct GLFWwindow;

namespace gfx {

// CPU-side triangle mesh. Used for procedurally generated geometry (road
// chunks, terrain, skid marks) built by game/. positions are required; any
// other attribute may be empty, in which case a default is used
// (normals: flat/computed, colors: white, uvs: 0).
struct MeshData
{
    std::vector<dm::Vec3> positions;
    std::vector<dm::Vec3> normals;
    std::vector<dm::Vec3> colors; // per-vertex albedo, linear 0..1
    std::vector<dm::Vec2> uvs;
    std::vector<uint32_t> indices; // triangles, CCW front faces
};

using MeshHandle = uint32_t;    // 0 = invalid
using ModelHandle = uint32_t;   // 0 = invalid
using TextureHandle = uint32_t; // 0 = invalid / solid colour (2D sprites)

// 2D sprite options (Renderer::drawSprite). UVs use a top-left origin
// (v = 0 is the top row of the image), so pixel rects from an atlas map
// directly: uv = pixel / textureSize.
struct SpriteParams
{
    dm::Vec2 uv0{0, 0}, uv1{1, 1}; // sub-rect of the texture (digit sheets, atlases)
    dm::Vec3 tint{1, 1, 1};
    float alpha = 1.0f;
    float rotation = 0.0f;          // radians, clockwise on screen
    dm::Vec2 pivot{0.5f, 0.5f};     // rotation pivot as a fraction of the quad (0,0 = top-left)
    bool additive = false;          // glows / lamps: colour * alpha is added
};

// Axis-aligned bounds of a loaded model in its local space (after the
// Unity->GL conversion), so game/ can place wheels / scale cars to size.
struct Bounds
{
    dm::Vec3 min, max;
};

struct Camera
{
    dm::Vec3 position;
    dm::Vec3 target;
    dm::Vec3 up{0, 1, 0};
    float fovY = dm::radians(60.0f);
    float zNear = 0.1f;
    float zFar = 1500.0f;
};

struct Lighting
{
    dm::Vec3 sunDirection = dm::normalize(dm::Vec3{-0.4f, -1.0f, -0.3f}); // direction light travels
    dm::Vec3 sunColor{1.0f, 0.97f, 0.9f};
    dm::Vec3 ambient{0.35f, 0.38f, 0.45f};
    dm::Vec3 fogColor{0.75f, 0.82f, 0.9f};
    float fogStart = 150.0f;
    float fogEnd = 600.0f;
};

// Per-draw material knobs for the toon shader.
struct DrawParams
{
    dm::Vec3 tint{1, 1, 1}; // multiplied with albedo
    float alpha = 1.0f;     // < 1 => drawn in the transparent pass, no outline
    bool outline = true;    // inverted-hull ink outline
    bool castShadow = true; // only meaningful if shadows are implemented
    // false for camera-attached objects (the local player's car + wheels):
    // camera-motion blur reprojects the world as static, which would smear
    // anything that moves *with* the camera.
    bool motionBlur = true;
};

// Per-frame post-processing / screen effects input (see Renderer::setFrameFx).
// All amounts are 0..1; the multipliers scale the automatic strengths.
struct FrameFx
{
    float speed01 = 0.0f;      // |velocity| / top speed: speed lines, slight radial blur
    float nitro01 = 0.0f;      // nitro burn (smoothed 0..1): strong radial blur, lines, tint
    float slipstream01 = 0.0f; // drafting: subtle air ribbons at the screen edges
    float motionBlur = 1.0f;   // camera-motion blur strength (reprojection; 0 = off)
    float radialBlur = 1.0f;   // multiplier for the speed/nitro radial blur
    float speedLines = 1.0f;   // multiplier for the speed lines / ribbons
    float time = 0.0f;         // seconds, animates the lines
};

// Effect quality (Renderer::setFxQuality). init() picks defaults from the GL
// renderer: full quality on a GPU, cheaper on llvmpipe/software. A game
// "--low" switch should set postFx = false.
struct FxQuality
{
    bool postFx = true;       // motion blur + speed lines + nitro tint pass
    bool motionBlur = true;   // the blur part of postFx (lines still drawn)
    bool halfResBlur = false; // blur at half resolution, composited full-res
    int blurSamples = 12;     // taps along the motion vector (4..16)
    bool flames = true;       // nitro flames / glows
};

// Recommended vertical FOV (radians) for the chase camera: 70 deg base,
// widening up to +14 deg with speed and +8 deg more with nitro (92 deg max).
// Feed it the *smoothed* nitro value and low-pass the result, e.g.
//   fov += (gfx::speedFovY(speed01, nitro01) - fov) * (1 - exp(-6 * dt));
inline float speedFovY(float speed01, float nitro01, float baseDeg = 70.0f, float speedDeg = 14.0f,
                       float nitroDeg = 8.0f)
{
    float s = dm::clampf(speed01, 0.0f, 1.0f);
    float n = dm::clampf(nitro01, 0.0f, 1.0f);
    s = s * s * (3.0f - 2.0f * s); // ease in/out: little change at low speed
    return dm::radians(baseDeg + speedDeg * s + nitroDeg * n);
}

class Renderer
{
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // assetDir is the root produced by `make assets` (default "assets").
    // The window must own an OpenGL 3.3+ core context (GLFW hints
    // CONTEXT_VERSION 3.3, OPENGL_CORE_PROFILE, OPENGL_FORWARD_COMPAT); init
    // makes it current and loads the GL functions. Shaders are read from
    // gfx/shaders (searched relative to the cwd, then the executable; override
    // with the DRIFTO_SHADER_DIR environment variable).
    bool init(GLFWwindow *window, const std::string &assetDir);
    void shutdown();

    // --- resources -------------------------------------------------------
    MeshHandle createMesh(const MeshData &mesh);
    void updateMesh(MeshHandle handle, const MeshData &mesh); // re-upload (e.g. skid marks)
    void destroyMesh(MeshHandle handle);

    // name = model file stem under <assetDir>/models/, e.g. "ae86", "rx7",
    // "tree_type0_01". Cached: loading the same name twice returns the same handle.
    ModelHandle loadModel(const std::string &name);
    Bounds modelBounds(ModelHandle handle) const;
    // Names of every model available under <assetDir>/models/.
    std::vector<std::string> availableModels() const;
    // Extra per-model data from <assetDir>/models/<name>.txt (written by the
    // asset converter). Cars ("ae86") provide points "wheel_fl", "wheel_fr",
    // "wheel_rl", "wheel_rr" (wheel centres in car-local space) and values
    // "wheel_radius", "wheel_width", "length". "ae86_body" + "ae86_wheel" are
    // the same car split for spinning/steering wheels: ae86_wheel is the
    // front-left wheel centred on its axle (axle along X, hub cap facing -X);
    // draw it yawed by pi for the right-hand wheels. Lookups by the full
    // car, body or wheel handle all read the car's .txt.
    bool modelPoint(ModelHandle handle, const std::string &key, dm::Vec3 &out) const;
    float modelValue(ModelHandle handle, const std::string &key, float fallback = 0.0f) const;

    // name = skybox folder under <assetDir>/skybox/, e.g. "SkySunset".
    bool setSkybox(const std::string &name);
    std::vector<std::string> availableSkyboxes() const;
    // Average colour of the current skybox around the horizon -- use it as
    // Lighting::fogColor so fogged geometry blends into the sky. Returns
    // Lighting{}.fogColor when no skybox is set.
    dm::Vec3 skyboxHorizonColor() const;

    // Tuning knobs (all optional). Outline width is in pixels at 720p and
    // scales with the framebuffer height.
    void setShadowsEnabled(bool enabled);
    void setOutlineWidth(float pixels);

    // --- per frame -------------------------------------------------------
    // Call in this order every frame: beginFrame, any number of draw*/text
    // calls, endFrame. Draw calls are queued and flushed in endFrame
    // (opaque -> outlines -> skybox -> transparent -> 2D overlay).
    void beginFrame(const Camera &camera, const Lighting &lighting);
    void drawMesh(MeshHandle handle, const dm::Mat4 &model, const DrawParams &params = {});
    void drawModel(ModelHandle handle, const dm::Mat4 &model, const DrawParams &params = {});
    // Camera-facing quad (tyre smoke, dust). size in metres.
    void drawBillboard(const dm::Vec3 &position, float size, const dm::Vec3 &color, float alpha);
    // 2D overlay in pixels, origin top-left. scale 1 ~= 8px glyphs.
    void drawText(float x, float y, float scale, const std::string &text, const dm::Vec3 &color = {1, 1, 1});
    void drawRect(float x, float y, float w, float h, const dm::Vec3 &color, float alpha = 1.0f);

    // --- 2D sprites / vector shapes (overlay pass, drawn in submission
    // order together with drawText/drawRect; pixels, origin top-left) ------
    // Load an image for the HUD (png/jpg/tga). Relative paths are tried as
    // given (cwd) and then under assetDir. Cached by path; 0 on failure.
    // Built-ins: "builtin:white", "builtin:glow" (soft radial falloff),
    // "builtin:disc" (anti-aliased filled circle).
    TextureHandle loadTexture(const std::string &path);
    bool textureSize(TextureHandle tex, int &w, int &h) const;
    // Textured quad; (x, y) = top-left of the unrotated quad, rotated by
    // params.rotation around params.pivot. tex = 0 draws a solid quad.
    void drawSprite(TextureHandle tex, float x, float y, float w, float h, const SpriteParams &params = {});
    // Anti-aliased thick line segment.
    void drawLine(float x0, float y0, float x1, float y1, float thickness, const dm::Vec3 &color,
                  float alpha = 1.0f, bool additive = false);
    // Anti-aliased ring segment around (cx, cy). Angles in radians, 0 = 12
    // o'clock, increasing clockwise. rInner = 0 gives a pie / disc.
    void drawArc(float cx, float cy, float rInner, float rOuter, float angle0, float angle1,
                 const dm::Vec3 &color, float alpha = 1.0f, bool additive = false);
    const std::string &assetDir() const;
    // Width in pixels of drawText(..., scale, text) (for centring/right-aligning).
    float textWidth(const std::string &text, float scale) const;
    // Nitro exhaust flame at one tail pipe. exhaustLocalPos / exhaustDirLocal
    // are in the car's local space (flames point +Z = backwards by default).
    // intensity 0..1 (0 draws nothing unless ignition > 0); ignition: 1 at the
    // moment nitro kicks in, decaying to 0 over ~0.3 s (burst + shock ring);
    // time in seconds drives the flicker.
    void drawNitroFlame(const dm::Mat4 &carModel, const dm::Vec3 &exhaustLocalPos, float intensity, float time,
                        float ignition = 0.0f, const dm::Vec3 &exhaustDirLocal = {0, 0, 1});
    // Same for every tail pipe of a converted car ("exhaust_<i>" points of
    // ae86 / ae86_body / rx7; a centred default if the model has none).
    void drawCarNitro(ModelHandle car, const dm::Mat4 &carModel, float intensity, float time,
                      float ignition = 0.0f);
    // Screen effects for this frame. Persistent: stays until changed. Until
    // the game calls this, no post-processing runs at all.
    void setFrameFx(const FrameFx &fx);
    void setFxQuality(const FxQuality &quality);
    FxQuality fxQuality() const;

    void endFrame(); // does NOT swap buffers; caller calls glfwSwapBuffers

    int framebufferWidth() const;
    int framebufferHeight() const;

    // Debug/CI helper: read back the last rendered frame to a PNG.
    bool saveScreenshot(const std::string &path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gfx
