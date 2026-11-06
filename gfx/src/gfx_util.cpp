// gfx_util.cpp -- see gfx_util.h.
//
// Small GL/asset helpers shared by the renderer:
//   - Program: GLSL program wrapper (load/compile/link from .vert/.frag files,
//     with a per-program uniform-location cache and typed uniform setters).
//   - Image loading (stb_image) and 2D texture upload; PNG writing (stb_image_write).
//   - Vertex buffer construction: converts mesh data into an interleaved 14-float
//     layout (pos, normal, colour, uv, smoothed normal). The smoothed normal is
//     averaged over all vertices sharing a position, so it stays continuous
//     across hard edges where the shading normal is split.
//   - Filesystem helpers: shader directory discovery and directory listing.
// Design notes: stb is compiled as static, file-local implementations (below) to
// avoid duplicate symbols; uniform locations are cached to avoid per-frame
// glGetUniformLocation calls; shader objects are deleted right after linking.
#include "gfx_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <filesystem>
#include <system_error>

#include "../../common/platform.h"

// stb as file-local (static) implementations so nothing clashes if another
// module also compiles stb_image.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using namespace gfxgl;

namespace gfx {

// --- shaders -------------------------------------------------------------------

// Read a whole file as binary into out. Returns false if it cannot be opened.
static bool readFile(const std::string &path, std::string &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Compile one shader stage from the file at path. Returns the shader object, or 0
// on read/compile failure (object deleted). The info log is printed to stderr on
// errors and also when the driver emits warnings.
static GLuint compile(GLenum type, const std::string &path)
{
    std::string src;
    if (!readFile(path, src))
    {
        std::fprintf(stderr, "[gfx] cannot read shader %s\n", path.c_str());
        return 0;
    }
    GLuint s = glCreateShader(type);
    const char *p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0, len = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
    // Log if compilation failed or the driver produced a non-empty log (the reported
    // length includes the terminating NUL, hence > 1).
    if (!ok || len > 1)
    {
        std::string log(len > 0 ? len : 1, '\0');
        glGetShaderInfoLog(s, (GLsizei)log.size(), nullptr, &log[0]);
        std::fprintf(stderr, "[gfx] %s %s:\n%s\n", ok ? "warnings in" : "COMPILE ERROR in", path.c_str(), log.c_str());
    }
    if (!ok)
    {
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// Convenience overload: vertex and fragment shader share one basename.
bool Program::load(const std::string &dir, const std::string &name) { return load(dir, name, name); }

// Compile <dir>/<vert>.vert and <dir>/<frag>.frag and link them into id_. Returns
// false on any compile/link error (id_ stays 0). name_ is kept for diagnostics.
bool Program::load(const std::string &dir, const std::string &vert, const std::string &frag)
{
    const std::string &name = frag;
    name_ = vert == frag ? frag : vert + "+" + frag;
    GLuint vs = compile(GL_VERTEX_SHADER, dir + "/" + vert + ".vert");
    GLuint fs = compile(GL_FRAGMENT_SHADER, dir + "/" + frag + ".frag");
    if (!vs || !fs)
    {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    id_ = glCreateProgram();
    glAttachShader(id_, vs);
    glAttachShader(id_, fs);
    glLinkProgram(id_);
    // Stage objects are no longer needed once linked; flag them for deletion.
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(id_, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        GLint len = 0;
        glGetProgramiv(id_, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 0 ? len : 1, '\0');
        glGetProgramInfoLog(id_, (GLsizei)log.size(), nullptr, &log[0]);
        std::fprintf(stderr, "[gfx] LINK ERROR in program '%s':\n%s\n", name.c_str(), log.c_str());
        glDeleteProgram(id_);
        id_ = 0;
        return false;
    }
    return true;
}

// Delete the GL program (if any) and drop the cached uniform locations.
void Program::destroy()
{
    if (id_) glDeleteProgram(id_);
    id_ = 0;
    locs_.clear();
}

// Make this the current program. Uniform setters below require it to be bound.
void Program::use() const { glUseProgram(id_); }

// Uniform location with a per-program cache keyed by name. Unknown uniforms are
// cached as -1; GL silently ignores uniform calls with location -1.
GLint Program::loc(const char *n)
{
    auto it = locs_.find(n);
    if (it != locs_.end()) return it->second;
    GLint l = glGetUniformLocation(id_, n);
    locs_[n] = l;
    return l;
}

// Typed uniform setters (program must be bound). Matrices are uploaded without
// transpose, i.e. column-major as GL expects; mat3 takes 9 floats.
void Program::mat4(const char *n, const dm::Mat4 &m) { glUniformMatrix4fv(loc(n), 1, GL_FALSE, m.data()); }
void Program::mat3(const char *n, const float m[9]) { glUniformMatrix3fv(loc(n), 1, GL_FALSE, m); }
void Program::vec2(const char *n, float x, float y) { glUniform2f(loc(n), x, y); }
void Program::vec3(const char *n, const dm::Vec3 &v) { glUniform3f(loc(n), v.x, v.y, v.z); }
void Program::vec4(const char *n, float x, float y, float z, float w) { glUniform4f(loc(n), x, y, z, w); }
void Program::f1(const char *n, float v) { glUniform1f(loc(n), v); }
void Program::i1(const char *n, int v) { glUniform1i(loc(n), v); }

// --- images --------------------------------------------------------------------

// Decode a PNG/JPEG/TGA into RGBA8 (4 channels forced). flipY makes row 0 the
// bottom row (OpenGL texture origin). Returns false if decoding fails.
bool loadImage(const std::string &path, Image &out, bool flipY)
{
    stbi_set_flip_vertically_on_load(flipY ? 1 : 0);
    int w, h, n;
    unsigned char *data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data) return false;
    out.w = w;
    out.h = h;
    out.pixels.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return true;
}

// Load an image into a new GL_TEXTURE_2D (RGBA8, flipped for GL origin, linear
// mag, trilinear min when mipmaps, GL_REPEAT wrap). Sets GL_UNPACK_ALIGNMENT to 1
// and unbinds on return. Returns the texture name, or 0 on failure.
GLuint loadTexture2D(const std::string &path, bool mipmaps)
{
    Image img;
    if (!loadImage(path, img, true))
    {
        std::fprintf(stderr, "[gfx] cannot load texture %s (%s)\n", path.c_str(), stbi_failure_reason());
        return 0;
    }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

// Write RGBA8 pixels (bottom-up, as from glReadPixels) as an RGB PNG: rows are
// reversed to top-down and the alpha channel is dropped.
bool writePngFlipped(const std::string &path, int w, int h, const std::vector<unsigned char> &pixels)
{
    std::vector<unsigned char> flipped((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                flipped[((size_t)y * w + x) * 3 + c] = pixels[((size_t)(h - 1 - y) * w + x) * 4 + c];
    if (!stbi_write_png(path.c_str(), w, h, 3, flipped.data(), w * 3))
    {
        std::fprintf(stderr, "[gfx] failed to write %s\n", path.c_str());
        return false;
    }
    return true;
}

// --- vertex buffers ------------------------------------------------------------

// Vertex layout: 14 floats per vertex = position(3) normal(3) colour(3) uv(2)
// smoothedNormal(3).
namespace {
// Key for welding vertices by position: coordinates quantised to integers.
struct PosKey
{
    long long x, y, z;
    bool operator==(const PosKey &o) const { return x == o.x && y == o.y && z == o.z; }
};
// Spatial hash: XOR of coordinate * large prime per axis.
struct PosHash
{
    size_t operator()(const PosKey &k) const
    {
        return (size_t)(k.x * 73856093LL) ^ (size_t)(k.y * 19349663LL) ^ (size_t)(k.z * 83492791LL);
    }
};
// Quantise to 1e-4 units so vertices coinciding up to float noise share a key.
PosKey key(const float *p)
{
    const float q = 1e4f; // 0.1 mm
    return {std::llround(p[0] * q), std::llround(p[1] * q), std::llround(p[2] * q)};
}

// fills slots 11..13 of each 14-float vertex with the smoothed normal
// Sum the face normals of all vertices at the same position (this merges the
// duplicates created for hard edges), then store the normalised sum in each
// vertex. Falls back to the vertex's own normal if the sum degenerates.
void smoothNormals(std::vector<float> &v, size_t count)
{
    // pass 1: accumulate normals per quantised position
    std::unordered_map<PosKey, dm::Vec3, PosHash> acc;
    acc.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        const float *p = &v[i * 14];
        acc[key(p)] += dm::Vec3{p[3], p[4], p[5]};
    }
    // pass 2: write back the normalised sum
    for (size_t i = 0; i < count; ++i)
    {
        float *p = &v[i * 14];
        dm::Vec3 n = dm::normalize(acc[key(p)]);
        if (dm::lengthSq(n) < 0.5f) n = dm::normalize(dm::Vec3{p[3], p[4], p[5]});
        p[11] = n.x;
        p[12] = n.y;
        p[13] = n.z;
    }
}
} // namespace

// Expand interleaved 11-float vertices (pos, normal, colour, uv) into the
// 14-float layout and fill the smoothed normal. The index list is unused.
std::vector<float> buildVertexBuffer(const std::vector<float> &src, uint32_t count, const std::vector<uint32_t> &)
{
    std::vector<float> v(count * 14);
    for (uint32_t i = 0; i < count; ++i)
        std::copy(&src[i * 11], &src[i * 11] + 11, &v[i * 14]);
    smoothNormals(v, count);
    return v;
}

// Build the 14-float vertex array from MeshData. If per-vertex normals are
// missing they are derived from the triangles in idx; missing colours default
// to white and missing uvs to (0,0).
std::vector<float> buildVertexBuffer(const MeshData &m, const std::vector<uint32_t> &idx)
{
    size_t n = m.positions.size();
    std::vector<dm::Vec3> normals = m.normals;
    if (normals.size() != n)
    {
        // area-weighted vertex normals from the triangles
        normals.assign(n, dm::Vec3{0, 0, 0});
        for (size_t t = 0; t + 2 < idx.size(); t += 3)
        {
            uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
            if (a >= n || b >= n || c >= n) continue;
            // unnormalised cross product has magnitude 2*area, so summing it weights by area
            dm::Vec3 fn = dm::cross(m.positions[b] - m.positions[a], m.positions[c] - m.positions[a]);
            normals[a] += fn;
            normals[b] += fn;
            normals[c] += fn;
        }
        // normalise; degenerate (zero-area) vertices fall back to +Y
        for (auto &nn : normals)
        {
            nn = dm::normalize(nn);
            if (dm::lengthSq(nn) < 0.5f) nn = {0, 1, 0};
        }
    }
    std::vector<float> v(n * 14);
    for (size_t i = 0; i < n; ++i)
    {
        float *p = &v[i * 14];
        const dm::Vec3 &P = m.positions[i];
        const dm::Vec3 &N = normals[i];
        dm::Vec3 C = i < m.colors.size() ? m.colors[i] : dm::Vec3{1, 1, 1};
        dm::Vec2 T = i < m.uvs.size() ? m.uvs[i] : dm::Vec2{0, 0};
        // pack into the 11-float source layout; the last 3 floats are filled below
        float tmp[11] = {P.x, P.y, P.z, N.x, N.y, N.z, C.x, C.y, C.z, T.x, T.y};
        std::copy(tmp, tmp + 11, p);
    }
    smoothNormals(v, n);
    return v;
}

// --- filesystem ----------------------------------------------------------------

// Non-throwing directory test.
static bool isDir(const std::string &p)
{
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
}

// Find the shader directory. First existing of: $DRIFTO_SHADER_DIR, ./gfx/shaders,
// <exe dir>/gfx/shaders, <exe dir>/../gfx/shaders. Returns "" if none exists.
std::string findShaderDir()
{
    std::vector<std::string> cands;
    if (const char *e = std::getenv("DRIFTO_SHADER_DIR")) cands.push_back(e);
    cands.push_back("gfx/shaders");
    std::string d = platform::executableDir();
    if (!d.empty())
    {
        cands.push_back(d + "/gfx/shaders");
        cands.push_back(d + "/../gfx/shaders");
    }
    for (const std::string &c : cands)
        if (isDir(c)) return c;
    return "";
}

// List entries of dir, skipping dotfiles, sorted. dirsOnly: subdirectory names;
// otherwise files ending in ext, returned with the extension stripped. A missing
// directory yields an empty list.
std::vector<std::string> listDir(const std::string &dir, const std::string &ext, bool dirsOnly)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::string name = platform::pathToUtf8(it->path().filename());
        if (name.empty() || name[0] == '.') continue;
        if (dirsOnly)
        {
            if (isDir(dir + "/" + name)) out.push_back(name);
        }
        else if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
            out.push_back(name.substr(0, name.size() - ext.size()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace gfx
