// display_confirm.hpp — modal "keep these display settings?" dialog.
//
// Applying a new display mode / resolution is risky: the monitor may not
// support it, or the result may be unusable (overscan, black screen, 30 Hz).
// So every change is provisional: the game applies it, shows this dialog with a
// countdown, and only writes the settings file when the user confirms.
//
//   Keep  — click KEEP, press Enter/Space
//   Revert— click REVERT, press Escape, or let the countdown run out
//
// The dialog owns no display state: Game performs the actual apply/revert and
// consumes the decision returned by update(). It is modal — the caller must not
// update the settings menu while it is active.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../core/input.hpp"
#include "../core/platform.hpp"
#include "../render/font.hpp"
#include "../render/renderer.hpp"

namespace aw {

class DisplayConfirm {
public:
    enum Decision { None = 0, Keep = 1, Revert = 2 };

    // Start the countdown (seconds). `label` is shown in the prompt, e.g.
    // "EXCLUSIVE 1920X1080 60HZ".
    // `heldKeys` (optional) is the current keyboard state: a key that is still
    // held when the dialog opens (Enter used to apply the setting, for example)
    // must not immediately confirm it — the user has to press it again.
    void begin(float seconds, const char* label, const uint8_t* heldKeys = nullptr) {
        total_ = seconds > 0.0f ? seconds : 1.0f;
        remaining_ = total_;
        active_ = true;
        mouseHeld_ = false;
        mouseWasDown_ = false;
        if (heldKeys) std::memcpy(prevKeys_, heldKeys, sizeof(prevKeys_));
        else std::memset(prevKeys_, 0, sizeof(prevKeys_));
        std::snprintf(label_, sizeof(label_), "%.*s", int(sizeof(label_) - 1),
                      label ? label : "");
    }
    void cancel() { active_ = false; }
    bool active() const { return active_; }
    float remaining() const { return remaining_; }

    // Returns Keep/Revert once, then None (the dialog closes itself first).
    Decision update(const FrameInput& in, float dt, float uiScale) {
        if (!active_) return None;
        computeLayout(in.width, in.height, uiScale);
        if (dt > 0.0f) remaining_ -= dt;
        if (remaining_ <= 0.0f) { active_ = false; return Revert; }

        // Mouse: hover highlights, click decides (edge triggered, like the menu).
        mouseHeld_ = (in.mousePressed[MBTN_LEFT] || mouseHeld_) && !in.mouseReleased[MBTN_LEFT];
        if (in.mousePressed[MBTN_LEFT] && !mouseWasDown_) {
            int hover = buttonAt(in.mouseX, in.mouseY);
            if (hover == 1) { active_ = false; return Keep; }
            if (hover == 2) { active_ = false; return Revert; }
        }

        auto edge = [&](uint32_t key) -> bool {
            return in.keys[key] != 0 && !prevKeys_[key];
        };
        if (edge(KEY_ENTER) || edge(KEY_SPACE)) { active_ = false; return Keep; }
        if (edge(KEY_ESC)) { active_ = false; return Revert; }
        std::memcpy(prevKeys_, in.keys, sizeof(prevKeys_));
        mouseWasDown_ = mouseHeld_;
        return None;
    }

