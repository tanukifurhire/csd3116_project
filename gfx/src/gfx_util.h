// gfx_util.h -- internal helpers for renderer.cpp (shaders, images, vertex
// buffer building, filesystem). Not part of the public gfx API.
#pragma once

#include "../include/renderer.h"
#include "gl_loader.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace gfx {

// GLSL program loaded from <dir>/<name>.vert + <name>.frag. Compile/link
// errors are logged with the file name and the driver's info log.
class Program
{
public:
    bool load(const std::string &dir, const std::string &name);
    // <dir>/<vert>.vert + <dir>/<frag>.frag
    bool load(const std::string &dir, const std::string &vert, const std::string &frag);
    void destroy();
    void use() const;
    GLint loc(const char *name);
    void mat4(const char *n, const dm::Mat4 &m);
    void mat3(const char *n, const float m[9]);
    void vec2(const char *n, float x, float y);
    void vec3(const char *n, const dm::Vec3 &v);
    void vec4(const char *n, float x, float y, float z, float w);
    void f1(const char *n, float v);
    void i1(const char *n, int v);

private:
    GLuint id_ = 0;
    std::string name_;
    std::unordered_map<std::string, GLint> locs_;
};

struct Image
{
    int w = 0, h = 0;
    std::vector<unsigned char> pixels; // RGBA8
};

// flipY: true for regular textures (GL/Unity UV origin bottom-left), false
// for cube map faces (cube map convention wants the top row first).
bool loadImage(const std::string &path, Image &out, bool flipY);
// 2D texture with mipmaps; 0 on failure (logged).
GLuint loadTexture2D(const std::string &path, bool mipmaps);
// pixels: RGBA8 bottom row first (glReadPixels order).
bool writePngFlipped(const std::string &path, int w, int h, const std::vector<unsigned char> &pixels);

// Interleaved GPU vertices (pos3 nrm3 col3 uv2 smoothNrm3). The smoothed
// normal (averaged over coincident positions) drives the outline extrusion
// so hard edges don't crack the inverted hull.
std::vector<float> buildVertexBuffer(const std::vector<float> &src11, uint32_t vertexCount,
                                     const std::vector<uint32_t> &indices);
std::vector<float> buildVertexBuffer(const MeshData &mesh, const std::vector<uint32_t> &indices);

std::string findShaderDir();
// Sorted entry names in dir: files ending in ext (ext stripped) or, if
// dirsOnly, sub-directories.
std::vector<std::string> listDir(const std::string &dir, const std::string &ext, bool dirsOnly);

} // namespace gfx
