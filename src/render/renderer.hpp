// renderer.hpp — instanced brick renderer (OpenGL 3.3 core).
// One cube mesh + one pre-allocated instanced buffer. Each resident chunk owns
// a fixed 256-instance range (keyed by its pool slot), uploaded incrementally
// via glBufferSubData when dirty. Per-frame CPU frustum culling drops entire
// off-screen chunks, and only visible chunks issue draw calls.
#pragma once

#include <cstdint>

#include "../core/math.hpp"
#include "../game/constants.hpp"
#include "../game/player.hpp"
#include "../game/wall.hpp"

namespace aw {

class Renderer {
public:
    // True on success (GL context must be current).
    bool init();
    void shutdown();

    // Pick the resolution the 3D scene is rendered at. This is independent of
    // the window size: the scene is drawn into an offscreen target and scaled
    // into the window (letterboxed, aspect preserved), so changing resolution
    // never moves or resizes the window — which is also what keeps the cursor
    // and the UI in the same coordinate space.
    // Returns false when offscreen targets are unavailable; the scene then
    // renders straight into the window at the window's size.
    bool setRenderSize(int w, int h);

    // The resolution render() will actually use for a window of (windowW,
    // windowH): the render size when an offscreen target exists, else the
    // window size. Never returns <= 0 unless the window itself is degenerate.
    void renderSize(int windowW, int windowH, int& outW, int& outH) const;

    // True when the scene is rendered offscreen at an independent resolution.
    bool hasRenderTarget() const { return fboW_ > 0 && fboH_ > 0; }

    // Draw a frame: the scene at the configured render resolution, presented
    // into the window's back buffer. `fovDeg` is the vertical FOV in degrees.
    // `windowW/H` must be the real drawable size (mouse/UI coordinate space).
    // Returns the number of brick instances actually drawn (after culling).
    int render(const Wall& wall, const Player& player, const Mat4& viewProj,
               float fovDeg, int windowW, int windowH, bool targetHot);

    // Immediate-mode UI overlay (settings menu). Coordinates are *window*
    // pixels with a top-left origin — the same space the platform reports the
    // cursor in, so hit-testing needs no scaling. Text uses the embedded 5x7
    // font (see font.hpp). Must be called after render(), which leaves the
    // default framebuffer bound.
    void uiBegin(int width, int height);
    void uiRect(float x, float y, float w, float h,
                float r, float g, float b, float a);
    void uiText(float x, float y, int scale,
                float r, float g, float b, float a, const char* text);
    void uiEnd();

private:
    struct Instance {
        float model[16];
        float shade;
        float pad[3];
    };

    // GL object handles/uniform locations are plain integers here (the GL type
    // aliases live in gl.h, included by the .cpp only).
    unsigned brickProg_ = 0, skyProg_ = 0, crosshairProg_ = 0;
    unsigned cubeVAO_ = 0, cubeVBO_ = 0;
    unsigned fullVAO_ = 0, fullVBO_ = 0;
    unsigned instVBO_ = 0;
    int uVP_ = -1, uCamPos_ = -1, uLodDist_ = -1;
    int skyForward_ = -1, skyRight_ = -1, skyUp_ = -1, skyTan_ = -1, skyRes_ = -1;
    int crossCenter_ = -1, crossSize_ = -1, crossHot_ = -1;
    unsigned uiRectProg_ = 0, uiTextProg_ = 0;
    unsigned uiRectVAO_ = 0, uiRectVBO_ = 0;
    unsigned uiTextVAO_ = 0, uiTextVBO_ = 0;
    unsigned fontTex_ = 0;
    int uiRectRes_ = -1, uiRectDst_ = -1, uiRectCol_ = -1;
    int uiTextRes_ = -1, uiTextDst_ = -1, uiTextUV_ = -1, uiTextCol_ = -1, uiTextTex_ = -1;
    int uiWidth_ = 0, uiHeight_ = 0;

    // Offscreen scene target (render resolution) and its depth attachment.
    unsigned fbo_ = 0, fboColor_ = 0, fboDepth_ = 0;
    int targetW_ = 0, targetH_ = 0;   // requested render resolution
    int fboW_ = 0, fboH_ = 0;         // allocated target size (0 = no target)

    bool createTarget(int w, int h);  // (re)allocate the offscreen target
    void destroyTarget();

    // Pre-allocated staging buffer for one chunk's instance range.
    static constexpr int kInstanceStride = sizeof(Instance);  // 80 bytes (mat4 + shade + pad)
    Instance staging_[CHUNK_BRICKS]{};
    bool ready_ = false;
};

}  // namespace aw
