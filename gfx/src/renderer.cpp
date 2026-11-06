// renderer.cpp -- toon renderer behind gfx/include/renderer.h.
//
// Frame flow (endFrame):
//   shadow map (sun, follows the camera)
//   -> scene FBO (MSAA): opaque toon -> inverted-hull outlines -> skybox
//      -> transparent (sorted) + billboards -> 2D overlay
//   -> resolve FBO (kept for screenshots) -> default framebuffer.
//
// Responsibilities
//   - GL resource ownership: meshes, models (.dmesh), textures, skybox, FBOs.
//   - Immediate-style draw API (drawMesh/drawModel/drawText/...) that only
//     records work; all GL submission happens in endFrame().
//   - Frame rendering: culling, sorting, passes, post effects, HUD, present.
//
// Pass order in endFrame() as implemented
//   1. shadow:      depth-only sun view into a 2D depth texture (PCF compare)
//   2. scene FBO:   opaque toon (sorted by mesh, frustum culled)
//                   -> outlines (inverted hull, front faces culled)
//                   -> skybox (depth LEQUAL, no depth write)
//                   -> transparent items (far to near, depth prepass per item)
//                   -> billboards (smoke) -> nitro flame shells + glow sprites
//   3. resolve:     blit MSAA scene to a texture FBO (depth only if blur needs it)
//   4. post:        optional half-res motion/radial blur, then composite
//                   (speed lines, nitro, slipstream) into the final FBO
//   5. overlay:     2D HUD batches drawn on the resolved or final image
//   6. present:     blit to the default framebuffer (kept for screenshots)
//
// Design notes
//   - Per-frame lists (opaque, transparent, instances, billboards, overlay
//     vertices) are cleared, not freed, so their capacity is reused.
//   - Opaque items are sorted by mesh to cut VAO switches; shadow and outline
//     passes draw a whole model per instance (index-buffer prefix) instead of
//     per submesh, because the asset converter orders submeshes accordingly.
//   - Overlay geometry is batched into one streamed VBO; consecutive quads with
//     the same texture/blend mode merge into a single draw call.
//   - The shadow box is texel-snapped to avoid shimmer; bounding spheres are
//     precomputed per draw item for view and light frustum culling.
//   - Fullscreen passes use an attribute-less VAO (triangle built in the shader).
//   - Software rasterisers get reduced MSAA/shadow/blur settings by default.
//
// Responsibilities
//   - GL resource ownership: meshes, models (.dmesh), textures, skybox, FBOs.
//   - Immediate-style draw API (drawMesh/drawModel/drawText/...) that only
//     records work; all GL submission happens in endFrame().
//   - Frame rendering: culling, sorting, passes, post effects, HUD, present.
//
// Pass order in endFrame() as implemented
//   1. shadow:      depth-only sun view into a 2D depth texture (PCF compare)
//   2. scene FBO:   opaque toon (sorted by mesh, frustum culled)
//                   -> outlines (inverted hull, front faces culled)
//                   -> skybox (depth LEQUAL, no depth write)
//                   -> transparent items (far to near, depth prepass per item)
//                   -> billboards (smoke) -> nitro flame shells + glow sprites
//   3. resolve:     blit MSAA scene to a texture FBO (depth only if blur needs it)
//   4. post:        optional half-res motion/radial blur, then composite
//                   (speed lines, nitro, slipstream) into the final FBO
//   5. overlay:     2D HUD batches drawn on the resolved or final image
//   6. present:     blit to the default framebuffer (kept for screenshots)
//
// Design notes
//   - Per-frame lists (opaque, transparent, instances, billboards, overlay
//     vertices) are cleared, not freed, so their capacity is reused.
//   - Opaque items are sorted by mesh to cut VAO switches; shadow and outline
//     passes draw a whole model per instance (index-buffer prefix) instead of
//     per submesh, because the asset converter orders submeshes accordingly.
//   - Overlay geometry is batched into one streamed VBO; consecutive quads with
//     the same texture/blend mode merge into a single draw call.
//   - The shadow box is texel-snapped to avoid shimmer; bounding spheres are
//     precomputed per draw item for view and light frustum culling.
//   - Fullscreen passes use an attribute-less VAO (triangle built in the shader).
//   - Software rasterisers get reduced MSAA/shadow/blur settings by default.
//
// Responsibilities
//   - GL resource ownership: meshes, models (.dmesh), textures, skybox, FBOs.
// Material flag bits stored per submesh in the .dmesh file.
//   - Immediate-style draw API (drawMesh/drawModel/drawText/...) that only
//     records work; all GL submission happens in endFrame().
// GPU-side mesh: VAO with static VBO/IBO plus object-space AABB. alive == false marks a free slot.
//   - Frame rendering: culling, sorting, passes, post effects, HUD, present.
//
// Pass order in endFrame() as implemented
//   1. shadow:      depth-only sun view into a 2D depth texture (PCF compare)
//   2. scene FBO:   opaque toon (sorted by mesh, frustum culled)
//                   -> outlines (inverted hull, front faces culled)
//                   -> skybox (depth LEQUAL, no depth write)
//                   -> transparent items (far to near, depth prepass per item)
// Index range [first, first+count) of a mesh that shares one material.
//                   -> billboards (smoke) -> nitro flame shells + glow sprites
//   3. resolve:     blit MSAA scene to a texture FBO (depth only if blur needs it)
//   4. post:        optional half-res motion/radial blur, then composite
//                   (speed lines, nitro, slipstream) into the final FBO
//   5. overlay:     2D HUD batches drawn on the resolved or final image
//   6. present:     blit to the default framebuffer (kept for screenshots)
//
// Design notes
//   - Per-frame lists (opaque, transparent, instances, billboards, overlay
// A loaded .dmesh: one GPU mesh, its material table and optional sidecar key/values.
//     vertices) are cleared, not freed, so their capacity is reused.
//   - Opaque items are sorted by mesh to cut VAO switches; shadow and outline
//     passes draw a whole model per instance (index-buffer prefix) instead of
//     per submesh, because the asset converter orders submeshes accordingly.
//   - Overlay geometry is batched into one streamed VBO; consecutive quads with
//     the same texture/blend mode merge into a single draw call.
//   - The shadow box is texel-snapped to avoid shimmer; bounding spheres are
//     precomputed per draw item for view and light frustum culling.
//   - Fullscreen passes use an attribute-less VAO (triangle built in the shader).
//   - Software rasterisers get reduced MSAA/shadow/blur settings by default.
#include "../../common/platform.h"
#include "../include/renderer.h"
// Colour-pass record (one submesh or whole mesh) with a precomputed world bounding sphere.

// Material flag bits stored per submesh in the .dmesh file.
#include "gl_loader.h"
#include "gfx_util.h"
// GPU-side mesh: VAO with static VBO/IBO plus object-space AABB. alive == false marks a free slot.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
// Index range [first, first+count) of a mesh that shares one material.
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
// A loaded .dmesh: one GPU mesh, its material table and optional sidecar key/values.
#endif
#include "stb_easy_font.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using namespace gfxgl;

namespace gfx {
// Inverse-transpose of the model matrix so normals stay perpendicular under non-uniform scale.

namespace {

// Colour-pass record (one submesh or whole mesh) with a precomputed world bounding sphere.
constexpr int kVertexFloats = 14; // pos3 nrm3 col3 uv2 smoothNrm3
// Material flag bits stored per submesh in the .dmesh file.
constexpr uint32_t kFlagDoubleSided = 1, kFlagNoOutline = 2, kFlagGlossy = 4, kFlagAlphaTest = 8;

// GPU-side mesh: VAO with static VBO/IBO plus object-space AABB. alive == false marks a free slot.
struct GpuMesh
// Plane i = row 3 +/- row i of the clip matrix, normalised so d is a world-space distance.
{
    GLuint vao = 0, vbo = 0, ibo = 0;
    GLsizei indexCount = 0;
    Bounds bounds{};
    bool alive = false;
};

// Index range [first, first+count) of a mesh that shares one material.
struct SubMesh
{
    uint32_t first = 0, count = 0;
    float color[4] = {1, 1, 1, 1};
    dm::Vec3 emission{0, 0, 0};
    // Conservative test: false only if the sphere lies fully behind one plane.
    uint32_t flags = 0;
    GLuint tex = 0;
};

// A loaded .dmesh: one GPU mesh, its material table and optional sidecar key/values.
struct GpuModel
{
    std::string name;
    // Copies the upper-left 3x3 into column-major float[9] (for glUniformMatrix3fv).
    MeshHandle mesh = 0;
    std::vector<SubMesh> subs;
    // The converter orders submeshes outlined-opaque, opaque, then cutout /
    // translucent, so outlines and shadows are one draw over a prefix.
    uint32_t outlineCount = 0, shadowCount = 0;
    Bounds bounds{};
    // Inverse-transpose of the model matrix so normals stay perpendicular under non-uniform scale.
    std::map<std::string, std::string> info;
// All renderer state is kept here (pimpl) so renderer.h exposes no GL types.
};

// Colour-pass record (one submesh or whole mesh) with a precomputed world bounding sphere.
struct DrawItem
{
    dm::Vec3 centre; // world bounding sphere
    float radius = 0;
    bool visible = true;
    MeshHandle mesh = 0;
    // Plane i = row 3 +/- row i of the clip matrix, normalised so d is a world-space distance.
    const SubMesh *sub = nullptr; // nullptr = whole mesh, default material
    dm::Mat4 model;
    DrawParams params;
    float sortKey = 0;
};

// One per drawMesh/drawModel call: used by the shadow and outline passes,
// which draw a whole model at once instead of per submesh.
struct Instance
{
    MeshHandle mesh = 0;
    dm::Mat4 model;
    DrawParams params;
    // Conservative test: false only if the sphere lies fully behind one plane.
    uint32_t outlineCount = 0, shadowCount = 0;
    dm::Vec3 centre;
    float radius = 0;
};

struct Billboard
{
    dm::Vec3 pos;
    // Copies the upper-left 3x3 into column-major float[9] (for glUniformMatrix3fv).
    float size;
    dm::Vec3 color;
    float alpha;
    float dist;
};

// Inverse-transpose of the model matrix so normals stay perpendicular under non-uniform scale.
dm::Mat4 normalMatrix4(const dm::Mat4 &m)
// All renderer state is kept here (pimpl) so renderer.h exposes no GL types.
{
    dm::Mat4 r = dm::transpose(dm::inverse(m));
    return r;
}

// Frustum planes (a,b,c,d; inside when a*x+b*y+c*z+d >= 0) of a clip matrix.
struct Frustum
{
    float p[6][4];
    // Plane i = row 3 +/- row i of the clip matrix, normalised so d is a world-space distance.
    void from(const dm::Mat4 &m)
    {
        for (int i = 0; i < 3; ++i)
            for (int sgn = 0; sgn < 2; ++sgn)
            {
                float *pl = p[i * 2 + sgn];
                float s = sgn ? -1.0f : 1.0f;
                for (int c = 0; c < 4; ++c) pl[c] = m.at(3, c) + s * m.at(i, c);
                float l = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
                if (l > 1e-8f)
                    for (int c = 0; c < 4; ++c) pl[c] /= l;
            }
    }
    // Conservative test: false only if the sphere lies fully behind one plane.
    bool sphere(const dm::Vec3 &c, float r) const
    {
        // Rebuilt every frame; vectors are cleared, not freed, to avoid per-frame allocation.
        for (const auto &pl : p)
            if (pl[0] * c.x + pl[1] * c.y + pl[2] * c.z + pl[3] < -r) return false;
        return true;
    }
};

// Copies the upper-left 3x3 into column-major float[9] (for glUniformMatrix3fv).
void mat3From4(const dm::Mat4 &m, float out[9])
{
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) out[c * 3 + r] = m.at(r, c);
}

} // namespace

// All renderer state is kept here (pimpl) so renderer.h exposes no GL types.
struct Renderer::Impl
{
    GLFWwindow *window = nullptr;
    std::string assetDir = "assets";
    std::string shaderDir;
    bool ready = false;

    Program toon, outline, shadow, sky, billboard, overlay;
    Program blur, composite, flame, glow;

    std::vector<GpuMesh> meshes; // handle = index + 1
    std::vector<MeshHandle> freeMeshes;
    // Loads every shader program; all are attempted so each failure is logged. False if any failed.
    std::vector<GpuModel> models; // handle = index + 1
    std::unordered_map<std::string, ModelHandle> modelByName;
    std::unordered_map<std::string, GLuint> textures;
    GLuint whiteTex = 0;
    GLuint smokeTex = 0;

    // skybox
    GLuint skyTex = 0, skyVao = 0, skyVbo = 0;
    std::string skyName;
    dm::Vec3 skyHorizon = Lighting{}.fogColor;
    bool hasSkyHorizon = false;

    // targets
    int fbW = 0, fbH = 0;
    // Rebuilt every frame; vectors are cleared, not freed, to avoid per-frame allocation.
    int samples = 4;
    // Drains glGetError and logs; capped at 20 messages in total to avoid flooding stderr.
    GLuint sceneFbo = 0, sceneColor = 0, sceneDepth = 0;
    GLuint resolveFbo = 0, resolveColor = 0, resolveDepth = 0; // textures (post input)
    GLuint blurFbo = 0, blurTex = 0;
    int blurW = 0, blurH = 0;
    GLuint finalFbo = 0, finalColor = 0; // post output + overlay
    GLuint presentFbo = 0;               // what was presented last (screenshots)

