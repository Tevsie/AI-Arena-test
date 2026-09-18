// renderer.cpp — implementation of the instanced brick renderer.
#include "renderer.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include "font.hpp"
#include "gl.h"
#include "shaders.hpp"
#include "texture.hpp"

namespace aw {

namespace {

const char* kCubeVS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec4 aCol0;
layout(location=3) in vec4 aCol1;
layout(location=4) in vec4 aCol2;
layout(location=5) in vec4 aCol3;
layout(location=6) in float aShade;
uniform mat4 uViewProj;
uniform vec3 uCamPos;
uniform float uLodDist;
out vec3 vNormal;
out vec3 vColor;
out float vFog;
void main() {
    mat4 model = mat4(aCol0, aCol1, aCol2, aCol3);
    vec4 world = model * vec4(aPos, 1.0);
    gl_Position = uViewProj * world;
    // Cube normals are axis-aligned; the model matrix is a uniform scale plus
    // translation (rigid slide), so mat3(model) is a valid normal transform
    // here (renormalized in the fragment shader).
    vNormal = mat3(model) * aNormal;
    float dist = distance(uCamPos, world.xyz);
    float sh = aShade * (1.0 / 255.0);
    vec3 base = mix(vec3(0.40, 0.37, 0.34), vec3(0.66, 0.62, 0.56), sh);
    float lf = smoothstep(uLodDist, uLodDist * 1.35, dist);
    vColor = mix(base, vec3(0.55, 0.52, 0.47), lf);
    vFog = smoothstep(120.0, 320.0, dist);
}
)GLSL";

const char* kCubeFS = R"GLSL(
#version 330 core
in vec3 vNormal;
in vec3 vColor;
in float vFog;
out vec4 fragColor;
void main() {
    vec3 n = normalize(vNormal);
    vec3 L = normalize(vec3(0.45, 0.85, 0.35));
    float diff = max(dot(n, L), 0.0) * 0.85 + 0.18;
    float ao = n.y > 0.5 ? 1.05 : (n.y < -0.5 ? 0.55 : 0.85);
    vec3 col = vColor * diff * ao;
    col = mix(col, vec3(0.66, 0.71, 0.79), vFog);
    fragColor = vec4(col, 1.0);
}
)GLSL";

const char* kFullVS = R"GLSL(
#version 330 core
layout(location=0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)GLSL";

const char* kSkyFS = R"GLSL(
#version 330 core
uniform vec3 uForward;
uniform vec3 uRight;
uniform vec3 uUp;
uniform vec2 uTan;
uniform vec2 uRes;
out vec4 fragColor;
void main() {
    vec2 ndc = (gl_FragCoord.xy / uRes) * 2.0 - 1.0;
    vec3 dir = normalize(uForward + uRight * (ndc.x * uTan.x) + uUp * (ndc.y * uTan.y));
    float h = dir.y;
    vec3 below = vec3(0.70, 0.68, 0.66);
    vec3 horizon = vec3(0.83, 0.81, 0.78);
    vec3 zenith = vec3(0.30, 0.47, 0.70);
    vec3 col = mix(below, horizon, smoothstep(0.0, 0.18, h + 0.05));
    col = mix(col, zenith, smoothstep(0.02, 0.70, h));
    vec3 sunDir = normalize(vec3(0.5, 0.5, -0.45));
    float sun = pow(max(dot(dir, sunDir), 0.0), 500.0) * 0.55;
    col += vec3(1.0, 0.95, 0.84) * sun;
    fragColor = vec4(col, 1.0);
}
)GLSL";

const char* kCrossFS = R"GLSL(
#version 330 core
uniform vec2 uCenter;
uniform vec2 uSize;   // half-length, half-thickness (pixels)
uniform float uHot;   // 1.0 = target locked
out vec4 fragColor;
void main() {
    vec2 p = gl_FragCoord.xy - uCenter;
    bool hz = abs(p.x) < uSize.x && abs(p.y) < uSize.y;
    bool vt = abs(p.y) < uSize.x && abs(p.x) < uSize.y;
    if (!hz && !vt) discard;
    vec3 col = uHot > 0.5 ? vec3(0.35, 1.0, 0.45) : vec3(1.0);
    fragColor = vec4(col, 0.95);
}
)GLSL";

// Upscale blit: draws the offscreen 3D target over the whole window, sampling
// it with linear filtering (render-resolution scaling). gl_FragCoord is in the
// destination resolution, so no vertex attributes beyond the fullscreen
// triangle are needed; GL's bottom-left origin is shared by both, no flip.
const char* kBlitFS = R"GLSL(
#version 330 core
uniform sampler2D uTex;
uniform vec2 uRes;      // destination size (window pixels)
out vec4 fragColor;
void main() {
    vec2 uv = gl_FragCoord.xy / uRes;
    fragColor = vec4(texture(uTex, uv).rgb, 1.0);
}
)GLSL";

// UI rect: unit quad (0..1) mapped to a pixel rect (top-left origin).
const char* kUiRectVS = R"GLSL(
#version 330 core
layout(location=0) in vec2 aPos;
uniform vec2 uRes;   // framebuffer size (pixels)
uniform vec4 uDst;   // x, y, w, h (pixels, top-left origin)
void main() {
    vec2 px = uDst.xy + aPos * uDst.zw;
    vec2 ndc = vec2(px.x / uRes.x * 2.0 - 1.0, 1.0 - px.y / uRes.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)GLSL";

const char* kUiRectFS = R"GLSL(
#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; }
)GLSL";

// UI text: unit quad mapped to a glyph rect, sampling the font atlas.
const char* kUiTextVS = R"GLSL(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
uniform vec2 uRes;
uniform vec4 uDst;   // x, y, w, h (pixels, top-left origin)
out vec2 vUV;
void main() {
    vec2 px = uDst.xy + aPos * uDst.zw;
    vec2 ndc = vec2(px.x / uRes.x * 2.0 - 1.0, 1.0 - px.y / uRes.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV;
}
)GLSL";

