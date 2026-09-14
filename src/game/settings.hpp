// settings.hpp — user settings (master volume, display mode + resolutions,
// field of view, mouse sensitivity) with clamping and persistence to a small
// key=value text file ("the game user settings" file, settings.cfg).
//
// The menu edits these live; the game applies them (audio gain, display mode,
// render resolution, projection FOV, look scale) and saves on confirm / menu
// close / quit / restart.
//
// Resolutions are *list driven* and depend on the display mode:
//   Windowed   — client sizes that fit the desktop minus decorations/taskbar
//                (see windowModes()), so a window is never larger than the screen.
//   Borderless — internal render resolutions up to 4K (see renderModes()); the
//                window itself is locked to the monitor's native resolution, so
//                a resolution here is a render scale, not a window size.
//   Exclusive  — the driver's supported display modes (see exclusiveModes()),
//                with a refresh rate per size.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../core/platform.hpp"

namespace aw {

struct Resolution {
    int w = 1280, h = 720;
};

inline bool operator==(const Resolution& a, const Resolution& b) {
    return a.w == b.w && a.h == b.h;
}

// Accepts the symbolic display-mode names used by the settings file (the
// numeric form is the one we write; names are for hand-edited configs).
inline DisplayMode displayModeFromName(const char* name) {
    if (!name) return DisplayMode::Windowed;
    if (std::strncmp(name, "borderless", 10) == 0 || std::strncmp(name, "fullscreen", 10) == 0)
        return DisplayMode::Borderless;
    if (std::strncmp(name, "exclusive", 9) == 0) return DisplayMode::Exclusive;
    return DisplayMode::Windowed;
}

class Settings {
public:
    // Resolution presets offered by the menu, ascending, up to 4K. In windowed
    // mode a preset is only listed when it fits the monitor's usable area; in
    // borderless mode they are render resolutions (above native = supersampling).
    static constexpr int kPresetCount = 5;
    static constexpr Resolution kPresets[kPresetCount] = {
        {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160},
    };
    // Presets + one extra entry (the "largest window that fits" / the native
    // render resolution).
    static constexpr int kMaxModes = kPresetCount + 1;
    // Display modes + refresh rates per size that the backend can report.
    static constexpr int kMaxDisplayModes = 256;

    // Vertical field of view (degrees) and its slider range.
    static constexpr float kFovDefault = 75.0f;   // classic 4:3-era vertical FOV
    static constexpr float kFovMin = 55.0f;
    static constexpr float kFovMax = 110.0f;

    // Window/render sizing limits.
    static constexpr int kMinWindowW = 320, kMinWindowH = 200;
    static constexpr int kMaxRenderW = 7680, kMaxRenderH = 4320;
    // Seconds before an unconfirmed display change is reverted.
    static constexpr float kDisplayConfirmSeconds = 15.0f;

    static constexpr const char* kDefaultPath = "settings.cfg";

    float volume = 0.8f;               // master gain, 0..1
    float sensitivity = 1.0f;          // mouse look multiplier, 0.1..3.0
    float fov = kFovDefault;           // vertical FOV, kFovMin..kFovMax

    DisplayMode mode = DisplayMode::Windowed;
    int width = 1280, height = 720;    // windowed client size
    // Borderless render resolution (0 = native, i.e. render at screen size).
    int renderWidth = 0, renderHeight = 0;
    // Exclusive fullscreen display mode (0 = native size / driver default).
    int modeWidth = 0, modeHeight = 0, modeRefresh = 0;

