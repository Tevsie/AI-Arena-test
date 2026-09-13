// menu.hpp — pause/settings overlay: master volume bar, resolution picker,
// field-of-view bar, mouse sensitivity bar, fullscreen toggle, plus Restart /
// Resume / Quit buttons.
//
// Driven by mouse (hover + click + slider drag) and keyboard (Up/Down/Tab to
// move, Left/Right to adjust, Enter to activate, Esc closes via the game).
// Edits Settings live, applies volume/FOV/window size immediately, and raises
// restart/quit/resume flags consumed by Game. Rendering goes through the
// renderer's immediate-mode UI overlay (pixels, top-left origin).
//
// The resolution row only offers window sizes the monitor can actually show
// (plus a "MAX" entry = the largest window that fits the screen), so changing
// the resolution can never produce a window bigger than the display.
//
// All geometry is expressed in pixels and scaled by the display's UI scale
// (Platform::uiScale, 1.0..2.0 in half steps) so the panel keeps its apparent
// size on high-DPI displays; the scale drops back if the panel would not fit
// the window. Text uses the embedded 5x7 font at an integer scale (2, 3 or 4).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../core/input.hpp"
#include "../core/platform.hpp"
#include "../audio/audio.hpp"
#include "../render/font.hpp"
#include "../render/renderer.hpp"
#include "settings.hpp"

namespace aw {

class Menu {
public:
    enum Item {
        Volume = 0,
        Resolution = 1,
        Fov = 2,
        Sensitivity = 3,
        Fullscreen = 4,
        Restart = 5,
        Resume = 6,
        Quit = 7,
        Count = 8,
    };
    static constexpr int kRows = 5;   // settable rows above the buttons

    void open() {
        selected_ = 0;
        drag_ = -1;
        restart_ = quit_ = resume_ = false;
        mouseHeld_ = false;
        std::memset(prevKeys_, 0, sizeof(prevKeys_));
    }

    bool consumeRestart() { bool r = restart_; restart_ = false; return r; }
    bool consumeQuit() { bool q = quit_; quit_ = false; return q; }
    bool consumeResume() { bool r = resume_; resume_ = false; return r; }
    int selected() const { return selected_; }

    void update(const FrameInput& in, Settings& s, Audio& a, Platform& p) {
        // The window can also be resized outside the menu (border drag,
        // maximize, display change), so mirror the real client size: the
        // resolution row then shows — and the settings save — the actual
        // window. Borderless fullscreen covers the monitor; its size must not
        // overwrite the windowed preference.
        if (!s.fullscreen &&
            in.width >= Settings::kMinWindowW && in.height >= Settings::kMinWindowH &&
            (in.width != s.width || in.height != s.height)) {
            s.width = in.width;
            s.height = in.height;
        }
        computeLayout(in.width, in.height, p.uiScale());
        refreshLabels(s, p);

        mouseHeld_ = (in.mousePressed[MBTN_LEFT] || mouseHeld_) && !in.mouseReleased[MBTN_LEFT];
        bool clicked = in.mousePressed[MBTN_LEFT];

        // Mouse hover selects; click/drag actuates.
        int hover = itemAt(in.mouseX, in.mouseY);
        if (hover >= 0) selected_ = hover;
        if (clicked && hover >= 0) {
            if (isSlider(hover)) {
                drag_ = hover;
                setSliderFromX(hover, in.mouseX, s, a);
                a.play(Sfx::Click);
            } else if (hover == Resolution) {
                float mid = (layout_.ctlX0 + layout_.ctlX1) * 0.5f;
                cycleResolution(in.mouseX < mid ? -1 : +1, s, a, p);
            } else if (hover == Fullscreen) {
                toggleFullscreen(s, a, p);
            } else {
                activate(hover, s, a);
            }
        }
        if (drag_ >= 0) {
            if (mouseHeld_) setSliderFromX(drag_, in.mouseX, s, a);
            else drag_ = -1;
        }

        // Keyboard navigation (edge-triggered).
        auto edge = [&](uint32_t key) -> bool {
            return in.keys[key] != 0 && !prevKeys_[key];
        };
        if (edge(KEY_DOWN) || edge(KEY_TAB)) { selected_ = (selected_ + 1) % Count; a.play(Sfx::Click); }
        if (edge(KEY_UP)) { selected_ = (selected_ + Count - 1) % Count; a.play(Sfx::Click); }
        if (edge(KEY_LEFT)) { adjust(selected_, -1, s, a, p); }
        if (edge(KEY_RIGHT)) { adjust(selected_, +1, s, a, p); }
        if (edge(KEY_ENTER)) {
            if (selected_ == Resolution) cycleResolution(+1, s, a, p);
            else if (selected_ == Fullscreen) toggleFullscreen(s, a, p);
            else if (isSlider(selected_)) a.play(Sfx::Click);
            else activate(selected_, s, a);
        }
        std::memcpy(prevKeys_, in.keys, sizeof(prevKeys_));
        // The resolution row may have resized the window a moment ago: re-lay
        // the panel out for the new client size so this very frame is drawn
        // (and hit-tested) correctly instead of jumping on the next one.
        int cw = 0, ch = 0;
        if (p.clientSize(cw, ch) && (cw != layout_.winW || ch != layout_.winH))
            computeLayout(cw, ch, wantedUi_);
        refreshLabels(s, p);
    }

