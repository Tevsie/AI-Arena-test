// settings.hpp — user settings (master volume, window size/resolution, field of
// view, mouse sensitivity, fullscreen) with clamping and persistence to a small
// key=value text file.
//
// The menu edits these live; the game applies them immediately (audio gain,
// window size, projection FOV, look scale) and saves on menu close / quit /
// restart.
//
// Windowed sizes are *monitor aware*: the selectable sizes are the presets that
// actually fit the display plus the largest window the monitor can show (see
// windowModes()), so picking "1920x1080" on a 1366x768 laptop can never produce
// a window that is bigger than the screen.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace aw {

struct Resolution {
    int w = 1280, h = 720;
};

struct Settings {
    // Resolution presets offered by the menu (ascending). A preset is only
    // selectable when it fits the monitor's usable area; see windowModes().
    static constexpr int kModes = 4;
    static constexpr Resolution kResolutions[kModes] = {
        {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440},
    };
    // Presets + the "largest window that fits the monitor" mode.
    static constexpr int kMaxWindowModes = kModes + 1;

    // Vertical field of view (degrees) and its slider range.
    static constexpr float kFovDefault = 75.0f;   // classic 4:3-era vertical FOV
    static constexpr float kFovMin = 55.0f;
    static constexpr float kFovMax = 110.0f;

    // Window sizing limits (also the smallest window the menu will offer).
    static constexpr int kMinWindowW = 320, kMinWindowH = 200;

    static constexpr const char* kDefaultPath = "settings.cfg";

    float volume = 0.8f;               // master gain, 0..1
    float sensitivity = 1.0f;          // mouse look multiplier, 0.1..3.0
    float fov = kFovDefault;           // vertical FOV, kFovMin..kFovMax
    int width = 1280, height = 720;    // window client size (windowed mode)
    bool fullscreen = false;

    void clamp() {
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 1.0f) volume = 1.0f;
        if (sensitivity < 0.1f) sensitivity = 0.1f;
        if (sensitivity > 3.0f) sensitivity = 3.0f;
        if (fov < kFovMin) fov = kFovMin;
        if (fov > kFovMax) fov = kFovMax;
        if (width < kMinWindowW) width = kMinWindowW;
        if (height < kMinWindowH) height = kMinWindowH;
        if (width > 7680) width = 7680;
        if (height > 4320) height = 4320;
    }

    // ---- monitor-aware window sizes ----------------------------------------
    // `availW x availH` is the largest client size a window may have on the
    // monitor (Platform::maxWindowSize). Anything <= 0 means "monitor unknown"
    // (headless / no display info), in which case presets are never filtered.

    // Writes the selectable window sizes into `out` (at most `cap` entries, in
    // ascending order) and returns the count (never 0). The last entry is the
    // usable monitor area itself — a maximized window — unless it coincides
    // with a preset that fits.
    static int windowModes(Resolution* out, int cap, int availW, int availH) {
        if (!out || cap <= 0) return 0;
        const bool constrained = availW > 0 && availH > 0;
        int n = 0;
        for (int i = 0; i < kModes && n < cap; ++i) {
            if (constrained &&
                (kResolutions[i].w > availW || kResolutions[i].h > availH))
                continue;   // does not fit on this monitor
            out[n++] = kResolutions[i];
        }
        if (constrained && n < cap) {
            Resolution fit{availW, availH};
            if (fit.w < kMinWindowW) fit.w = kMinWindowW;
            if (fit.h < kMinWindowH) fit.h = kMinWindowH;
            if (n == 0 || out[n - 1].w != fit.w || out[n - 1].h != fit.h)
                out[n++] = fit;
        }
        if (n == 0) out[n++] = kResolutions[0];   // never empty
        return n;
    }

    // Index of the mode closest (by area) to w x h, or -1 when empty.
    static int windowModeIndex(const Resolution* modes, int n, int w, int h) {
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

    // Snap width/height onto the closest window size this monitor can show.
    void fitToMonitor(int availW, int availH) {
        Resolution modes[kMaxWindowModes];
        int n = windowModes(modes, kMaxWindowModes, availW, availH);
        int i = windowModeIndex(modes, n, width, height);
        if (i >= 0) { width = modes[i].w; height = modes[i].h; }
        clamp();
    }

    // Step to the next (dir > 0) / previous (dir < 0) selectable size, skipping
    // presets that are too large for the monitor. Wraps around.
    void cycleMode(int dir, int availW, int availH) {
        Resolution modes[kMaxWindowModes];
        int n = windowModes(modes, kMaxWindowModes, availW, availH);
        if (n <= 0) return;
        int area = width * height;
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
        width = modes[pick].w;
        height = modes[pick].h;
        clamp();
    }

    // Serialize to / parse from "key=value" lines. Unknown keys are ignored;
    // missing keys keep their current values. Used by load/save and by tests.
    void serialize(char* out, size_t n) const {
        std::snprintf(out, n,
                      "volume=%.3f\nsensitivity=%.3f\nfov=%.3f\n"
                      "width=%d\nheight=%d\nfullscreen=%d\n",
                      double(volume), double(sensitivity), double(fov),
                      width, height, fullscreen ? 1 : 0);
    }

    bool parse(const char* text) {
        if (!text) return false;
        char line[128];
        const char* p = text;
        while (*p) {
            size_t len = 0;
            while (p[len] && p[len] != '\n' && len + 1 < sizeof(line)) ++len;
            std::memcpy(line, p, len);
            line[len] = '\0';
            p += len + (p[len] == '\n' ? 1 : 0);
            float f = 0.0f;
            int v = 0;
            if (std::sscanf(line, "volume=%f", &f) == 1) volume = f;
            else if (std::sscanf(line, "sensitivity=%f", &f) == 1) sensitivity = f;
            else if (std::sscanf(line, "fov=%f", &f) == 1) fov = f;
            else if (std::sscanf(line, "width=%d", &v) == 1) width = v;
            else if (std::sscanf(line, "height=%d", &v) == 1) height = v;
            else if (std::sscanf(line, "fullscreen=%d", &v) == 1) fullscreen = v != 0;
        }
        clamp();
        return true;
    }

    bool load(const char* path = kDefaultPath) {
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        char buf[1024]{};
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        buf[n] = '\0';
        return parse(buf);
    }

    bool save(const char* path = kDefaultPath) const {
        FILE* f = std::fopen(path, "wb");
        if (!f) return false;
        char buf[256];
        serialize(buf, sizeof(buf));
        size_t n = std::strlen(buf);
        size_t wrote = std::fwrite(buf, 1, n, f);
        std::fclose(f);
        return wrote == n;
    }
};

}  // namespace aw