    void clamp() {
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 1.0f) volume = 1.0f;
        if (sensitivity < 0.1f) sensitivity = 0.1f;
        if (sensitivity > 3.0f) sensitivity = 3.0f;
        if (fov < kFovMin) fov = kFovMin;
        if (fov > kFovMax) fov = kFovMax;
        if (width < kMinWindowW) width = kMinWindowW;
        if (height < kMinWindowH) height = kMinWindowH;
        if (width > kMaxRenderW) width = kMaxRenderW;
        if (height > kMaxRenderH) height = kMaxRenderH;
        clampRender();
        if (modeWidth < 0) modeWidth = 0;
        if (modeHeight < 0) modeHeight = 0;
        if (modeWidth > kMaxRenderW) modeWidth = kMaxRenderW;
        if (modeHeight > kMaxRenderH) modeHeight = kMaxRenderH;
        if (modeRefresh < 0) modeRefresh = 0;
        if (modeRefresh > 480) modeRefresh = 480;
        if (static_cast<int>(mode) < 0) mode = DisplayMode::Windowed;
        if (static_cast<int>(mode) > kDisplayModeCount - 1) mode = DisplayMode::Windowed;
    }

    // Render resolution: 0 means "follow the window/screen size".
    void clampRender() {
        if (renderWidth < 0) renderWidth = 0;
        if (renderHeight < 0) renderHeight = 0;
        if (renderWidth == 0 || renderHeight == 0) { renderWidth = renderHeight = 0; return; }
        if (renderWidth < kMinWindowW) renderWidth = kMinWindowW;
        if (renderHeight < kMinWindowH) renderHeight = kMinWindowH;
        if (renderWidth > kMaxRenderW) renderWidth = kMaxRenderW;
        if (renderHeight > kMaxRenderH) renderHeight = kMaxRenderH;
    }

    // ---- resolution lists (pure, monitor aware, allocation free) -------------

    // Windowed client sizes: every preset that fits `availW x availH`, plus the
    // usable area itself ("largest window that fits"). `availW/H` <= 0 means the
    // monitor is unknown: all presets are offered. Never returns 0.
    static int windowModes(Resolution* out, int cap, int availW, int availH) {
        if (!out || cap <= 0) return 0;
        const bool constrained = availW > 0 && availH > 0;
        int n = 0;
        for (int i = 0; i < kPresetCount && n < cap; ++i) {
            if (constrained && (kPresets[i].w > availW || kPresets[i].h > availH))
                continue;   // does not fit on this monitor
            out[n++] = kPresets[i];
        }
        if (constrained && n < cap) {
            Resolution fit{availW, availH};
            if (fit.w < kMinWindowW) fit.w = kMinWindowW;
            if (fit.h < kMinWindowH) fit.h = kMinWindowH;
            if (n == 0 || !(out[n - 1] == fit)) out[n++] = fit;
        }
        if (n == 0) out[n++] = kPresets[0];   // never empty
        return n;
    }

    // Borderless render resolutions: presets up to 4K (higher than native =
    // supersampling) plus the monitor's native resolution (100%). Ascending.
    static int renderModes(Resolution* out, int cap, int nativeW, int nativeH) {
        if (!out || cap <= 0) return 0;
        int n = 0;
        for (int i = 0; i < kPresetCount && n < cap; ++i) out[n++] = kPresets[i];
        if (nativeW > 0 && nativeH > 0 && n < cap) {
            Resolution nat{nativeW, nativeH};
            int insert = n;
            for (int i = 0; i < n; ++i)
                if (out[i].w * out[i].h > nat.w * nat.h) { insert = i; break; }
            // Already listed? (the preset at/just below the insert point)
            if ((insert < n && out[insert] == nat) ||
                (insert > 0 && out[insert - 1] == nat))
                return n;
            for (int i = n; i > insert; --i) out[i] = out[i - 1];
            out[insert] = nat;
            ++n;
        }
        return n;
    }

    // Display modes from the backend's driver pool, deduplicated by pixel size
    // (refresh rates are handled separately), ascending by area.
    static int exclusiveModes(Resolution* out, int cap, const DisplayModeInfo* modes, int count) {
        if (!out || cap <= 0 || !modes) return 0;
        int n = 0;
        for (int i = 0; i < count; ++i) {
            if (modes[i].width <= 0 || modes[i].height <= 0) continue;
            Resolution r{modes[i].width, modes[i].height};
            bool dup = false;
            for (int j = 0; j < n; ++j) if (out[j] == r) { dup = true; break; }
            if (dup) continue;
            if (n >= cap) break;
            int insert = n;
            for (int j = 0; j < n; ++j)
                if (out[j].w * out[j].h > r.w * r.h) { insert = j; break; }
            for (int j = n; j > insert; --j) out[j] = out[j - 1];
            out[insert] = r;
            ++n;
        }
        return n;
    }

