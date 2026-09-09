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

    // Draw a frame. `viewProj` is proj*view; `aspect` = w/h.
    // Returns the number of brick instances actually drawn (after culling).
    int render(const Wall& wall, const Player& player, const Mat4& viewProj,
               float aspect, int width, int height, bool targetHot);

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

    // Pre-allocated staging buffer for one chunk's instance range.
    static constexpr int kInstanceStride = sizeof(Instance);  // 80 bytes (mat4 + shade + pad)
    Instance staging_[CHUNK_BRICKS]{};
    bool ready_ = false;
};

}  // namespace aw