    void render(Renderer& r) const {
        if (!active_) return;
        const float k = layout_.ui;
        const int ts = layout_.ts;
        r.uiRect(0, 0, float(layout_.winW), float(layout_.winH), 0, 0, 0, 0.55f);
        r.uiRect(layout_.px, layout_.py, layout_.pw, layout_.ph, 0.10f, 0.08f, 0.07f, 0.98f);
        const float bw = 2 * k;
        r.uiRect(layout_.px, layout_.py, layout_.pw, bw, 0.95f, 0.72f, 0.25f, 1);
        r.uiRect(layout_.px, layout_.py + layout_.ph - bw, layout_.pw, bw, 0.95f, 0.72f, 0.25f, 1);
        r.uiRect(layout_.px, layout_.py, bw, layout_.ph, 0.95f, 0.72f, 0.25f, 1);
        r.uiRect(layout_.px + layout_.pw - bw, layout_.py, bw, layout_.ph, 0.95f, 0.72f, 0.25f, 1);

        centered(r, "KEEP THESE DISPLAY SETTINGS?", ts + 1,
                 layout_.py + 34 * k, 1.0f, 0.92f, 0.72f);
        if (label_[0]) centered(r, label_, ts, layout_.py + 76 * k, 0.80f, 0.84f, 0.88f);

        char line[64];
        int secs = int(remaining_ + 0.999f);           // count up to the shown second
        if (secs < 0) secs = 0;
        std::snprintf(line, sizeof(line), "REVERTING TO PREVIOUS SETTINGS IN %d SECONDS", secs);
        centered(r, line, ts, layout_.py + 108 * k, 1.0f, 0.62f, 0.30f);

        for (int i = 0; i < 2; ++i) {
            const bool keep = (i == 0);
            float bx = keep ? layout_.keepX : layout_.revertX;
            float col = keep ? 0.30f : 0.42f;
            r.uiRect(bx, layout_.btnY, layout_.btnW, layout_.btnH,
                     keep ? 0.16f : 0.22f, keep ? 0.38f : col * 0.5f, keep ? 0.24f : 0.24f, 1);
            centeredIn(r, keep ? "KEEP - ENTER" : "REVERT - ESC", ts, bx + layout_.btnW * 0.5f,
                       layout_.btnY + layout_.btnH * 0.5f - 4 * k, 1, 1, 1);
        }
        centered(r, "NO INPUT REVERTS AUTOMATICALLY", ts > 1 ? ts - 1 : 1,
                 layout_.py + layout_.ph - 26 * k, 0.45f, 0.48f, 0.52f);
    }

private:
    struct Layout {
        static constexpr float baseW = 640, baseH = 250;
        int winW = 0, winH = 0;
        float ui = 1.0f;
        int ts = 2;
        float px = 0, py = 0, pw = baseW, ph = baseH;
        float btnW = 210, btnH = 44, btnY = 0, keepX = 0, revertX = 0;
    };

    void computeLayout(int w, int h, float wanted) {
        Layout& L = layout_;
        L.winW = w;
        L.winH = h;
        L.ui = quantizeUiScale(wanted);
        float k = L.ui;
        L.ts = int(k * 2.0f + 0.5f);
        if (L.ts < 2) L.ts = 2;
        L.pw = Layout::baseW * k;
        L.ph = Layout::baseH * k;
        L.px = float(w > int(L.pw) ? (w - int(L.pw)) / 2 : 4);
        L.py = float(h > int(L.ph) ? (h - int(L.ph)) / 2 : 4);
        L.btnW = 210 * k;
        L.btnH = 44 * k;
        const float gap = 24 * k;
        L.keepX = L.px + L.pw * 0.5f - L.btnW - gap * 0.5f;
        L.revertX = L.px + L.pw * 0.5f + gap * 0.5f;
        L.btnY = L.py + L.ph - L.btnH - 52 * k;
    }

    int buttonAt(float mx, float my) const {
        const Layout& L = layout_;
        if (my < L.btnY || my > L.btnY + L.btnH) return 0;
        if (mx >= L.keepX && mx <= L.keepX + L.btnW) return 1;
        if (mx >= L.revertX && mx <= L.revertX + L.btnW) return 2;
        return 0;
    }

    void centered(Renderer& r, const char* s, int scale, float y,
                  float cr, float cg, float cb) const {
        float cx = layout_.px + layout_.pw * 0.5f;
        r.uiText(cx - float(textWidthPx(s, scale)) * 0.5f, y, scale, cr, cg, cb, 1, s);
    }
    static void centeredIn(Renderer& r, const char* s, int scale, float cx, float y,
                            float cr, float cg, float cb) {
        r.uiText(cx - float(textWidthPx(s, scale)) * 0.5f, y, scale, cr, cg, cb, 1, s);
    }

    bool active_ = false;
    bool mouseHeld_ = false;
    bool mouseWasDown_ = false;   // click already in progress when the dialog opened
    float total_ = 1.0f, remaining_ = 0.0f;
    char label_[48]{};
    bool prevKeys_[512]{};
    Layout layout_;
};

}  // namespace aw