const char* kUiTextFS = R"GLSL(
#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform vec4 uColor;
out vec4 fragColor;
void main() {
    float a = texture(uTex, vUV).a;
    if (a < 0.01) discard;
    fragColor = vec4(uColor.rgb, uColor.a * a);
}
)GLSL";

GLuint compile(GLenum type, const char* src) {
    GLuint s = gl.CreateShader(type);
    gl.ShaderSource(s, 1, &src, nullptr);
    gl.CompileShader(s);
    GLint ok = GL_FALSE;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.GetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "[aw] shader compile error:\n%s\n", log);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link(const char* vs, const char* fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, v);
    gl.AttachShader(p, f);
    gl.LinkProgram(p);
    gl.DeleteShader(v);
    gl.DeleteShader(f);
    GLint ok = GL_FALSE;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.GetProgramInfoLog(p, sizeof(log), nullptr, log);
        fprintf(stderr, "[aw] program link error:\n%s\n", log);
        gl.DeleteProgram(p);
        return 0;
    }
    return p;
}

// Prepend the version directive and the shared look block (see look.hpp), so
// every look shader sees the same sun, the same air and the same helpers.
std::string buildSrc(const char* body) {
    // Version + the shared look + the stone profile table (generated from the
    // same C++ data the texture baker uses, so they cannot disagree).
    return std::string("#version 330 core\n") + lookGLSL() + brickProfileGLSL() + body;
}

// Vertex + fragment pair, both look-prefixed.
GLuint linkLook(const char* vsBody, const char* fsBody) {
    std::string vs = buildSrc(vsBody);
    std::string fs = buildSrc(fsBody);
    return link(vs.c_str(), fs.c_str());
}

// Fullscreen pass (shared legacy VS) + look-prefixed fragment shader.
GLuint linkLookFS(const char* fsBody) {
    std::string fs = buildSrc(fsBody);
    return link(kFullVS, fs.c_str());
}

// Push a whole Look to a program's shared uniform block (-1 locations are
// stripped/unused uniforms and are simply skipped).
void setLookUniforms(const LookUniforms& u, const Look& L, float time, const Vec3& camPos) {
    auto set3 = [](int loc, const Vec3& v) { if (loc >= 0) gl.Uniform3f(loc, v.x, v.y, v.z); };
    auto set1 = [](int loc, float v) { if (loc >= 0) gl.Uniform1f(loc, v); };
    set3(u.sunDir, L.sunDir);
    set3(u.sunColor, L.sunColor);
    set3(u.skyZenith, L.skyZenith);
    set3(u.skyHorizon, L.skyHorizon);
    set3(u.skyGround, L.skyGround);
    set3(u.hazeColor, L.hazeColor);
    set3(u.camPos, camPos);
    set1(u.sunIntensity, L.sunIntensity);
    set1(u.ambientSky, L.ambientSky);
    set1(u.ambientGround, L.ambientGround);
    set1(u.specular, L.specular);
    set1(u.rimStrength, L.rimStrength);
    set1(u.hazeStrength, L.hazeStrength);
    set1(u.cloudCover, L.cloudCover);
    set1(u.starAmount, L.starAmount);
    set1(u.fogDensity, L.fogDensity);
    set1(u.fogSkyMix, L.fogSkyMix);
    set1(u.exposure, L.exposure);
    set1(u.bloomStrength, L.bloomStrength);
    set1(u.bloomThreshold, L.bloomThreshold);
    set1(u.vignette, L.vignette);
    set1(u.grain, L.grain);
    set1(u.time, time);
}

}  // namespace

