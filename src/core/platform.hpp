// platform.hpp — platform abstraction: window creation, input, GL context.
// A real X11/OpenGL backend backs the game when a display + GPU are available;
// a headless backend keeps the engine runnable (tests / CI / servers) without one.
#pragma once

#include <cstdint>
#include <string>

namespace aw {

struct FrameInput {
    // Key state is polled per-frame: bit i = keycode i held this frame.
    // The keycode mapping is defined per-backend and exposed via KeyCode constants.
    uint8_t keys[512]{};
    // Mouse delta since the last frame (pixels).
    float mouseDX = 0.0f, mouseDY = 0.0f;
    // Per-frame button events (edge triggered, not held state).
    bool mousePressed[8]{};
    bool mouseReleased[8]{};
    // Window dimensions.
    int32_t width = 0, height = 0;
    // True when the user closed the window / asked to quit.
    bool shouldQuit = false;
};

// Backend capability flags.
struct BackendInfo {
    bool hasWindow = false;
    bool hasGL = false;
    const char* name = "headless";
};

class Platform {
public:
    virtual ~Platform() = default;

    // Create the window + GL context. Returns false if unavailable.
    virtual bool init(const char* title, int width, int height) = 0;
    // Poll OS events, refresh mouse deltas, and fill `in`.
    // Returns false when the window/context was lost (caller should exit).
    virtual bool frame(FrameInput& in) = 0;
    // Present the back buffer (no-op on headless). Called after rendering.
    virtual void swapBuffers() = 0;
    virtual void shutdown() = 0;
    virtual BackendInfo info() const = 0;

    // Hide/release the mouse cursor (look capture).
    virtual void setCursorCaptured(bool captured) = 0;

    // GL function loading (no-op on headless). Returns nullptr if not loaded.
    virtual void* loadGLProc(const char* name) = 0;
};

// Factory selected by the build configuration (see main.cpp). Tries the
// windowed backend and falls back to headless when no display/GPU is present.
Platform* createPlatform();
// Unconditionally headless (tests / CI / demo).
Platform* createHeadlessPlatform();

}  // namespace aw