    // screen effects
    FrameFx fx;
    bool fxSet = false;
    FxQuality quality;
    dm::Mat4 prevViewProj;
    bool havePrev = false;
    GLuint emptyVao = 0;
// Handle (1-based index) to live mesh, or nullptr for invalid / freed handles.

    // nitro flames
    struct FlameDraw
    {
        dm::Mat4 frame; // exhaust frame (orthonormal, origin at the pipe)
        float intensity, ignition, time, seed;
    // Recycles a freed handle if one exists, otherwise appends a new slot.
    };
    std::vector<FlameDraw> flames;
    GLuint flameVao = 0, flameVbo = 0, flameIbo = 0;
    GLsizei flameIndexCount = 0;
    GLuint glowVao = 0, glowVbo = 0;
    std::vector<float> glowVerts; // pos3 uv2 rgba4 params2
    bool haveFrame = false;
// Loads every shader program; all are attempted so each failure is logged. False if any failed.

    // shadows
    bool shadowsEnabled = true;
    int shadowSize = 2048;
    // Uploads interleaved vertices (kVertexFloats per vertex) and 32-bit indices
    // as static buffers, creating the VAO on first use, and recomputes the AABB.
    float shadowRadius = 32.0f;
    GLuint shadowFbo = 0, shadowTex = 0;

    float outlineWidth = 2.2f;

    // dynamic buffers
    GLuint bbVao = 0, bbVbo = 0;
    GLuint ovVao = 0, ovVbo = 0;

    // frame state
    // Rebuilt every frame; vectors are cleared, not freed, to avoid per-frame allocation.
    // attribute sizes: position, normal, colour, uv, smoothed normal (used by the outline shader)
    bool inFrame = false;
    // Drains glGetError and logs; capped at 20 messages in total to avoid flooding stderr.
    Camera cam;
    Lighting light;
    dm::Mat4 view, proj, viewProj, lightVP;
    Frustum viewFrustum, lightFrustum;
    std::vector<DrawItem> opaque, transparent;
    std::vector<Instance> instances;
    std::vector<Billboard> billboards;
    std::vector<float> overlayVerts; // x y r g b a u v
    struct OverlayCmd
    {
        GLuint tex;
        bool additive;
        uint32_t first, count; // vertices
    };
    // object-space AABB over all vertex positions
    // Handle (1-based index) to live mesh, or nullptr for invalid / freed handles.
    std::vector<OverlayCmd> overlayCmds;

    // HUD textures (top-left UV origin, separate from model textures)
    struct HudTex
    {
        GLuint id;
        // Recycles a freed handle if one exists, otherwise appends a new slot.
        int w, h;
    };
    std::vector<HudTex> hudTextures; // handle = index + 1
    // Deletes the GL objects and resets the slot (the handle is recycled by the caller).
    std::unordered_map<std::string, TextureHandle> hudTexByPath;
    int glErrorsLogged = 0;

    // ------------------------------------------------------------------
    // Loads every shader program; all are attempted so each failure is logged. False if any failed.
    bool loadPrograms()
    {
        bool ok = true;
        ok &= toon.load(shaderDir, "toon");
        // Cached lookup of assets/textures/<name>; a failed load caches 0. Empty name returns 0 (untextured).
        // Uploads interleaved vertices (kVertexFloats per vertex) and 32-bit indices
        // as static buffers, creating the VAO on first use, and recomputes the AABB.
        ok &= outline.load(shaderDir, "outline");
        ok &= shadow.load(shaderDir, "shadow");
        ok &= sky.load(shaderDir, "sky");
        ok &= billboard.load(shaderDir, "billboard");
        ok &= overlay.load(shaderDir, "overlay");
        ok &= blur.load(shaderDir, "post", "blur");
        ok &= composite.load(shaderDir, "post", "composite");
        ok &= flame.load(shaderDir, "flame");
        ok &= glow.load(shaderDir, "glow");
        // (Re)creates size-dependent render targets when the framebuffer size changes:
        // MSAA scene FBO (renderbuffers), resolve FBO (textures), final FBO, blur FBO.
        // Leaves the default framebuffer bound.
        return ok;
    }
// attribute sizes: position, normal, colour, uv, smoothed normal (used by the outline shader)

    // Drains glGetError and logs; capped at 20 messages in total to avoid flooding stderr.
    void checkGL(const char *where)
    {
        GLenum e;
        while ((e = glGetError()) != GL_NO_ERROR)
        // clamp requested MSAA sample count to the hardware maximum
        {
            if (glErrorsLogged < 20)
            {
                std::fprintf(stderr, "[gfx] GL error 0x%04x at %s\n", e, where);
                ++glErrorsLogged;
            }
        }
    }

    // ------------------------------------------------------------------
    // object-space AABB over all vertex positions
    // Handle (1-based index) to live mesh, or nullptr for invalid / freed handles.
    GpuMesh *mesh(MeshHandle h)
    {
        if (h == 0 || h > meshes.size() || !meshes[h - 1].alive) return nullptr;
        return &meshes[h - 1];
    }

    // Recycles a freed handle if one exists, otherwise appends a new slot.
    MeshHandle allocMesh()
    {
        if (!freeMeshes.empty())
        // Deletes the GL objects and resets the slot (the handle is recycled by the caller).
        {
            MeshHandle h = freeMeshes.back();
            freeMeshes.pop_back();
            return h;
        }
        meshes.emplace_back();
        return (MeshHandle)meshes.size();
    // final target: receives the post-processed image, then the HUD overlay
    }

    // Cached lookup of assets/textures/<name>; a failed load caches 0. Empty name returns 0 (untextured).
    // Uploads interleaved vertices (kVertexFloats per vertex) and 32-bit indices
    // as static buffers, creating the VAO on first use, and recomputes the AABB.
    void upload(GpuMesh &m, const std::vector<float> &verts, const std::vector<uint32_t> &idx)
    {
        if (!m.vao)
        {
            glGenVertexArrays(1, &m.vao);
            glGenBuffers(1, &m.vbo);
            glGenBuffers(1, &m.ibo);
            // Allocates an empty 2D texture (clamp-to-edge) with the given formats and min/mag filter.
            glBindVertexArray(m.vao);
            glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
            // (Re)creates size-dependent render targets when the framebuffer size changes:
            // MSAA scene FBO (renderbuffers), resolve FBO (textures), final FBO, blur FBO.
            // Leaves the default framebuffer bound.
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
            const GLsizei stride = kVertexFloats * sizeof(float);
            // attribute sizes: position, normal, colour, uv, smoothed normal (used by the outline shader)
            const int sizes[5] = {3, 3, 3, 2, 3};
            size_t off = 0;
            for (int a = 0; a < 5; ++a)
            {
                glEnableVertexAttribArray(a);
                glVertexAttribPointer(a, sizes[a], GL_FLOAT, GL_FALSE, stride, (const void *)(off * sizeof(float)));
                // Blur target is half resolution when quality.halfResBlur is set; recreated only on size change.
                // clamp requested MSAA sample count to the hardware maximum
                off += sizes[a];
            }
        }
        glBindVertexArray(m.vao);
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(uint32_t), idx.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
        m.indexCount = (GLsizei)idx.size();
        m.alive = true;
        // object-space AABB over all vertex positions
        Bounds b{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
        for (size_t i = 0; i + kVertexFloats <= verts.size(); i += kVertexFloats)
            for (int k = 0; k < 3; ++k)
            {
                b.min[k] = std::min(b.min[k], verts[i + k]);
                b.max[k] = std::max(b.max[k], verts[i + k]);
            }
        // Releases all size-dependent targets and forgets the previous view-projection (motion blur reset).
        if (verts.empty()) b = Bounds{};
        m.bounds = b;
    }

    // Deletes the GL objects and resets the slot (the handle is recycled by the caller).
    void freeGpu(GpuMesh &m)
    {
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.ibo) glDeleteBuffers(1, &m.ibo);
        m = GpuMesh{};
    }
// final target: receives the post-processed image, then the HUD overlay

    // ------------------------------------------------------------------
    // Cached lookup of assets/textures/<name>; a failed load caches 0. Empty name returns 0 (untextured).
    GLuint texture(const std::string &name)
    // Lazily creates the shadow depth texture + FBO (no colour buffer); disables shadows if incomplete.
    {
        if (name.empty()) return 0;
        auto it = textures.find(name);
        if (it != textures.end()) return it->second;
        GLuint t = loadTexture2D(assetDir + "/textures/" + name, true);
        textures[name] = t;
        return t;
    }
// outside the map the border depth of 1.0 means "lit"
// Allocates an empty 2D texture (clamp-to-edge) with the given formats and min/mag filter.

    // ------------------------------------------------------------------
    // (Re)creates size-dependent render targets when the framebuffer size changes:
    // hardware depth comparison: sampler2DShadow returns a filtered 0..1 visibility
    // MSAA scene FBO (renderbuffers), resolve FBO (textures), final FBO, blur FBO.
    // Leaves the default framebuffer bound.
    void ensureTargets()
    {
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        if (w <= 0 || h <= 0) { fbW = fbH = 0; return; }
        if (w == fbW && h == fbH && sceneFbo) return;
        destroyTargets();
        fbW = w;
        fbH = h;
        // Blur target is half resolution when quality.halfResBlur is set; recreated only on size change.
        // clamp requested MSAA sample count to the hardware maximum
        GLint maxSamples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        int s = std::min(samples, (int)maxSamples);
// Builds lightVP: an orthographic sun view of side 2*shadowRadius (world units)
// centred ahead of the camera, so shadow resolution stays on the visible area.

        glGenFramebuffers(1, &sceneFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo);
        glGenRenderbuffers(1, &sceneColor);
        glBindRenderbuffer(GL_RENDERBUFFER, sceneColor);
        if (s > 1) glRenderbufferStorageMultisample(GL_RENDERBUFFER, s, GL_RGBA8, w, h);
        else glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sceneColor);
        glGenRenderbuffers(1, &sceneDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, sceneDepth);
        if (s > 1) glRenderbufferStorageMultisample(GL_RENDERBUFFER, s, GL_DEPTH_COMPONENT24, w, h);
        else glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sceneDepth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::fprintf(stderr, "[gfx] scene framebuffer incomplete (samples=%d)\n", s);
// Releases all size-dependent targets and forgets the previous view-projection (motion blur reset).
// light looks down -z, so near/far are -c.z -/+ depth around the snapped centre