bool Renderer::init() {
    if (!gl.ready) return false;

    brickProg_ = link(kCubeVS, kCubeFS);
    skyProg_ = link(kFullVS, kSkyFS);
    crosshairProg_ = link(kFullVS, kCrossFS);
    if (!brickProg_ || !skyProg_ || !crosshairProg_) return false;

    uVP_ = gl.GetUniformLocation(brickProg_, "uViewProj");
    uCamPos_ = gl.GetUniformLocation(brickProg_, "uCamPos");
    uLodDist_ = gl.GetUniformLocation(brickProg_, "uLodDist");
    skyForward_ = gl.GetUniformLocation(skyProg_, "uForward");
    skyRight_ = gl.GetUniformLocation(skyProg_, "uRight");
    skyUp_ = gl.GetUniformLocation(skyProg_, "uUp");
    skyTan_ = gl.GetUniformLocation(skyProg_, "uTan");
    skyRes_ = gl.GetUniformLocation(skyProg_, "uRes");
    crossCenter_ = gl.GetUniformLocation(crosshairProg_, "uCenter");
    crossSize_ = gl.GetUniformLocation(crosshairProg_, "uSize");
    crossHot_ = gl.GetUniformLocation(crosshairProg_, "uHot");

    // ---- unit cube mesh (centered at origin), pos + normal -----------------
    static const float s = 0.5f;
    float verts[36 * 6];
    int vi = 0;
    auto quad = [&](float nx, float ny, float nz,
                    float x0, float y0, float z0, float x1, float y1, float z1,
                    float x2, float y2, float z2, float x3, float y3, float z3) {
        float pts[6][3] = {
            {x0, y0, z0}, {x1, y1, z1}, {x2, y2, z2},
            {x0, y0, z0}, {x2, y2, z2}, {x3, y3, z3},
        };
        for (int i = 0; i < 6; ++i) {
            verts[vi++] = pts[i][0]; verts[vi++] = pts[i][1]; verts[vi++] = pts[i][2];
            verts[vi++] = nx; verts[vi++] = ny; verts[vi++] = nz;
        }
    };
    quad(0, 0, 1,   -s, -s, s,   s, -s, s,   s, s, s,   -s, s, s);   // +Z (front)
    quad(0, 0, -1,  s, -s, -s,  -s, -s, -s,  -s, s, -s,  s, s, -s);  // -Z
    quad(1, 0, 0,   s, -s, s,   s, -s, -s,   s, s, -s,   s, s, s);   // +X
    quad(-1, 0, 0,  -s, -s, -s,  -s, -s, s,   -s, s, s,  -s, s, -s); // -X
    quad(0, 1, 0,   -s, s, s,   s, s, s,   s, s, -s,   -s, s, -s);   // +Y (top)
    quad(0, -1, 0,  -s, -s, -s,  s, -s, -s,  s, -s, s,   -s, -s, s); // -Y

    gl.GenVertexArrays(1, &cubeVAO_);
    gl.BindVertexArray(cubeVAO_);
    gl.GenBuffers(1, &cubeVBO_);
    gl.BindBuffer(GL_ARRAY_BUFFER, cubeVBO_);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));

    // ---- instance buffer (per-brick model + shade) --------------------------
    // Full resident capacity; per-chunk ranges are uploaded via glBufferSubData.
    gl.GenBuffers(1, &instVBO_);
    gl.BindBuffer(GL_ARRAY_BUFFER, instVBO_);
    gl.BufferData(GL_ARRAY_BUFFER,
                  GLsizeiptr(MAX_RESIDENT_BRICKS) * kInstanceStride, nullptr, GL_STREAM_DRAW);
    for (int i = 0; i < 4; ++i) {
        gl.EnableVertexAttribArray(2 + i);
        gl.VertexAttribPointer(2 + i, 4, GL_FLOAT, GL_FALSE, kInstanceStride,
                               (void*)(size_t(i) * 4 * sizeof(float)));
        gl.VertexAttribDivisorARB(2 + i, 1);
    }
    gl.EnableVertexAttribArray(6);
    gl.VertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, kInstanceStride,
                           (void*)(16 * sizeof(float)));
    gl.VertexAttribDivisorARB(6, 1);
    gl.EnableVertexAttribArray(7);
    gl.VertexAttribPointer(7, 1, GL_FLOAT, GL_FALSE, kInstanceStride,
                           (void*)(17 * sizeof(float)));
    gl.VertexAttribDivisorARB(7, 1);

    // ---- fullscreen triangle VAO -------------------------------------------
    float tri[6] = {-1, -1, 3, -1, -1, 3};
    gl.GenVertexArrays(1, &fullVAO_);
    gl.BindVertexArray(fullVAO_);
    gl.GenBuffers(1, &fullVBO_);
    gl.BindBuffer(GL_ARRAY_BUFFER, fullVBO_);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);

    // ---- UI overlay (settings menu): rect + text programs, dynamic quads ---
    uiRectProg_ = link(kUiRectVS, kUiRectFS);
    uiTextProg_ = link(kUiTextVS, kUiTextFS);
    if (!uiRectProg_ || !uiTextProg_) return false;
    if (gl.hasFBO) {
        blitProg_ = link(kFullVS, kBlitFS);
        blitRes_ = gl.GetUniformLocation(blitProg_, "uRes");
        blitTex_ = gl.GetUniformLocation(blitProg_, "uTex");
    }
    uiRectRes_ = gl.GetUniformLocation(uiRectProg_, "uRes");
    uiRectDst_ = gl.GetUniformLocation(uiRectProg_, "uDst");
    uiRectCol_ = gl.GetUniformLocation(uiRectProg_, "uColor");
    uiTextRes_ = gl.GetUniformLocation(uiTextProg_, "uRes");
    uiTextDst_ = gl.GetUniformLocation(uiTextProg_, "uDst");
    uiTextCol_ = gl.GetUniformLocation(uiTextProg_, "uColor");
    uiTextTex_ = gl.GetUniformLocation(uiTextProg_, "uTex");

    // Unit quad, triangle strip: (0,0) (1,0) (0,1) (1,1).
    float rquad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    gl.GenVertexArrays(1, &uiRectVAO_);
    gl.BindVertexArray(uiRectVAO_);
    gl.GenBuffers(1, &uiRectVBO_);
    gl.BindBuffer(GL_ARRAY_BUFFER, uiRectVBO_);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(rquad), rquad, GL_STATIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);

    float tquad[16] = {0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1};
    gl.GenVertexArrays(1, &uiTextVAO_);
    gl.BindVertexArray(uiTextVAO_);
    gl.GenBuffers(1, &uiTextVBO_);
    gl.BindBuffer(GL_ARRAY_BUFFER, uiTextVBO_);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(tquad), nullptr, GL_DYNAMIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                           (void*)(2 * sizeof(float)));

    // ---- golden-hour look pipeline -----------------------------------------
    // Bricks, sky, mortar, bloom/tonemap. This group is optional on purpose: if
    // a driver rejects any of it (or lacks 2D array textures) the legacy shading
    // above stays in place and the game still runs. Errors are logged so a
    // report from a real machine tells us exactly what failed.
    if (gl.hasTexArray) {
        GLuint brick = linkLook(shaders::kBrickVS, shaders::kBrickFS);
        GLuint sky = linkLookFS(shaders::kSkyFS);
        if (brick && sky) {
            if (brickProg_) gl.DeleteProgram(brickProg_);
            if (skyProg_) gl.DeleteProgram(skyProg_);
            brickProg_ = brick;
            skyProg_ = sky;

            // Uniform locations belong to the program object they were queried
            // from, so every shared name is looked up again for the new programs
            // (the legacy locations above are stale the moment we swap).
            uVP_ = gl.GetUniformLocation(brickProg_, "uViewProj");
            uCamPos_ = gl.GetUniformLocation(brickProg_, "uCamPos");
            uLodDist_ = gl.GetUniformLocation(brickProg_, "uLodDist");
            uStone_ = gl.GetUniformLocation(brickProg_, "uStone");
            uBump_ = gl.GetUniformLocation(brickProg_, "uBump");
            skyForward_ = gl.GetUniformLocation(skyProg_, "uForward");
            skyRight_ = gl.GetUniformLocation(skyProg_, "uRight");
            skyUp_ = gl.GetUniformLocation(skyProg_, "uUp");
            skyTan_ = gl.GetUniformLocation(skyProg_, "uTan");
            skyRes_ = gl.GetUniformLocation(skyProg_, "uRes");
            // Sanity gate: a link that drops one of these would render with a
            // missing uniform, so fall back to the legacy programs instead.
            bool uniformsOk = uVP_ >= 0 && uCamPos_ >= 0 && skyForward_ >= 0 && skyRes_ >= 0;
            if (!uniformsOk) {
                fprintf(stderr, "[aw] look shaders linked without the expected uniforms; "
                                "keeping legacy shading\n");
                gl.DeleteProgram(brickProg_);
                gl.DeleteProgram(skyProg_);
                brickProg_ = link(kCubeVS, kCubeFS);
                skyProg_ = link(kFullVS, kSkyFS);
                uVP_ = gl.GetUniformLocation(brickProg_, "uViewProj");
                uCamPos_ = gl.GetUniformLocation(brickProg_, "uCamPos");
                uLodDist_ = gl.GetUniformLocation(brickProg_, "uLodDist");
                skyForward_ = gl.GetUniformLocation(skyProg_, "uForward");
                skyRight_ = gl.GetUniformLocation(skyProg_, "uRight");
                skyUp_ = gl.GetUniformLocation(skyProg_, "uUp");
                skyTan_ = gl.GetUniformLocation(skyProg_, "uTan");
                skyRes_ = gl.GetUniformLocation(skyProg_, "uRes");
            } else {
            lookPipeline_ = true;
            brickLook_ = lookUniformsFor([&](const char* n) {
                return gl.GetUniformLocation(brickProg_, n);
            });
            skyLook_ = lookUniformsFor([&](const char* n) {
                return gl.GetUniformLocation(skyProg_, n);
            });

            // The mortar/crevasse plane behind the bricks.
            wallProg_ = linkLook(shaders::kWallVS, shaders::kWallFS);
            if (wallProg_) {
                wallVP_ = gl.GetUniformLocation(wallProg_, "uViewProj");
                wallOffset_ = gl.GetUniformLocation(wallProg_, "uOffset");
                wallSize_ = gl.GetUniformLocation(wallProg_, "uSize");
                wallZ_ = gl.GetUniformLocation(wallProg_, "uZ");
                wallTex_ = gl.GetUniformLocation(wallProg_, "uWall");
                wallLook_ = lookUniformsFor([&](const char* n) {
                    return gl.GetUniformLocation(wallProg_, n);
                });
                float wquad[8] = {0, 0, 1, 0, 0, 1, 1, 1};  // unit quad, strip
                gl.GenVertexArrays(1, &wallVAO_);
                gl.BindVertexArray(wallVAO_);
                gl.GenBuffers(1, &wallVBO_);
                gl.BindBuffer(GL_ARRAY_BUFFER, wallVBO_);
                gl.BufferData(GL_ARRAY_BUFFER, sizeof(wquad), wquad, GL_STATIC_DRAW);
                gl.EnableVertexAttribArray(0);
                gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
            }

            // Post chain: bright pass -> separable blur -> ACES + vignette.
            if (gl.hasFBO) {
                brightProg_ = linkLookFS(shaders::kBrightFS);
                blurProg_ = linkLookFS(shaders::kBlurFS);
                postProg_ = linkLookFS(shaders::kPostFS);
                if (brightProg_ && blurProg_ && postProg_) {
                    brightScene_ = gl.GetUniformLocation(brightProg_, "uScene");
                    brightSize_ = gl.GetUniformLocation(brightProg_, "uSceneSize");
                    brightThreshold_ = gl.GetUniformLocation(brightProg_, "uThreshold");
                    blurSrc_ = gl.GetUniformLocation(blurProg_, "uSrc");
                    blurSize_ = gl.GetUniformLocation(blurProg_, "uSize");
                    blurStep_ = gl.GetUniformLocation(blurProg_, "uStep");
                    postScene_ = gl.GetUniformLocation(postProg_, "uScene");
                    postBloom_ = gl.GetUniformLocation(postProg_, "uBloom");
                    postRes_ = gl.GetUniformLocation(postProg_, "uRes");
                    postLook_ = lookUniformsFor([&](const char* n) {
                        return gl.GetUniformLocation(postProg_, n);
                    });
                    postReady_ = true;
                } else {
                    fprintf(stderr, "[aw] bloom/tonemap unavailable; scene is blitted as-is\n");
                }
            }

            uploadStoneTextures();
            }   // uniformsOk
        } else {
            fprintf(stderr,
                    "[aw] look shaders rejected; using legacy flat shading "
                    "(see the GLSL errors above)\n");
        }
    } else {
        fprintf(stderr, "[aw] no 2D array textures; using legacy flat shading\n");
    }

    const FontAtlas& atlas = fontAtlas();
    gl.GenTextures(1, &fontTex_);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, fontTex_);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, FontAtlas::kW, FontAtlas::kH, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, atlas.px);

    gl.BindVertexArray(0);
    ready_ = true;
    return true;
}

