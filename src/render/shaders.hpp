// shaders.hpp — the golden-hour shader set (GLSL 330 core).
//
// Every program here is prefixed at link time with "#version 330 core" plus the
// shared look block (aw::lookGLSL()), so the sky, the bricks, the mortar in the
// gaps and the post pass all describe the *same* sun and the same air.
//
// The suite is deliberately defensive: if any of these fail to compile on a
// driver, renderer.cpp falls back to the legacy shading and logs the GLSL error
// (see Renderer::init), so the game always starts.
//
// Bodies carry no #version line — buildSrc() in renderer.cpp adds it.
#pragma once

namespace aw {
namespace shaders {

// ---------------------------------------------------------------------------
// Bricks
// ---------------------------------------------------------------------------
inline const char* kBrickVS = R"GLSL(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec4 aCol0;
layout(location=3) in vec4 aCol1;
layout(location=4) in vec4 aCol2;
layout(location=5) in vec4 aCol3;
layout(location=6) in float aShade;   // per-brick hash byte: tone + weathering
layout(location=7) in float aType;    // stone profile id (see texture.hpp)

uniform mat4 uViewProj;
uniform vec3 uCamPos;

out vec3 vNormal;
out vec3 vWorld;
out vec3 vLocal;     // 0..1 across the brick box
out vec2 vBrickSize; // world size of the brick (from the instance matrix)
out float vShade;
out float vType;
out float vDist;

void main() {
    mat4 model = mat4(aCol0, aCol1, aCol2, aCol3);
    vec4 world = model * vec4(aPos, 1.0);
    gl_Position = uViewProj * world;
    // The model matrix is a uniform scale plus translation, so mat3(model) is a
    // valid normal transform (renormalized per fragment).
    vNormal = mat3(model) * aNormal;
    vWorld = world.xyz;
    vLocal = aPos + 0.5;
    // Column lengths of the model matrix are the brick's world size: used to keep
    // the stone texture at a constant density no matter the mosaic size.
    vBrickSize = vec2(length(vec3(aCol0)), length(vec3(aCol1)));
    vShade = aShade;
    vType = aType;
    vDist = distance(uCamPos, world.xyz);
}
)GLSL";

inline const char* kBrickFS = R"GLSL(
uniform sampler2DArray uStone;
uniform float uLodDist;
uniform float uBump;

in vec3 vNormal;
in vec3 vWorld;
in vec3 vLocal;
in vec2 vBrickSize;
in float vShade;
in float vType;
in float vDist;

out vec4 fragColor;

// One layer holds four quadrant sub-tiles (see TextureSet in texture.hpp):
// 0 = mortar, 1 = clean face, 2 = pitted, 3 = stained.
vec4 stoneSample(int layer, vec2 uv, int q) {
    vec2 off = vec2(float(q & 1), float(q >> 1)) * 0.5;
    return texture(uStone, vec3(off + clamp(uv, 0.002, 0.998) * 0.5, float(layer)));
}

void main() {
    vec3 n = normalize(vNormal);
    vec3 an = abs(n);
    int layer = int(clamp(vType, 0.0, 7.0));

    vec3 tint;
    float relief, rough, speck, stain;
    stoneProfile(layer, tint, relief, rough, speck, stain);

    // Texture coordinates: the two axes tangential to the dominant face normal.
    // Side faces look into the mortar gap, so they get the mortar tile outright.
    vec2 uv;
    float sideFace;
    if (an.x >= an.y && an.x >= an.z)     { uv = vLocal.zy; sideFace = 1.0; }
    else if (an.y >= an.z)                { uv = vLocal.xz; sideFace = 1.0; }
    else                                  { uv = vLocal.xy; sideFace = 0.0; }

    // Per-brick weathering from the generator's shade byte: a stable hash scatter
    // of cracks, pits and streaks so no two bricks age the same way.
    float weather = fract(vShade * 0.6180339);
    float crack = clamp(smoothstep(0.55, 0.95, weather) * (0.25 + speck), 0.0, 1.0);
    float stainW = clamp(stain * smoothstep(0.70, 0.02, uv.y), 0.0, 1.0);
    // The mortar/occlusion rim is measured in *world units* (so a 4x4 slab gets
    // the same 0.3 m dark edge as a 1x1 brick), while the texture itself tiles at
    // a constant density across any brick size.
    vec2 edgeUnits = min(uv, 1.0 - uv) * max(vBrickSize, vec2(1.0));
    float edge = min(edgeUnits.x, edgeUnits.y);
    float mortarW = max(sideFace, 1.0 - smoothstep(0.0, 0.22, edge));
    vec2 uvT = uv * max(vBrickSize, vec2(1.0));

    vec4 tex = mix(stoneSample(layer, uvT, 1), stoneSample(layer, uvT, 2), crack);
    tex = mix(tex, stoneSample(layer, uvT, 3), stainW);
    tex = mix(tex, stoneSample(layer, uvT, 0), mortarW);

    float tone = vShade * (1.0 / 255.0);
    vec3 albedo = tex.rgb * tint * (0.72 + 0.46 * tone);
    float height = tex.a;
    float microAO = mix(1.0, height, 0.65);

    // Relief: the height channel *is* the bump field. Its screen-space gradient
    // perturbs the normal (derivative bump mapping, no extra texture fetches).
    vec3 dpdx = dFdx(vWorld);
    vec3 dpdy = dFdy(vWorld);
    float dhdx = dFdx(height);
    float dhdy = dFdy(height);
    vec3 surfGrad = cross(dpdy, n) * dhdx + cross(n, dpdx) * dhdy;
    if (dot(surfGrad, surfGrad) > 1e-10) {
        vec3 bumped = normalize(n - surfGrad * (uBump * (0.30 + relief)));
        n = normalize(mix(n, bumped, 0.75));
    }

    // ---- lighting -----------------------------------------------------------
    vec3 V = normalize(uCamPos - vWorld);
    vec3 L = normalize(uSunDir);
    float ndl = dot(n, L);

    // Hemisphere ambient: the sky's own colours, warm at the horizon.
    vec3 ambSky = mix(uSkyHorizon, uSkyZenith, 0.55) * uAmbientSky;
    vec3 ambGnd = uSkyGround * uAmbientGround;
    vec3 ambient = mix(ambGnd, ambSky, n.y * 0.5 + 0.5);

    // Warm key light with a soft, hazy terminator (golden hour is diffuse).
    float diff = smoothstep(-0.12, 0.42, ndl);
    vec3 key = uSunColor * (uSunIntensity * diff);

    // Cool fill from the sky opposite the sun, so shadowed faces are never flat.
    vec3 fillDir = normalize(vec3(-L.x, 0.45, -L.z));
    vec3 fill = mix(uSkyHorizon, uSkyZenith, 0.5) * (max(dot(n, fillDir), 0.0) * 0.30);

    vec3 lit = albedo * microAO * (ambient + key + fill);

    // Broad stone sheen.
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(n, H), 0.0), mix(90.0, 10.0, rough)) * (1.0 - rough * 0.80);
    lit += uSunColor * (spec * uSpecular * max(ndl, 0.0));

    // Rim: separates bricks from the wall behind them, strongest when backlit.
    float rim = pow(1.0 - max(dot(n, V), 0.0), 3.0) * max(1.0 - diff, 0.0);
    lit += uSunColor * (rim * uRimStrength * 1.6);

    // Distance flattening: far bricks settle on a flat tone instead of shimmering.
    float lf = smoothstep(uLodDist, uLodDist * 1.35, vDist);
    vec3 flatCol = albedo * 0.85 * (ambient * 0.60 + uSunColor * (uSunIntensity * 0.55));
    lit = mix(lit, flatCol, lf);

    // Aerial perspective: distance reads as *air*, by fading into the sky that is
    // actually behind the geometry instead of into a flat grey.
    vec3 vd = normalize(vWorld - uCamPos);
    lit = mix(lit, awSkyAir(vd) * uFogSkyMix + uHazeColor * (1.0 - uFogSkyMix),
              1.0 - exp(-vDist * uFogDensity));

    fragColor = vec4(lit, 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// Sky (full-screen pass, reconstructs view rays from the half-FOV tangents)
// ---------------------------------------------------------------------------
inline const char* kSkyFS = R"GLSL(
uniform vec3 uForward;
uniform vec3 uRight;
uniform vec3 uUp;
uniform vec2 uTan;
uniform vec2 uRes;

out vec4 fragColor;

void main() {
    vec2 ndc = (gl_FragCoord.xy / uRes) * 2.0 - 1.0;
    vec3 dir = normalize(uForward + uRight * (ndc.x * uTan.x) + uUp * (ndc.y * uTan.y));
    fragColor = vec4(awSkyFull(dir), 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// The wall body behind the bricks: textured mortar, lit as a crevasse.
// ---------------------------------------------------------------------------
inline const char* kWallVS = R"GLSL(
layout(location=0) in vec2 aPos;   // unit quad, 0..1
uniform vec2 uOffset;
uniform vec2 uSize;
uniform float uZ;
uniform mat4 uViewProj;
out vec2 vUv;
out vec3 vWorld;
void main() {
    vec3 w = vec3(uOffset + aPos * uSize, uZ);
    vWorld = w;
    vUv = w.xy * 0.25;   // world-space courses: 4 units per texture tile
    gl_Position = uViewProj * vec4(w, 1.0);
}
)GLSL";

inline const char* kWallFS = R"GLSL(
uniform sampler2D uWall;

in vec2 vUv;
in vec3 vWorld;
out vec4 fragColor;

void main() {
    vec4 t = texture(uWall, vUv);
    float open = mix(0.30, 1.0, t.a);        // mortar sits deep: heavy occlusion
    vec3 amb = mix(uSkyGround * uAmbientGround,
                   mix(uSkyHorizon, uSkyZenith, 0.55) * uAmbientSky, 0.5);
    vec3 col = t.rgb * amb * (0.22 + 0.78 * open);
    col += uSunColor * (uSunIntensity * 0.08 * open);   // a little sun spills in
    vec3 vd = normalize(vWorld - uCamPos);
    float dist = distance(uCamPos, vWorld);
    col = mix(col, awSkyAir(vd), 1.0 - exp(-dist * uFogDensity));
    fragColor = vec4(col, 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// Post chain: bright pass -> separable blur -> tonemap (ACES) + vignette + dither
// ---------------------------------------------------------------------------
inline const char* kBrightFS = R"GLSL(
uniform sampler2D uScene;
uniform vec2 uSceneSize;
uniform float uThreshold;
out vec4 fragColor;
void main() {
    // Half-resolution target: each output pixel averages a 2x2 block of the scene.
    vec2 uv = (gl_FragCoord.xy * 2.0) / uSceneSize;
    vec2 ts = 1.0 / uSceneSize;
    vec3 c = 0.25 * (texture(uScene, uv + vec2(-0.5, -0.5) * ts).rgb +
                     texture(uScene, uv + vec2( 0.5, -0.5) * ts).rgb +
                     texture(uScene, uv + vec2(-0.5,  0.5) * ts).rgb +
                     texture(uScene, uv + vec2( 0.5,  0.5) * ts).rgb);
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float k = clamp((l - uThreshold) / max(l, 1e-4), 0.0, 1.0);
    fragColor = vec4(c * k, 1.0);
}
)GLSL";

inline const char* kBlurFS = R"GLSL(
uniform sampler2D uSrc;
uniform vec2 uSize;
uniform vec2 uStep;    // texel step along the blur axis
out vec4 fragColor;
void main() {
    vec2 uv = gl_FragCoord.xy / uSize;
    vec3 s = texture(uSrc, uv).rgb * 0.227027;
    s += (texture(uSrc, uv + uStep * 1.3846).rgb + texture(uSrc, uv - uStep * 1.3846).rgb) * 0.3162162;
    s += (texture(uSrc, uv + uStep * 3.2308).rgb + texture(uSrc, uv - uStep * 3.2308).rgb) * 0.0702702;
    fragColor = vec4(s, 1.0);
}
)GLSL";

inline const char* kPostFS = R"GLSL(
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform vec2 uRes;     // destination (window) size
out vec4 fragColor;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec2 uv = gl_FragCoord.xy / uRes;
    vec3 c = texture(uScene, uv).rgb;
    c += texture(uBloom, uv).rgb * uBloomStrength;
    c *= uExposure;
    c = aces(c);

    // Vignette: pulls the eye to the crosshair and sells the "ruin" mood.
    vec2 q = uv * 2.0 - 1.0;
    c *= mix(1.0, smoothstep(1.45, 0.45, length(q)), uVignette);

    // Static dither + a whisper of grain: kills banding in the big sky gradient.
    float d = awHash(gl_FragCoord.xy) - 0.5;
    c += vec3(d * (1.5 / 255.0) + d * uGrain * 0.25);

    fragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)GLSL";

}  // namespace shaders
}  // namespace aw
