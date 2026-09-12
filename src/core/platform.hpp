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
    // Absolute cursor position in pixels (top-left origin). Updated from motion
    // and button events; used by the settings menu (meaningful when the cursor
    // is not captured for FPS look).
    float mouseX = 0.0f, mouseY = 0.0f;
    // Per-frame button events (edge triggered, not held state).
    bool mousePressed[8]{};
    bool mouseReleased[8]{};
    // Window client dimensions: the real drawable size, which is also the
    // coordinate space of mouseX/mouseY and of the UI overlay. The 3D scene
    // renders at its own resolution and is scaled into this.
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

    // Resize the window client area. Only the *window* setting uses this — the
    // render resolution is a renderer concern and never touches the window.
    // Implementations must store the size the window actually ended up with
    // (a WM may clamp it) so the reported size never lies about the drawable.
    virtual void resize(int width, int height) = 0;

    // Usable screen area of the display the window lives on (work area on
    // Windows, root window size on X11). Used to pick a windowed size that
    // fits; (0,0) when unknown (headless).
    virtual void screenSize(int& width, int& height) const = 0;

    // Borderless fullscreen toggle (used by the fullscreen setting).
    virtual void setFullscreen(bool on) = 0;

    // GL function loading (no-op on headless). Returns nullptr if not loaded.
    virtual void* loadGLProc(const char* name) = 0;
};

// Factory selected by the build configuration (see main.cpp). Tries the
// windowed backend and falls back to headless when no display/GPU is present.
Platform* createPlatform();
// Unconditionally headless (tests / CI / demo).
Platform* createHeadlessPlatform();

}  // namespace aw
