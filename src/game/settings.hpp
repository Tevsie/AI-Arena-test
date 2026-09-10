// settings.hpp — user settings (master volume, resolution, mouse sensitivity)
// with clamping and persistence to a small key=value text file.
//
// The menu edits these live; the game applies them immediately (audio gain,
// window size, look scale) and saves on menu close / quit / restart.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace aw {

struct Resolution {
    int w = 1280, h = 720;
};

struct Settings {
    static constexpr int kModes = 4;
    static constexpr Resolution kResolutions[kModes] = {
        {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440},
    };
    static constexpr const char* kDefaultPath = "settings.cfg";

    float volume = 0.8f;       // master gain, 0..1
    float sensitivity = 1.0f;  // mouse look multiplier, 0.1..3.0
    int width = 1280, height = 720;

    void clamp() {
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 1.0f) volume = 1.0f;
        if (sensitivity < 0.1f) sensitivity = 0.1f;
        if (sensitivity > 3.0f) sensitivity = 3.0f;
        if (width < 320) width = 320;
        if (height < 200) height = 200;
        if (width > 7680) width = 7680;
        if (height > 4320) height = 4320;
    }

    // Index of the current resolution in kResolutions (nearest by area on miss).
    int modeIndex() const {
        for (int i = 0; i < kModes; ++i)
            if (kResolutions[i].w == width && kResolutions[i].h == height) return i;
        int best = 0, bestDiff = 0x7fffffff;
        int area = width * height;
        for (int i = 0; i < kModes; ++i) {
            int d = kResolutions[i].w * kResolutions[i].h - area;
            if (d < 0) d = -d;
            if (d < bestDiff) { bestDiff = d; best = i; }
        }
        return best;
    }

    void setMode(int i) {
        if (i < 0) i = 0;
        if (i >= kModes) i = kModes - 1;
        width = kResolutions[i].w;
        height = kResolutions[i].h;
    }

    void cycleMode(int dir) {
        int i = (modeIndex() + dir) % kModes;
        if (i < 0) i += kModes;
        setMode(i);
    }

    // Serialize to / parse from "key=value" lines. Unknown keys are ignored;
    // missing keys keep their current values. Used by load/save and by tests.
    void serialize(char* out, size_t n) const {
        std::snprintf(out, n, "volume=%.3f\nsensitivity=%.3f\nwidth=%d\nheight=%d\n",
                      double(volume), double(sensitivity), width, height);
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
            else if (std::sscanf(line, "width=%d", &v) == 1) width = v;
            else if (std::sscanf(line, "height=%d", &v) == 1) height = v;
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