    // Distinct refresh rates offered for `w x h` in the driver pool, ascending.
    // A trailing 0 (driver default) is appended when the pool has entries.
    static int refreshRates(int* out, int cap, const DisplayModeInfo* modes, int count,
                            int w, int h) {
        if (!out || cap <= 0 || !modes) return 0;
        int n = 0;
        for (int i = 0; i < count; ++i) {
            if (modes[i].width != w || modes[i].height != h) continue;
            int hz = modes[i].refreshHz;
            if (hz <= 0) continue;
            bool dup = false;
            for (int j = 0; j < n; ++j) if (out[j] == hz) { dup = true; break; }
            if (dup || n >= cap) continue;
            int insert = n;
            for (int j = 0; j < n; ++j) if (out[j] > hz) { insert = j; break; }
            for (int j = n; j > insert; --j) out[j] = out[j - 1];
            out[insert] = hz;
            ++n;
        }
        if (n > 0 && n < cap) out[n++] = 0;   // "DEFAULT" entry
        return n;
    }

    // Index of the entry closest (by area) to w x h, or -1 when empty.
    static int modeIndex(const Resolution* modes, int n, int w, int h) {
        if (!modes || n <= 0) return -1;
        int best = 0, bestDiff = 0x7fffffff;
        int area = w * h;
        for (int i = 0; i < n; ++i) {
            int d = modes[i].w * modes[i].h - area;
            if (d < 0) d = -d;
            if (d < bestDiff) { bestDiff = d; best = i; }
        }
        return best;
    }

    // Step through an ascending list (wraps) to the entry above/below the
    // current size, so cycling can only land on supported resolutions.
    // Fits a render-resolution choice to the shape of the window: the pixel
    // budget (area) of the choice is kept, but the aspect ratio becomes the
    // window's, so upscaling a 3D frame can never stretch the image (a 16:9
    // render preset picked on a 16:10 or 4:3 monitor, for example).
    static void fitRenderAspect(int wantW, int wantH, int winW, int winH, int& rw, int& rh) {
        rw = wantW;
        rh = wantH;
        if (wantW <= 0 || wantH <= 0 || winW <= 0 || winH <= 0) return;
        const double area = double(wantW) * double(wantH);
        const double aspect = double(winW) / double(winH);
        double h = std::sqrt(area / aspect);
        double w = h * aspect;
        rw = int(w + 0.5);
        rh = int(h + 0.5);
        if (rw < kMinWindowW) rw = kMinWindowW;
        if (rh < kMinWindowH) rh = kMinWindowH;
    }

    static void stepMode(int dir, int& w, int& h, const Resolution* modes, int n) {
        if (!modes || n <= 0) return;
        int area = w * h;
        int pick = -1;
        if (dir >= 0) {
            for (int i = 0; i < n; ++i)
                if (modes[i].w * modes[i].h > area) { pick = i; break; }
            if (pick < 0) pick = 0;
        } else {
            for (int i = n - 1; i >= 0; --i)
                if (modes[i].w * modes[i].h < area) { pick = i; break; }
            if (pick < 0) pick = n - 1;
        }
        w = modes[pick].w;
        h = modes[pick].h;
    }

    // Snap the windowed size onto the closest size this monitor can show.
    void fitToMonitor(int availW, int availH) {
        Resolution modes[kMaxModes];
        int n = windowModes(modes, kMaxModes, availW, availH);
        int i = modeIndex(modes, n, width, height);
        if (i >= 0) { width = modes[i].w; height = modes[i].h; }
        clamp();
    }

