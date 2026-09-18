// look.hpp — the visual "look": one source of truth for sun, sky, fog, palette
// and post-processing, shared by the CPU (which picks the values for the
// player's altitude) and by the shaders (which get the same values as uniforms
// plus a shared GLSL prefix, so sky, bricks, fog and post always agree).
//
// Before this existed the brick shader lit from normalize(0.45, 0.85, 0.35)
// while the sky drew its sun at normalize(0.5, 0.5, -0.45): highlights and
// shadows could never line up with the sun you could see. Everything visual
// now comes from here.
//
// Style: golden-hour ruin. Warm low sun raking across pale stone, long shadows,
// dust in the air, deep blue haze above; climbing out of the haze thins the
// atmosphere, cools the palette and brings out stars.
#pragma once

#include <cmath>
#include <cstdint>

#include "../core/math.hpp"

namespace aw {

// Local smoothstep (hermite) — kept here so this header needs nothing but math.hpp.
inline float lookSmoothstep(float e0, float e1, float x) {
    float d = e1 - e0;
    if (d == 0.0f) return x < e0 ? 0.0f : 1.0f;
    float t = clampf((x - e0) / d, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// One keyframe of the palette (see lookAtAltitude).
struct Look {
    Vec3 sunDir{0.558f, 0.225f, 0.798f};   // unit vector, points *toward* the sun
    Vec3 sunColor{1.00f, 0.72f, 0.42f};    // warm golden key light
    Vec3 skyZenith{0.16f, 0.29f, 0.55f};
    Vec3 skyHorizon{0.92f, 0.62f, 0.36f};
    Vec3 skyGround{0.30f, 0.26f, 0.24f};   // what the wall fades into when looking down
    Vec3 hazeColor{1.00f, 0.80f, 0.55f};   // ground-haze band, sun-tinted

    float sunIntensity = 1.55f;
    float ambientSky = 0.42f;              // hemisphere ambient from the sky
    float ambientGround = 0.20f;           // ... and from the ground bounce
    float specular = 0.22f;                // soft stone sheen
    float rimStrength = 0.10f;             // silhouette separation
    float hazeStrength = 0.55f;            // width of the warm band at the horizon
    float cloudCover = 0.35f;
    float starAmount = 0.0f;
    float fogDensity = 0.0052f;            // 1/units (exponential approach)
    float fogSkyMix = 0.85f;               // how much fog takes the sky's colour
    float exposure = 1.15f;
    float bloomStrength = 0.55f;
    float bloomThreshold = 0.72f;
    float vignette = 0.30f;
    float grain = 0.030f;
};

// ---------------------------------------------------------------------------
// Altitude palette: the climb is the story. Ground level is a warm sunset,
// mid-climb is clear golden hour, high up the air thins out into cold blue with
// stars — all interpolated, so no assets and no pops.
// ---------------------------------------------------------------------------
struct LookKey {
    float altitude;     // world Y (bricks = 1 unit)
    Look look;
};

inline Look lerpLook(const Look& a, const Look& b, float t) {
    auto mix3 = [t](const Vec3& x, const Vec3& y) { return x + (y - x) * t; };
    auto mixf = [t](float x, float y) { return x + (y - x) * t; };
    Look o;
    o.sunDir = normalize(mix3(a.sunDir, b.sunDir));   // keeps the sun unit-length
    o.sunColor = mix3(a.sunColor, b.sunColor);
    o.skyZenith = mix3(a.skyZenith, b.skyZenith);
    o.skyHorizon = mix3(a.skyHorizon, b.skyHorizon);
    o.skyGround = mix3(a.skyGround, b.skyGround);
    o.hazeColor = mix3(a.hazeColor, b.hazeColor);
    o.sunIntensity = mixf(a.sunIntensity, b.sunIntensity);
    o.ambientSky = mixf(a.ambientSky, b.ambientSky);
    o.ambientGround = mixf(a.ambientGround, b.ambientGround);
    o.specular = mixf(a.specular, b.specular);
    o.rimStrength = mixf(a.rimStrength, b.rimStrength);
    o.hazeStrength = mixf(a.hazeStrength, b.hazeStrength);
    o.cloudCover = mixf(a.cloudCover, b.cloudCover);
    o.starAmount = mixf(a.starAmount, b.starAmount);
    o.fogDensity = mixf(a.fogDensity, b.fogDensity);
    o.fogSkyMix = mixf(a.fogSkyMix, b.fogSkyMix);
    o.exposure = mixf(a.exposure, b.exposure);
    o.bloomStrength = mixf(a.bloomStrength, b.bloomStrength);
    o.bloomThreshold = mixf(a.bloomThreshold, b.bloomThreshold);
    o.vignette = mixf(a.vignette, b.vignette);
    o.grain = mixf(a.grain, b.grain);
    return o;
}

// The three palette keyframes (altitude in world units).
inline const LookKey* lookKeys(int& count) {
    static const LookKey keys[] = {
        // Ground: heavy warm haze, sun just above the horizon.
        {0.0f,
         [] {
             Look l;
             l.sunDir = normalize(Vec3{0.42f, 0.130f, 0.90f});
             l.sunColor = {1.00f, 0.58f, 0.30f};
             l.skyZenith = {0.10f, 0.20f, 0.44f};
             l.skyHorizon = {0.98f, 0.55f, 0.28f};
             l.hazeColor = {1.00f, 0.70f, 0.42f};
             l.hazeStrength = 0.85f;
             l.fogDensity = 0.0072f;
             l.cloudCover = 0.42f;
             l.exposure = 1.18f;
             return l;
         }()},
        // Mid climb: clear golden hour, long shadows.
        {100.0f,
         [] {
             Look l;
             l.sunDir = normalize(Vec3{0.558f, 0.225f, 0.798f});
             l.skyZenith = {0.16f, 0.29f, 0.55f};
             l.hazeStrength = 0.55f;
             l.fogDensity = 0.0052f;
             l.starAmount = 0.05f;
             return l;
         }()},
        // Thin air: cold, deep sky, stars, still raking light from below.
        {320.0f,
         [] {
             Look l;
             l.sunDir = normalize(Vec3{0.600f, 0.340f, 0.722f});
             l.sunColor = {1.00f, 0.86f, 0.72f};
             l.skyZenith = {0.03f, 0.05f, 0.16f};
             l.skyHorizon = {0.30f, 0.42f, 0.68f};
             l.skyGround = {0.10f, 0.12f, 0.18f};
             l.hazeColor = {0.55f, 0.60f, 0.80f};
             l.sunIntensity = 1.75f;
             l.ambientSky = 0.30f;
             l.ambientGround = 0.12f;
             l.hazeStrength = 0.30f;
             l.cloudCover = 0.10f;
             l.starAmount = 0.85f;
             l.fogDensity = 0.0026f;
             l.exposure = 1.10f;
             l.grain = 0.022f;
             return l;
         }()},
    };
    count = int(sizeof(keys) / sizeof(keys[0]));
    return keys;
}

// Palette for a given altitude (monotone interpolation between the keyframes).
inline Look lookAtAltitude(float y) {
    int n = 0;
    const LookKey* keys = lookKeys(n);
    if (y <= keys[0].altitude) return keys[0].look;
    for (int i = 1; i < n; ++i) {
        if (y <= keys[i].altitude) {
            float span = keys[i].altitude - keys[i - 1].altitude;
            float t = span > 0.0f ? (y - keys[i - 1].altitude) / span : 0.0f;
            return lerpLook(keys[i - 1].look, keys[i].look, t);
        }
    }
    return keys[n - 1].look;
}

// ---------------------------------------------------------------------------
// CPU mirror of the sky function (same maths as the GLSL in kLookGLSL). Used for
// the fog colour, for tests, and as documentation of what the shader does.
// ---------------------------------------------------------------------------
inline float skyHash2(float x, float y) {
    float h = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
    return h - std::floor(h);
}

inline float skyNoise2(float x, float y) {
    float ix = std::floor(x), iy = std::floor(y);
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    float a = skyHash2(ix, iy), b = skyHash2(ix + 1.0f, iy);
    float c = skyHash2(ix, iy + 1.0f), d = skyHash2(ix + 1.0f, iy + 1.0f);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

inline float skyFbm2(float x, float y) {
    float v = 0.0f, amp = 0.5f;
    for (int i = 0; i < 4; ++i) {
        v += skyNoise2(x, y) * amp;
        x *= 2.03f; y *= 2.03f;
        amp *= 0.5f;
    }
    return v;
}

// "Air": the cheap part of the sky — gradient, haze band and sun glow. This is
// what geometry fades into with distance (aerial perspective). The shader has a
// matching awSkyAir() so CPU and GPU agree.
inline Vec3 skyAirColor(const Vec3& dir, const Look& L) {
    Vec3 d = normalize(dir);
    float h = d.y;
    // Above the horizon: horizon -> zenith, with the warm band hugging the ground.
    Vec3 col = L.skyHorizon + (L.skyZenith - L.skyHorizon) * lookSmoothstep(0.02f, 0.62f, h);
    // Below the horizon: fade into the ground haze.
    col = col + (L.skyGround - col) * lookSmoothstep(0.0f, -0.22f, h);
    // Ground haze band: strongest right at the horizon, warm, sun-tinted.
    float band = (1.0f - lookSmoothstep(0.0f, 0.28f * (L.hazeStrength + 0.35f), std::fabs(h))) *
                 L.hazeStrength;
    col = col + L.hazeColor * (band * 0.35f);
    // Wide warm glow around the sun, so the key light has a visible source.
    float sd = dot(d, L.sunDir);
    float glow = std::pow(std::max(sd, 0.0f), 6.0f) * 0.45f * L.hazeStrength;
    col = col + L.sunColor * (glow * L.sunIntensity);
    return col;
}

// Full sky (CPU mirror; the shader additionally adds drifting clouds, the sun
// disc and stars — both need uTime, which a test cannot pin down).
inline Vec3 skyColor(const Vec3& dir, const Look& L) {
    Vec3 d = normalize(dir);
    Vec3 col = skyAirColor(d, L);
    float sd = dot(d, L.sunDir);
    col = col + L.sunColor * (std::pow(std::max(sd, 0.0f), 900.0f) * 2.2f * L.sunIntensity);
    return col;
}

// World -> sky direction for a wall fragment (used by the CPU mirror and tests).
inline Vec3 wallFragmentDir(const Vec3& camPos, const Vec3& fragPos) {
    return normalize(fragPos - camPos);
}

// How much of a surface at `dist` has turned into air (0 = clear, 1 = gone).
// The shaders use exactly this curve: 1 - exp(-dist * density).
inline float fogAmount(float dist, const Look& L) {
    return 1.0f - std::exp(-std::max(dist, 0.0f) * L.fogDensity);
}

// ---------------------------------------------------------------------------
// GLSL shared prefix. Declares the look uniforms and the sky/noise functions so
// the sky pass, the brick pass and the post pass describe the same world.
// ---------------------------------------------------------------------------
inline const char* lookGLSL() {
    return R"GLSL(
// ---- shared look (see src/render/look.hpp; one source of truth) ------------
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uSkyZenith;
uniform vec3 uSkyHorizon;
uniform vec3 uSkyGround;
uniform vec3 uHazeColor;
uniform float uSunIntensity;
uniform float uAmbientSky;
uniform float uAmbientGround;
uniform float uSpecular;
uniform float uRimStrength;
uniform float uHazeStrength;
uniform float uCloudCover;
uniform float uStarAmount;
uniform float uFogDensity;
uniform float uFogSkyMix;
uniform float uExposure;
uniform float uBloomStrength;
uniform float uBloomThreshold;
uniform float uVignette;
uniform float uGrain;
uniform float uTime;
uniform vec3 uCamPos;

float awHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float awNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = awHash(i), b = awHash(i + vec2(1.0, 0.0));
    float c = awHash(i + vec2(0.0, 1.0)), d = awHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float awFbm(vec2 p) {
    float v = 0.0, amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += awNoise(p) * amp;
        p *= 2.03;
        amp *= 0.5;
    }
    return v;
}

// Cheap sky: gradient + haze band + sun glow. Geometry fades into this with
// distance (aerial perspective), so it must stay cheap enough to run per pixel.
vec3 awSkyAir(vec3 dir) {
    vec3 d = normalize(dir);
    float h = d.y;
    vec3 col = mix(uSkyHorizon, uSkyZenith, smoothstep(0.02, 0.62, h));
    col = mix(col, uSkyGround, smoothstep(0.0, -0.22, h));
    float band = (1.0 - smoothstep(0.0, 0.28 * (uHazeStrength + 0.35), abs(h))) * uHazeStrength;
    col += uHazeColor * (band * 0.35);
    float sd = dot(d, uSunDir);
    col += uSunColor * (pow(max(sd, 0.0), 6.0) * 0.45 * uHazeStrength * uSunIntensity);
    return col;
}

// Full sky: everything the player can see looking up or out over the world.
vec3 awSkyFull(vec3 dir) {
    vec3 d = normalize(dir);
    vec3 col = awSkyAir(d);

    // Drifting cloud layer, projected onto a plane above the camera. Thin near
    // the zenith and at the horizon, like real cloud cover.
    if (d.y > 0.015 && uCloudCover > 0.001) {
        vec2 cp = d.xz / d.y;
        cp = cp * 0.55 + vec2(uTime * 0.010, uTime * 0.004);
        float n = awFbm(cp * 1.7);
        float cover = smoothstep(0.62 - uCloudCover * 0.45, 0.92 - uCloudCover * 0.25, n);
        float fade = smoothstep(0.015, 0.16, d.y) * (1.0 - smoothstep(0.55, 0.95, d.y));
        vec3 lit = uHazeColor * 0.55 + uSunColor * 0.45;
        vec3 shade = mix(uSkyZenith, uSkyHorizon, 0.35) * 0.85;
        vec3 cloud = mix(shade, lit, smoothstep(0.35, 0.95, n));
        col = mix(col, cloud, cover * fade * 0.85);
    }

    // Sun disc: the visible source of the key light.
    float sd = dot(d, uSunDir);
    col += uSunColor * (pow(max(sd, 0.0), 900.0) * 2.2 * uSunIntensity);

    // Stars, only where the air is thin.
    if (uStarAmount > 0.001 && d.y > 0.0) {
        vec2 sp = d.xz / max(d.y, 0.08) * 26.0;
        vec2 cell = floor(sp);
        float rnd = awHash(cell);
        if (rnd > 0.988) {
            vec2 c = fract(sp) - 0.5;
            float star = smoothstep(0.42, 0.0, length(c));
            float twinkle = 0.65 + 0.35 * sin(uTime * 2.3 + rnd * 40.0);
            col += vec3(0.85, 0.9, 1.0) * (star * twinkle * uStarAmount *
                                           smoothstep(0.03, 0.35, d.y));
        }
    }
    return col;
}

// Aerial perspective: geometry fades into the sky behind it.
vec3 awFog(vec3 viewDir, float dist) {
    float f = 1.0 - exp(-max(dist, 0.0) * uFogDensity);
    vec3 air = awSkyAir(viewDir) * uFogSkyMix + uHazeColor * (1.0 - uFogSkyMix);
    return air * f;
}
)GLSL";
}

// Uniform locations of the shared look block for one program (-1 when unused).
struct LookUniforms {
    int sunDir = -1, sunColor = -1, skyZenith = -1, skyHorizon = -1, skyGround = -1;
    int hazeColor = -1, sunIntensity = -1, ambientSky = -1, ambientGround = -1;
    int specular = -1, rimStrength = -1, hazeStrength = -1, cloudCover = -1;
    int starAmount = -1, fogDensity = -1, fogSkyMix = -1, exposure = -1;
    int bloomStrength = -1, bloomThreshold = -1, vignette = -1, grain = -1, time = -1;
    int camPos = -1;
};

// Fill in the locations for `prog` (a tiny local helper so this header stays
// free of GL types: the caller passes a lambda that resolves names).
template <typename F>
inline LookUniforms lookUniformsFor(F&& loc) {
    LookUniforms u;
    u.sunDir = loc("uSunDir");
    u.sunColor = loc("uSunColor");
    u.skyZenith = loc("uSkyZenith");
    u.skyHorizon = loc("uSkyHorizon");
    u.skyGround = loc("uSkyGround");
    u.hazeColor = loc("uHazeColor");
    u.sunIntensity = loc("uSunIntensity");
    u.ambientSky = loc("uAmbientSky");
    u.ambientGround = loc("uAmbientGround");
    u.specular = loc("uSpecular");
    u.rimStrength = loc("uRimStrength");
    u.hazeStrength = loc("uHazeStrength");
    u.cloudCover = loc("uCloudCover");
    u.starAmount = loc("uStarAmount");
    u.fogDensity = loc("uFogDensity");
    u.fogSkyMix = loc("uFogSkyMix");
    u.exposure = loc("uExposure");
    u.bloomStrength = loc("uBloomStrength");
    u.bloomThreshold = loc("uBloomThreshold");
    u.vignette = loc("uVignette");
    u.grain = loc("uGrain");
    u.time = loc("uTime");
    u.camPos = loc("uCamPos");
    return u;
}

}  // namespace aw