    void render(Renderer& r) const {
        const Layout& L = layout_;
        const int ts = L.ts;
        const float k = L.ui;   // pixel constants are authored for a 1.0 scale
        // Dim + panel + border.
        r.uiRect(0, 0, float(L.winW), float(L.winH), 0, 0, 0, 0.62f);
        r.uiRect(L.px, L.py, L.pw, L.ph, 0.07f, 0.08f, 0.11f, 0.97f);
        r.uiRect(L.px, L.py, L.pw, 2 * k, 0.25f, 0.55f, 0.95f, 1);
        r.uiRect(L.px, L.py + L.ph - 2 * k, L.pw, 2 * k, 0.25f, 0.55f, 0.95f, 1);
        r.uiRect(L.px, L.py, 2 * k, L.ph, 0.25f, 0.55f, 0.95f, 1);
        r.uiRect(L.px + L.pw - 2 * k, L.py, 2 * k, L.ph, 0.25f, 0.55f, 0.95f, 1);

        drawCentered(r, "SETTINGS", ts + 1, L.px + L.pw * 0.5f, L.py + 26 * k, 1, 1, 1);

        static const char* kLabels[kRows] = {
            "MASTER VOLUME", "RESOLUTION", "FOV (DEGREES)", "MOUSE SENSITIVITY", "FULLSCREEN",
        };
        for (int i = 0; i < kRows; ++i) {
            bool sel = (selected_ == i);
            float cr = sel ? 1.0f : 0.62f, cg = sel ? 1.0f : 0.65f, cb = sel ? 1.0f : 0.70f;
            float ty = L.rowY[i] + 6 * k;
            r.uiText(L.px + 28 * k, ty, ts, cr, cg, cb, 1, kLabels[i]);
            if (i == Resolution) {
                float cx = (L.ctlX0 + L.ctlX1) * 0.5f;
                float cw = float(textWidthPx(resText_, ts));
                if (sel)
                    r.uiRect(cx - cw * 0.5f - 10 * k, L.rowY[i], cw + 20 * k, 30 * k,
                             0.22f, 0.35f, 0.55f, 1);
                drawCentered(r, resText_, ts, cx, ty, 0.88f, 0.92f, 0.96f);
            } else if (i == Fullscreen) {
                float cx = (L.ctlX0 + L.ctlX1) * 0.5f;
                float cw = float(textWidthPx(fsText_, ts));
                if (sel)
                    r.uiRect(cx - cw * 0.5f - 10 * k, L.rowY[i], cw + 20 * k, 30 * k,
                             0.22f, 0.35f, 0.55f, 1);
                drawCentered(r, fsText_, ts, cx, ty, 0.88f, 0.92f, 0.96f);
            } else {
                const char* val = valueText(i);
                float vw = float(textWidthPx(val, ts));
                r.uiText(L.valX - vw, ty, ts, 0.88f, 0.92f, 0.96f, 1, val);
                drawSlider(r, L.rowY[i], sliderFrac(i), sel);
            }
        }

        static const char* kButtons[3] = {"RESTART", "RESUME", "QUIT"};
        for (int i = 0; i < 3; ++i) {
            bool sel = (selected_ == Restart + i);
            if (sel)
                r.uiRect(L.btnX - 3 * k, L.btnY[i] - 3 * k, L.btnW + 6 * k, L.btnH + 6 * k,
                         0.25f, 0.55f, 0.95f, 1);
            r.uiRect(L.btnX, L.btnY[i], L.btnW, L.btnH, sel ? 0.22f : 0.13f, sel ? 0.35f : 0.15f,
                     sel ? 0.55f : 0.20f, 1);
            drawCentered(r, kButtons[i], ts, L.btnX + L.btnW * 0.5f, L.btnY[i] + 10 * k, 1, 1, 1);
        }

        drawCentered(r, "ARROWS SELECT - ENTER APPLY - ESC CLOSE", ts > 1 ? ts - 1 : 1,
                     L.px + L.pw * 0.5f, L.py + L.ph - 28 * k, 0.45f, 0.48f, 0.52f);
    }