void Renderer::shutdown() {
    if (!ready_) return;
    if (brickProg_) gl.DeleteProgram(brickProg_);
    if (skyProg_) gl.DeleteProgram(skyProg_);
    if (crosshairProg_) gl.DeleteProgram(crosshairProg_);
    if (uiRectProg_) gl.DeleteProgram(uiRectProg_);
    if (uiTextProg_) gl.DeleteProgram(uiTextProg_);
    if (blitProg_) gl.DeleteProgram(blitProg_);
    if (wallProg_) gl.DeleteProgram(wallProg_);
    if (brightProg_) gl.DeleteProgram(brightProg_);
    if (blurProg_) gl.DeleteProgram(blurProg_);
    if (postProg_) gl.DeleteProgram(postProg_);
    releaseSceneTarget();
    releaseBloomTarget();
    releaseStoneTextures();
    if (wallVBO_) gl.DeleteBuffers(1, &wallVBO_);
    if (wallVAO_) gl.DeleteVertexArrays(1, &wallVAO_);
    if (cubeVBO_) gl.DeleteBuffers(1, &cubeVBO_);
    if (instVBO_) gl.DeleteBuffers(1, &instVBO_);
    if (fullVBO_) gl.DeleteBuffers(1, &fullVBO_);
    if (uiRectVBO_) gl.DeleteBuffers(1, &uiRectVBO_);
    if (uiTextVBO_) gl.DeleteBuffers(1, &uiTextVBO_);
    if (cubeVAO_) gl.DeleteVertexArrays(1, &cubeVAO_);
    if (fullVAO_) gl.DeleteVertexArrays(1, &fullVAO_);
    if (uiRectVAO_) gl.DeleteVertexArrays(1, &uiRectVAO_);
    if (uiTextVAO_) gl.DeleteVertexArrays(1, &uiTextVAO_);
    if (fontTex_) gl.DeleteTextures(1, &fontTex_);
    brickProg_ = skyProg_ = crosshairProg_ = uiRectProg_ = uiTextProg_ = blitProg_ = 0;
    wallProg_ = brightProg_ = blurProg_ = postProg_ = 0;
    cubeVBO_ = instVBO_ = fullVBO_ = uiRectVBO_ = uiTextVBO_ = wallVBO_ = 0;
    cubeVAO_ = fullVAO_ = uiRectVAO_ = uiTextVAO_ = wallVAO_ = fontTex_ = 0;
    postReady_ = lookPipeline_ = false;
    ready_ = false;
}