        // resolve target: colour (+ blur mask in alpha) and depth as textures
        // so the post pass can read them
        resolveColor = makeTex(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
        resolveDepth = makeTex(GL_DEPTH_COMPONENT24, w, h, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_NEAREST);
        // Sets it.centre / it.radius: world bounding sphere of the local AABB; the
        // radius is scaled by the largest axis scale of the model matrix.
        glGenFramebuffers(1, &resolveFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolveColor, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, resolveDepth, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::fprintf(stderr, "[gfx] resolve framebuffer incomplete\n");

        // final target: receives the post-processed image, then the HUD overlay
        GLuint fc = makeTex(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
        finalColor = fc;
        glGenFramebuffers(1, &finalFbo);
        // Draws one submesh range (or the whole mesh when it.sub is null). Index offset is in bytes.
        glBindFramebuffer(GL_FRAMEBUFFER, finalFbo);
        // Lazily creates the shadow depth texture + FBO (no colour buffer); disables shadows if incomplete.
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, finalColor, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::fprintf(stderr, "[gfx] final framebuffer incomplete\n");
        ensureBlurTarget();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        haveFrame = false;
    }

    // Shadow pass: renders casters into the shadow map from the sun. Changes FBO,
    // viewport, culling and polygon offset (offset is restored; FBO/viewport are not).
    // outside the map the border depth of 1.0 means "lit"
    // Allocates an empty 2D texture (clamp-to-edge) with the given formats and min/mag filter.
    static GLuint makeTex(GLenum ifmt, int w, int h, GLenum fmt, GLenum type, GLenum filter)
    {
        GLuint t = 0;
        // hardware depth comparison: sampler2DShadow returns a filtered 0..1 visibility
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        return t;
    }

    // Blur target is half resolution when quality.halfResBlur is set; recreated only on size change.
    void ensureBlurTarget()
    {
        if (fbW <= 0) return;
        int w = quality.halfResBlur ? std::max(1, fbW / 2) : fbW;
        // Builds lightVP: an orthographic sun view of side 2*shadowRadius (world units)
        // centred ahead of the camera, so shadow resolution stays on the visible area.
        int h = quality.halfResBlur ? std::max(1, fbH / 2) : fbH;
        if (blurFbo && w == blurW && h == blurH) return;
        if (blurFbo) glDeleteFramebuffers(1, &blurFbo);
        // Uploads per-frame toon uniforms; the shadow map is bound to texture unit 1, albedo uses unit 0.
        if (blurTex) glDeleteTextures(1, &blurTex);
        blurW = w;
        blurH = h;
        blurTex = makeTex(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
        glGenFramebuffers(1, &blurFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, blurFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blurTex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::fprintf(stderr, "[gfx] blur framebuffer incomplete\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Releases all size-dependent targets and forgets the previous view-projection (motion blur reset).
    // light looks down -z, so near/far are -c.z -/+ depth around the snapped centre
    void destroyTargets()
    {
        GLuint fbos[4] = {sceneFbo, resolveFbo, finalFbo, blurFbo};
        // the shadow map lives on unit 1; bind 0 when disabled so the sampler stays valid
        for (GLuint f : fbos)
            if (f) glDeleteFramebuffers(1, &f);
        // Sets it.centre / it.radius: world bounding sphere of the local AABB; the
        // radius is scaled by the largest axis scale of the model matrix.
        GLuint rbs[2] = {sceneColor, sceneDepth};
        // Draws one item with the toon program: per-draw model/normal matrices, material and culling mode.
        for (GLuint rb : rbs)
            if (rb) glDeleteRenderbuffers(1, &rb);
        GLuint texs[4] = {resolveColor, resolveDepth, finalColor, blurTex};
        for (GLuint t : texs)
            if (t) glDeleteTextures(1, &t);
        sceneFbo = resolveFbo = finalFbo = blurFbo = presentFbo = 0;
        sceneColor = sceneDepth = resolveColor = resolveDepth = finalColor = blurTex = 0;
        blurW = blurH = 0;
        havePrev = false;
    }
// Draws one submesh range (or the whole mesh when it.sub is null). Index offset is in bytes.

    // Lazily creates the shadow depth texture + FBO (no colour buffer); disables shadows if incomplete.
    void ensureShadowMap()
    {
        if (shadowFbo) return;
        glGenTextures(1, &shadowTex);
        glBindTexture(GL_TEXTURE_2D, shadowTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, shadowSize, shadowSize, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        // albedo = material colour * per-draw tint; alpha = material alpha * per-draw alpha
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // Shadow pass: renders casters into the shadow map from the sun. Changes FBO,
        // viewport, culling and polygon offset (offset is restored; FBO/viewport are not).
        // outside the map the border depth of 1.0 means "lit"
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        float border[4] = {1, 1, 1, 1};
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
        // hardware depth comparison: sampler2DShadow returns a filtered 0..1 visibility
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glGenFramebuffers(1, &shadowFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTex, 0);
        // Outline pass: inverted hull. Back faces are drawn (front faces culled) with
        // vertices pushed outwards in the shader; width is in pixels at a 720p baseline.
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            std::fprintf(stderr, "[gfx] shadow framebuffer incomplete; shadows disabled\n");
            shadowsEnabled = false;
        // scale the pixel width with framebuffer height so outlines keep their proportion
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ------------------------------------------------------------------
    // Builds lightVP: an orthographic sun view of side 2*shadowRadius (world units)
    // centred ahead of the camera, so shadow resolution stays on the visible area.
    void computeLightVP()
    {
        dm::Vec3 L = dm::normalize(light.sunDirection);
        // Uploads per-frame toon uniforms; the shadow map is bound to texture unit 1, albedo uses unit 0.
        // centre the shadow box a bit ahead of the camera, around its target
        dm::Vec3 fwd = cam.target - cam.position;
        float fl = dm::length(fwd);
        fwd = fl > 1e-4f ? fwd / fl : dm::Vec3{0, 0, -1};
        dm::Vec3 centre = cam.position + fwd * std::min(fl + shadowRadius * 0.35f, shadowRadius * 0.9f);
        dm::Vec3 up = std::fabs(L.y) > 0.99f ? dm::Vec3{0, 0, 1} : dm::Vec3{0, 1, 0};
        dm::Mat4 lv = dm::lookAt({0, 0, 0}, L, up);
        // snap the centre to shadow texels (in light space) to avoid shimmer
        dm::Vec3 c = dm::transformPoint(lv, centre);
        float texel = 2.0f * shadowRadius / (float)shadowSize;
        c.x = std::floor(c.x / texel) * texel;
        c.y = std::floor(c.y / texel) * texel;
        float depth = 250.0f;
        // light looks down -z, so near/far are -c.z -/+ depth around the snapped centre
        dm::Mat4 lp = dm::ortho(c.x - shadowRadius, c.x + shadowRadius, c.y - shadowRadius, c.y + shadowRadius,
                                // Draws the cubemap sky after opaque geometry; depth LEQUAL with depth writes off.
                                -c.z - depth, -c.z + depth);
        lightVP = lp * lv;
    // the shadow map lives on unit 1; bind 0 when disabled so the sampler stays valid
    // drop the translation so the cube stays centred on the camera
    }

    // Sets it.centre / it.radius: world bounding sphere of the local AABB; the
    // radius is scaled by the largest axis scale of the model matrix.
    template <class T> void setBoundsSphere(T &it, const Bounds &b)
    // Draws one item with the toon program: per-draw model/normal matrices, material and culling mode.
    {
        dm::Vec3 c = (b.min + b.max) * 0.5f;
        float r = dm::length(b.max - b.min) * 0.5f;
        float sx = dm::length(dm::Vec3{it.model.at(0, 0), it.model.at(1, 0), it.model.at(2, 0)});
        float sy = dm::length(dm::Vec3{it.model.at(0, 1), it.model.at(1, 1), it.model.at(2, 1)});
        float sz = dm::length(dm::Vec3{it.model.at(0, 2), it.model.at(1, 2), it.model.at(2, 2)});
        it.centre = dm::transformPoint(it.model, c);
        it.radius = r * std::max(sx, std::max(sy, sz));
    }

    // Draws one submesh range (or the whole mesh when it.sub is null). Index offset is in bytes.
    void drawItemGeometry(const DrawItem &it, GpuMesh &m)
    {
        // Sorted smoke/particle sprites: camera-facing quads built on the CPU each frame
        // and streamed. Assumes blending is on and depth writes are off (set by endFrame).
        glBindVertexArray(m.vao);
        if (it.sub)
            glDrawElements(GL_TRIANGLES, (GLsizei)it.sub->count, GL_UNSIGNED_INT,
                           // sort back to front for correct alpha blending
                           (const void *)(uintptr_t)(it.sub->first * sizeof(uint32_t)));
        else
            glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
    // camera right / up axes are rows 0 and 1 of the view rotation
    // albedo = material colour * per-draw tint; alpha = material alpha * per-draw alpha
    }
// 6 vertices x 9 floats (pos3 uv2 rgba4) per billboard

    // Shadow pass: renders casters into the shadow map from the sun. Changes FBO,
    // viewport, culling and polygon offset (offset is restored; FBO/viewport are not).
    void shadowPass()
    {
        ensureShadowMap();
        if (!shadowsEnabled) return;
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo);
        glViewport(0, 0, shadowSize, shadowSize);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE); // thin car panels / tree cards cast from both sides
        glEnable(GL_POLYGON_OFFSET_FILL);
        // Outline pass: inverted hull. Back faces are drawn (front faces culled) with
        // vertices pushed outwards in the shader; width is in pixels at a 720p baseline.
        glPolygonOffset(2.0f, 4.0f);
        shadow.use();
        shadow.mat4("uLightVP", lightVP);
        for (const Instance &it : instances)
        {
            if (!it.params.castShadow || it.shadowCount == 0) continue;
            // scale the pixel width with framebuffer height so outlines keep their proportion
            if (!lightFrustum.sphere(it.centre, it.radius)) continue;
            GpuMesh *m = mesh(it.mesh);
            if (!m) continue;
            shadow.mat4("uModel", it.model);
            glBindVertexArray(m->vao);
            glDrawElements(GL_TRIANGLES, (GLsizei)it.shadowCount, GL_UNSIGNED_INT, nullptr);
        }
        // Draws the batched 2D overlay in pixel coordinates (top-left origin). Uploads
        // all vertices once, then one draw per command (texture + blend mode run).
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // Uploads per-frame toon uniforms; the shadow map is bound to texture unit 1, albedo uses unit 0.
    void setFrameUniformsToon(bool opaquePass)
    {
        toon.use();
        toon.i1("uOpaque", opaquePass ? 1 : 0);
        toon.mat4("uViewProj", viewProj);
        toon.mat4("uLightVP", lightVP);
        toon.vec3("uCamPos", cam.position);
        toon.vec3("uCamUp", dm::Vec3{view.at(1, 0), view.at(1, 1), view.at(1, 2)});
        toon.vec3("uSunDir", dm::normalize(light.sunDirection));
        toon.vec3("uSunColor", light.sunColor);
        toon.vec3("uAmbient", light.ambient);
        // additive: colour adds, destination alpha kept; normal: standard alpha blend
        toon.vec3("uFogColor", light.fogColor);
        toon.vec2("uFogRange", light.fogStart, light.fogEnd);
        toon.i1("uTex", 0);
        toon.i1("uShadowMap", 1);
        // Draws the cubemap sky after opaque geometry; depth LEQUAL with depth writes off.
        toon.i1("uShadowOn", shadowsEnabled ? 1 : 0);
        toon.f1("uShadowTexel", 1.0f / (float)shadowSize);
        // the shadow map lives on unit 1; bind 0 when disabled so the sampler stays valid
        // drop the translation so the cube stays centred on the camera
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, shadowsEnabled ? shadowTex : 0);
        glActiveTexture(GL_TEXTURE0);
    }

    // Draws one item with the toon program: per-draw model/normal matrices, material and culling mode.
    void drawToon(const DrawItem &it)
    {
        GpuMesh *m = mesh(it.mesh);
        if (!m) return;
        toon.mat4("uModel", it.model);
        float nm[9];
        mat3From4(normalMatrix4(it.model), nm);
        toon.mat3("uNormalMat", nm);
        float c[4] = {1, 1, 1, 1};
        dm::Vec3 em{0, 0, 0};
        uint32_t flags = 0;
        GLuint tex = 0;
        if (it.sub)
        // Appends one overlay vertex (x y in pixels, then colour, then uv) to the current command.
        // Sorted smoke/particle sprites: camera-facing quads built on the CPU each frame
        // and streamed. Assumes blending is on and depth writes are off (set by endFrame).
        {
            std::memcpy(c, it.sub->color, sizeof c);
            em = it.sub->emission;
            // sort back to front for correct alpha blending
            // Axis-aligned solid rectangle (two triangles) from corner (x0,y0) to (x1,y1).
            flags = it.sub->flags;
            tex = it.sub->tex;
        }
        // camera right / up axes are rows 0 and 1 of the view rotation
        // albedo = material colour * per-draw tint; alpha = material alpha * per-draw alpha
        toon.vec4("uAlbedo", c[0] * it.params.tint.x, c[1] * it.params.tint.y, c[2] * it.params.tint.z,
                  // 6 vertices x 9 floats (pos3 uv2 rgba4) per billboard
                  c[3] * it.params.alpha);
        toon.vec3("uEmission", em);
        toon.i1("uHasTex", tex ? 1 : 0);
        toon.i1("uGlossy", (flags & kFlagGlossy) ? 1 : 0);
        bool ds = (flags & kFlagDoubleSided) != 0;
        toon.i1("uDoubleSided", ds ? 1 : 0);
        toon.i1("uAlphaTest", (flags & kFlagAlphaTest) ? 1 : 0);
        // HUD texture handle (1-based) to GL id; 0 if invalid.
        toon.f1("uBlurMask", it.params.motionBlur ? 1.0f : 0.0f);
        if (ds) glDisable(GL_CULL_FACE);
        else glEnable(GL_CULL_FACE);
        glBindTexture(GL_TEXTURE_2D, tex ? tex : whiteTex);
        drawItemGeometry(it, *m);
    // Uploads RGBA8 pixels as a HUD texture (optionally mipmapped) and caches it under key.
    }

    // Outline pass: inverted hull. Back faces are drawn (front faces culled) with
    // vertices pushed outwards in the shader; width is in pixels at a 720p baseline.
    void outlinePass()
    {
        outline.use();
        outline.mat4("uView", view);
        outline.mat4("uProj", proj);
        outline.vec2("uViewport", (float)fbW, (float)fbH);
        // scale the pixel width with framebuffer height so outlines keep their proportion
        outline.f1("uWidthPx", outlineWidth * (float)fbH / 720.0f);
        outline.vec3("uCamPos", cam.position);
        outline.vec3("uInk", dm::Vec3{0.05f, 0.04f, 0.07f});
        outline.vec3("uFogColor", light.fogColor);
        outline.vec2("uFogRange", light.fogStart, light.fogEnd);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        // Draws the batched 2D overlay in pixel coordinates (top-left origin). Uploads
        // Generates the shared 1x1 white, soft radial glow and anti-aliased disc textures.
        // all vertices once, then one draw per command (texture + blend mode run).
        for (const Instance &it : instances)
        {
            if (!it.params.outline || it.outlineCount == 0) continue;
            if (!viewFrustum.sphere(it.centre, it.radius)) continue;
            GpuMesh *m = mesh(it.mesh);
            if (!m) continue;
            // skip outlines that would be sub-pixel / fully fogged
            if (dm::length(it.centre - cam.position) - it.radius > light.fogEnd) continue;
            outline.mat4("uModel", it.model);
            // soft falloff from the centre outwards
            float nm[9];
            mat3From4(normalMatrix4(it.model), nm);
            outline.mat3("uNormalMat", nm);
            outline.f1("uBlurMask", it.params.motionBlur ? 1.0f : 0.0f);
            glBindVertexArray(m->vao);
            glDrawElements(GL_TRIANGLES, (GLsizei)it.outlineCount, GL_UNSIGNED_INT, nullptr);
        // additive: colour adds, destination alpha kept; normal: standard alpha blend
        }
        glCullFace(GL_BACK);
    }

    // Draws the cubemap sky after opaque geometry; depth LEQUAL with depth writes off.
    void skyPass()
    {
        sky.use();
        // drop the translation so the cube stays centred on the camera
        // Speed-line strength: speed ramps in above 45%, nitro adds on top; scaled by the user setting.
        dm::Mat4 vr = view;
        vr.at(0, 3) = vr.at(1, 3) = vr.at(2, 3) = 0;
        sky.mat4("uViewRot", vr);
        sky.mat4("uProj", proj);
        sky.vec3("uFogColor", light.fogColor);
        // Radial blur strength in UV units: grows with speed and, more strongly, nitro.
        sky.f1("uHorizonFog", 0.85f);
        sky.i1("uSky", 0);
        sky.i1("uHasSky", skyTex ? 1 : 0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, skyTex);
        glDepthFunc(GL_LEQUAL);
        // True when the blur pass will run (post enabled, motion blur allowed and some blur requested).
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glBindVertexArray(skyVao);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        // True when any post effect is visible; otherwise the post passes are skipped entirely.
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }

    // Appends one overlay vertex (x y in pixels, then colour, then uv) to the current command.
    // Sorted smoke/particle sprites: camera-facing quads built on the CPU each frame
    // and streamed. Assumes blending is on and depth writes are off (set by endFrame).
    // Uniforms shared by the blur and composite programs; scene on unit 0, depth on unit 1.
    void billboardPass()
    {
        // inverse view-proj rebuilds world position from depth; with last frame's matrix it gives screen-space motion
        if (billboards.empty()) return;
        // sort back to front for correct alpha blending
        // Axis-aligned solid rectangle (two triangles) from corner (x0,y0) to (x1,y1).
        for (Billboard &b : billboards) b.dist = dm::length(b.pos - cam.position);
        std::sort(billboards.begin(), billboards.end(),
                  [](const Billboard &a, const Billboard &b) { return a.dist > b.dist; });
        // camera right / up axes are rows 0 and 1 of the view rotation
        dm::Vec3 right{view.at(0, 0), view.at(0, 1), view.at(0, 2)};
        dm::Vec3 up{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
        // 6 vertices x 9 floats (pos3 uv2 rgba4) per billboard
        // Post pass: blur (optional, half res) from the resolved scene, then composite
        // into finalFbo using a fullscreen triangle. Disables depth/cull/blend and
        // re-enables depth testing on exit; unbinds units 1 and 2.
        std::vector<float> v;
        v.reserve(billboards.size() * 6 * 9);
        for (const Billboard &b : billboards)
        {
            float h = b.size * 0.5f;
            dm::Vec3 c[4] = {b.pos - right * h - up * h, b.pos + right * h - up * h, b.pos + right * h + up * h,
                             b.pos - right * h + up * h};
            // HUD texture handle (1-based) to GL id; 0 if invalid.
            float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            int order[6] = {0, 1, 2, 0, 2, 3};
            for (int k : order)
            {
                v.insert(v.end(), {c[k].x, c[k].y, c[k].z, uv[k][0], uv[k][1], b.color.x, b.color.y, b.color.z,
                                   // Uploads RGBA8 pixels as a HUD texture (optionally mipmapped) and caches it under key.
                                   b.alpha});
            }
        }
        glBindVertexArray(bbVao);
        glBindBuffer(GL_ARRAY_BUFFER, bbVbo);
        glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STREAM_DRAW);
        billboard.use();
        // composite: scene + blur + speed lines / nitro / slipstream at full resolution
        billboard.mat4("uViewProj", viewProj);
        billboard.vec3("uCamPos", cam.position);
        billboard.vec3("uFogColor", light.fogColor);
        billboard.vec2("uFogRange", light.fogStart, light.fogEnd);
        billboard.i1("uTex", 0);
        billboard.i1("uHasTex", smokeTex ? 1 : 0);
        glBindTexture(GL_TEXTURE_2D, smokeTex);
        glDisable(GL_CULL_FACE);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size() / 9));
    }

    // Draws the batched 2D overlay in pixel coordinates (top-left origin). Uploads
    // Generates the shared 1x1 white, soft radial glow and anti-aliased disc textures.
    // all vertices once, then one draw per command (texture + blend mode run).
    void overlayPass()
    {
        if (overlayVerts.empty()) return;
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        overlay.use();
        overlay.vec2("uViewport", (float)fbW, (float)fbH);
        overlay.i1("uTex", 0);
        // soft falloff from the centre outwards
        glActiveTexture(GL_TEXTURE0);
        // Builds the unit flame shell (VAO/VBO/IBO) and the VAO used for glow sprites. Called once, lazily.
        glBindVertexArray(ovVao);
        glBindBuffer(GL_ARRAY_BUFFER, ovVbo);
        glBufferData(GL_ARRAY_BUFFER, overlayVerts.size() * sizeof(float), overlayVerts.data(), GL_STREAM_DRAW);
        for (const OverlayCmd &c : overlayCmds)
        {
            // additive: colour adds, destination alpha kept; normal: standard alpha blend
            if (c.additive) glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
            else glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glBindTexture(GL_TEXTURE_2D, c.tex ? c.tex : whiteTex);
            glDrawArrays(GL_TRIANGLES, (GLint)c.first, (GLsizei)c.count);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        // ring r of seg vertices; normal tilts by the radius slope so shading follows the taper
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }
// Speed-line strength: speed ramps in above 45%, nitro adds on top; scaled by the user setting.

    // start / extend an overlay command for (tex, additive); returns nothing,
    // callers then append whole triangles with ov()
    void ovState(GLuint tex, bool additive)
    {
        // Radial blur strength in UV units: grows with speed and, more strongly, nitro.
        uint32_t nverts = (uint32_t)(overlayVerts.size() / 8);
        if (!overlayCmds.empty())
        {
            OverlayCmd &c = overlayCmds.back();
            // two triangles per quad between ring r and r+1
            if (c.tex == tex && c.additive == additive) return;
            // True when the blur pass will run (post enabled, motion blur allowed and some blur requested).
            if (c.count == 0)
            {
                c.tex = tex;
                c.additive = additive;
                // True when any post effect is visible; otherwise the post passes are skipped entirely.
                return;
            }
        }
        overlayCmds.push_back({tex, additive, nverts, 0});
    }
    // Appends one overlay vertex (x y in pixels, then colour, then uv) to the current command.
    void ov(float x, float y, float u, float v, float r, float g, float b, float a)
    {
        // Uniforms shared by the blur and composite programs; scene on unit 0, depth on unit 1.
        overlayVerts.insert(overlayVerts.end(), {x, y, r, g, b, a, u, v});
        overlayCmds.back().count++;
    // inverse view-proj rebuilds world position from depth; with last frame's matrix it gives screen-space motion
    }

    // glow sprite buffer: pos3 uv2 rgba4 params2(kind, ring), stride 11 floats
    // Axis-aligned solid rectangle (two triangles) from corner (x0,y0) to (x1,y1).
    void pushQuad2D(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
    {
        ovState(0, false);
        float q[6][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
        for (auto &p : q) ov(p[0], p[1], 0.5f, 0.5f, r, g, b, a);
    }

    // Post pass: blur (optional, half res) from the resolved scene, then composite
    // into finalFbo using a fullscreen triangle. Disables depth/cull/blend and
    // re-enables depth testing on exit; unbinds units 1 and 2.
    // generic quad: 4 corners (clockwise from top-left) + uvs, one colour
    void pushQuadPts(const float px[4][2], const float uv[4][2], float r, float g, float b, float a)
    {
        int order[6] = {0, 1, 2, 0, 2, 3};
        for (int k : order) ov(px[k][0], px[k][1], uv[k][0], uv[k][1], r, g, b, a);
    // Appends a camera-facing quad (world position and size) to the glow vertex list.
    }

    // HUD texture handle (1-based) to GL id; 0 if invalid.
    GLuint hudTexId(TextureHandle h) const
    {
        return (h == 0 || h > hudTextures.size()) ? 0 : hudTextures[h - 1].id;
    }

    // Uploads RGBA8 pixels as a HUD texture (optionally mipmapped) and caches it under key.
    TextureHandle addHudTexture(const std::string &key, int w, int h, const unsigned char *rgba, bool mip)
    {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        // Draws nitro flames as three nested shells per exhaust plus halo sprites.
        // Uses premultiplied alpha, no depth writes; restores the standard blend func.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        // composite: scene + blur + speed lines / nitro / slipstream at full resolution
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (mip) glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
        hudTextures.push_back({t, w, h});
        TextureHandle hnd = (TextureHandle)hudTextures.size();
        hudTexByPath[key] = hnd;
        return hnd;
    }

    // Generates the shared 1x1 white, soft radial glow and anti-aliased disc textures.
    void createBuiltinHudTextures()
    {
        const int N = 128;
        std::vector<unsigned char> glow(N * N * 4), disc(N * N * 4);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
            {
                float dx = (x + 0.5f) / N * 2 - 1, dy = (y + 0.5f) / N * 2 - 1;
                float r = std::sqrt(dx * dx + dy * dy);
                float g = std::max(0.0f, 1.0f - r);
                // soft falloff from the centre outwards
                g = g * g * (0.6f + 0.4f * g);
                // Builds the unit flame shell (VAO/VBO/IBO) and the VAO used for glow sprites. Called once, lazily.
                float d = dm::clampf((1.0f - r) * N * 0.5f + 0.5f, 0.0f, 1.0f); // ~1px AA edge
                unsigned char *pg = &glow[(y * N + x) * 4], *pd = &disc[(y * N + x) * 4];
                pg[0] = pg[1] = pg[2] = 255;
                pg[3] = (unsigned char)(g * 255.0f + 0.5f);
                pd[0] = pd[1] = pd[2] = 255;
                pd[3] = (unsigned char)(d * 255.0f + 0.5f);
            }
        const unsigned char white[4] = {255, 255, 255, 255};
        // unit shell scaled to radius x radius x length, placed by the exhaust frame
        addHudTexture("builtin:white", 1, 1, white, false);
        addHudTexture("builtin:glow", N, N, glow.data(), true);
        addHudTexture("builtin:disc", N, N, disc.data(), true);
    }
// ring r of seg vertices; normal tilts by the radius slope so shading follows the taper

    // ------------------------------------------------------------------
    // Screen effects
    // Speed-line strength: speed ramps in above 45%, nitro adds on top; scaled by the user setting.
    float linesAmount() const
    {
        float s = dm::clampf(fx.speed01, 0.0f, 1.0f), n = dm::clampf(fx.nitro01, 0.0f, 1.0f);
        return fx.speedLines * (dm::clampf((s - 0.45f) / 0.55f, 0.0f, 1.0f) * 0.6f + n * 0.7f);
    }
    // Radial blur strength in UV units: grows with speed and, more strongly, nitro.
    float radialAmount() const
    {
        float s = dm::clampf(fx.speed01, 0.0f, 1.0f), n = dm::clampf(fx.nitro01, 0.0f, 1.0f);
        return fx.radialBlur * (0.004f * s + 0.022f * n);
    // two triangles per quad between ring r and r+1
    }
    // True when the blur pass will run (post enabled, motion blur allowed and some blur requested).
    bool blurActive() const
    {
        return fxSet && quality.postFx && quality.motionBlur && (fx.motionBlur > 0.0f || radialAmount() > 0.0f);
    }
    // True when any post effect is visible; otherwise the post passes are skipped entirely.
    bool postActive() const
    {
        if (!fxSet || !quality.postFx) return false;
        bool blurOn = blurActive();
        return blurOn || linesAmount() > 0.001f || fx.slipstream01 * fx.speedLines > 0.001f ||
               fx.nitro01 > 0.001f;
    }

    // Uniforms shared by the blur and composite programs; scene on unit 0, depth on unit 1.
    void setPostUniforms(Program &p)
    {
        // inverse view-proj rebuilds world position from depth; with last frame's matrix it gives screen-space motion
        dm::Mat4 inv = dm::inverse(viewProj);
        p.mat4("uInvViewProj", inv);
        // glow sprite buffer: pos3 uv2 rgba4 params2(kind, ring), stride 11 floats
        p.mat4("uPrevViewProj", havePrev ? prevViewProj : viewProj);
        p.f1("uCamBlur", std::max(0.0f, fx.motionBlur) * 0.5f); // half the frame's motion
        p.f1("uRadial", radialAmount());
        p.f1("uMaxBlur", 0.035f);
        p.i1("uScene", 0);
        p.i1("uDepth", 1);
    }

    // Post pass: blur (optional, half res) from the resolved scene, then composite
    // into finalFbo using a fullscreen triangle. Disables depth/cull/blend and
    // re-enables depth testing on exit; unbinds units 1 and 2.
    void postPass()
    {
        glDisable(GL_DEPTH_TEST);
        // Reads assets/models/<name>.dmesh: header, submesh table, 11-float vertices, indices;
        // then uploads to the GPU and parses an optional sidecar .txt. False on any parse error.
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        // Appends a camera-facing quad (world position and size) to the glow vertex list.
        glBindVertexArray(emptyVao);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, resolveDepth);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, resolveColor);
        bool blurOn = blurActive();
        if (blurOn)
        {
            ensureBlurTarget();
            // bounds-checked sequential read from the file buffer
            glBindFramebuffer(GL_FRAMEBUFFER, blurFbo);
            glViewport(0, 0, blurW, blurH);
            blur.use();
            setPostUniforms(blur);
            blur.i1("uSamples", std::max(2, std::min(32, quality.blurSamples)));
            // Draws nitro flames as three nested shells per exhaust plus halo sprites.
            // header: magic, version (1), vertex / index / submesh counts, then the AABB (min, max)
            // Uses premultiplied alpha, no depth writes; restores the standard blend func.
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        // composite: scene + blur + speed lines / nitro / slipstream at full resolution
        glBindFramebuffer(GL_FRAMEBUFFER, finalFbo);
        glViewport(0, 0, fbW, fbH);
        composite.use();
        setPostUniforms(composite);
        composite.i1("uBlur", 2);
        composite.i1("uHasBlur", blurOn ? 1 : 0);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, blurOn ? blurTex : whiteTex);
        // submesh table: index range, RGBA, emission, flags and a 64-byte texture name
        glActiveTexture(GL_TEXTURE0);
        composite.vec2("uResolution", (float)fbW, (float)fbH);
        composite.f1("uTime", fx.time);
        composite.f1("uLines", linesAmount());
        composite.f1("uNitro", dm::clampf(fx.nitro01, 0.0f, 1.0f));
        composite.f1("uSlip", dm::clampf(fx.slipstream01, 0.0f, 1.0f) * fx.speedLines);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_DEPTH_TEST);
    }

    // ------------------------------------------------------------------
    // Nitro flames
    // Builds the unit flame shell (VAO/VBO/IBO) and the VAO used for glow sprites. Called once, lazily.
    void buildFlameMesh()
    {
        // file vertices are 11 floats; buildVertexBuffer expands them to the runtime layout (adds smoothed normals)
        // unit teardrop shell along +Z: bulges just behind the nozzle, then
        // tapers to a point at z = 1
        const int seg = 14, rings = 12;
        std::vector<float> v;
        std::vector<uint32_t> idx;
        auto radius = [](float z) {
            // unit shell scaled to radius x radius x length, placed by the exhaust frame
            // Outline / shadow passes draw an index-buffer prefix. Extend it submesh by
            // submesh and stop at the first cutout / translucent one (and, for outlines,
            // at no-outline or double-sided ones).
            float bulge = std::sin(std::min(1.0f, z / 0.22f) * dm::kPi * 0.5f);
            float taper = std::pow(1.0f - z, 0.75f);
            return 0.55f + 0.45f * bulge * taper / 0.88f - 0.55f * z;
        };
        // ring r of seg vertices; normal tilts by the radius slope so shading follows the taper
        for (int r = 0; r <= rings; ++r)
        {
            float z = (float)r / rings;
            float rad = std::max(0.0f, radius(z));
            float dz = 0.01f;
            float slope = (radius(std::min(1.0f, z + dz)) - radius(std::max(0.0f, z - dz))) / (2 * dz);
            for (int k = 0; k < seg; ++k)
            {
                float a = (float)k / seg * 2 * dm::kPi;
                float c = std::cos(a), sn = std::sin(a);
                dm::Vec3 n = dm::normalize(dm::Vec3{c, sn, -slope});
                v.insert(v.end(), {c * rad, sn * rad, z, n.x, n.y, n.z});
            }
        }
        // two triangles per quad between ring r and r+1
        for (int r = 0; r < rings; ++r)
            for (int k = 0; k < seg; ++k)
            {
                uint32_t a = r * seg + k, b = r * seg + (k + 1) % seg, c = a + seg, d = b + seg;
                idx.insert(idx.end(), {a, b, d, a, d, c}); // CCW seen from outside
            }
        glGenVertexArrays(1, &flameVao);
        glGenBuffers(1, &flameVbo);
        glGenBuffers(1, &flameIbo);
        glBindVertexArray(flameVao);
        glBindBuffer(GL_ARRAY_BUFFER, flameVbo);
        glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, flameIbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(uint32_t), idx.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, nullptr);
        // Loads six PNG faces into a mipmapped cubemap (replacing any previous sky) and reads optional sky.txt.
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (const void *)12);
        glBindVertexArray(0);
        flameIndexCount = (GLsizei)idx.size();

        // glow sprite buffer: pos3 uv2 rgba4 params2(kind, ring), stride 11 floats
        glGenVertexArrays(1, &glowVao);
        glGenBuffers(1, &glowVbo);
        glBindVertexArray(glowVao);
        glBindBuffer(GL_ARRAY_BUFFER, glowVbo);
        const int stride = 11 * sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)12);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (const void *)20);
        // face files are px nx py ny pz nz, matching GL_TEXTURE_CUBE_MAP_POSITIVE_X + i
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (const void *)36);
        glBindVertexArray(0);
    // Reads assets/models/<name>.dmesh: header, submesh table, 11-float vertices, indices;
    // then uploads to the GPU and parses an optional sidecar .txt. False on any parse error.
    }

    // Appends a camera-facing quad (world position and size) to the glow vertex list.
    void pushGlow(const dm::Vec3 &pos, float size, const dm::Vec3 &rgb, float opacity, float kind, float ring)
    {
        dm::Vec3 right{view.at(0, 0), view.at(0, 1), view.at(0, 2)};
        dm::Vec3 up{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
        float h = size * 0.5f;
        dm::Vec3 c[4] = {pos - right * h - up * h, pos + right * h - up * h, pos + right * h + up * h,
                         // sky.txt may contain "horizon=r g b", used to blend fog into the horizon
                         pos - right * h + up * h};
        float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        int order[6] = {0, 1, 2, 0, 2, 3};
        // bounds-checked sequential read from the file buffer
        for (int k : order)
            glowVerts.insert(glowVerts.end(), {c[k].x, c[k].y, c[k].z, uv[k][0], uv[k][1], rgb.x, rgb.y, rgb.z,
                                               opacity, kind, ring});
    }

    // Draws nitro flames as three nested shells per exhaust plus halo sprites.
    // header: magic, version (1), vertex / index / submesh counts, then the AABB (min, max)
    // Uses premultiplied alpha, no depth writes; restores the standard blend func.
    void flamePass()
    {
        if (flames.empty() || !quality.flames) return;
        if (!flameVao) buildFlameMesh();
        // premultiplied colour; alpha channel (blur mask) is reduced where the
        // flame covers, so flames on the player's car stay crisp
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        // Loads GL, shaders and fixed resources (white texture, sky cube, dynamic
        // buffers, HUD textures). Needs a current 3.3 core context. Env overrides are
        // DRIFTO_MSAA, DRIFTO_SHADOWS, DRIFTO_SHADOW_SIZE, DRIFTO_POSTFX, DRIFTO_BLUR_*.
        flame.use();
        // submesh table: index range, RGBA, emission, flags and a 64-byte texture name
        flame.mat4("uViewProj", viewProj);
        flame.vec3("uCamPos", cam.position);
        glBindVertexArray(flameVao);
        struct Layer
        {
            float r, l;             // radius / length factors
            dm::Vec3 base, tip;
            float opacity, gain, threshold, diamonds;
        };
        // outer orange -> purple, mid electric blue, white-blue core
        static const Layer layers[3] = {
            {1.00f, 1.00f, {1.00f, 0.55f, 0.15f}, {0.80f, 0.20f, 0.85f}, 0.75f, 1.0f, 0.10f, 0.0f},
            {0.72f, 0.78f, {0.35f, 0.65f, 1.00f}, {0.45f, 0.35f, 1.00f}, 0.55f, 1.1f, 0.22f, 0.0f},
            {0.42f, 0.52f, {1.00f, 1.00f, 1.00f}, {0.70f, 0.90f, 1.00f}, 0.35f, 1.3f, 0.30f, 0.35f},
        };
        for (const FlameDraw &f : flames)
        {
            float I = dm::clampf(f.intensity, 0.0f, 1.0f);
            float ign = dm::clampf(f.ignition, 0.0f, 1.0f);
            if (I <= 0.0f && ign <= 0.0f) continue;
            // file vertices are 11 floats; buildVertexBuffer expands them to the runtime layout (adds smoothed normals)
            float len = (0.55f + 1.35f * I) * (1.0f + 0.6f * ign);
            float rad = (0.12f + 0.08f * I) * (1.0f + 0.45f * ign);
            flame.f1("uTime", f.time);
            flame.f1("uSeed", f.seed);
            for (const Layer &L : layers)
            {
                // unit shell scaled to radius x radius x length, placed by the exhaust frame
                // Outline / shadow passes draw an index-buffer prefix. Extend it submesh by
                // submesh and stop at the first cutout / translucent one (and, for outlines,
                // at no-outline or double-sided ones).
                dm::Mat4 m = f.frame * dm::scale({rad * L.r, rad * L.r, len * L.l});
                flame.mat4("uModel", m);
                flame.vec3("uColorBase", L.base);
                flame.vec3("uColorTip", L.tip);
                flame.f1("uOpacity", L.opacity);
                flame.f1("uGain", L.gain * (0.75f + 0.25f * std::max(I, ign)));
                flame.f1("uThreshold", L.threshold);
                flame.f1("uDiamonds", L.diamonds);
                glDrawElements(GL_TRIANGLES, flameIndexCount, GL_UNSIGNED_INT, nullptr);
            }
        }
        // halos + ignition burst sprites
        glowVerts.clear();
        for (const FlameDraw &f : flames)
        {
            float I = dm::clampf(f.intensity, 0.0f, 1.0f);
            float ign = dm::clampf(f.ignition, 0.0f, 1.0f);
            if (I <= 0.0f && ign <= 0.0f) continue;
            dm::Vec3 pipe{f.frame.at(0, 3), f.frame.at(1, 3), f.frame.at(2, 3)};
            dm::Vec3 dir{f.frame.at(0, 2), f.frame.at(1, 2), f.frame.at(2, 2)};
            dm::Vec3 toCam = dm::normalize(cam.position - pipe);
            // pull the sprites towards the camera so the bumper doesn't clip them
            dm::Vec3 p = pipe + dir * 0.12f + toCam * 0.35f;
            float flick = 0.9f + 0.1f * std::sin(f.time * 53.0f + f.seed);
            pushGlow(p, (0.8f + 0.9f * I + 1.6f * ign) * flick, {0.45f, 0.35f, 0.95f}, 0.15f, 0, 0);
            pushGlow(p, (0.35f + 0.4f * I + 0.7f * ign) * flick, {0.75f, 0.9f, 1.0f}, 0.25f, 0, 0);
            if (ign > 0.0f)
            {
                float prog = 1.0f - ign; // 0 at ignition -> 1
                pushGlow(p + dir * (0.3f * prog), 0.5f + 2.2f * prog, dm::Vec3{1.0f, 0.75f, 0.45f} * ign, 0.2f * ign,
                         1, 0.8f);
                pushGlow(p, 1.2f * ign, dm::Vec3{1.0f, 1.0f, 1.0f} * ign, 0.3f * ign, 0, 0);
            }
        }
        if (!glowVerts.empty())
        {
            // Loads six PNG faces into a mipmapped cubemap (replacing any previous sky) and reads optional sky.txt.
            glDisable(GL_CULL_FACE);
            glow.use();
            glow.mat4("uViewProj", viewProj);
            glBindVertexArray(glowVao);
            glBindBuffer(GL_ARRAY_BUFFER, glowVbo);
            glBufferData(GL_ARRAY_BUFFER, glowVerts.size() * sizeof(float), glowVerts.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(glowVerts.size() / 11));
        }
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    // ------------------------------------------------------------------
    bool loadModelFile(const std::string &name, GpuModel &out);
    bool loadSkybox(const std::string &name);
};

// =============================================================================
// face files are px nx py ny pz nz, matching GL_TEXTURE_CUBE_MAP_POSITIVE_X + i
// .dmesh loading (format: see tools/convert_drifto_assets.py)
// =============================================================================

// Reads assets/models/<name>.dmesh: header, submesh table, 11-float vertices, indices;
// then uploads to the GPU and parses an optional sidecar .txt. False on any parse error.
bool Renderer::Impl::loadModelFile(const std::string &name, GpuModel &out)
{
    // attribute-less VAO for fullscreen passes (post.vert derives the triangle from gl_VertexID)
    std::string path = assetDir + "/models/" + name + ".dmesh";
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        std::fprintf(stderr, "[gfx] model '%s' not found (%s) -- did you run `make assets`?\n", name.c_str(),
                     path.c_str());
        return false;
    // sky.txt may contain "horizon=r g b", used to blend fog into the horizon
    }
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    // bounds-checked sequential read from the file buffer
    auto take = [&](void *dst, size_t n) {
        // Releases every GL object owned by the renderer. Safe to call twice.
        if (pos + n > data.size()) return false;
        std::memcpy(dst, data.data() + pos, n);
        pos += n;
        return true;
    };
    // header: magic, version (1), vertex / index / submesh counts, then the AABB (min, max)
    char magic[4];
    uint32_t hdr[4];
    float bb[6];
    if (!take(magic, 4) || std::memcmp(magic, "DMSH", 4) != 0 || !take(hdr, sizeof hdr) || !take(bb, sizeof bb) ||
        hdr[0] != 1)
    {
        std::fprintf(stderr, "[gfx] %s: bad .dmesh header\n", path.c_str());
        return false;
    }
    uint32_t nv = hdr[1], ni = hdr[2], ns = hdr[3];
    out.name = name;
    // Loads GL, shaders and fixed resources (white texture, sky cube, dynamic
    // buffers, HUD textures). Needs a current 3.3 core context. Env overrides are
    // DRIFTO_MSAA, DRIFTO_SHADOWS, DRIFTO_SHADOW_SIZE, DRIFTO_POSTFX, DRIFTO_BLUR_*.
    out.bounds = Bounds{{bb[0], bb[1], bb[2]}, {bb[3], bb[4], bb[5]}};
    // submesh table: index range, RGBA, emission, flags and a 64-byte texture name
    for (uint32_t s = 0; s < ns; ++s)
    {
        SubMesh sm;
        uint32_t fc[2];
        float col[4], em[3];
        uint32_t flags;
        char tex[65] = {0};
        if (!take(fc, 8) || !take(col, 16) || !take(em, 12) || !take(&flags, 4) || !take(tex, 64))
        {
            std::fprintf(stderr, "[gfx] %s: truncated submesh table\n", path.c_str());
            return false;
        }
        sm.first = fc[0];
        sm.count = fc[1];
        std::memcpy(sm.color, col, sizeof col);
        sm.emission = {em[0], em[1], em[2]};
        sm.flags = flags;
        sm.tex = texture(tex);
        // Creates a mesh from CPU data; returns 0 if not initialised or empty.
        out.subs.push_back(sm);
    }
    // file vertices are 11 floats; buildVertexBuffer expands them to the runtime layout (adds smoothed normals)
    std::vector<float> src(nv * 11);
    std::vector<uint32_t> idx(ni);
    if (!take(src.data(), src.size() * 4) || !take(idx.data(), idx.size() * 4))
    {
        std::fprintf(stderr, "[gfx] %s: truncated vertex/index data\n", path.c_str());
        // Re-uploads a mesh; generates sequential triangle indices when none are supplied.
        return false;
    }
    // Outline / shadow passes draw an index-buffer prefix. Extend it submesh by
    // submesh and stop at the first cutout / translucent one (and, for outlines,
    // at no-outline or double-sided ones).
    bool outlinePrefix = true, shadowPrefix = true;
    for (const SubMesh &sm : out.subs)
    {
        bool cutout = (sm.flags & kFlagAlphaTest) || sm.color[3] < 0.999f;
        outlinePrefix = outlinePrefix && !cutout && !(sm.flags & (kFlagNoOutline | kFlagDoubleSided));
        shadowPrefix = shadowPrefix && !cutout;
        // Frees a user mesh; meshes owned by a loaded model are left alone.
        if (outlinePrefix) out.outlineCount = sm.first + sm.count;
        if (shadowPrefix) out.shadowCount = sm.first + sm.count;
    }
    std::vector<float> verts = buildVertexBuffer(src, nv, idx);
    MeshHandle h = allocMesh();
    upload(meshes[h - 1], verts, idx);
    out.mesh = h;

    // optional sidecar: <name>.txt (or the car's .txt for "<car>_body"/"<car>_wheel")
    std::string base = name;
    // Loads (or returns cached) model by name; failures are cached as handle 0.
    for (const char *suf : {"_body", "_wheel"})
    {
        size_t l = std::strlen(suf);
        if (base.size() > l && base.compare(base.size() - l, l, suf) == 0) base = base.substr(0, base.size() - l);
    }
    std::ifstream info(assetDir + "/models/" + base + ".txt");
    std::string line;
    while (std::getline(info, line))
    {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) out.info[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return true;
}

// =============================================================================
// Skybox
// =============================================================================

// Loads six PNG faces into a mipmapped cubemap (replacing any previous sky) and reads optional sky.txt.
bool Renderer::Impl::loadSkybox(const std::string &name)
{
    static const char *faces[6] = {"px", "nx", "py", "ny", "pz", "nz"};
    std::string dir = assetDir + "/skybox/" + name + "/";
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    for (int i = 0; i < 6; ++i)
    {
        Image img;
        if (!loadImage(dir + faces[i] + ".png", img, false))
        {
            std::fprintf(stderr, "[gfx] skybox '%s': cannot load %s.png\n", name.c_str(), faces[i]);
            glDeleteTextures(1, &tex);
            return false;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // face files are px nx py ny pz nz, matching GL_TEXTURE_CUBE_MAP_POSITIVE_X + i
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA8, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     img.pixels.data());
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // attribute-less VAO for fullscreen passes (post.vert derives the triangle from gl_VertexID)
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    if (skyTex) glDeleteTextures(1, &skyTex);
    skyTex = tex;
    skyName = name;
    hasSkyHorizon = false;
    // sky.txt may contain "horizon=r g b", used to blend fog into the horizon
    std::ifstream f(dir + "sky.txt");
    std::string line;
    while (std::getline(f, line))
    {
        float r, g, b;
        // Releases every GL object owned by the renderer. Safe to call twice.
        if (std::sscanf(line.c_str(), "horizon=%f %f %f", &r, &g, &b) == 3)
        {
            skyHorizon = {r, g, b};
            hasSkyHorizon = true;
        }
    }
    if (!hasSkyHorizon) skyHorizon = Lighting{}.fogColor;
    // Starts a frame: stores camera and lighting, clears the per-frame lists and
    // builds view / projection / light matrices and both culling frusta.
    return true;
}

// =============================================================================
// Renderer
// =============================================================================

Renderer::Renderer() : impl_(new Impl) {}
Renderer::~Renderer() { shutdown(); }

// Loads GL, shaders and fixed resources (white texture, sky cube, dynamic
// buffers, HUD textures). Needs a current 3.3 core context. Env overrides are
// DRIFTO_MSAA, DRIFTO_SHADOWS, DRIFTO_SHADOW_SIZE, DRIFTO_POSTFX, DRIFTO_BLUR_*.
bool Renderer::init(GLFWwindow *window, const std::string &assetDir)
{
    Impl &I = *impl_;
    if (I.ready) return true;
    if (!window)
    {
        std::fprintf(stderr, "[gfx] init: null window\n");
        return false;
    }
    I.window = window;
    // relative to the cwd, else next to the executable (Windows: double-click)
    // Queues a raw mesh. Opaque meshes also get a shadow/outline Instance; alpha < 1 goes to the transparent list.
    I.assetDir = platform::resolveRuntimePath(assetDir.empty() ? "assets" : assetDir);
    glfwMakeContextCurrent(window);
    if (!loadGL())
    {
        std::fprintf(stderr, "[gfx] failed to load OpenGL functions (need a 3.3 core context)\n");
        return false;
    }
    std::fprintf(stderr, "[gfx] GL %s | %s\n", (const char *)glGetString(GL_VERSION),
                 (const char *)glGetString(GL_RENDERER));
    // Creates a mesh from CPU data; returns 0 if not initialised or empty.
    I.shaderDir = findShaderDir();
    if (I.shaderDir.empty())
    {
        std::fprintf(stderr, "[gfx] cannot find gfx/shaders (run from the repo root or set DRIFTO_SHADER_DIR)\n");
        return false;
    }
    if (!I.loadPrograms()) return false;
    // Software rasterisers (llvmpipe under WSLg, softpipe, swrast) can't
    // Re-uploads a mesh; generates sequential triangle indices when none are supplied.
    // afford 4x MSAA + a 2k shadow map at 60 fps; scale down by default.
    {
        const char *rend = (const char *)glGetString(GL_RENDERER);
        std::string r = rend ? rend : "";
        // Queues a model: one Instance (shadow/outline, whole model) if opaque, plus one DrawItem per submesh.
        bool software = r.find("llvmpipe") != std::string::npos || r.find("softpipe") != std::string::npos ||
                        r.find("SWR") != std::string::npos || r.find("Software") != std::string::npos;
        if (software)
        {
            I.samples = 0;
            I.shadowSize = 1024;
            I.quality.halfResBlur = true;
            // Frees a user mesh; meshes owned by a loaded model are left alone.
            I.quality.blurSamples = 6;
            std::fprintf(stderr, "[gfx] software renderer: MSAA off, 1024 shadow map "
                                 "(override with DRIFTO_MSAA / DRIFTO_SHADOW_SIZE)\n");
        }
    }
    if (const char *s = std::getenv("DRIFTO_MSAA")) I.samples = std::max(0, std::atoi(s));
    if (const char *s = std::getenv("DRIFTO_SHADOWS")) I.shadowsEnabled = std::atoi(s) != 0;
    if (const char *s = std::getenv("DRIFTO_SHADOW_SIZE")) I.shadowSize = std::max(256, std::atoi(s));

    // 1x1 white texture so untextured draws can always sample unit 0
    // Loads (or returns cached) model by name; failures are cached as handle 0.
    glGenTextures(1, &I.whiteTex);
    glBindTexture(GL_TEXTURE_2D, I.whiteTex);
    const unsigned char white[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // skybox cube
    static const float cube[] = {
        -1, 1, -1, -1, -1, -1, 1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, -1, -1, -1, -1, 1, -1,
        // Queues a camera-facing smoke sprite (world position, size in world units).
        -1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1, 1, -1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, 1, -1, -1,
        -1, -1, 1, -1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, 1, -1, -1, 1, -1, 1, -1, 1, 1, -1, 1, 1, 1,
        1, 1, 1, -1, 1, 1, -1, 1, -1, -1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, -1, 1, 1, -1, 1};
    glGenVertexArrays(1, &I.skyVao);
    glGenBuffers(1, &I.skyVbo);
    glBindVertexArray(I.skyVao);
    // Queues text via stb_easy_font; x, y in pixels, scale multiplies the 1x glyph size.
    glBindBuffer(GL_ARRAY_BUFFER, I.skyVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof cube, cube, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, nullptr);

    // billboards: pos3 uv2 rgba4
    glGenVertexArrays(1, &I.bbVao);
    glGenBuffers(1, &I.bbVbo);
    glBindVertexArray(I.bbVao);
    glBindBuffer(GL_ARRAY_BUFFER, I.bbVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 36, (const void *)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 36, (const void *)20);

    // Queues a solid rectangle in pixel coordinates.
    // overlay: xy rgba uv
    glGenVertexArrays(1, &I.ovVao);
    glGenBuffers(1, &I.ovVbo);
    glBindVertexArray(I.ovVao);
    glBindBuffer(GL_ARRAY_BUFFER, I.ovVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, nullptr);
    glEnableVertexAttribArray(1);
    // Pixel width of text at the given scale.
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 32, (const void *)8);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (const void *)24);
    glBindVertexArray(0);

    // attribute-less VAO for fullscreen passes (post.vert derives the triangle from gl_VertexID)
    // Loads a HUD image (path as given, else under the asset dir); results are cached, including failures.
    glGenVertexArrays(1, &I.emptyVao);
    if (const char *s = std::getenv("DRIFTO_POSTFX")) I.quality.postFx = std::atoi(s) != 0;
    if (const char *s = std::getenv("DRIFTO_BLUR_SAMPLES")) I.quality.blurSamples = std::max(2, std::atoi(s));
    if (const char *s = std::getenv("DRIFTO_BLUR_HALFRES")) I.quality.halfResBlur = std::atoi(s) != 0;
    I.createBuiltinHudTextures();
    I.smokeTex = I.texture("smoke.png");
    if (!I.smokeTex) std::fprintf(stderr, "[gfx] (no smoke.png; billboards use a procedural puff)\n");

    I.checkGL("init");
    I.ready = true;
    return true;
}

// Releases every GL object owned by the renderer. Safe to call twice.
void Renderer::shutdown()
{
    if (!impl_ || !impl_->ready) return;
    Impl &I = *impl_;
    if (I.window) glfwMakeContextCurrent(I.window);
    for (GpuMesh &m : I.meshes) I.freeGpu(m);
    I.meshes.clear();
    // Starts a frame: stores camera and lighting, clears the per-frame lists and
    // builds view / projection / light matrices and both culling frusta.
    I.freeMeshes.clear();
    I.models.clear();
    I.modelByName.clear();
    // Queues a textured quad in pixels, rotated by p.rotation (radians) about a pivot given as fractions of w, h.
    for (auto &t : I.textures)
        if (t.second) glDeleteTextures(1, &t.second);
    I.textures.clear();
    for (auto &t : I.hudTextures)
        if (t.id) glDeleteTextures(1, &t.id);
    // pivot in pixels; corners are rotated around it
    I.hudTextures.clear();
    I.hudTexByPath.clear();
    GLuint texs[4] = {I.whiteTex, I.skyTex, I.shadowTex, 0};
    for (GLuint t : texs)
        if (t) glDeleteTextures(1, &t);
    I.whiteTex = I.skyTex = I.shadowTex = I.smokeTex = 0;
    if (I.shadowFbo) glDeleteFramebuffers(1, &I.shadowFbo);
    I.shadowFbo = 0;
    I.destroyTargets();
    GLuint vaos[6] = {I.skyVao, I.bbVao, I.ovVao, I.emptyVao, I.flameVao, I.glowVao};
    GLuint vbos[6] = {I.skyVbo, I.bbVbo, I.ovVbo, I.flameVbo, I.flameIbo, I.glowVbo};
    for (GLuint v : vaos)
        // untextured sprites sample one constant texel
        if (v) glDeleteVertexArrays(1, &v);
    for (GLuint b : vbos)
        if (b) glDeleteBuffers(1, &b);
    I.skyVao = I.bbVao = I.ovVao = I.skyVbo = I.bbVbo = I.ovVbo = 0;
    // Queues an anti-aliased line in pixels: a solid core plus a 1 px border fading to alpha 0.
    // Queues a raw mesh. Opaque meshes also get a shadow/outline Instance; alpha < 1 goes to the transparent list.
    I.emptyVao = I.flameVao = I.glowVao = I.flameVbo = I.flameIbo = I.glowVbo = 0;
    for (Program *p : {&I.toon, &I.outline, &I.shadow, &I.sky, &I.billboard, &I.overlay, &I.blur, &I.composite,
                       &I.flame, &I.glow})
        p->destroy();
    I.ready = false;
}

// --- resources ---------------------------------------------------------------

// Creates a mesh from CPU data; returns 0 if not initialised or empty.
MeshHandle Renderer::createMesh(const MeshData &mesh)
{
    if (!impl_->ready || mesh.positions.empty()) return 0;
    MeshHandle h = impl_->allocMesh();
    updateMesh(h, mesh);
    return h;
}

// Re-uploads a mesh; generates sequential triangle indices when none are supplied.
void Renderer::updateMesh(MeshHandle handle, const MeshData &mesh)
{
    Impl &I = *impl_;
    if (!I.ready || handle == 0 || handle > I.meshes.size()) return;
    // Queues a model: one Instance (shadow/outline, whole model) if opaque, plus one DrawItem per submesh.
    std::vector<uint32_t> idx = mesh.indices;
    if (idx.empty())
        for (uint32_t i = 0; i + 2 < mesh.positions.size(); i += 3) idx.insert(idx.end(), {i, i + 1, i + 2});
    std::vector<float> verts = buildVertexBuffer(mesh, idx);
    I.upload(I.meshes[handle - 1], verts, idx);
}

// Frees a user mesh; meshes owned by a loaded model are left alone.
void Renderer::destroyMesh(MeshHandle handle)
{
    Impl &I = *impl_;
    if (!I.mesh(handle)) return;
    for (const GpuModel &m : I.models)
        if (m.mesh == handle) return; // owned by a model
    I.freeGpu(I.meshes[handle - 1]);
    I.freeMeshes.push_back(handle);
}

// Loads (or returns cached) model by name; failures are cached as handle 0.
// Queues an anti-aliased ring segment. Angles are radians, 0 = up, increasing clockwise;
// radii in pixels.
ModelHandle Renderer::loadModel(const std::string &name)
{
    Impl &I = *impl_;
    if (!I.ready) return 0;
    auto it = I.modelByName.find(name);
    if (it != I.modelByName.end()) return it->second;
    GpuModel m;
    // roughly one segment per 4 px of arc length, clamped to [4, 256]
    ModelHandle h = 0;
    if (I.loadModelFile(name, m))
    {
        // Queues a camera-facing smoke sprite (world position, size in world units).
        I.models.push_back(std::move(m));
        h = (ModelHandle)I.models.size();
    }
    I.modelByName[name] = h; // cache failures too (no log spam)
    return h;
}
// x = sin, y = -cos: 0 rad points up on a y-down screen
// Queues text via stb_easy_font; x, y in pixels, scale multiplies the 1x glyph size.

Bounds Renderer::modelBounds(ModelHandle handle) const
{
    if (handle == 0 || handle > impl_->models.size()) return Bounds{};
    return impl_->models[handle - 1].bounds;
}

std::vector<std::string> Renderer::availableModels() const
{
    return listDir(impl_->assetDir + "/models", ".dmesh", false);
}

bool Renderer::modelPoint(ModelHandle handle, const std::string &key, dm::Vec3 &out) const
{
    if (handle == 0 || handle > impl_->models.size()) return false;
    // Queues one flame at a car-local exhaust position and direction; intensity and
    // ignition are 0..1, time in seconds.
    const auto &info = impl_->models[handle - 1].info;
    auto it = info.find(key);
    // Queues a solid rectangle in pixel coordinates.
    if (it == info.end()) return false;
    float x, y, z;
    if (std::sscanf(it->second.c_str(), "%f %f %f", &x, &y, &z) != 3) return false;
    out = {x, y, z};
    return true;
// orthonormal exhaust frame: columns are x, y, z (flame axis) and the pipe position
}

float Renderer::modelValue(ModelHandle handle, const std::string &key, float fallback) const
// Pixel width of text at the given scale.
{
    if (handle == 0 || handle > impl_->models.size()) return fallback;
    const auto &info = impl_->models[handle - 1].info;
    auto it = info.find(key);
    if (it == info.end()) return fallback;
    return std::strtof(it->second.c_str(), nullptr);
// Loads a HUD image (path as given, else under the asset dir); results are cached, including failures.
// position-derived seed so multiple flames flicker out of phase
}

bool Renderer::setSkybox(const std::string &name)
{
    // Queues a flame per exhaust point listed in the model's sidecar, or a fallback pipe at the rear of the bounds.
    if (!impl_->ready) return false;
    if (name == impl_->skyName && impl_->skyTex) return true;
    return impl_->loadSkybox(name);
}

std::vector<std::string> Renderer::availableSkyboxes() const
{
    return listDir(impl_->assetDir + "/skybox", "", true);
}

dm::Vec3 Renderer::skyboxHorizonColor() const { return impl_->skyHorizon; }

void Renderer::setShadowsEnabled(bool enabled) { impl_->shadowsEnabled = enabled; }
void Renderer::setOutlineWidth(float pixels) { impl_->outlineWidth = std::max(0.0f, pixels); }

// --- per frame -----------------------------------------------------------------

// Starts a frame: stores camera and lighting, clears the per-frame lists and
// builds view / projection / light matrices and both culling frusta.
void Renderer::beginFrame(const Camera &camera, const Lighting &lighting)
{
    Impl &I = *impl_;
    // Queues a textured quad in pixels, rotated by p.rotation (radians) about a pivot given as fractions of w, h.
    // Stores per-frame screen effect parameters (speed, nitro, blur...).
    I.inFrame = true;
    I.cam = camera;
    I.light = lighting;
    I.opaque.clear();
    I.transparent.clear();
    // pivot in pixels; corners are rotated around it
    I.instances.clear();
    I.flames.clear();
    I.billboards.clear();
    I.overlayVerts.clear();
    I.overlayCmds.clear();
    if (!I.ready) return;
    I.ensureTargets();
    float aspect = I.fbH > 0 ? (float)I.fbW / (float)I.fbH : 1.0f;
    // Executes the frame: renders all passes in the order given in the file header and
    // presents. Records timings (glFinish between passes) only when DRIFTO_PROFILE is set.
    I.view = dm::lookAt(camera.position, camera.target, camera.up);
    I.proj = dm::perspective(camera.fovY, aspect, camera.zNear, camera.zFar);
    I.viewProj = I.proj * I.view;
    I.computeLightVP();
    // untextured sprites sample one constant texel
    I.viewFrustum.from(I.viewProj);
    I.lightFrustum.from(I.lightVP);
}

// Queues an anti-aliased line in pixels: a solid core plus a 1 px border fading to alpha 0.
// Queues a raw mesh. Opaque meshes also get a shadow/outline Instance; alpha < 1 goes to the transparent list.
void Renderer::drawMesh(MeshHandle handle, const dm::Mat4 &model, const DrawParams &params)
{
    Impl &I = *impl_;
    if (!I.inFrame || !I.mesh(handle)) return;
    DrawItem it;
    it.mesh = handle;
    it.model = model;
    it.params = params;
    I.setBoundsSphere(it, I.mesh(handle)->bounds);
    (params.alpha < 0.999f ? I.transparent : I.opaque).push_back(it);
    if (params.alpha >= 0.999f)
    {
        Instance in;
        in.mesh = handle;
        in.model = model;
        in.params = params;
        in.outlineCount = in.shadowCount = (uint32_t)I.mesh(handle)->indexCount;
        in.centre = it.centre;
        in.radius = it.radius;
        I.instances.push_back(in);
    // view-frustum cull once, then draw visible items
    }
}

// Queues a model: one Instance (shadow/outline, whole model) if opaque, plus one DrawItem per submesh.
void Renderer::drawModel(ModelHandle handle, const dm::Mat4 &model, const DrawParams &params)
{
    Impl &I = *impl_;
    if (!I.inFrame || handle == 0 || handle > I.models.size()) return;
    const GpuModel &m = I.models[handle - 1];
    if (params.alpha >= 0.999f)
    {
        Instance in;
        in.mesh = m.mesh;
        in.model = model;
        in.params = params;
        in.outlineCount = m.outlineCount;
        in.shadowCount = m.shadowCount;
        I.setBoundsSphere(in, m.bounds);
        I.instances.push_back(in);
    }
    // farthest first, using squared distance of the model origin
    for (const SubMesh &s : m.subs)
    {
        DrawItem it;
        // Queues an anti-aliased ring segment. Angles are radians, 0 = up, increasing clockwise;
        // radii in pixels.
        it.mesh = m.mesh;
        it.sub = &s;
        it.model = model;
        it.params = params;
        I.setBoundsSphere(it, m.bounds);
        bool transp = params.alpha * s.color[3] < 0.999f;
        (transp ? I.transparent : I.opaque).push_back(it);
    // roughly one segment per 4 px of arc length, clamped to [4, 256]
    }
}

// Queues a camera-facing smoke sprite (world position, size in world units).
void Renderer::drawBillboard(const dm::Vec3 &position, float size, const dm::Vec3 &color, float alpha)
{
    if (!impl_->inFrame || alpha <= 0.0f || size <= 0.0f) return;
    impl_->billboards.push_back({position, size, color, alpha, 0});
}

// x = sin, y = -cos: 0 rad points up on a y-down screen
// Queues text via stb_easy_font; x, y in pixels, scale multiplies the 1x glyph size.
void Renderer::drawText(float x, float y, float scale, const std::string &text, const dm::Vec3 &color)
{
    Impl &I = *impl_;
    if (!I.inFrame || text.empty()) return;
    // stb_easy_font emits quads: 4 verts of (x, y, z, rgba8) = 16 bytes each
    std::vector<char> buf(text.size() * 300 + 1024);
    std::string t = text;
    int quads = stb_easy_font_print(0, 0, &t[0], nullptr, buf.data(), (int)buf.size());
    for (int q = 0; q < quads; ++q)
    {
        const float *v0 = (const float *)(buf.data() + (q * 4 + 0) * 16);
        const float *v2 = (const float *)(buf.data() + (q * 4 + 2) * 16);
        I.pushQuad2D(x + v0[0] * scale, y + v0[1] * scale, x + v2[0] * scale, y + v2[1] * scale, color.x, color.y,
                     color.z, 1.0f);
    // depth is only needed by the motion blur pass
    }
// Queues one flame at a car-local exhaust position and direction; intensity and
// ignition are 0..1, time in seconds.
}

// Queues a solid rectangle in pixel coordinates.
void Renderer::drawRect(float x, float y, float w, float h, const dm::Vec3 &color, float alpha)
{
    // HUD goes on top of the post result (or the resolved image when post is off)
    if (!impl_->inFrame) return;
    impl_->pushQuad2D(x, y, x + w, y + h, color.x, color.y, color.z, alpha);
}
// orthonormal exhaust frame: columns are x, y, z (flame axis) and the pipe position

const std::string &Renderer::assetDir() const { return impl_->assetDir; }

// Pixel width of text at the given scale.
float Renderer::textWidth(const std::string &text, float scale) const
{
    std::string t = text;
    return t.empty() ? 0.0f : (float)stb_easy_font_width(&t[0]) * scale;
}

// Loads a HUD image (path as given, else under the asset dir); results are cached, including failures.
// position-derived seed so multiple flames flicker out of phase
TextureHandle Renderer::loadTexture(const std::string &path)
{
    Impl &I = *impl_;
    if (!I.ready || path.empty()) return 0;
    // Queues a flame per exhaust point listed in the model's sidecar, or a fallback pipe at the rear of the bounds.
    auto it = I.hudTexByPath.find(path);
    if (it != I.hudTexByPath.end()) return it->second;
    Image img;
    bool ok = loadImage(path, img, false);
    if (!ok && path[0] != '/') ok = loadImage(I.assetDir + "/" + path, img, false);
    if (!ok)
    {
        // Reads back the last presented image (RGBA) and writes it as a PNG, flipped to top-down.
        std::fprintf(stderr, "[gfx] cannot load HUD texture '%s'\n", path.c_str());
        I.hudTexByPath[path] = 0;
        return 0;
    }
    return I.addHudTexture(path, img.w, img.h, img.pixels.data(), true);
}

bool Renderer::textureSize(TextureHandle tex, int &w, int &h) const
{
    if (tex == 0 || tex > impl_->hudTextures.size()) return false;
    w = impl_->hudTextures[tex - 1].w;
    h = impl_->hudTextures[tex - 1].h;
    return true;
}

// Queues a textured quad in pixels, rotated by p.rotation (radians) about a pivot given as fractions of w, h.
// Stores per-frame screen effect parameters (speed, nitro, blur...).
void Renderer::drawSprite(TextureHandle tex, float x, float y, float w, float h, const SpriteParams &p)
{
    Impl &I = *impl_;
    if (!I.inFrame || p.alpha <= 0.0f) return;
    I.ovState(I.hudTexId(tex), p.additive);
    // pivot in pixels; corners are rotated around it
    float px0 = x + p.pivot.x * w, py0 = y + p.pivot.y * h;
    float c = std::cos(p.rotation), s = std::sin(p.rotation);
    float corners[4][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    float pts[4][2];
    for (int k = 0; k < 4; ++k)
    {
        float dx = corners[k][0] - px0, dy = corners[k][1] - py0;
        // clockwise on screen (y down)
        // Executes the frame: renders all passes in the order given in the file header and
        // presents. Records timings (glFinish between passes) only when DRIFTO_PROFILE is set.
        pts[k][0] = px0 + dx * c - dy * s;
        pts[k][1] = py0 + dx * s + dy * c;
    }
    float uv[4][2] = {{p.uv0.x, p.uv0.y}, {p.uv1.x, p.uv0.y}, {p.uv1.x, p.uv1.y}, {p.uv0.x, p.uv1.y}};
    // untextured sprites sample one constant texel
    if (!tex) for (auto &u : uv) u[0] = u[1] = 0.5f;
    I.pushQuadPts(pts, uv, p.tint.x, p.tint.y, p.tint.z, p.alpha);
}

// Queues an anti-aliased line in pixels: a solid core plus a 1 px border fading to alpha 0.
void Renderer::drawLine(float x0, float y0, float x1, float y1, float thickness, const dm::Vec3 &color, float alpha,
                        bool additive)
{
    Impl &I = *impl_;
    if (!I.inFrame || alpha <= 0.0f) return;
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    dx /= len;
    dy /= len;
    float nx = -dy, ny = dx;
    // core quad + 1 px feathered border on every side (no MSAA in the overlay)
    float hw = std::max(0.25f, thickness * 0.5f - 0.5f), f = 1.0f;
    float a = alpha * std::min(1.0f, thickness); // sub-pixel lines fade out
    I.ovState(0, additive);
    auto P = [&](float along, float side, float out[2]) {
        out[0] = x0 + dx * along + nx * side;
        out[1] = y0 + dy * along + ny * side;
    };
    float L0 = -0.0f, L1 = len;
    float inner[4][2], outer[4][2];
    // view-frustum cull once, then draw visible items
    P(L0, -hw, inner[0]);
    P(L1, -hw, inner[1]);
    P(L1, hw, inner[2]);
    P(L0, hw, inner[3]);
    P(L0 - f, -hw - f, outer[0]);
    P(L1 + f, -hw - f, outer[1]);
    P(L1 + f, hw + f, outer[2]);
    P(L0 - f, hw + f, outer[3]);
    float uv[4][2] = {{0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}};
    I.pushQuadPts(inner, uv, color.x, color.y, color.z, a);
    for (int e = 0; e < 4; ++e)
    {
        int n = (e + 1) % 4;
        float r = color.x, g = color.y, b = color.z;
        I.ov(inner[e][0], inner[e][1], 0.5f, 0.5f, r, g, b, a);
        I.ov(outer[e][0], outer[e][1], 0.5f, 0.5f, r, g, b, 0);
        I.ov(outer[n][0], outer[n][1], 0.5f, 0.5f, r, g, b, 0);
        I.ov(inner[e][0], inner[e][1], 0.5f, 0.5f, r, g, b, a);
        I.ov(outer[n][0], outer[n][1], 0.5f, 0.5f, r, g, b, 0);
        I.ov(inner[n][0], inner[n][1], 0.5f, 0.5f, r, g, b, a);
    // farthest first, using squared distance of the model origin
    }
}

// Queues an anti-aliased ring segment. Angles are radians, 0 = up, increasing clockwise;
// radii in pixels.
void Renderer::drawArc(float cx, float cy, float rInner, float rOuter, float angle0, float angle1,
                       const dm::Vec3 &color, float alpha, bool additive)
{
    Impl &I = *impl_;
    if (!I.inFrame || alpha <= 0.0f || rOuter <= rInner) return;
    if (angle1 < angle0) std::swap(angle0, angle1);
    float span = std::min(angle1 - angle0, 2.0f * dm::kPi);
    // roughly one segment per 4 px of arc length, clamped to [4, 256]
    int segs = std::max(4, std::min(256, (int)std::ceil(span * rOuter / 4.0f)));
    I.ovState(0, additive);
    // radii: feathered inner edge (if any), core, feathered outer edge
    float rad[4] = {std::max(0.0f, rInner - 0.5f), rInner + 0.5f, rOuter - 0.5f, rOuter + 0.5f};
    float al[4] = {rInner > 0.0f ? 0.0f : alpha, alpha, alpha, 0.0f};
    if (rad[2] < rad[1]) rad[1] = rad[2] = 0.5f * (rInner + rOuter); // thin ring
    float r = color.x, g = color.y, b = color.z;
    for (int i = 0; i < segs; ++i)
    {
        float a0 = angle0 + span * i / segs, a1 = angle0 + span * (i + 1) / segs;
        // x = sin, y = -cos: 0 rad points up on a y-down screen
        float s0 = std::sin(a0), c0 = -std::cos(a0), s1 = std::sin(a1), c1 = -std::cos(a1);
        for (int k = 0; k < 3; ++k)
        {
            float ra = rad[k], rb = rad[k + 1];
            float A[2] = {cx + s0 * ra, cy + c0 * ra}, B[2] = {cx + s1 * ra, cy + c1 * ra};
            float C[2] = {cx + s1 * rb, cy + c1 * rb}, D[2] = {cx + s0 * rb, cy + c0 * rb};
            I.ov(A[0], A[1], 0.5f, 0.5f, r, g, b, al[k]);
            I.ov(B[0], B[1], 0.5f, 0.5f, r, g, b, al[k]);
            I.ov(C[0], C[1], 0.5f, 0.5f, r, g, b, al[k + 1]);
            I.ov(A[0], A[1], 0.5f, 0.5f, r, g, b, al[k]);
            I.ov(C[0], C[1], 0.5f, 0.5f, r, g, b, al[k + 1]);
            I.ov(D[0], D[1], 0.5f, 0.5f, r, g, b, al[k + 1]);
        }
    }
}
// depth is only needed by the motion blur pass

// Queues one flame at a car-local exhaust position and direction; intensity and
// ignition are 0..1, time in seconds.
void Renderer::drawNitroFlame(const dm::Mat4 &carModel, const dm::Vec3 &exhaustLocalPos, float intensity, float time,
                              float ignition, const dm::Vec3 &exhaustDirLocal)
{
    Impl &I = *impl_;
    if (!I.inFrame || (intensity <= 0.0f && ignition <= 0.0f)) return;
    // HUD goes on top of the post result (or the resolved image when post is off)
    dm::Vec3 p = dm::transformPoint(carModel, exhaustLocalPos);
    dm::Vec3 z = dm::normalize(dm::transformDir(carModel, exhaustDirLocal));
    if (dm::lengthSq(z) < 0.5f) z = {0, 0, 1};
    // orthonormal exhaust frame: columns are x, y, z (flame axis) and the pipe position
    dm::Vec3 up = std::fabs(z.y) > 0.95f ? dm::Vec3{1, 0, 0} : dm::Vec3{0, 1, 0};
    dm::Vec3 x = dm::normalize(dm::cross(up, z));
    dm::Vec3 y = dm::cross(z, x);
    dm::Mat4 f;
    for (int r = 0; r < 3; ++r)
    {
        f.at(r, 0) = x[r];
        f.at(r, 1) = y[r];
        f.at(r, 2) = z[r];
        f.at(r, 3) = p[r];
    }
    // position-derived seed so multiple flames flicker out of phase
    float seed = std::fmod(std::fabs(p.x * 12.9898f + p.z * 78.233f + exhaustLocalPos.x * 37.1f), 100.0f);
    I.flames.push_back({f, intensity, ignition, time, seed});
}

// Queues a flame per exhaust point listed in the model's sidecar, or a fallback pipe at the rear of the bounds.
void Renderer::drawCarNitro(ModelHandle car, const dm::Mat4 &carModel, float intensity, float time, float ignition)
{
    int n = (int)modelValue(car, "exhaust_count", 0.0f);
    bool any = false;
    for (int i = 0; i < n; ++i)
    {
        dm::Vec3 p;
        // Reads back the last presented image (RGBA) and writes it as a PNG, flipped to top-down.
        if (modelPoint(car, "exhaust_" + std::to_string(i), p))
        {
            drawNitroFlame(carModel, p, intensity, time, ignition);
            any = true;
        }
    }
    if (!any)
    {
        // no data: one centred pipe at the back of the bounds
        Bounds b = modelBounds(car);
        dm::Vec3 p{0.0f, b.min.y + 0.25f * (b.max.y - b.min.y), b.max.z - 0.05f};
        if (b.max.z <= b.min.z) p = {0.0f, 0.3f, 2.0f};
        drawNitroFlame(carModel, p, intensity, time, ignition);
    }
}

// Stores per-frame screen effect parameters (speed, nitro, blur...).
void Renderer::setFrameFx(const FrameFx &fx)
{
    impl_->fx = fx;
    impl_->fxSet = true;
}

void Renderer::setFxQuality(const FxQuality &quality)
{
    impl_->quality = quality;
    impl_->quality.blurSamples = std::max(2, std::min(32, quality.blurSamples));
}

FxQuality Renderer::fxQuality() const { return impl_->quality; }

// Executes the frame: renders all passes in the order given in the file header and
// presents. Records timings (glFinish between passes) only when DRIFTO_PROFILE is set.
void Renderer::endFrame()
{
    Impl &I = *impl_;
    if (!I.inFrame) return;
    I.inFrame = false;
    if (!I.ready || I.fbW <= 0 || I.fbH <= 0) return;

    static const bool profile = std::getenv("DRIFTO_PROFILE") != nullptr;
    double tp0 = 0, tp1 = 0, tp2 = 0, tp3 = 0, tp4 = 0, tp5 = 0, tp6 = 0;
    if (profile) { glFinish(); tp0 = glfwGetTime(); }
    // 1. shadow map
    if (I.shadowsEnabled) I.shadowPass();
    if (profile) { glFinish(); tp1 = glfwGetTime(); }

    // 2. scene
    glBindFramebuffer(GL_FRAMEBUFFER, I.sceneFbo);
    glViewport(0, 0, I.fbW, I.fbH);
    glDepthMask(GL_TRUE);
    glClearColor(I.light.fogColor.x, I.light.fogColor.y, I.light.fogColor.z, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glDisable(GL_BLEND);

    // opaque: group by mesh to limit state changes
    std::stable_sort(I.opaque.begin(), I.opaque.end(),
                     [](const DrawItem &a, const DrawItem &b) { return a.mesh < b.mesh; });
    I.setFrameUniformsToon(true);
    // view-frustum cull once, then draw visible items
    for (DrawItem &it : I.opaque) it.visible = I.viewFrustum.sphere(it.centre, it.radius);
    for (const DrawItem &it : I.opaque)
        if (it.visible) I.drawToon(it);

    if (profile) { glFinish(); tp2 = glfwGetTime(); }
    // outlines (inverted hull)
    if (I.outlineWidth > 0.0f) I.outlinePass();
    if (profile) { glFinish(); tp3 = glfwGetTime(); }

    // skybox at the far plane
    I.skyPass();

    // transparent geometry, back to front, no depth writes
    if (!I.transparent.empty())
    {
        for (DrawItem &it : I.transparent)
        {
            dm::Vec3 p{it.model.at(0, 3), it.model.at(1, 3), it.model.at(2, 3)};
            it.sortKey = dm::lengthSq(p - I.cam.position);
        }
        // farthest first, using squared distance of the model origin
        std::sort(I.transparent.begin(), I.transparent.end(),
                  [](const DrawItem &a, const DrawItem &b) { return a.sortKey > b.sortKey; });
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        I.setFrameUniformsToon(false);
        // per item: depth-only prepass, then colour with LEQUAL, so a
        // see-through car (ghost) shows only its outer shell
        for (const DrawItem &it : I.transparent)
        {
            if (!I.viewFrustum.sphere(it.centre, it.radius)) continue;
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDepthMask(GL_TRUE);
            I.drawToon(it);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_FALSE);
            glDepthFunc(GL_LEQUAL);
            I.drawToon(it);
            glDepthFunc(GL_LESS);
        }
        glDepthMask(GL_TRUE);
    }
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    I.billboardPass();
    if (profile) { glFinish(); tp4 = glfwGetTime(); }
    // nitro flames + glows (premultiplied; they clear the blur mask)
    I.flamePass();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    if (profile) { glFinish(); tp5 = glfwGetTime(); }

    // 3. resolve MSAA -> resolve textures; post -> final; overlay; present
    bool post = I.postActive();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, I.sceneFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, I.resolveFbo);
    glBlitFramebuffer(0, 0, I.fbW, I.fbH, 0, 0, I.fbW, I.fbH,
                      // depth is only needed by the motion blur pass
                      GL_COLOR_BUFFER_BIT | (I.blurActive() ? GL_DEPTH_BUFFER_BIT : 0), GL_NEAREST);
    GLuint target = I.resolveFbo;
    if (post)
    {
        I.postPass();
        target = I.finalFbo;
    }
    if (profile) { glFinish(); tp6 = glfwGetTime(); }
    // HUD goes on top of the post result (or the resolved image when post is off)
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, I.fbW, I.fbH);
    I.overlayPass();
    glBindVertexArray(0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, I.fbW, I.fbH, 0, 0, I.fbW, I.fbH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    I.presentFbo = target;
    I.haveFrame = true;
    I.prevViewProj = I.viewProj;
    I.havePrev = true;
    I.checkGL("endFrame");
    if (profile)
    {
        glFinish();
        double tp7 = glfwGetTime();
        std::fprintf(stderr,
                     "[gfx] shadow %.1f opaque %.1f outline %.1f sky+transp %.1f flames %.1f post %.1f "
                     "overlay+present %.1f ms (%zu opaque items)\n",
                     (tp1 - tp0) * 1e3, (tp2 - tp1) * 1e3, (tp3 - tp2) * 1e3, (tp4 - tp3) * 1e3,
                     (tp5 - tp4) * 1e3, (tp6 - tp5) * 1e3, (tp7 - tp6) * 1e3, I.opaque.size());
    }
}

int Renderer::framebufferWidth() const { return impl_->fbW; }
int Renderer::framebufferHeight() const { return impl_->fbH; }

// Reads back the last presented image (RGBA) and writes it as a PNG, flipped to top-down.
bool Renderer::saveScreenshot(const std::string &path)
{
    Impl &I = *impl_;
    if (!I.ready || !I.haveFrame || I.fbW <= 0) return false;
    std::vector<unsigned char> px((size_t)I.fbW * I.fbH * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, I.presentFbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, I.fbW, I.fbH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    I.checkGL("saveScreenshot");
    return writePngFlipped(path, I.fbW, I.fbH, px);
}

} // namespace gfx