    // Largest UI scale (from the half steps 2.0, 1.5, 1.0) at which the panel
    // still fits inside a window of `w` x `h` pixels.
    static float fitUiScale(float wanted, int w, int h) {
        float ui = quantizeUiScale(wanted);
        while (ui > 1.0f &&
               (Layout::baseW * ui + 8.0f > float(w) || Layout::baseH * ui + 8.0f > float(h)))
            ui -= 0.5f;
        return ui < 1.0f ? 1.0f : ui;
    }

private:
    struct Layout {
        // Panel size at UI scale 1.0 (scaled by `ui` at layout time).
        static constexpr float baseW = 560, baseH = 560;
        int winW = 0, winH = 0;
        float ui = 1.0f;
        int ts = 2;                     // text scale (integer, 2..4)
        float pw = baseW, ph = baseH;
        float px = 0, py = 0;
        float rowY[kRows]{};
        float barX = 0, barW = 0, barH = 12;
        float valX = 0;                 // value text right edge
        float ctlX0 = 0, ctlX1 = 0;     // control zone for the resolution text
        float btnX = 0, btnW = 240, btnH = 38, btnY[3]{};
    };

    static bool isSlider(int item) {
        return item == Volume || item == Fov || item == Sensitivity;
    }

    void computeLayout(int w, int h, float wantedScale) {
        Layout& L = layout_;
        L.winW = w;
        L.winH = h;
        wantedUi_ = wantedScale;              // remembered for re-layouts
        L.ui = fitUiScale(wantedScale, w, h);
        const float k = L.ui;
        L.ts = int(k * 2.0f + 0.5f);           // 1.0 -> 2, 1.5 -> 3, 2.0 -> 4
        if (L.ts < 2) L.ts = 2;
        L.pw = Layout::baseW * k;
        L.ph = Layout::baseH * k;
        L.px = float(w > int(L.pw) ? (w - int(L.pw)) / 2 : 4);
        L.py = float(h > int(L.ph) ? (h - int(L.ph)) / 2 : 4);
        float y0 = L.py + 92 * k;
        for (int i = 0; i < kRows; ++i) L.rowY[i] = y0 + float(i) * 54 * k;
        L.valX = L.px + L.pw - 28 * k;
        L.barX = L.px + 300 * k;
        L.barW = L.valX - 64 * k - L.barX;  // room for the right-aligned value
        if (L.barW < 40 * k) L.barW = 40 * k;
        L.barH = 12 * k;
        L.ctlX0 = L.px + 290 * k;
        L.ctlX1 = L.px + L.pw - 28 * k;
        L.btnW = 240 * k;
        L.btnH = 38 * k;
        L.btnX = L.px + (L.pw - L.btnW) * 0.5f;
        float by = y0 + float(kRows) * 54 * k + 18 * k;
        for (int i = 0; i < 3; ++i) L.btnY[i] = by + float(i) * (L.btnH + 12 * k);
    }

    int itemAt(float mx, float my) const {
        const Layout& L = layout_;
        const float k = L.ui;
        for (int i = 0; i < kRows; ++i) {
            if (mx >= L.px + 12 * k && mx <= L.px + L.pw - 12 * k &&
                my >= L.rowY[i] - 4 * k && my <= L.rowY[i] + 44 * k)
                return i;
        }
        for (int i = 0; i < 3; ++i) {
            if (mx >= L.btnX && mx <= L.btnX + L.btnW &&
                my >= L.btnY[i] && my <= L.btnY[i] + L.btnH)
                return Restart + i;
        }
        return -1;
    }

    void setSliderFromX(int item, float mx, Settings& s, Audio& a) {
        float frac = (mx - layout_.barX) / layout_.barW;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        if (item == Volume) {
            s.volume = frac;
            a.setVolume(frac);
        } else if (item == Fov) {
            s.fov = Settings::kFovMin + frac * (Settings::kFovMax - Settings::kFovMin);
            s.clamp();
        } else if (item == Sensitivity) {
            s.sensitivity = 0.1f + frac * (3.0f - 0.1f);
            s.clamp();
        }
    }

    void adjust(int item, int dir, Settings& s, Audio& a, Platform& p) {
        if (item == Volume) {
            s.volume += float(dir) * 0.05f;
            s.clamp();
            a.setVolume(s.volume);
            a.play(Sfx::Click);
        } else if (item == Fov) {
            s.fov += float(dir) * 2.0f;
            s.clamp();
            a.play(Sfx::Click);
        } else if (item == Sensitivity) {
            s.sensitivity += float(dir) * 0.1f;
            s.clamp();
            a.play(Sfx::Click);
        } else if (item == Resolution) {
            cycleResolution(dir, s, a, p);
        } else if (item == Fullscreen) {
            toggleFullscreen(s, a, p);
        }
    }