// ---- render-resolution scaling: offscreen 3D target ------------------------
bool Renderer::ensureSceneTarget(int w, int h) {
    if (!gl.hasFBO || w <= 0 || h <= 0) return false;
    if (sceneReady_ && sceneW_ == w && sceneH_ == h) return true;
    releaseSceneTarget();

    gl.GenTextures(1, &sceneColor_);
    gl.BindTexture(GL_TEXTURE_2D, sceneColor_);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl.GenTextures(1, &sceneDepth_);
    gl.BindTexture(GL_TEXTURE_2D, sceneDepth_);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                  GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    gl.GenFramebuffers(1, &sceneFBO_);
    gl.BindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneColor_, 0);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sceneDepth_, 0);
    bool ok = gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.BindTexture(GL_TEXTURE_2D, 0);
    if (!ok) {
        fprintf(stderr, "[aw] offscreen render target incomplete (%dx%d)\n", w, h);
        releaseSceneTarget();
        return false;
    }
    sceneW_ = w; sceneH_ = h; sceneReady_ = true;
    return true;
}

// ---- procedural textures (no asset files, ever) ----------------------------
bool Renderer::uploadStoneTextures() {
    if (!gl.hasTexArray) return false;

    // Every stone layer is baked in code at startup: 8 stone types x 4 quadrant
    // tiles (mortar / face / pitted / stained) at 256x256, plus the wall body.
    TextureSet set = bakeStoneTextures();
    if (gl.PixelStorei) gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);

    gl.ActiveTexture(GL_TEXTURE0);
    gl.GenTextures(1, &stoneTex_);
    gl.BindTexture(GL_TEXTURE_2D_ARRAY, stoneTex_);
    gl.TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, TextureSet::kSize, TextureSet::kSize,
                  TextureSet::kLayers, 0, GL_RGBA, GL_UNSIGNED_BYTE, set.data.data());
    gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    gl.GenerateMipmap(GL_TEXTURE_2D_ARRAY);

    std::vector<uint8_t> wall = bakeWallBody();
    gl.ActiveTexture(GL_TEXTURE1);
    gl.GenTextures(1, &wallBodyTex_);
    gl.BindTexture(GL_TEXTURE_2D, wallBodyTex_);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TextureSet::kSize, TextureSet::kSize, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, wall.data());
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    gl.GenerateMipmap(GL_TEXTURE_2D);
    gl.ActiveTexture(GL_TEXTURE0);
    return true;
}

void Renderer::releaseStoneTextures() {
    if (stoneTex_) gl.DeleteTextures(1, &stoneTex_);
    if (wallBodyTex_) gl.DeleteTextures(1, &wallBodyTex_);
    stoneTex_ = wallBodyTex_ = 0;
}

