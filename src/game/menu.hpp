// menu.hpp — pause/settings overlay: master volume bar, resolution picker,
// mouse sensitivity bar, fullscreen toggle, plus Restart / Resume / Quit buttons.
//
// Driven by mouse (hover + click + slider drag) and keyboard (Up/Down/Tab to
// move, Left/Right to adjust, Enter to activate, Esc closes via the game).
// Edits Settings live, applies volume/resolution immediately, and raises
// restart/quit/resume flags consumed by Game. Rendering goes through the
// renderer's immediate-mode UI overlay (pixels, top-left origin).
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
    enum Item { Volume = 0, Resolution = 1, Sensitivity = 2, Fullscreen = 3, Restart = 4, Resume = 5, Quit = 6, Count = 7 };

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
        computeLayout(in.width, in.height);
        refreshLabels(s);

        mouseHeld_ = (in.mousePressed[MBTN_LEFT] || mouseHeld_) && !in.mouseReleased[MBTN_LEFT];
        bool clicked = in.mousePressed[MBTN_LEFT];

        // Mouse hover selects; click/drag actuates.
        int hover = itemAt(in.mouseX, in.mouseY);
        if (hover >= 0) selected_ = hover;
        if (clicked && hover >= 0) {
            if (hover == Volume || hover == Sensitivity) {
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
            else if (selected_ == Volume || selected_ == Sensitivity) a.play(Sfx::Click);
            else activate(selected_, s, a);
        }
        std::memcpy(prevKeys_, in.keys, sizeof(prevKeys_));
        refreshLabels(s);
    }

    void render(Renderer& r) const {
        const Layout& L = layout_;
        // Dim + panel + border.
        r.uiRect(0, 0, float(L.winW), float(L.winH), 0, 0, 0, 0.62f);
        r.uiRect(L.px, L.py, L.pw, L.ph, 0.07f, 0.08f, 0.11f, 0.97f);
        r.uiRect(L.px, L.py, L.pw, 2, 0.25f, 0.55f, 0.95f, 1);
        r.uiRect(L.px, L.py + L.ph - 2, L.pw, 2, 0.25f, 0.55f, 0.95f, 1);
        r.uiRect(L.px, L.py, 2, L.ph, 0.25f, 0.55f, 0.95f, 1);
        r.uiRect(L.px + L.pw - 2, L.py, 2, L.ph, 0.25f, 0.55f, 0.95f, 1);

        drawCentered(r, "SETTINGS", 3, L.px + L.pw * 0.5f, L.py + 26, 1, 1, 1);

        static const char* kLabels[4] = {"MASTER VOLUME", "RESOLUTION", "MOUSE SENSITIVITY", "FULLSCREEN"};
        for (int i = 0; i < 4; ++i) {
            bool sel = (selected_ == i);
            float cr = sel ? 1.0f : 0.62f, cg = sel ? 1.0f : 0.65f, cb = sel ? 1.0f : 0.70f;
            r.uiText(L.px + 28, L.rowY[i] + 6, 2, cr, cg, cb, 1, kLabels[i]);
            if (i == Resolution) {
                float cx = (L.ctlX0 + L.ctlX1) * 0.5f;
                float cw = float(textWidthPx(resText_, 2));
                if (sel) r.uiRect(cx - cw * 0.5f - 10, L.rowY[i], cw + 20, 30, 0.22f, 0.35f, 0.55f, 1);
                drawCentered(r, resText_, 2, cx, L.rowY[i] + 6, 0.88f, 0.92f, 0.96f);
            } else if (i == Fullscreen) {
                float cx = (L.ctlX0 + L.ctlX1) * 0.5f;
                float cw = float(textWidthPx(fsText_, 2));
                if (sel) r.uiRect(cx - cw * 0.5f - 10, L.rowY[i], cw + 20, 30, 0.22f, 0.35f, 0.55f, 1);
                drawCentered(r, fsText_, 2, cx, L.rowY[i] + 6, 0.88f, 0.92f, 0.96f);
            } else {
                const char* val = (i == Volume) ? volText_ : sensText_;
                float vw = float(textWidthPx(val, 2));
                r.uiText(L.valX - vw, L.rowY[i] + 6, 2, 0.88f, 0.92f, 0.96f, 1, val);
                drawSlider(r, i, (i == Volume) ? lastVolFrac_ : lastSensFrac_, sel);
            }
        }

        static const char* kButtons[3] = {"RESTART", "RESUME", "QUIT"};
        for (int i = 0; i < 3; ++i) {
            bool sel = (selected_ == Restart + i);
            if (sel) r.uiRect(L.btnX - 3, L.btnY[i] - 3, L.btnW + 6, L.btnH + 6, 0.25f, 0.55f, 0.95f, 1);
            r.uiRect(L.btnX, L.btnY[i], L.btnW, L.btnH, sel ? 0.22f : 0.13f, sel ? 0.35f : 0.15f,
                     sel ? 0.55f : 0.20f, 1);
            drawCentered(r, kButtons[i], 2, L.btnX + L.btnW * 0.5f, L.btnY[i] + 10, 1, 1, 1);
        }

        drawCentered(r, "ARROWS SELECT - ENTER APPLY - ESC CLOSE", 1,
                     L.px + L.pw * 0.5f, L.py + L.ph - 28, 0.45f, 0.48f, 0.52f);
    }

