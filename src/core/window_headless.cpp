// window_headless.cpp — headless backend (no window, no GL).
// Used for tests, CI, and benchmarks: the game simulation runs with a scripted
// camera and emits per-frame stats. This keeps the engine fully runnable on a
// machine without a display or GPU.
#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace aw {

namespace {
// Scripted camera: orbit around the wall while slowly climbing, which exercises
// the same chunk streaming + culling code paths as interactive play.
constexpr int kHeadlessMaxFramesDefault = 1200;
}  // namespace

class PlatformHeadless final : public Platform {
public:
    bool init(const char*, int w, int h) override {
        width_ = w; height_ = h;
        const char* env = std::getenv("AW_HEADLESS_FRAMES");
        maxFrames_ = env ? std::atoi(env) : kHeadlessMaxFramesDefault;
        return true;
    }

    bool frame(FrameInput& in) override {
        for (int i = 0; i < 8; ++i) { in.mousePressed[i] = false; in.mouseReleased[i] = false; }
        std::memset(in.keys, 0, sizeof(in.keys));
        in.width = width_; in.height = height_;
        in.mouseDX = 0.0f; in.mouseDY = 0.0f;
        in.mouseX = float(width_ / 2); in.mouseY = float(height_ / 2);
        in.shouldQuit = (frame_++ >= maxFrames_);
        return !in.shouldQuit;
    }

    void swapBuffers() override {}
    void shutdown() override {}
    void resize(int w, int h) override {
        if (w > 0 && h > 0) { width_ = w; height_ = h; }
    }
    // Headless has no display: every mode is accepted as a no-op so that a
    // saved configuration never fails in tests/CI (nothing is switched, and the
    // settings file is left untouched).
    bool applyDisplayMode(DisplayMode mode, int w, int h, int) override {
        mode_ = mode;
        // The windowed size is an internal property here (there is no monitor);
        // borderless/exclusive keep whatever size the run was started with.
        if (mode == DisplayMode::Windowed && w > 0 && h > 0) { width_ = w; height_ = h; }
        return true;
    }
    DisplayMode currentDisplayMode() const override { return mode_; }
    // No monitor to query: headless runs keep whatever size they were given.
    bool maxWindowSize(int& w, int& h) const override { w = 0; h = 0; return false; }
    bool monitorSize(int& w, int& h) const override { w = 0; h = 0; return false; }
    int displayModeCount() const override { return 0; }
    bool clientSize(int& w, int& h) const override { w = width_; h = height_; return true; }
    BackendInfo info() const override {
        BackendInfo b;
        b.hasWindow = false;
        b.hasGL = false;
        b.name = "headless";
        return b;
    }
    void setCursorCaptured(bool) override {}
    void* loadGLProc(const char*) override { return nullptr; }

    int frameCount() const { return frame_; }

private:
    DisplayMode mode_ = DisplayMode::Windowed;
    int width_ = 1280, height_ = 720;
    int frame_ = 0;
    int maxFrames_ = kHeadlessMaxFramesDefault;
};

Platform* createHeadlessPlatform() { return new PlatformHeadless(); }

}  // namespace aw