// ---- bloom targets ---------------------------------------------------------
bool Renderer::ensureBloomTarget(int w, int h) {
    if (!gl.hasFBO || w <= 0 || h <= 0) return false;
    int bw = w / 2 > 1 ? w / 2 : 1;
    int bh = h / 2 > 1 ? h / 2 : 1;
    if (bloomA_ && bloomW_ == bw && bloomH_ == bh) return true;
    releaseBloomTarget();

    GLuint tex[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        gl.GenTextures(1, &tex[i]);
        gl.BindTexture(GL_TEXTURE_2D, tex[i]);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    bloomA_ = tex[0];
    bloomB_ = tex[1];

    for (int i = 0; i < 2; ++i) {
        GLuint fbo = 0;
        gl.GenFramebuffers(1, &fbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                i == 0 ? bloomA_ : bloomB_, 0);
        if (i == 0) bloomFBO_ = fbo; else blurFBO_ = fbo;
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
            fprintf(stderr, "[aw] bloom target incomplete (%dx%d)\n", bw, bh);
            releaseBloomTarget();
            return false;
        }
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    bloomW_ = bw;
    bloomH_ = bh;
    return true;
}

void Renderer::releaseBloomTarget() {
    if (gl.hasFBO) {
        if (bloomFBO_) gl.DeleteFramebuffers(1, &bloomFBO_);
        if (blurFBO_) gl.DeleteFramebuffers(1, &blurFBO_);
    }
    if (bloomA_) gl.DeleteTextures(1, &bloomA_);
    if (bloomB_) gl.DeleteTextures(1, &bloomB_);
    bloomFBO_ = blurFBO_ = bloomA_ = bloomB_ = 0;
    bloomW_ = bloomH_ = 0;
}

// ---- post: bright pass -> separable blur -> ACES + vignette + dither -------
void Renderer::postProcess(int windowW, int windowH) {
    if (!postReady_ || !ensureBloomTarget(sceneW_, sceneH_)) {
        blitScene(windowW, windowH);
        return;
    }
    const float bw = float(bloomW_), bh = float(bloomH_);
    gl.BindVertexArray(fullVAO_);
    gl.Disable(GL_DEPTH_TEST);
    gl.DepthMask(GL_FALSE);

    // 1) bright pass at half resolution
    gl.BindFramebuffer(GL_FRAMEBUFFER, bloomFBO_);
    gl.Viewport(0, 0, bloomW_, bloomH_);
    gl.UseProgram(brightProg_);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, sceneColor_);
    gl.Uniform1i(brightScene_, 0);
    gl.Uniform2f(brightSize_, float(sceneW_), float(sceneH_));
    gl.Uniform1f(brightThreshold_, look_.bloomThreshold);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);

    // 2) separable gaussian: X into bloomB_, Y back into bloomA_
    gl.UseProgram(blurProg_);
    gl.Uniform1i(blurSrc_, 0);
    gl.Uniform2f(blurSize_, bw, bh);
    gl.BindFramebuffer(GL_FRAMEBUFFER, blurFBO_);
    gl.BindTexture(GL_TEXTURE_2D, bloomA_);
    gl.Uniform2f(blurStep_, 1.0f / bw, 0.0f);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    gl.BindFramebuffer(GL_FRAMEBUFFER, bloomFBO_);
    gl.BindTexture(GL_TEXTURE_2D, bloomB_);
    gl.Uniform2f(blurStep_, 0.0f, 1.0f / bh);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);

    // 3) grade + upscale straight into the window framebuffer
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, windowW, windowH);
    gl.UseProgram(postProg_);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, sceneColor_);
    gl.ActiveTexture(GL_TEXTURE1);
    gl.BindTexture(GL_TEXTURE_2D, bloomA_);
    gl.Uniform1i(postScene_, 0);
    gl.Uniform1i(postBloom_, 1);
    gl.Uniform2f(postRes_, float(windowW), float(windowH));
    setLookUniforms(postLook_, look_, time_, Vec3{});
    gl.DrawArrays(GL_TRIANGLES, 0, 3);

    gl.ActiveTexture(GL_TEXTURE0);
    gl.DepthMask(GL_TRUE);
}

void Renderer::releaseSceneTarget() {
    if (gl.hasFBO) {
        if (sceneFBO_) gl.DeleteFramebuffers(1, &sceneFBO_);
        if (sceneColor_) gl.DeleteTextures(1, &sceneColor_);
        if (sceneDepth_) gl.DeleteTextures(1, &sceneDepth_);
    }
    sceneFBO_ = sceneColor_ = sceneDepth_ = 0;
    sceneW_ = sceneH_ = 0;
    sceneReady_ = false;
}

void Renderer::blitScene(int windowW, int windowH) {
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, windowW, windowH);
    gl.Disable(GL_DEPTH_TEST);
    gl.DepthMask(GL_FALSE);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);   // letterbox-free; only visible if scaling fails
    gl.UseProgram(blitProg_);
    gl.Uniform2f(blitRes_, float(windowW), float(windowH));
    gl.Uniform1i(blitTex_, 0);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, sceneColor_);
    gl.BindVertexArray(fullVAO_);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    gl.BindVertexArray(0);
    gl.DepthMask(GL_TRUE);
}

