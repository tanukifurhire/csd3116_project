// dmath.h -- tiny header-only 3D math shared by game/, gfx/ and net/.
//
// Conventions (every module relies on these -- do not change them):
//   * Right-handed, Y-up world. Units are metres, seconds, radians.
//   * Object-local axes: +X right, +Y up, -Z forward (OpenGL camera style).
//     Assets converted from Unity (left-handed, +Z forward) are flipped on Z
//     at conversion time by tools/convert_drifto_assets.py.
//   * Mat4 is column-major (m[col*4 + row]) so it uploads to GL with
//     transpose = GL_FALSE. Vectors are column vectors: p' = M * p.
//   * Quat is (x, y, z, w), unit length, rotates by q * v * q^-1.
#pragma once

#include <cmath>
#include <cstring>

namespace dm {

constexpr float kPi = 3.14159265358979323846f;
// Scalar helpers. clampf bounds v to [lo, hi]; lerpf is a + (b-a)*t (t not clamped).
inline float radians(float deg) { return deg * (kPi / 180.0f); }
inline float degrees(float rad) { return rad * (180.0f / kPi); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
// Wrap an angle to (-pi, pi].
inline float wrapAngle(float a)
{
    a = std::fmod(a + kPi, 2.0f * kPi);
    if (a < 0) a += 2.0f * kPi;
    return a - kPi;
}

// ---- Vectors ----
// Plain POD vector types with free-function operators; no SIMD or alignment requirements.
struct Vec2
{
    float x = 0, y = 0;
    Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
};
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float length(Vec2 a) { return std::sqrt(dot(a, a)); }

struct Vec3
{
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    float &operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return a * s; }
inline Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Vec3 operator/(Vec3 a, float s) { return a * (1.0f / s); }
inline Vec3 &operator+=(Vec3 &a, Vec3 b) { return a = a + b; }
inline Vec3 &operator-=(Vec3 &a, Vec3 b) { return a = a - b; }
inline Vec3 &operator*=(Vec3 &a, float s) { return a = a * s; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline float lengthSq(Vec3 a) { return dot(a, a); }
// Unit-length copy; returns the zero vector for near-zero input instead of dividing by ~0.
inline Vec3 normalize(Vec3 a)
{
    float l = length(a);
    return l > 1e-8f ? a / l : Vec3{0, 0, 0};
}
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

struct Vec4
{
    float x = 0, y = 0, z = 0, w = 0;
    Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(Vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    Vec3 xyz() const { return {x, y, z}; }
};

// ---- Quaternions ----
struct Quat
{
    float x = 0, y = 0, z = 0, w = 1;
    Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};
// Hamilton product: (a * b) applies b first, then a.
inline Quat operator*(Quat a, Quat b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline float dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline Quat normalize(Quat q)
{
    float l = std::sqrt(dot(q, q));
    return l > 1e-8f ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}
inline Quat conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
// Rotation of `angle` radians about `axis` (axis need not be unit length).
inline Quat quatAxisAngle(Vec3 axis, float angle)
{
    Vec3 n = normalize(axis);
    float s = std::sin(angle * 0.5f);
    return {n.x * s, n.y * s, n.z * s, std::cos(angle * 0.5f)};
}
// Rotation about world +Y; yaw = 0 faces -Z, positive yaw turns left (CCW from above).
inline Quat quatYaw(float yaw) { return quatAxisAngle({0, 1, 0}, yaw); }
// Rotate v by unit quaternion q using v + 2w(u x v) + 2u x (u x v), u = (x,y,z); avoids building a matrix.
inline Vec3 rotate(Quat q, Vec3 v)
{
    Vec3 u{q.x, q.y, q.z};
    Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
// Spherical interpolation (t in 0..1). Takes the shortest arc (flips b if dot < 0) and
// falls back to normalised lerp when the quats are nearly parallel (sin(theta) ~ 0).
inline Quat slerp(Quat a, Quat b, float t)
{
    float d = dot(a, b);
    if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f)
        return normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                              a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    float th = std::acos(d), s = std::sin(th);
    float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}
// Local axes of an orientation (see conventions above).
inline Vec3 forwardOf(Quat q) { return rotate(q, {0, 0, -1}); }
inline Vec3 rightOf(Quat q) { return rotate(q, {1, 0, 0}); }
inline Vec3 upOf(Quat q) { return rotate(q, {0, 1, 0}); }

// ---- 4x4 matrices ----
struct Mat4
{
    float m[16]; // column-major
    Mat4() { std::memset(m, 0, sizeof m); m[0] = m[5] = m[10] = m[15] = 1; }
    float &at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }
    const float *data() const { return m; }
};
// Matrix product a * b (b is applied first when transforming column vectors).
inline Mat4 operator*(const Mat4 &a, const Mat4 &b)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int rI = 0; rI < 4; ++rI)
        {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.at(rI, k) * b.at(k, c);
            r.at(rI, c) = s;
        }
    return r;
}
inline Vec4 operator*(const Mat4 &a, Vec4 v)
{
    Vec4 r;
    r.x = a.at(0, 0) * v.x + a.at(0, 1) * v.y + a.at(0, 2) * v.z + a.at(0, 3) * v.w;
    r.y = a.at(1, 0) * v.x + a.at(1, 1) * v.y + a.at(1, 2) * v.z + a.at(1, 3) * v.w;
    r.z = a.at(2, 0) * v.x + a.at(2, 1) * v.y + a.at(2, 2) * v.z + a.at(2, 3) * v.w;
    r.w = a.at(3, 0) * v.x + a.at(3, 1) * v.y + a.at(3, 2) * v.z + a.at(3, 3) * v.w;
    return r;
}
// Point transform uses w = 1 (translation applies); direction transform uses w = 0 (it does not).
// Neither performs the perspective divide.
inline Vec3 transformPoint(const Mat4 &a, Vec3 p) { return (a * Vec4(p, 1)).xyz(); }
inline Vec3 transformDir(const Mat4 &a, Vec3 d) { return (a * Vec4(d, 0)).xyz(); }

inline Mat4 translate(Vec3 t)
{
    Mat4 r;
    r.at(0, 3) = t.x; r.at(1, 3) = t.y; r.at(2, 3) = t.z;
    return r;
}
inline Mat4 scale(Vec3 s)
{
    Mat4 r;
    r.at(0, 0) = s.x; r.at(1, 1) = s.y; r.at(2, 2) = s.z;
    return r;
}
// Rotation matrix from a unit quaternion (translation part is identity).
inline Mat4 toMat4(Quat q)
{
    Mat4 r;
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r.at(0, 0) = 1 - 2 * (yy + zz); r.at(0, 1) = 2 * (xy - wz);     r.at(0, 2) = 2 * (xz + wy);
    r.at(1, 0) = 2 * (xy + wz);     r.at(1, 1) = 1 - 2 * (xx + zz); r.at(1, 2) = 2 * (yz - wx);
    r.at(2, 0) = 2 * (xz - wy);     r.at(2, 1) = 2 * (yz + wx);     r.at(2, 2) = 1 - 2 * (xx + yy);
    return r;
}
// Translation * Rotation * Scale.
inline Mat4 trs(Vec3 t, Quat q, Vec3 s = {1, 1, 1}) { return translate(t) * toMat4(q) * scale(s); }

inline Mat4 transpose(const Mat4 &a)
{
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.at(i, j) = a.at(j, i);
    return r;
}
// General 4x4 inverse (cofactor expansion). Returns identity if singular.
inline Mat4 inverse(const Mat4 &a)
{
    const float *m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r;
    if (std::fabs(det) < 1e-12f) return r;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * det;
    return r;
}
// OpenGL clip space (z in [-1, 1]), fovY in radians.
inline Mat4 perspective(float fovY, float aspect, float zNear, float zFar)
{
    Mat4 r;
    float f = 1.0f / std::tan(fovY * 0.5f);
    r.at(0, 0) = f / aspect;
    r.at(1, 1) = f;
    r.at(2, 2) = (zFar + zNear) / (zNear - zFar);
    r.at(2, 3) = 2 * zFar * zNear / (zNear - zFar);
    r.at(3, 2) = -1;
    r.at(3, 3) = 0;
    return r;
}
// Orthographic projection to OpenGL clip space; arguments are the view-volume planes
// (left, right, bottom, top, near, far).
inline Mat4 ortho(float l, float r_, float b, float t, float n, float f)
{
    Mat4 r;
    r.at(0, 0) = 2 / (r_ - l); r.at(0, 3) = -(r_ + l) / (r_ - l);
    r.at(1, 1) = 2 / (t - b);  r.at(1, 3) = -(t + b) / (t - b);
    r.at(2, 2) = -2 / (f - n); r.at(2, 3) = -(f + n) / (f - n);
    return r;
}
// Right-handed view matrix: camera at `eye` looking at `target`; s/u/-f are the camera
// right/up/back axes forming the rotation rows.
inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up)
{
    Vec3 f = normalize(target - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r;
    r.at(0, 0) = s.x;  r.at(0, 1) = s.y;  r.at(0, 2) = s.z;  r.at(0, 3) = -dot(s, eye);
    r.at(1, 0) = u.x;  r.at(1, 1) = u.y;  r.at(1, 2) = u.z;  r.at(1, 3) = -dot(u, eye);
    r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z; r.at(2, 3) = dot(f, eye);
    return r;
}

} // namespace dm