    void cycleResolution(int dir, Settings& s, Audio& a, Platform& p) {
        int availW = 0, availH = 0;
        p.maxWindowSize(availW, availH);
        s.cycleMode(dir, availW, availH);   // skips sizes the monitor cannot show
        applyWindowSize(s, p);
        a.play(Sfx::Click);
    }

    // Borderless fullscreen already covers the monitor, so only a windowed
    // window is resized here.
    void applyWindowSize(Settings& s, Platform& p) {
        if (!s.fullscreen) p.resize(s.width, s.height);
    }

    void toggleFullscreen(Settings& s, Audio& a, Platform& p) {
        s.fullscreen = !s.fullscreen;
        p.setFullscreen(s.fullscreen);
        if (!s.fullscreen) p.resize(s.width, s.height);  // back to the chosen size
        a.play(Sfx::Click);
    }

    void activate(int item, Settings&, Audio& a) {
        if (item == Restart) restart_ = true;   // game plays the Restart sound
        else if (item == Resume) { resume_ = true; a.play(Sfx::Click); }
        else if (item == Quit) { quit_ = true; a.play(Sfx::Click); }
    }

    const char* valueText(int item) const {
        if (item == Volume) return volText_;
        if (item == Fov) return fovText_;
        return sensText_;
    }

    float sliderFrac(int item) const {
        if (item == Volume) return lastVolFrac_;
        if (item == Fov) return lastFovFrac_;
        return lastSensFrac_;
    }

    void refreshLabels(Settings& s, Platform& p) {
        s.clamp();
        lastVolFrac_ = s.volume;
        lastFovFrac_ = (s.fov - Settings::kFovMin) / (Settings::kFovMax - Settings::kFovMin);
        lastSensFrac_ = (s.sensitivity - 0.1f) / (3.0f - 0.1f);
        std::snprintf(volText_, sizeof(volText_), "%d%%", int(s.volume * 100.0f + 0.5f));
        std::snprintf(fovText_, sizeof(fovText_), "%d", int(s.fov + 0.5f));
        std::snprintf(sensText_, sizeof(sensText_), "%d%%", int(s.sensitivity * 100.0f + 0.5f));
        // "MAX" = the largest window this monitor can show.
        int availW = 0, availH = 0;
        p.maxWindowSize(availW, availH);
        bool atMonitorMax = availW > 0 && availH > 0 && s.width >= availW && s.height >= availH;
        std::snprintf(resText_, sizeof(resText_),
                      atMonitorMax ? "< %dx%d MAX >" : "< %dx%d >", s.width, s.height);
        std::snprintf(fsText_, sizeof(fsText_), "< %s >", s.fullscreen ? "ON" : "OFF");
    }

    void drawSlider(Renderer& r, float rowY, float frac, bool sel) const {
        const Layout& L = layout_;
        const float k = L.ui;
        float y = rowY + 8 * k;
        r.uiRect(L.barX, y, L.barW, L.barH, 0.16f, 0.18f, 0.22f, 1);
        float fw = L.barW * frac;
        if (fw > 0.5f)
            r.uiRect(L.barX, y, fw, L.barH, sel ? 0.30f : 0.22f, sel ? 0.62f : 0.42f,
                     sel ? 1.0f : 0.70f, 1);
        float kw = 8 * k;
        float kx = L.barX + fw - kw * 0.5f;
        if (kx < L.barX) kx = L.barX;
        if (kx > L.barX + L.barW - kw) kx = L.barX + L.barW - kw;
        r.uiRect(kx, y - 4 * k, kw, L.barH + 8 * k, 0.92f, 0.94f, 0.97f, 1);
    }

    static void drawCentered(Renderer& r, const char* s, int scale, float cx, float y,
                             float cr, float cg, float cb) {
        r.uiText(cx - float(textWidthPx(s, scale)) * 0.5f, y, scale, cr, cg, cb, 1, s);
    }

    Layout layout_;
    float wantedUi_ = 1.0f;   // scale requested by the display, before fitting
    char volText_[8]{}, fovText_[8]{}, sensText_[8]{}, resText_[32]{}, fsText_[12]{};
    float lastVolFrac_ = 0.8f, lastFovFrac_ = 0.36f, lastSensFrac_ = 0.5f;
    int selected_ = 0;
    int drag_ = -1;
    bool mouseHeld_ = false;
    bool prevKeys_[512]{};
    bool restart_ = false, quit_ = false, resume_ = false;
};

}  // namespace aw