int Renderer::render(const Wall& wall, const Player& player, const Mat4& viewProj,
                     float aspect, int width, int height, int renderW, int renderH,
                     float fovDeg) {
    if (!ready_) return 0;

    // The whole look is a function of the player's altitude: ground-level haze
    // and a low sun give way to thin, cold air as the climb goes up.
    look_ = lookAtAltitude(player.eye().y);
    Vec3 eye = player.eye();

    // Render the 3D scene into the offscreen target when the requested render
    // resolution differs from the window (the UI is drawn later, at window
    // resolution, so text stays crisp). With post-processing enabled the scene
    // *always* goes through the offscreen target: the post pass is what applies
    // the grade and upscales to the window.
    bool scaled = (renderW != width || renderH != height) && renderW > 0 && renderH > 0;
    const int sceneW = scaled ? renderW : width;
    const int sceneH = scaled ? renderH : height;
    bool useTarget = gl.hasFBO && (scaled || (postReady_ && postEnabled_)) &&
                     ensureSceneTarget(sceneW, sceneH);
    if (useTarget) gl.BindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);

    // ---- frame setup (viewport + clear every frame) -------------------------
    gl.Viewport(0, 0, sceneW, sceneH);
    gl.ClearColor(0.16f, 0.19f, 0.24f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl.DepthFunc(GL_LESS);
    gl.Disable(GL_CULL_FACE);

    // ---- upload dirty chunks + frustum-cull in one pass ---------------------
    Frustum frustum = Frustum::fromMatrix(viewProj);
    int visible = 0;

    gl.BindBuffer(GL_ARRAY_BUFFER, instVBO_);
    wall.forEachResident([&](const Chunk& ch) {
        // Rebuild this chunk's instances if it changed: one per mosaic brick
        // (origins only; at most CHUNK_BRICKS when the chunk is all 1x1s).
        if (ch.dirty) {
            int32_t ox = ch.coord.cx * CHUNK_X;
            int32_t oy = ch.coord.cy * CHUNK_Y;
            int32_t n = 0;
            for (int32_t i = 0; i < CHUNK_BRICKS; ++i) {
                int32_t bx = ox + (i % CHUNK_X);
                int32_t by = oy + (i / CHUNK_X);
                BrickRect r = brickAt(bx, by);
                if (r.ox != bx || r.oy != by) continue;  // not an origin
                float d = wall.brickDepth(r.ox, r.oy);
                float w = float(r.w), h = float(r.h);
                float e = brickExtent(r);
                // Micro-jitter (visual only, collision stays exact): opens
                // hairline mortar gaps between the exactly-tiled bricks and
                // keeps pulled-brick faces off neighbor planes.
                uint32_t jh = hash2d(bx ^ 0x1234, by ^ 0x5678);
                float jx = float((jh >> 0) & 7u) * 0.0025f;
                float jy = float((jh >> 3) & 7u) * 0.0025f;
                float jz = float((jh >> 6) & 7u) * 0.0025f;
                Instance& inst = staging_[n++];
                std::memset(inst.model, 0, sizeof(inst.model));
                inst.model[0] = w;
                inst.model[5] = h;
                inst.model[10] = e;
                inst.model[12] = float(bx) + w * 0.5f + jx;
                inst.model[13] = float(by) + h * 0.5f + jy;
                inst.model[14] = d - e * 0.5f + jz;   // rigid slide outward (+Z)
                inst.model[15] = 1.0f;
                inst.shade = float(ch.shade[i]);
                // Stone profile purely from the generator's per-brick hash, so the
                // wall never repeats and no gameplay data is touched.
                inst.type = float(brickTypeFromShade(ch.shade[i]));
            }
            GLsizeiptr offset = GLsizeiptr(size_t(ch.slot) * CHUNK_BRICKS * kInstanceStride);
            gl.BufferSubData(GL_ARRAY_BUFFER, offset,
                             GLsizeiptr(n) * kInstanceStride, staging_);
            const_cast<Chunk&>(ch).dirty = false;
        }
        // Frustum culling: the mosaic tiles the chunk exactly, so the chunk
        // box is exact in X/Y (plus jitter epsilon) and only needs the body
        // depth (-Z) and the maximum brick protrusion (+Z). Off-screen chunks
        // are skipped entirely.
        float x0 = float(ch.coord.cx) * CHUNK_WORLD_W;
        float y0 = float(ch.coord.cy) * CHUNK_WORLD_H;
        AABB chunkBox{{x0 - 0.05f, y0 - 0.05f, -BRICK_MAX_EXTENT - 0.5f},
                      {x0 + CHUNK_WORLD_W + 0.05f,
                       y0 + CHUNK_WORLD_H + 0.05f, 1.5f}};
        if (frustum.intersects(chunkBox)) visible += ch.brickCount;
    });

    // ---- sky ----------------------------------------------------------------
    gl.Disable(GL_DEPTH_TEST);
    gl.DepthMask(GL_FALSE);
    gl.UseProgram(skyProg_);
    Vec3 fwd = player.forward();
    Vec3 right = normalize(cross(fwd, Vec3{0, 1, 0}));
    Vec3 up = cross(right, fwd);
    // Match the projection exactly: the sky is a full-screen pass that
    // reconstructs view rays from the half-FOV tangents.
    float tanHalf = std::tan(deg2rad(fovDeg) * 0.5f);
    gl.Uniform3f(skyForward_, fwd.x, fwd.y, fwd.z);
    gl.Uniform3f(skyRight_, right.x, right.y, right.z);
    gl.Uniform3f(skyUp_, up.x, up.y, up.z);
    gl.Uniform2f(skyTan_, tanHalf * aspect, tanHalf);
    gl.Uniform2f(skyRes_, float(sceneW), float(sceneH));
    if (lookPipeline_) setLookUniforms(skyLook_, look_, time_, eye);
    gl.BindVertexArray(fullVAO_);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);

    // ---- mortar behind the bricks -------------------------------------------
    // One textured plane per visible chunk, just behind the deepest brick body.
    // The hairline gaps between bricks then read as mortar (and pulled bricks
    // reveal a real crevasse) instead of letting the sky through.
    if (lookPipeline_ && wallProg_) {
        gl.Enable(GL_DEPTH_TEST);
        gl.DepthMask(GL_TRUE);
        gl.UseProgram(wallProg_);
        gl.UniformMatrix4fv(wallVP_, 1, GL_FALSE, viewProj.data());
        setLookUniforms(wallLook_, look_, time_, eye);
        gl.Uniform1i(wallTex_, 1);
        gl.ActiveTexture(GL_TEXTURE1);
        gl.BindTexture(GL_TEXTURE_2D, wallBodyTex_);
        gl.Uniform1f(wallZ_, -BRICK_MAX_EXTENT - 0.05f);
        gl.BindVertexArray(wallVAO_);
        const float wz = -BRICK_MAX_EXTENT - 0.05f;
        wall.forEachResident([&](const Chunk& ch) {
            float x0 = float(ch.coord.cx) * CHUNK_WORLD_W;
            float y0 = float(ch.coord.cy) * CHUNK_WORLD_H;
            AABB box{{x0, y0, wz - 0.05f}, {x0 + CHUNK_WORLD_W, y0 + CHUNK_WORLD_H, 0.1f}};
            if (!frustum.intersects(box)) return;
            gl.Uniform2f(wallOffset_, x0, y0);
            gl.Uniform2f(wallSize_, CHUNK_WORLD_W, CHUNK_WORLD_H);
            gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        });
    }

    // ---- bricks (one instanced draw per visible chunk) ----------------------
    gl.Enable(GL_DEPTH_TEST);
    gl.DepthMask(GL_TRUE);
    gl.UseProgram(brickProg_);
    gl.UniformMatrix4fv(uVP_, 1, GL_FALSE, viewProj.data());
    gl.Uniform3f(uCamPos_, eye.x, eye.y, eye.z);
    gl.Uniform1f(uLodDist_, LOD_DIST_NEAR);
    if (lookPipeline_) {
        setLookUniforms(brickLook_, look_, time_, eye);
        gl.Uniform1i(uStone_, 0);
        gl.Uniform1f(uBump_, bumpScale_);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D_ARRAY, stoneTex_);
    }

    gl.BindVertexArray(cubeVAO_);
    gl.BindBuffer(GL_ARRAY_BUFFER, instVBO_);  // instance attribs reference this buffer
    wall.forEachResident([&](const Chunk& ch) {
        float x0 = float(ch.coord.cx) * CHUNK_WORLD_W;
        float y0 = float(ch.coord.cy) * CHUNK_WORLD_H;
        AABB chunkBox{{x0 - 0.05f, y0 - 0.05f, -BRICK_MAX_EXTENT - 0.5f},
                      {x0 + CHUNK_WORLD_W + 0.05f,
                       y0 + CHUNK_WORLD_H + 0.05f, 1.5f}};
        if (!frustum.intersects(chunkBox)) return;
        // Re-point the 5 instanced attributes at this chunk's range (the GL 3.3
        // way of supplying a per-chunk base instance without an index offset).
        GLsizeiptr base = GLsizeiptr(size_t(ch.slot) * CHUNK_BRICKS * kInstanceStride);
        for (int i = 0; i < 4; ++i)
            gl.VertexAttribPointer(2 + i, 4, GL_FLOAT, GL_FALSE, kInstanceStride,
                                   (void*)(base + size_t(i) * 4 * sizeof(float)));
        gl.VertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, kInstanceStride,
                               (void*)(base + 16 * sizeof(float)));
        gl.DrawArraysInstancedARB(GL_TRIANGLES, 0, 36, ch.brickCount);
    });

    // ---- present to the window ----------------------------------------------
    // Post-processing grades the scene and upscales it in the same pass; without
    // it the scene is blitted straight over (render-resolution scaling).
    if (useTarget) {
        if (postReady_ && postEnabled_) postProcess(width, height);
        else blitScene(width, height);
    }
    gl.BindVertexArray(0);
    return visible;   // accumulated brick counts of the visible chunks
}