private:
    struct Layout {
        static constexpr float pw = 520, ph = 500;
        int winW = 0, winH = 0;
        float px = 0, py = 0;
        float rowY[4]{};
        float barX = 0, barW = 0, barH = 12;
        float valX = 0;                 // value text right edge
        float ctlX0 = 0, ctlX1 = 0;     // control zone for the resolution text
        float btnX = 0, btnW = 240, btnH = 38, btnY[3]{};
    };

    void computeLayout(int w, int h) {
        Layout& L = layout_;
        L.winW = w;
        L.winH = h;
        L.px = float(w > Layout::pw ? (w - int(Layout::pw)) / 2 : 4);
        L.py = float(h > Layout::ph ? (h - int(Layout::ph)) / 2 : 4);
        float y0 = L.py + 92;
        for (int i = 0; i < 4; ++i) L.rowY[i] = y0 + float(i) * 54;
        L.valX = L.px + Layout::pw - 28;
        L.barX = L.px + 300;
        L.barW = L.valX - 64 - L.barX;  // room for the right-aligned value
        if (L.barW < 40) L.barW = 40;
        L.ctlX0 = L.px + 290;
        L.ctlX1 = L.px + Layout::pw - 28;
        L.btnX = L.px + (Layout::pw - L.btnW) * 0.5f;
        float by = y0 + 4 * 54 + 18;
        for (int i = 0; i < 3; ++i) L.btnY[i] = by + float(i) * (L.btnH + 12);
    }

    int itemAt(float mx, float my) const {
        const Layout& L = layout_;
        for (int i = 0; i < 4; ++i) {
            if (mx >= L.px + 12 && mx <= L.px + Layout::pw - 12 &&
                my >= L.rowY[i] - 4 && my <= L.rowY[i] + 44)
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
        } else if (item == Sensitivity) {
            s.sensitivity = 0.1f + frac * (3.0f - 0.1f);
        }
    }

    void adjust(int item, int dir, Settings& s, Audio& a, Platform& p) {
        if (item == Volume) {
            s.volume += float(dir) * 0.05f;
            s.clamp();
            a.setVolume(s.volume);
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
        s.cycleMode(dir);
        p.resize(s.width, s.height);
        a.play(Sfx::Click);
    }

    void toggleFullscreen(Settings& s, Audio& a, Platform& p) {
        s.fullscreen = !s.fullscreen;
        p.setFullscreen(s.fullscreen);
        a.play(Sfx::Click);
    }

    void activate(int item, Settings&, Audio& a) {
        if (item == Restart) restart_ = true;   // game plays the Restart sound
        else if (item == Resume) { resume_ = true; a.play(Sfx::Click); }
        else if (item == Quit) { quit_ = true; a.play(Sfx::Click); }
    }

    void refreshLabels(Settings& s) {
        s.clamp();
        lastVolFrac_ = s.volume;
        lastSensFrac_ = (s.sensitivity - 0.1f) / (3.0f - 0.1f);
        std::snprintf(volText_, sizeof(volText_), "%d%%", int(s.volume * 100.0f + 0.5f));
        std::snprintf(sensText_, sizeof(sensText_), "%d%%", int(s.sensitivity * 100.0f + 0.5f));
        std::snprintf(resText_, sizeof(resText_), "< %dx%d >", s.width, s.height);
        std::snprintf(fsText_, sizeof(fsText_), "< %s >", s.fullscreen ? "ON" : "OFF");
    }

    void drawSlider(Renderer& r, int item, float frac, bool sel) const {
        const Layout& L = layout_;
        float y = layout_.rowY[item] + 8;
        r.uiRect(L.barX, y, L.barW, L.barH, 0.16f, 0.18f, 0.22f, 1);
        float fw = L.barW * frac;
        if (fw > 0.5f)
            r.uiRect(L.barX, y, fw, L.barH, sel ? 0.30f : 0.22f, sel ? 0.62f : 0.42f,
                     sel ? 1.0f : 0.70f, 1);
        float kx = L.barX + fw - 4;
        if (kx < L.barX) kx = L.barX;
        if (kx > L.barX + L.barW - 8) kx = L.barX + L.barW - 8;
        r.uiRect(kx, y - 4, 8, L.barH + 8, 0.92f, 0.94f, 0.97f, 1);
    }

    static void drawCentered(Renderer& r, const char* s, int scale, float cx, float y,
                             float cr, float cg, float cb) {
        r.uiText(cx - float(textWidthPx(s, scale)) * 0.5f, y, scale, cr, cg, cb, 1, s);
    }

    Layout layout_;
    char volText_[8]{}, sensText_[8]{}, resText_[24]{}, fsText_[12]{};
    float lastVolFrac_ = 0.8f, lastSensFrac_ = 0.5f;
    int selected_ = 0;
    int drag_ = -1;
    bool mouseHeld_ = false;
    bool prevKeys_[512]{};
    bool restart_ = false, quit_ = false, resume_ = false;
};

}  // namespace aw
