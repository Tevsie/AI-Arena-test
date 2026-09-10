// math.hpp — minimal, dependency-free SIMD-friendly math for the engine.
// Everything is header-only and hot-path friendly (no allocation, no virtuals).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace aw {

// ---------------------------------------------------------------------------
// 2D vector
// ---------------------------------------------------------------------------
struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

// ---------------------------------------------------------------------------
// 3D vector
// ---------------------------------------------------------------------------
struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

inline Vec3 operator*(float s, const Vec3& v) { return {v.x * s, v.y * s, v.z * s}; }

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float lengthSq(const Vec3& v) { return dot(v, v); }
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    float l = length(v);
    return l > 1e-12f ? v / l : Vec3{0, 1, 0};
}
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

// ---------------------------------------------------------------------------
// 4D vector (used by the matrix routines / projection)
// ---------------------------------------------------------------------------
struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    constexpr Vec4() = default;
    constexpr Vec4(float X, float Y, float Z, float W) : x(X), y(Y), z(Z), w(W) {}
};

// ---------------------------------------------------------------------------
// 4x4 matrix, column-major (OpenGL convention), right-handed.
// ---------------------------------------------------------------------------
struct Mat4 {
    float m[16]{};

    static Mat4 identity() {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    // Right-handed perspective projection (maps to clip space, -1..1 depth).
    static Mat4 perspective(float fovYRad, float aspect, float nearZ, float farZ) {
        Mat4 r;
        float t = std::tan(fovYRad * 0.5f);
        r.m[0] = 1.0f / (aspect * t);
        r.m[5] = 1.0f / t;
        r.m[10] = (farZ + nearZ) / (nearZ - farZ);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
        r.m[15] = 0.0f;
        return r;
    }

    static Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
        Vec3 f = normalize(center - eye);      // forward
        Vec3 s = normalize(cross(f, up));      // side (right)
        Vec3 u = cross(s, f);                  // recomputed up
        Mat4 r;
        r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
        r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -dot(s, eye);
        r.m[13] = -dot(u, eye);
        r.m[14] =  dot(f, eye);
        r.m[15] = 1.0f;
        return r;
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                float acc = 0.0f;
                for (int k = 0; k < 4; ++k) acc += m[k * 4 + row] * o.m[c * 4 + k];
                r.m[c * 4 + row] = acc;
            }
        return r;
    }

    // Transform a point (w = 1).
    Vec3 transformPoint(const Vec3& p) const {
        float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        float inv = 1.0f / w;
        return {(m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12]) * inv,
                (m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13]) * inv,
                (m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]) * inv};
    }
    // Transform a direction (w = 0, no translation).
    Vec3 transformDir(const Vec3& p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z,
                m[1] * p.x + m[5] * p.y + m[9] * p.z,
                m[2] * p.x + m[6] * p.y + m[10] * p.z};
    }

    const float* data() const { return m; }
    float* data() { return m; }
};

// ---------------------------------------------------------------------------
// Axis-aligned bounding box
// ---------------------------------------------------------------------------
struct AABB {
    Vec3 mn, mx;

    AABB() = default;
    AABB(const Vec3& a, const Vec3& b) : mn(a), mx(b) {}

    bool overlaps(const AABB& o) const {
        return mn.x <= o.mx.x && mx.x >= o.mn.x &&
               mn.y <= o.mx.y && mx.y >= o.mn.y &&
               mn.z <= o.mx.z && mx.z >= o.mn.z;
    }
    bool contains(const Vec3& p) const {
        return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y &&
               p.z >= mn.z && p.z <= mx.z;
    }
    void expand(float e) { mn.x -= e; mn.y -= e; mn.z -= e; mx.x += e; mx.y += e; mx.z += e; }
    Vec3 center() const { return {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f}; }
    float distSqTo(const Vec3& p) const {
        float dx = std::max(mn.x - p.x, std::max(0.0f, p.x - mx.x));
        float dy = std::max(mn.y - p.y, std::max(0.0f, p.y - mx.y));
        float dz = std::max(mn.z - p.z, std::max(0.0f, p.z - mx.z));
        return dx * dx + dy * dy + dz * dz;
    }
};

