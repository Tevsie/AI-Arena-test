// menu.hpp — pause/settings overlay: master volume, display mode, resolution,
// refresh rate, field of view and mouse sensitivity, plus Restart / Resume /
// Quit buttons.
//
// Driven by mouse (hover + click + slider drag) and keyboard (Up/Down/Tab to
// move, Left/Right to adjust, Enter to activate, Esc closes via the game).
// Edits Settings live and raises apply requests consumed by Game:
//   * volume / FOV / sensitivity apply instantly,
//   * the display rows (mode / resolution / refresh) only change the *pending*
//     values and set displayApply_: Game applies them and opens the modal
//     confirmation dialog, so an unsupported mode can never leave the user with
//     a blind screen (see display_confirm.hpp).
//
// Resolution rows are list driven and depend on the display mode:
//   Windowed   "WINDOW SIZE"  — standard client sizes (HD..4K) that fit the
//                               desktop minus decorations/taskbar; a leftover
//                               non-standard value is snapped to the largest
//                               standard size the screen can show.
//   Borderless "RENDER RES"   — internal 3D render resolution (up to 4K, above
//                               native = supersampling) with a scale percentage;
//                               the window itself stays at the native size.
//   Exclusive  "DISPLAY RES"  — the driver's supported display modes, with a
//                               hardware mode switch (resolution + refresh).
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
        Mode = 1,
        Resolution = 2,
        Refresh = 3,
        Fov = 4,
        Sensitivity = 5,
        Restart = 6,
        Resume = 7,
        Quit = 8,
        Count = 9,
    };
    static constexpr int kRows = 6;   // settable rows above the buttons

    void open() {
        selected_ = 0;
        drag_ = -1;
        restart_ = quit_ = resume_ = false;
        displayApply_ = false;
        mouseHeld_ = false;
        notice_[0] = '\0';
        std::memset(prevKeys_, 0, sizeof(prevKeys_));
    }

    bool consumeRestart() { bool r = restart_; restart_ = false; return r; }
    bool consumeQuit() { bool q = quit_; quit_ = false; return q; }
    bool consumeResume() { bool r = resume_; resume_ = false; return r; }
    // True when the display rows were changed and the new configuration should
    // be applied provisionally (Game applies it and starts the confirm dialog).
    bool consumeDisplayApply() { bool a = displayApply_; displayApply_ = false; return a; }
    int selected() const { return selected_; }

    // One-line status shown instead of the key hint (e.g. when a display change
    // was refused). Cleared as soon as the user touches the display rows again.
    void setNotice(const char* text) {
        std::snprintf(notice_, sizeof(notice_), "%.*s", int(sizeof(notice_) - 1),
                      text ? text : "");
    }

    // Keeps the keyboard/mouse edge state current while something else owns the
    // input (the confirmation dialog). Without this the key that dismisses the
    // dialog (Enter) would look like a *fresh* press to the menu on the next
    // frame and step the resolution a second time — one action, two changes and
    // two dialogs. The menu is frozen, but it must not re-fire stale keys.
    void syncInput(const FrameInput& in) {
        std::memcpy(prevKeys_, in.keys, sizeof(prevKeys_));
        mouseHeld_ = in.mousePressed[MBTN_LEFT] && !in.mouseReleased[MBTN_LEFT];
    }

    void update(const FrameInput& in, Settings& s, Audio& a, Platform& p) {
        // The window size is *not* mirrored from the live client size: the
        // resolution setting is always one of the standard presets (HD..4K), and
        // a backend that had to clamp the window to the screen must not turn
        // that into a made-up resolution like 1003x986 (nor make the setting
        // disagree with what was just applied, which re-triggered an apply).
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
            } else if (hover >= Restart) {
                activate(hover, s, a);            // Restart / Resume / Quit
            } else {
                // Selectors step towards the side that was clicked.
                float mid = rowControlMid(hover);
                adjust(hover, in.mouseX < mid ? -1 : +1, s, a, p);
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
            if (isSlider(selected_)) a.play(Sfx::Click);
            else if (selected_ == Mode || selected_ == Resolution || selected_ == Refresh)
                adjust(selected_, +1, s, a, p);   // step the selector
            else activate(selected_, s, a);       // buttons
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

        for (int i = 0; i < kRows; ++i) {
            bool sel = (selected_ == i);
            bool enabled = rowEnabled(i);
            float cr = sel ? 1.0f : 0.62f, cg = sel ? 1.0f : 0.65f, cb = sel ? 1.0f : 0.70f;
            if (!enabled) { cr *= 0.5f; cg *= 0.5f; cb *= 0.5f; }
            float ty = L.rowY[i] + 6 * k;
            r.uiText(L.px + 28 * k, ty, ts, cr, cg, cb, 1, rowLabel(i));
            const char* val = valueText(i);
            if (isSlider(i)) {
                float vw = float(textWidthPx(val, ts));
                r.uiText(L.valX - vw, ty, ts, 0.88f, 0.92f, 0.96f, 1, val);
                drawSlider(r, L.rowY[i], sliderFrac(i), sel && enabled);
            } else {
                float cx = (L.ctlX0 + L.ctlX1) * 0.5f;
                float cw = float(textWidthPx(val, ts));
                if (sel && enabled)
                    r.uiRect(cx - cw * 0.5f - 10 * k, L.rowY[i], cw + 20 * k, 30 * k,
                             0.22f, 0.35f, 0.55f, 1);
                float vr = enabled ? 0.88f : 0.45f, vg = enabled ? 0.92f : 0.47f,
                      vb = enabled ? 0.96f : 0.50f;
                drawCentered(r, val, ts, cx, ty, vr, vg, vb);
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

        if (notice_[0]) {
            drawCentered(r, notice_, ts > 1 ? ts - 1 : 1, L.px + L.pw * 0.5f,
                         L.py + L.ph - 28 * k, 0.95f, 0.62f, 0.30f);
        } else {
            drawCentered(r, "ARROWS SELECT - ENTER APPLY - ESC CLOSE", ts > 1 ? ts - 1 : 1,
                         L.px + L.pw * 0.5f, L.py + L.ph - 28 * k, 0.45f, 0.48f, 0.52f);
        }
    }

    // Centre of a row or button in window pixels (from the last layout).
    void itemCenter(int item, float& x, float& y) const {
        const Layout& L = layout_;
        if (item >= 0 && item < kRows) {
            x = (L.ctlX0 + L.ctlX1) * 0.5f;
            y = L.rowY[item] + 12 * L.ui;
        } else {
            int b = item - Restart;
            if (b < 0) b = 0;
            if (b > 2) b = 2;
            x = L.btnX + L.btnW * 0.5f;
            y = L.btnY[b] + L.btnH * 0.5f;
        }
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
        static constexpr float baseW = 620, baseH = 620;
        int winW = 0, winH = 0;
        float ui = 1.0f;
        int ts = 2;                     // text scale (integer, 2..4)
        float pw = baseW, ph = baseH;
        float px = 0, py = 0;
        float rowSpacing = 54;
        float rowY[kRows]{};
        float barX = 0, barW = 0, barH = 12;
        float valX = 0;                 // value text right edge
        float ctlX0 = 0, ctlX1 = 0;     // control zone for the value texts
        float btnX = 0, btnW = 240, btnH = 38, btnY[3]{};
    };

    static bool isSlider(int item) {
        return item == Volume || item == Fov || item == Sensitivity;
    }

    bool rowEnabled(int item) const {
        if (item == Refresh) return pending_.mode == DisplayMode::Exclusive;
        if (item == Resolution) return resolutionCount() > 0;
        return true;
    }

    const char* rowLabel(int item) const {
        switch (item) {
            case Volume: return "MASTER VOLUME";
            case Mode: return "DISPLAY MODE";
            case Resolution:
                if (pending_.mode == DisplayMode::Windowed) return "WINDOW SIZE";
                if (pending_.mode == DisplayMode::Borderless) return "RENDER RES";
                return "DISPLAY RES";
            case Refresh: return "REFRESH RATE";
            case Fov: return "FOV (DEGREES)";
            default: return "MOUSE SENSITIVITY";
        }
    }

    // ---- resolution lists (fixed pools, no allocation) ----------------------
    void fillResolutionList(const Settings& s, Platform& p) {
        resCount_ = 0;
        switch (s.mode) {
            case DisplayMode::Windowed: {
                int aw = 0, ah = 0;
                p.maxWindowSize(aw, ah);
                resCount_ = Settings::windowModes(resolutions_, Settings::kMaxModes, aw, ah);
                break;
            }
            case DisplayMode::Borderless: {
                int nw = 0, nh = 0;
                p.monitorSize(nw, nh);
                resCount_ = Settings::renderModes(resolutions_, Settings::kMaxModes, nw, nh);
                break;
            }
            case DisplayMode::Exclusive:
            default: {
                int n = p.displayModeCount();
                if (n > Settings::kMaxDisplayModes) n = Settings::kMaxDisplayModes;
                driverCount_ = 0;
                DisplayModeInfo info;
                for (int i = 0; i < n; ++i)
                    if (p.displayModeAt(i, info)) driverModes_[driverCount_++] = info;
                resCount_ = Settings::exclusiveModes(resolutions_, Settings::kMaxModes,
                                                     driverModes_, driverCount_);
                break;
            }
        }
    }

    int resolutionCount() const { return resCount_; }

    // ---------------------------------------------------------------- layout
    void computeLayout(int w, int h, float wantedScale) {
        Layout& L = layout_;
        L.winW = w;
        L.winH = h;
        wantedUi_ = wantedScale;              // remembered for re-layouts
        L.ui = fitUiScale(wantedScale, w, h);
        const float k = L.ui;
        L.ts = int(k * 2.0f + 0.5f);           // 1.0 -> 2, 1.5 -> 3, 2.0 -> 4
        if (L.ts < 2) L.ts = 2;

        // Rows keep their 54 px rhythm when there is room; on shorter windows
        // they compress (down to 34 px) so the panel and the buttons stay usable.
        const float fixed = (92 + 16 + 3 * 50 + 28) * k;   // header + buttons + footer
        float spacing = 54 * k;
        const float avail = float(h) - 8 * k;
        if (fixed + kRows * spacing > avail) spacing = (avail - fixed) / float(kRows);
        if (spacing < 34 * k) spacing = 34 * k;
        L.rowSpacing = spacing;
        L.ph = fixed + kRows * spacing;
        L.pw = Layout::baseW * k;
        if (L.ph < 120 * k) L.ph = 120 * k;
        L.px = float(w > int(L.pw) ? (w - int(L.pw)) / 2 : 4);
        L.py = float(h > int(L.ph) ? (h - int(L.ph)) / 2 : 4);
        float y0 = L.py + 92 * k;
        for (int i = 0; i < kRows; ++i) L.rowY[i] = y0 + float(i) * spacing;
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
        float by = y0 + float(kRows) * spacing + 16 * k;
        for (int i = 0; i < 3; ++i) L.btnY[i] = by + float(i) * 50 * k;
    }

    int itemAt(float mx, float my) const {
        const Layout& L = layout_;
        const float k = L.ui;
        float rowH = L.rowSpacing > 44 * k ? 44 * k : L.rowSpacing - 6 * k;
        for (int i = 0; i < kRows; ++i) {
            if (mx >= L.px + 12 * k && mx <= L.px + L.pw - 12 * k &&
                my >= L.rowY[i] - 4 * k && my <= L.rowY[i] + rowH)
                return i;
        }
        for (int i = 0; i < 3; ++i) {
            if (mx >= L.btnX && mx <= L.btnX + L.btnW &&
                my >= L.btnY[i] && my <= L.btnY[i] + L.btnH)
                return Restart + i;
        }
        return -1;
    }

    float rowControlMid(int item) const {
        return isSlider(item) ? (layout_.barX + layout_.barX + layout_.barW) * 0.5f
                              : (layout_.ctlX0 + layout_.ctlX1) * 0.5f;
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
        } else if (item == Mode) {
            cycleMode(dir, s, a);
        } else if (item == Resolution) {
            cycleResolution(dir, s, a, p);
        } else if (item == Refresh) {
            cycleRefresh(dir, s, a, p);
        }
    }

    // Display changes are provisional: the pending values are shown immediately
    // and Game applies them (then asks for confirmation).
    void requestApply(Settings& s, Audio& a) {
        notice_[0] = '\0';
        displayApply_ = true;
        s.clamp();
        a.play(Sfx::Click);
    }

    void cycleMode(int dir, Settings& s, Audio& a) {
        int m = int(s.mode) + (dir >= 0 ? 1 : -1);
        if (m < 0) m = kDisplayModeCount - 1;
        if (m >= kDisplayModeCount) m = 0;
        s.mode = DisplayMode(m);
        requestApply(s, a);
    }

    void cycleResolution(int dir, Settings& s, Audio& a, Platform& p) {
        fillResolutionList(s, p);
        if (resCount_ <= 0) return;
        switch (s.mode) {
            case DisplayMode::Borderless:
                if (s.renderWidth <= 0 || s.renderHeight <= 0) {
                    // Currently "native" (0): start from the monitor size.
                    int nw = 0, nh = 0;
                    p.monitorSize(nw, nh);
                    s.renderWidth = nw > 0 ? nw : s.width;
                    s.renderHeight = nh > 0 ? nh : s.height;
                }
                Settings::stepMode(dir, s.renderWidth, s.renderHeight, resolutions_, resCount_);
                break;
            case DisplayMode::Exclusive: {
                int w = s.modeWidth, h = s.modeHeight;
                if (w <= 0 || h <= 0) {
                    // Start from the entry closest to the current monitor size.
                    int nw = 0, nh = 0;
                    p.monitorSize(nw, nh);
                    w = nw; h = nh;
                }
                Settings::stepMode(dir, w, h, resolutions_, resCount_);
                s.modeWidth = w;
                s.modeHeight = h;
                // Refresh rates belong to a size: pick a sensible one for it.
                if (!refreshValidFor(s, p)) s.modeRefresh = 0;
                break;
            }
            case DisplayMode::Windowed:
            default:
                Settings::stepMode(dir, s.width, s.height, resolutions_, resCount_);
                break;
        }
        requestApply(s, a);
    }

    void cycleRefresh(int dir, Settings& s, Audio& a, Platform& p) {
        if (s.mode != DisplayMode::Exclusive) return;   // row is disabled
        int hz[32];
        int n = refreshListFor(s, p, hz, 32);
        if (n <= 0) return;
        int idx = -1;
        for (int i = 0; i < n; ++i) if (hz[i] == s.modeRefresh) { idx = i; break; }
        if (idx < 0) idx = n - 1;                       // unknown -> default entry
        idx += dir >= 0 ? 1 : -1;
        if (idx < 0) idx = n - 1;
        if (idx >= n) idx = 0;
        s.modeRefresh = hz[idx];
        requestApply(s, a);
    }

    int refreshListFor(const Settings& s, Platform& p, int* out, int cap) const {
        int n = p.displayModeCount();
        if (n > Settings::kMaxDisplayModes) n = Settings::kMaxDisplayModes;
        driverCount_ = 0;
        DisplayModeInfo info;
        for (int i = 0; i < n; ++i)
            if (p.displayModeAt(i, info) && driverCount_ < Settings::kMaxDisplayModes)
                driverModes_[driverCount_++] = info;
        int w = s.modeWidth, h = s.modeHeight;
        if (w <= 0 || h <= 0) {
            int nw = 0, nh = 0;
            p.monitorSize(nw, nh);
            w = nw; h = nh;
        }
        return Settings::refreshRates(out, cap, driverModes_, driverCount_, w, h);
    }

    bool refreshValidFor(const Settings& s, Platform& p) const {
        if (s.modeRefresh <= 0) return true;
        int hz[32];
        int n = refreshListFor(s, p, hz, 32);
        for (int i = 0; i < n; ++i) if (hz[i] == s.modeRefresh) return true;
        return false;
    }

    void activate(int item, Settings&, Audio& a) {
        if (item == Restart) restart_ = true;   // game plays the Restart sound
        else if (item == Resume) { resume_ = true; a.play(Sfx::Click); }
        else if (item == Quit) { quit_ = true; a.play(Sfx::Click); }
    }

    const char* valueText(int item) const {
        if (item == Volume) return volText_;
        if (item == Mode) return modeText_;
        if (item == Resolution) return resText_;
        if (item == Refresh) return refreshText_;
        if (item == Fov) return fovText_;
        return sensText_;
    }

    float sliderFrac(int item) const {
        if (item == Volume) return lastVolFrac_;
        if (item == Fov) return lastFovFrac_;
        return lastSensFrac_;
    }

    void refreshLabels(Settings& s, Platform& p) {
        // The labels mirror the pending configuration the user is choosing.
        pending_ = s;
        s.clamp();
        lastVolFrac_ = s.volume;
        lastFovFrac_ = (s.fov - Settings::kFovMin) / (Settings::kFovMax - Settings::kFovMin);
        lastSensFrac_ = (s.sensitivity - 0.1f) / (3.0f - 0.1f);
        std::snprintf(volText_, sizeof(volText_), "%d%%", int(s.volume * 100.0f + 0.5f));
        std::snprintf(fovText_, sizeof(fovText_), "%d", int(s.fov + 0.5f));
        std::snprintf(sensText_, sizeof(sensText_), "%d%%", int(s.sensitivity * 100.0f + 0.5f));
        std::snprintf(modeText_, sizeof(modeText_), "< %s >", displayModeName(s.mode));

        fillResolutionList(s, p);
        switch (s.mode) {
            case DisplayMode::Borderless: {
                // The window covers the monitor, so the render size shown is what
                // the renderer will really use (preset fitted to the monitor
                // shape) plus how much of the native resolution that is.
                int nw = 0, nh = 0;
                p.monitorSize(nw, nh);
                if (nw <= 0 || nh <= 0) { nw = layout_.winW > 0 ? layout_.winW : s.width;
                                          nh = layout_.winH > 0 ? layout_.winH : s.height; }
                int rw = 0, rh = 0;
                if (s.renderWidth > 0 && s.renderHeight > 0)
                    Settings::fitRenderAspect(s.renderWidth, s.renderHeight, nw, nh, rw, rh);
                else { rw = nw; rh = nh; }          // native
                int pct = (nw > 0) ? int(float(s.renderWidth > 0 ? s.renderWidth : nw) * 100.0f /
                                         float(nw) + 0.5f)
                                   : 100;
                if (s.renderWidth > 0 && pct > 0 && pct != 100)
                    std::snprintf(resText_, sizeof(resText_), "< %dx%d %d%% >", rw, rh, pct);
                else
                    std::snprintf(resText_, sizeof(resText_), "< %dx%d >", rw, rh);
                break;
            }
            case DisplayMode::Exclusive: {
                int w = s.modeWidth, h = s.modeHeight;
                if (w <= 0 || h <= 0) {
                    int nw = 0, nh = 0;
                    p.monitorSize(nw, nh);
                    w = nw; h = nh;
                }
                if (w <= 0 || h <= 0)
                    std::snprintf(resText_, sizeof(resText_), "< NO MODES >");
                else
                    std::snprintf(resText_, sizeof(resText_), "< %dx%d >", w, h);
                break;
            }
            case DisplayMode::Windowed:
            default: {
                int aw = 0, ah = 0;
                p.maxWindowSize(aw, ah);
                bool atMonitorMax = aw > 0 && ah > 0 && s.width >= aw && s.height >= ah;
                std::snprintf(resText_, sizeof(resText_),
                              atMonitorMax ? "< %dx%d MAX >" : "< %dx%d >", s.width, s.height);
                break;
            }
        }
        if (s.modeRefresh > 0)
            std::snprintf(refreshText_, sizeof(refreshText_), "< %d HZ >", s.modeRefresh);
        else
            std::snprintf(refreshText_, sizeof(refreshText_), "< DEFAULT >");
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
    Settings pending_;        // last rendered configuration (for row enablement)
    char volText_[8]{}, fovText_[8]{}, sensText_[8]{}, resText_[48]{}, refreshText_[32]{},
        modeText_[32]{}, notice_[64]{};
    float lastVolFrac_ = 0.8f, lastFovFrac_ = 0.36f, lastSensFrac_ = 0.5f;
    // Resolution lists (rebuilt every frame; fixed pools, no allocation).
    // Qualified: the Resolution *enumerator* above would hide the type here.
    aw::Resolution resolutions_[Settings::kMaxModes]{};
    mutable DisplayModeInfo driverModes_[Settings::kMaxDisplayModes]{};
    int resCount_ = 0;
    mutable int driverCount_ = 0;
    int selected_ = 0;
    int drag_ = -1;
    bool mouseHeld_ = false;
    bool prevKeys_[512]{};
    bool restart_ = false, quit_ = false, resume_ = false, displayApply_ = false;
};

}  // namespace aw
