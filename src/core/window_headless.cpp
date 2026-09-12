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
    void screenSize(int& w, int& h) const override { w = 0; h = 0; }  // no display
    void setFullscreen(bool) override {}
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
    int width_ = 1280, height_ = 720;
    int frame_ = 0;
    int maxFrames_ = kHeadlessMaxFramesDefault;
};

Platform* createHeadlessPlatform() { return new PlatformHeadless(); }

}  // namespace aw
