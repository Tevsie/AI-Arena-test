// renderer.cpp — implementation of the instanced brick renderer.
#include "renderer.hpp"

#include <cstdio>
#include <cstring>

#include "gl.h"

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
    // Cube normals are axis-aligned; the model matrix only translates (rigid
    // slide), so mat3(model) is a valid normal transform here.
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

    // ---- fullscreen triangle VAO -------------------------------------------
    float tri[6] = {-1, -1, 3, -1, -1, 3};
    gl.GenVertexArrays(1, &fullVAO_);
    gl.BindVertexArray(fullVAO_);
    gl.GenBuffers(1, &fullVBO_);
    gl.BindBuffer(GL_ARRAY_BUFFER, fullVBO_);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);

    gl.BindVertexArray(0);
    ready_ = true;
    return true;
}

void Renderer::shutdown() {
    if (!ready_) return;
    if (brickProg_) gl.DeleteProgram(brickProg_);
    if (skyProg_) gl.DeleteProgram(skyProg_);
    if (crosshairProg_) gl.DeleteProgram(crosshairProg_);
    if (cubeVBO_) gl.DeleteBuffers(1, &cubeVBO_);
    if (instVBO_) gl.DeleteBuffers(1, &instVBO_);
    if (fullVBO_) gl.DeleteBuffers(1, &fullVBO_);
    if (cubeVAO_) gl.DeleteVertexArrays(1, &cubeVAO_);
    if (fullVAO_) gl.DeleteVertexArrays(1, &fullVAO_);
    brickProg_ = skyProg_ = crosshairProg_ = 0;
    cubeVBO_ = instVBO_ = fullVBO_ = cubeVAO_ = fullVAO_ = 0;
    ready_ = false;
}

int Renderer::render(const Wall& wall, const Player& player, const Mat4& viewProj,
                     float aspect, int width, int height, bool targetHot) {
    if (!ready_) return 0;

    // ---- upload dirty chunks + frustum-cull in one pass ---------------------
    Frustum frustum = Frustum::fromMatrix(viewProj);
    int visible = 0;

    gl.BindBuffer(GL_ARRAY_BUFFER, instVBO_);
    wall.forEachResident([&](const Chunk& ch) {
        // Rebuild this chunk's 256 instances if it changed.
        if (ch.dirty) {
            int32_t ox = ch.coord.cx * CHUNK_X;
            int32_t oy = ch.coord.cy * CHUNK_Y;
            for (int32_t i = 0; i < CHUNK_BRICKS; ++i) {
                int32_t bx = ox + (i % CHUNK_X);
                int32_t by = oy + (i / CHUNK_X);
                float d = wall.brickDepth(bx, by);
                Instance& inst = staging_[i];
                std::memset(inst.model, 0, sizeof(inst.model));
                inst.model[0] = 1.0f;
                inst.model[5] = 1.0f;
                inst.model[10] = 1.0f;
                inst.model[12] = float(bx) + 0.5f;
                inst.model[13] = float(by) + 0.5f;
                inst.model[14] = d - 0.5f;   // rigid slide outward (+Z)
                inst.model[15] = 1.0f;
                inst.shade = float(ch.shade[i]);
            }
            GLsizeiptr offset = GLsizeiptr(size_t(ch.slot) * CHUNK_BRICKS * kInstanceStride);
            gl.BufferSubData(GL_ARRAY_BUFFER, offset,
                             CHUNK_BRICKS * kInstanceStride, staging_);
            const_cast<Chunk&>(ch).dirty = false;
        }
        // Frustum culling: conservative AABB of this chunk's column and row,
        // including the maximum brick protrusion (+Z). Vertical rows beyond the
        // camera frustum and columns behind the camera are skipped entirely.
        float x0 = float(ch.coord.cx) * CHUNK_WORLD_W;
        float y0 = float(ch.coord.cy) * CHUNK_WORLD_H;
        AABB chunkBox{{x0, y0, -1.5f}, {x0 + CHUNK_WORLD_W, y0 + CHUNK_WORLD_H, 1.5f}};
        if (frustum.intersects(chunkBox)) ++visible;
    });

    // ---- sky ----------------------------------------------------------------
    gl.Disable(GL_DEPTH_TEST);
    gl.DepthMask(GL_FALSE);
    gl.UseProgram(skyProg_);
    Vec3 fwd = player.forward();
    Vec3 right = normalize(cross(fwd, Vec3{0, 1, 0}));
    Vec3 up = cross(right, fwd);
    float tanHalf = std::tan(deg2rad(FOV_DEG) * 0.5f);
    gl.Uniform3f(skyForward_, fwd.x, fwd.y, fwd.z);
    gl.Uniform3f(skyRight_, right.x, right.y, right.z);
    gl.Uniform3f(skyUp_, up.x, up.y, up.z);
    gl.Uniform2f(skyTan_, tanHalf * aspect, tanHalf);
    gl.Uniform2f(skyRes_, float(width), float(height));
    gl.BindVertexArray(fullVAO_);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);

    // ---- bricks (one instanced draw per visible chunk) ----------------------
    gl.Enable(GL_DEPTH_TEST);
    gl.DepthMask(GL_TRUE);
    gl.UseProgram(brickProg_);
    gl.UniformMatrix4fv(uVP_, 1, GL_FALSE, viewProj.data());
    Vec3 eye = player.eye();
    gl.Uniform3f(uCamPos_, eye.x, eye.y, eye.z);
    gl.Uniform1f(uLodDist_, LOD_DIST_NEAR);

    gl.BindVertexArray(cubeVAO_);
    gl.BindBuffer(GL_ARRAY_BUFFER, instVBO_);  // instance attribs reference this buffer
    wall.forEachResident([&](const Chunk& ch) {
        float x0 = float(ch.coord.cx) * CHUNK_WORLD_W;
        float y0 = float(ch.coord.cy) * CHUNK_WORLD_H;
        AABB chunkBox{{x0, y0, -1.5f}, {x0 + CHUNK_WORLD_W, y0 + CHUNK_WORLD_H, 1.5f}};
        if (!frustum.intersects(chunkBox)) return;
        // Re-point the 5 instanced attributes at this chunk's range (the GL 3.3
        // way of supplying a per-chunk base instance without an index offset).
        GLsizeiptr base = GLsizeiptr(size_t(ch.slot) * CHUNK_BRICKS * kInstanceStride);
        for (int i = 0; i < 4; ++i)
            gl.VertexAttribPointer(2 + i, 4, GL_FLOAT, GL_FALSE, kInstanceStride,
                                   (void*)(base + size_t(i) * 4 * sizeof(float)));
        gl.VertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, kInstanceStride,
                               (void*)(base + 16 * sizeof(float)));
        gl.DrawArraysInstancedARB(GL_TRIANGLES, 0, 36, CHUNK_BRICKS);
    });

    // ---- crosshair ----------------------------------------------------------
    gl.Disable(GL_DEPTH_TEST);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.UseProgram(crosshairProg_);
    gl.Uniform2f(crossCenter_, float(width) * 0.5f, float(height) * 0.5f);
    gl.Uniform2f(crossSize_, 10.0f, 1.6f);
    gl.Uniform1f(crossHot_, targetHot ? 1.0f : 0.0f);
    gl.BindVertexArray(fullVAO_);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    gl.Disable(GL_BLEND);

    gl.BindVertexArray(0);
    return visible * CHUNK_BRICKS;
}

}  // namespace aw
