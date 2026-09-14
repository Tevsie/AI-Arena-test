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

    // Draw a frame. `viewProj` is proj*view; `aspect` = window w/h; `fovDeg` is
    // the vertical field of view (degrees) the projection was built with — the
    // sky pass must match it exactly.
    // The 3D scene is rendered at `renderW x renderH` and, when that differs
    // from the window size, upscaled to `width x height` (render-resolution
    // scaling: the UI stays at the window resolution, see uiBegin).
    // Returns the number of brick instances actually drawn (after culling).
    int render(const Wall& wall, const Player& player, const Mat4& viewProj,
               float aspect, int width, int height, int renderW, int renderH,
               float fovDeg);

    // Immediate-mode UI overlay (settings menu, modal dialogs, crosshair).
    // Coordinates are pixels with a top-left origin and always refer to the
    // *window* resolution, so UI and fonts stay crisp independently of the 3D
    // render resolution. Text uses the embedded 5x7 font (see font.hpp).
    void uiBegin(int width, int height);
    void uiRect(float x, float y, float w, float h,
                float r, float g, float b, float a);
    void uiText(float x, float y, int scale,
                float r, float g, float b, float a, const char* text);
    // Aiming crosshair, centered, `scale` = display UI scale (1.0 = 96 dpi).
    void uiCrosshair(float scale, bool targetHot);
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
    unsigned uiRectProg_ = 0, uiTextProg_ = 0, blitProg_ = 0;
    unsigned uiRectVAO_ = 0, uiRectVBO_ = 0;
    unsigned uiTextVAO_ = 0, uiTextVBO_ = 0;
    unsigned fontTex_ = 0;
    int uiRectRes_ = -1, uiRectDst_ = -1, uiRectCol_ = -1;
    int uiTextRes_ = -1, uiTextDst_ = -1, uiTextUV_ = -1, uiTextCol_ = -1, uiTextTex_ = -1;
    int uiWidth_ = 0, uiHeight_ = 0;
    // Offscreen target for render-resolution scaling (3D only).
    int blitRes_ = -1, blitTex_ = -1, blitScale_ = -1;
    unsigned sceneFBO_ = 0, sceneColor_ = 0, sceneDepth_ = 0;
    int sceneW_ = 0, sceneH_ = 0;
    bool sceneReady_ = false;

    // (Re)create the offscreen target for `w x h`; false when unavailable
    // (no FBO support, or the framebuffer is incomplete) — the caller then
    // renders straight to the window instead.
    bool ensureSceneTarget(int w, int h);
    void releaseSceneTarget();
    // Draw the upscaled scene into the currently bound framebuffer.
    void blitScene(int windowW, int windowH);

    // Pre-allocated staging buffer for one chunk's instance range.
    static constexpr int kInstanceStride = sizeof(Instance);  // 80 bytes (mat4 + shade + pad)
    Instance staging_[CHUNK_BRICKS]{};
    bool ready_ = false;
};

}  // namespace aw
