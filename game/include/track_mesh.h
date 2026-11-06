// track_mesh.h -- procedural geometry for track sections (the original's
// SketchUp/Blender road meshes are replaced by this), plus deterministic
// tree placements and small fallback meshes used when a model is missing.
//
// Produces gfx::MeshData in world space (draw with an identity model
// matrix). Uses only the *data structs* from renderer.h; nothing here calls
// the renderer, so it links into the headless tests.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/dmath.h"
#include "game/include/track.h"
#include "gfx/include/renderer.h"

namespace game {

struct TreePlacement
{
    std::string model;   // BrokenVector LowPolyTreePack name, e.g. "tree_type3_02"
    int fallbackVariant; // 0..2: which procedural tree to draw if the model is missing
    dm::Vec3 position;   // base of the trunk
    float yaw;
    float scale;
};

struct SectionGeometry
{
    uint32_t section = 0;
    gfx::MeshData road;    // asphalt, lines, kerbs, shoulders (smooth normals)
    gfx::MeshData terrain; // mountainside / drop-off strips (flat shaded)
    gfx::MeshData props;   // guardrails and posts (flat shaded)
    std::vector<TreePlacement> trees;
};

// All tree model names of the LowPolyTreePack as produced by GFX's asset
// converter: "tree_type<T>_<NN>", T 0..7 (6,5,5,5,5,4,4,4 variants).
const std::vector<std::string> &treeModelNames();

// Height of the mountainside strip relative to the road edge at lateral
// distance x (>= 0) beyond the shoulder, for uphillness u in [-1, 1].
float terrainOffset(float uphillness, float x);

// Deterministic for a given (track seed, section index).
SectionGeometry buildSectionGeometry(const Track &track, uint32_t index);

// Fallbacks. Car: local space, forward -Z, ground at y = -rideHeight,
// sized length x width (default ~AE86).
gfx::MeshData buildBoxCarMesh(const dm::Vec3 &bodyColor, float rideHeight, float length = 4.2f,
                              float width = 1.65f);
// Simple low-poly tree, base at the origin, ~6 m tall at scale 1.
gfx::MeshData buildFallbackTreeMesh(int variant);

} // namespace game