    // Snap the borderless render resolution onto a supported entry (0 = native).
    void fitRenderToMonitor(int nativeW, int nativeH) {
        if (renderWidth <= 0 || renderHeight <= 0) return;
        Resolution modes[kMaxModes];
        int n = renderModes(modes, kMaxModes, nativeW, nativeH);
        int i = modeIndex(modes, n, renderWidth, renderHeight);
        if (i >= 0) { renderWidth = modes[i].w; renderHeight = modes[i].h; }
        clampRender();
    }

    // Serialize to / parse from "key=value" lines. Unknown keys are ignored;
    // missing keys keep their current values. Used by load/save and by tests.
    void serialize(char* out, size_t n) const {
        std::snprintf(out, n,
                      "volume=%.3f\nsensitivity=%.3f\nfov=%.3f\n"
                      "displaymode=%d\nwidth=%d\nheight=%d\n"
                      "renderwidth=%d\nrenderheight=%d\n"
                      "modewidth=%d\nmodeheight=%d\nmoderefresh=%d\n",
                      double(volume), double(sensitivity), double(fov),
                      int(mode), width, height,
                      renderWidth, renderHeight,
                      modeWidth, modeHeight, modeRefresh);
    }

    bool parse(const char* text) {
        if (!text) return false;
        char line[128];
        const char* p = text;
        bool sawMode = false;
        float legacyFullscreen = -1.0f;
        while (*p) {
            size_t len = 0;
            while (p[len] && p[len] != '\n' && len + 1 < sizeof(line)) ++len;
            std::memcpy(line, p, len);
            line[len] = '\0';
            p += len + (p[len] == '\n' ? 1 : 0);
            float f = 0.0f;
            int v = 0;
            char word[16] = {0};
            if (std::sscanf(line, "volume=%f", &f) == 1) volume = f;
            else if (std::sscanf(line, "sensitivity=%f", &f) == 1) sensitivity = f;
            else if (std::sscanf(line, "fov=%f", &f) == 1) fov = f;
            else if (std::sscanf(line, "displaymode=%d", &v) == 1) {
                mode = v == 1 ? DisplayMode::Borderless
                              : (v == 2 ? DisplayMode::Exclusive : DisplayMode::Windowed);
                sawMode = true;
            } else if (std::sscanf(line, "displaymode=%15s", word) == 1) {
                // The file is hand-editable: accept the names as well.
                mode = displayModeFromName(word);
                sawMode = true;
            } else if (std::sscanf(line, "width=%d", &v) == 1) width = v;
            else if (std::sscanf(line, "height=%d", &v) == 1) height = v;
            else if (std::sscanf(line, "renderwidth=%d", &v) == 1) renderWidth = v;
            else if (std::sscanf(line, "renderheight=%d", &v) == 1) renderHeight = v;
            else if (std::sscanf(line, "modewidth=%d", &v) == 1) modeWidth = v;
            else if (std::sscanf(line, "modeheight=%d", &v) == 1) modeHeight = v;
            else if (std::sscanf(line, "moderefresh=%d", &v) == 1) modeRefresh = v;
            else if (std::sscanf(line, "fullscreen=%f", &f) == 1) legacyFullscreen = f;
        }
        // Older configs only had a fullscreen flag: borderless fullscreen.
        if (!sawMode && legacyFullscreen > 0.5f) mode = DisplayMode::Borderless;
        clamp();
        return true;
    }

    // The settings file can be redirected (the tests use a scratch file).
    static void setPathForTests(const char* path) { testPath_ = path; }
    static const char* filePath() { return testPath_ ? testPath_ : kDefaultPath; }

    bool load(const char* path = nullptr) {
        FILE* f = std::fopen(path ? path : filePath(), "rb");
        if (!f) return false;
        char buf[1024]{};
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        buf[n] = '\0';
        return parse(buf);
    }

    bool save(const char* path = nullptr) const {
        FILE* f = std::fopen(path ? path : filePath(), "wb");
        if (!f) return false;
        char buf[384];
        serialize(buf, sizeof(buf));
        size_t n = std::strlen(buf);
        size_t wrote = std::fwrite(buf, 1, n, f);
        std::fclose(f);
        return wrote == n;
    }

private:
    static inline const char* testPath_ = nullptr;
};

}  // namespace aw