// ---------------------------------------------------------------------------
// Plane (ax + by + cz + d = 0), normal NOT necessarily normalized.
// ---------------------------------------------------------------------------
struct Plane {
    float a, b, c, d;
    Plane() = default;
    Plane(float A, float B, float C, float D) : a(A), b(B), c(C), d(D) {}
    float dist(const Vec3& p) const { return a * p.x + b * p.y + c * p.z + d; }
};

// ---------------------------------------------------------------------------
// Frustum: 6 planes extracted from a clip-space matrix (Gribb/Hartmann).
// Planes are oriented so that a point inside the frustum has dist() > 0.
// ---------------------------------------------------------------------------
struct Frustum {
    Plane p[6];

    static Frustum fromMatrix(const Mat4& clip) {
        Frustum f;
        const float* m = clip.m;
        auto pl = [&](int i, float a, float b, float c, float d) {
            float len = std::sqrt(a * a + b * b + c * c);
            f.p[i] = Plane(a / len, b / len, c / len, d / len);
        };
        pl(0, m[3] - m[0], m[7] - m[4], m[11] - m[8],  m[15] - m[12]);  // right
        pl(1, m[3] + m[0], m[7] + m[4], m[11] + m[8],  m[15] + m[12]);  // left
        pl(2, m[3] - m[1], m[7] - m[5], m[11] - m[9],  m[15] - m[13]);  // bottom
        pl(3, m[3] + m[1], m[7] + m[5], m[11] + m[9],  m[15] + m[13]);  // top
        pl(4, m[3] - m[2], m[7] - m[6], m[11] - m[10], m[15] - m[14]);  // near
        pl(5, m[3] + m[2], m[7] + m[6], m[11] + m[10], m[15] + m[14]);  // far
        return f;
    }

    // Conservative AABB test: an AABB is (partially) inside when at least one
    // corner lies on the positive side of every plane.
    bool intersects(const AABB& b) const {
        for (int i = 0; i < 6; ++i) {
            const Plane& pl = p[i];
            // p-vertex: the corner furthest in the plane's normal direction.
            Vec3 v{b.mn.x, b.mn.y, b.mn.z};
            if (pl.a >= 0.0f) v.x = b.mx.x;
            if (pl.b >= 0.0f) v.y = b.mx.y;
            if (pl.c >= 0.0f) v.z = b.mx.z;
            if (pl.dist(v) < 0.0f) return false;  // entirely outside this plane
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Ray
// ---------------------------------------------------------------------------
struct Ray {
    Vec3 o, d;
    Ray() = default;
    Ray(const Vec3& origin, const Vec3& dir) : o(origin), d(dir) {}

    // Intersect with an axis-aligned plane (only valid when dir has a component).
    // Returns false if the ray is parallel to the plane.
    bool intersectPlaneX(float x, float& t) const {
        if (std::fabs(d.x) < 1e-9f) return false;
        t = (x - o.x) / d.x;
        return t >= 0.0f;
    }
};

// ---------------------------------------------------------------------------
// Deterministic hashing (procedural brick variation, chunk keys)
// ---------------------------------------------------------------------------
inline uint32_t mix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
inline uint32_t hash2d(int32_t x, int32_t y) {
    return mix32(uint32_t(x) * 0x9e3779b9u ^ mix32(uint32_t(y)) * 0x85ebca6bu);
}
inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 27; x *= 0x94d049bb133111ebull; x ^= x >> 31;
    return x;
}

// Small deterministic PRNG (xorshift) — used for procedural generation only.
struct Rng {
    uint32_t s = 0x12345678u;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s;
    }
    float unit() { return float(next() >> 8) / float(1u << 24); }
};

// Common constants
constexpr float PI = 3.14159265358979323846f;
inline float deg2rad(float d) { return d * PI / 180.0f; }
inline float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
inline int32_t floori(float v) { return int32_t(std::floor(v)); }

}  // namespace aw
