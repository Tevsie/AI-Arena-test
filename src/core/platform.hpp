// platform.hpp — platform abstraction: window creation, input, GL context and
// display-mode management.
// A real X11/OpenGL backend backs the game when a display + GPU are available;
// a headless backend keeps the engine runnable (tests / CI / servers) without one.
//
// Display modes: the game offers three presentations (see DisplayMode).
//   Windowed   — a normal OS window; the resolution setting sets its *client*
//                area, clamped to the desktop minus decorations and taskbar.
//   Borderless — a borderless window covering 100% of the monitor at its native
//                resolution; the resolution setting then drives the internal 3D
//                render resolution (upscaled to the screen, UI stays native).
//   Exclusive  — a hardware display-mode switch (resolution + refresh rate) from
//                the driver's supported-mode pool.
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
    // Window dimensions.
    int32_t width = 0, height = 0;
    // True when the user closed the window / asked to quit.
    bool shouldQuit = false;
};

// How the game is presented (see the file header).
enum class DisplayMode : int {
    Windowed = 0,
    Borderless = 1,
    Exclusive = 2,
};

constexpr int kDisplayModeCount = 3;

inline const char* displayModeName(DisplayMode m) {
    switch (m) {
        case DisplayMode::Windowed:   return "WINDOWED";
        case DisplayMode::Borderless: return "BORDERLESS";
        case DisplayMode::Exclusive:  return "EXCLUSIVE";
    }
    return "?";
}

// A single display mode from the driver's pool (exclusive fullscreen) or a
// window client size. `refreshHz` <= 0 means "not applicable / driver default".
struct DisplayModeInfo {
    int width = 0;
    int height = 0;
    int refreshHz = 0;
};

// Quantize a DPI-derived UI scale onto the supported half steps, clamped to
// 1.0..2.0: 96 dpi -> 1.0, 120/144 dpi -> 1.5, 192 dpi -> 2.0. Half steps keep
// the menu's pixel font and panel layout aligned (see Menu::computeLayout).
inline float quantizeUiScale(float scale) {
    if (!(scale > 1.0f)) return 1.0f;          // also catches NaN
    if (scale > 2.0f) scale = 2.0f;
    int half = int(scale * 2.0f + 0.5f);       // 1.0 -> 2, 1.5 -> 3, 2.0 -> 4
    if (half < 2) half = 2;
    return float(half) * 0.5f;
}

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

    // Resize the window client area (used by the windowed resolution setting).
    // The backend clamps the request so the window always fits on the monitor
    // and re-centers it on the monitor the window currently lives on. The new
    // size is reported back through FrameInput in subsequent frames.
    virtual void resize(int width, int height) = 0;

    // ---- display modes ------------------------------------------------------

    // Apply a presentation mode. `width`/`height` are the client size for
    // Windowed and the display mode for Exclusive; Borderless ignores them and
    // uses the monitor's native resolution (pass 0/0). `refreshHz` selects the
    // driver refresh rate for Exclusive (<= 0 = driver default).
    // Returns false when the backend cannot honour the request (no such mode,
    // driver refused the switch, unsupported on this platform) — the caller
    // then restores the previous configuration.
    virtual bool applyDisplayMode(DisplayMode mode, int width, int height, int refreshHz) {
        (void)mode; (void)width; (void)height; (void)refreshHz;
        return false;
    }

    // The mode currently applied by this backend (Borderless/Exclusive are only
    // reported after a successful applyDisplayMode).
    virtual DisplayMode currentDisplayMode() const { return DisplayMode::Windowed; }

    // Native resolution of the monitor the window is on: the desktop size a
    // borderless window covers (unaffected by a temporary exclusive mode).
    // Returns false when unknown (headless).
    virtual bool monitorSize(int& width, int& height) const {
        width = 0;
        height = 0;
        return false;
    }

    // The driver's supported display modes for the monitor the window is on,
    // for exclusive fullscreen. Only 32-bit modes are reported, duplicates
    // (same size + refresh) are filtered, and refresh rates are valid only for
    // exclusive mode. Returns 0 when unavailable (headless).
    virtual int displayModeCount() const { return 0; }
    virtual bool displayModeAt(int index, DisplayModeInfo& out) const {
        (void)index; (void)out;
        return false;
    }

    // Largest client size (pixels) a *windowed* window may have while still
    // being fully visible on the monitor's usable area (screen minus taskbar
    // and window decorations). May be called before init(); returns false when
    // the backend cannot tell (headless), in which case nothing is clamped.
    virtual bool maxWindowSize(int& width, int& height) const {
        width = 0;
        height = 0;
        return false;
    }

    // Current client size in pixels. Normally identical to the size reported by
    // the last frame(), but already updated right after resize() so callers can
    // render the new size in the very same frame. Returns false when unknown.
    virtual bool clientSize(int& width, int& height) const {
        width = 0;
        height = 0;
        return false;
    }

    // Scale factor for the fixed-pixel UI (settings menu): 1.0 at 96 dpi,
    // larger on high-DPI displays so the menu keeps its apparent size now that
    // the window really is in screen pixels. Quantized to half steps (see
    // quantizeUiScale) so the bitmap font stays crisp.
    virtual float uiScale() const { return 1.0f; }

    // GL function loading (no-op on headless). Returns nullptr if not loaded.
    virtual void* loadGLProc(const char* name) = 0;
};

// Factory selected by the build configuration (see main.cpp). Tries the
// windowed backend and falls back to headless when no display/GPU is present.
Platform* createPlatform();
// Unconditionally headless (tests / CI / demo).
Platform* createHeadlessPlatform();

}  // namespace aw