void Renderer::uiBegin(int width, int height) {
    if (!ready_) return;
    uiWidth_ = width;
    uiHeight_ = height;
    // Overlays always target the window framebuffer, whatever the 3D scene did.
    if (gl.hasFBO) gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, width, height);
    gl.Disable(GL_DEPTH_TEST);
    gl.DepthMask(GL_FALSE);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void Renderer::uiRect(float x, float y, float w, float h,
                      float r, float g, float b, float a) {
    if (!ready_ || w <= 0.0f || h <= 0.0f) return;
    gl.UseProgram(uiRectProg_);
    gl.Uniform2f(uiRectRes_, float(uiWidth_), float(uiHeight_));
    gl.Uniform4f(uiRectDst_, x, y, w, h);
    gl.Uniform4f(uiRectCol_, r, g, b, a);
    gl.BindVertexArray(uiRectVAO_);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiText(float x, float y, int scale,
                      float r, float g, float b, float a, const char* text) {
    if (!ready_ || !text || scale < 1) return;
    gl.UseProgram(uiTextProg_);
    gl.Uniform2f(uiTextRes_, float(uiWidth_), float(uiHeight_));
    gl.Uniform4f(uiTextCol_, r, g, b, a);
    gl.Uniform1i(uiTextTex_, 0);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, fontTex_);
    gl.BindVertexArray(uiTextVAO_);
    gl.BindBuffer(GL_ARRAY_BUFFER, uiTextVBO_);

    float cx = x;
    float gw = 6.0f * float(scale);   // 6px advance
    float gh = 8.0f * float(scale);
    for (const char* p = text; *p; ++p, cx += gw) {
        unsigned char c = (unsigned char)*p;
        if (c >= 128) continue;
        // Atlas data row 0 uploads to v=0 (bottom), so a glyph's top art sits
        // at the LOW-v edge of its cell. Exact 6x8 spans keep NEAREST 1:1.
        float u0 = float((c % 16) * 8) / 128.0f;
        float u1 = u0 + 6.0f / 128.0f;
        float vTop = float((c / 16) * 8) / 64.0f;
        float vBot = vTop + 8.0f / 64.0f;
        float v[16] = {0, 0, u0, vTop, 1, 0, u1, vTop, 0, 1, u0, vBot, 1, 1, u1, vBot};
        gl.BufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
        gl.Uniform4f(uiTextDst_, cx, y, gw, gh);
        gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
}

void Renderer::uiCrosshair(float scale, bool targetHot) {
    if (!ready_ || uiWidth_ <= 0 || uiHeight_ <= 0) return;
    if (!(scale > 0.0f)) scale = 1.0f;
    gl.UseProgram(crosshairProg_);
    gl.Uniform2f(crossCenter_, float(uiWidth_) * 0.5f, float(uiHeight_) * 0.5f);
    gl.Uniform2f(crossSize_, 10.0f * scale, 1.6f * scale);
    gl.Uniform1f(crossHot_, targetHot ? 1.0f : 0.0f);
    gl.BindVertexArray(fullVAO_);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

void Renderer::uiEnd() {
    if (!ready_) return;
    gl.BindVertexArray(0);
    gl.UseProgram(0);
    gl.Disable(GL_BLEND);
    gl.Enable(GL_DEPTH_TEST);
    gl.DepthMask(GL_TRUE);
}

}  // namespace aw
