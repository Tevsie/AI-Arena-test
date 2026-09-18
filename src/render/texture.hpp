// texture.hpp — procedural, asset-free textures.
//
// The project ships as a single self-contained executable (no files loaded at
// runtime, ever), so every surface texture is generated in code at startup and
// uploaded to GL as a small mipmapped texture array.
//
// Layout: a GL_TEXTURE_2D_ARRAY of 256x256x4 layers. One layer per stone type,
// with a corner layout that lets the brick shader pick a *sub-tile* of a layer
// (mortar / clean face / pitted / water-stained) from a single texel fetch, and
// a whole second layer set used with a triplanar-ish projection for the wall
// body behind the bricks. All of it is deterministic.
//
// Pure CPU code (no GL calls) so tests can bake and inspect the pixels headless.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../core/math.hpp"

namespace aw {

// ---------------------------------------------------------------------------
// Small deterministic helpers (no engine dependency, no trig-heavy hashing).
// ---------------------------------------------------------------------------
struct TexRng {
    uint32_t s;
    explicit TexRng(uint32_t seed) : s(seed ? seed : 0x9e3779b9u) {}
    uint32_t next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    float f01() { return float(next() & 0xffffffu) / float(0x1000000u); }
    float range(float a, float b) { return a + (b - a) * f01(); }
};

inline float texHash(int x, int y, uint32_t seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h & 0xffffffu) / float(0x1000000u);
}

inline float texSmoothstep(float e0, float e1, float x) {
    float d = e1 - e0;
    if (d == 0.0f) return x < e0 ? 0.0f : 1.0f;
    float t = clampf((x - e0) / d, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Tiling value noise: sample on a wrapped lattice so the texture repeats.
inline float texValueNoise(float x, float y, int period, uint32_t seed) {
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = x - float(ix), fy = y - float(iy);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    auto at = [period, seed](int a, int b) {
        a = ((a % period) + period) % period;
        b = ((b % period) + period) % period;
        return texHash(a, b, seed);
    };
    float a = at(ix, iy), b = at(ix + 1, iy), c = at(ix, iy + 1), d = at(ix + 1, iy + 1);
    float top = a + (b - a) * fx;
    float bot = c + (d - c) * fx;
    return top + (bot - top) * fy;
}

// Tileable fbm: each octave doubles both frequency and lattice period.
inline float texFbm(float x, float y, int basePeriod, int octaves, uint32_t seed) {
    float v = 0.0f, amp = 0.5f, norm = 0.0f;
    int period = basePeriod;
    float fx = x, fy = y;
    for (int i = 0; i < octaves; ++i) {
        v += texValueNoise(fx, fy, period, seed + uint32_t(i) * 977u) * amp;
        norm += amp;
        fx *= 2.0f; fy *= 2.0f;
        period *= 2;
        amp *= 0.5f;
    }
    return norm > 0.0f ? v / norm : 0.0f;
}

// ---------------------------------------------------------------------------
// Brick shading profiles. The shader reads them by brick-type id, so the look
// of a brick is data, not a branch that drifts between shaders.
// ---------------------------------------------------------------------------
struct BrickProfile {
    float tint[3] = {1, 1, 1};      // albedo multiplier
    float relief = 0.35f;           // bump strength
    float roughness = 0.85f;        // 1 = matte, 0 = mirror-ish
    float roughnessVar = 0.10f;     // per-brick variation of the roughness
    float ao = 1.0f;                // mortar-gap occlusion multiplier
    float speckle = 0.25f;          // pitting density override (0 = texture default)
    float stain = 0.0f;             // extra downward staining
};

inline int brickProfileCount() { return 8; }

inline BrickProfile brickProfile(int type) {
    switch (type) {
        case 0:  return {};                                        // plain stone
        case 1:  return {{1.07f, 0.98f, 0.85f}, 0.28f, 0.78f, 0.10f, 1.0f, 0.18f, 0.0f};   // sandstone
        case 2:  return {{0.80f, 0.83f, 0.87f}, 0.55f, 0.62f, 0.18f, 1.0f, 0.35f, 0.10f};  // granite
        case 3:  return {{0.92f, 0.72f, 0.52f}, 0.30f, 0.86f, 0.08f, 1.0f, 0.10f, 0.0f};   // terracotta
        case 4:  return {{0.70f, 0.76f, 0.72f}, 0.70f, 0.80f, 0.22f, 0.96f, 0.45f, 0.35f}; // cracked
        case 5:  return {{0.62f, 0.64f, 0.66f}, 0.62f, 0.88f, 0.12f, 0.94f, 0.50f, 0.55f}; // water-stained
        case 6:  return {{0.86f, 0.86f, 0.80f}, 0.18f, 0.92f, 0.06f, 1.0f, 0.05f, 0.0f};   // smooth slab
        default: return {{0.55f, 0.50f, 0.45f}, 0.45f, 0.96f, 0.04f, 0.90f, 0.60f, 0.75f}; // dark / mossy
    }
}

// GLSL source for the profile table, generated from brickProfile() so the
// shader and the texture baker can never drift apart. Indexed by the same
// per-brick type id the instance stream carries.
inline std::string brickProfileGLSL() {
    std::string s =
        "// Generated from aw::brickProfile() (src/render/texture.hpp).\n"
        "void stoneProfile(int t, out vec3 tint, out float relief, out float rough,\n"
        "                  out float speck, out float stain) {\n"
        "    tint = vec3(1.0); relief = 0.35; rough = 0.85; speck = 0.25; stain = 0.0;\n";
    char buf[256];
    for (int i = 1; i < brickProfileCount(); ++i) {
        const BrickProfile p = brickProfile(i);
        std::snprintf(buf, sizeof(buf),
                      "    if (t == %d) { tint = vec3(%.3f, %.3f, %.3f); relief = %.3f;"
                      " rough = %.3f; speck = %.3f; stain = %.3f; }\n",
                      i, double(p.tint[0]), double(p.tint[1]), double(p.tint[2]),
                      double(p.relief), double(p.roughness), double(p.speckle),
                      double(p.stain));
        s += buf;
    }
    s += "}\n";
    return s;
}

// Stable mapping from the game's per-brick hash byte to a profile id: mostly
// plain stone with a scatter of the interesting ones (weights are data).
inline int brickTypeFromShade(int shadeByte) {
    int h = shadeByte & 0xff;
    if (h < 92) return 0;    // plain
    if (h < 122) return 1;   // sandstone
    if (h < 146) return 2;   // granite
    if (h < 166) return 3;   // terracotta
    if (h < 186) return 4;   // cracked
    if (h < 206) return 5;   // water-stained
    if (h < 226) return 6;   // smooth slab
    return 7;                // dark
}

// ---------------------------------------------------------------------------
// The baked texture array.
// ---------------------------------------------------------------------------
struct TextureSet {
    static constexpr int kSize = 256;     // per layer, power of two
    static constexpr int kLayers = 8;     // one stone layer per brick type
    static constexpr int kQuadrant = kSize / 2;

    // Layout of the four quadrants inside a layer.
    enum Quadrant { QMortar = 0, QFace = 1, QPitted = 2, QStained = 3 };

    std::vector<uint8_t> data;            // kLayers * kSize * kSize * 4 (RGBA)

    // --- sampling ---
    // Tile-local uv is in [0,1) per sub-tile. Returns a straight RGBA texel.
    void sample(int type, int quadrant, float u, float v, uint8_t out[4]) const {
        int qx = (quadrant & 1) * kQuadrant;         // 0,0 | 1,0
        int qy = (quadrant >> 1) * kQuadrant;        // 0,1 | 1,1
        int x = qx + int(clampf(u, 0.0f, 0.9999f) * float(kQuadrant));
        int y = qy + int(clampf(v, 0.0f, 0.9999f) * float(kQuadrant));
        int t = type < 0 ? 0 : (type >= kLayers ? kLayers - 1 : type);
        const uint8_t* p = &data[(size_t(t) * kSize * kSize + size_t(y) * kSize + size_t(x)) * 4];
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
    }

    // Height (0..1) stored in the alpha channel; the shader derives AO from it.
    float sampleHeight(int type, int quadrant, float u, float v) const {
        uint8_t c[4];
        sample(type, quadrant, u, v, c);
        return float(c[3]) / 255.0f;
    }
};

// Bake one layer (one stone type) into `dst` (kSize*kSize*4).
inline void bakeStoneLayer(TextureSet& set, int type) {
    const BrickProfile prof = brickProfile(type);
    const int S = TextureSet::kSize;
    const int Q = TextureSet::kQuadrant;
    uint8_t* base = &set.data[size_t(type) * S * S * 4];

    // Hue jitter per type so types stay distinguishable even in flat light.
    float tint[3] = {prof.tint[0], prof.tint[1], prof.tint[2]};

    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            int q = (x >= Q ? 1 : 0) + (y >= Q ? 2 : 0);
            float u = float(q & 1 ? x - Q : x) / float(Q);
            float v = float(q & 2 ? y - Q : y) / float(Q);

            // Two fbm fields: a low-frequency mottle and a high-frequency grain.
            float mottle = texFbm(u * 4.0f, v * 4.0f, 4, 3, 1000u + uint32_t(type) * 17u);
            float grain = texFbm(u * 24.0f, v * 24.0f, 24, 2, 2000u + uint32_t(type) * 29u);

            // Alpha stores HEIGHT (0 = deep, 0.5 = neutral, 1 = proud). AO is
            // derived from it in the shader: recesses are both darker *and*
            // lower, so one channel carries both and they can never disagree.
            float l = 1.0f;
            float relief = 0.5f;

            if (q == TextureSet::QFace) {
                // Clean face: subtle mottling + grain.
                l = 0.86f + 0.20f * mottle + 0.06f * (grain - 0.5f);
                relief = 0.62f + 0.10f * (grain - 0.5f) + 0.06f * mottle;
            } else if (q == TextureSet::QMortar) {
                // Mortar: rough, darker and set back from the face.
                float mortar = texFbm(u * 18.0f, v * 18.0f, 18, 3, 3000u + uint32_t(type) * 31u);
                l = 0.42f + 0.16f * mortar;
                relief = 0.26f + 0.16f * mortar;
            } else if (q == TextureSet::QPitted) {
                // Pitted: craters carved by a thresholded fbm — the strong relief.
                float pits = texFbm(u * 9.0f, v * 9.0f, 9, 3, 4000u + uint32_t(type) * 37u);
                float craters = clampf((0.60f - pits) * 4.0f, 0.0f, 1.0f) * (0.45f + prof.speckle);
                l = 0.80f + 0.22f * mottle - 0.35f * craters;
                relief = 0.55f - 0.45f * craters + 0.12f * (grain - 0.5f);
            } else {
                // Stained: vertical streaks that run down the face (v = down).
                float streak = texFbm(u * 7.0f, v * 1.6f, 7, 3, 5000u + uint32_t(type) * 41u);
                float wet = clampf((0.58f - streak) * 2.6f, 0.0f, 1.0f) * (0.35f + prof.stain);
                l = 0.84f + 0.18f * mottle - 0.42f * wet;
                relief = 0.52f + 0.05f * (grain - 0.5f) - 0.10f * wet;
            }

            l = clampf(l, 0.0f, 1.35f);
            float r = clampf(l * tint[0], 0.0f, 1.0f);
            float g = clampf(l * tint[1] + 0.03f * (grain - 0.5f), 0.0f, 1.0f);
            float b = clampf(l * tint[2] * 0.97f, 0.0f, 1.0f);

            uint8_t* p = &base[(size_t(y) * S + size_t(x)) * 4];
            p[0] = uint8_t(clampf(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[1] = uint8_t(clampf(g, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[2] = uint8_t(clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[3] = uint8_t(clampf(relief, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

inline TextureSet bakeStoneTextures() {
    TextureSet set;
    set.data.assign(size_t(TextureSet::kLayers) * TextureSet::kSize * TextureSet::kSize * 4, 0);
    for (int t = 0; t < TextureSet::kLayers; ++t) bakeStoneLayer(set, t);
    return set;
}

// ---------------------------------------------------------------------------
// The wall body behind the bricks: horizontal courses of stone, procedural.
// Returns kSize*kSize RGBA (alpha = height, mortar set back). Tileable, baked once.
// ---------------------------------------------------------------------------
inline std::vector<uint8_t> bakeWallBody() {
    const int S = TextureSet::kSize;
    std::vector<uint8_t> px(size_t(S) * S * 4, 0);
    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            float u = float(x) / float(S), v = float(y) / float(S);
            // Four courses of bricks per tile, offset every other row.
            float row = v * 4.0f;
            int rowI = int(row);
            float rowF = row - float(rowI);
            float shifted = u + (rowI & 1 ? 0.5f : 0.0f);
            float col = shifted * 2.0f;
            int colI = int(std::floor(col));
            float colF = col - float(colI);

            // Mortar lines (soft, so no aliasing when minified).
            float mortarV = texSmoothstep(0.0f, 0.045f, rowF) *
                            texSmoothstep(0.0f, 0.045f, 1.0f - rowF);
            float mortarU = texSmoothstep(0.0f, 0.030f, colF) *
                            texSmoothstep(0.0f, 0.030f, 1.0f - colF);
            float mortar = 1.0f - mortarV * mortarU;

            float face = texFbm(u * 10.0f, v * 10.0f, 10, 3, 7000u);
            float grain = texFbm(u * 30.0f, v * 30.0f, 30, 2, 8000u);
            float l = 0.52f + 0.20f * face + 0.05f * (grain - 0.5f) - 0.26f * mortar;

            // Grime collecting at the bottom of each course.
            float grime = clampf((rowF - 0.82f) * 3.0f, 0.0f, 1.0f);
            l -= grime * 0.10f * (0.4f + face);
            l = clampf(l, 0.0f, 1.0f);

            float height = clampf(0.62f - 0.34f * mortar - 0.05f * grime + 0.08f * (face - 0.5f),
                                  0.0f, 1.0f);
            uint8_t* p = &px[(size_t(y) * S + size_t(x)) * 4];
            p[0] = uint8_t(clampf(l * 0.92f, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[1] = uint8_t(clampf(l * 0.88f, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[2] = uint8_t(clampf(l * 0.82f, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[3] = uint8_t(height * 255.0f + 0.5f);
        }
    }
    return px;
}

// ---------------------------------------------------------------------------
// A tiny 2D LUT texture used for the post pass (currently a 1-row identity, kept
// so the plumbing is exercised and tests can assert the shape).
// ---------------------------------------------------------------------------
inline std::vector<uint8_t> bakeIdentityLut() {
    std::vector<uint8_t> px(256 * 2, 255);
    return px;
}

}  // namespace aw
