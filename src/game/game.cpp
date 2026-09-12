// game.cpp — game orchestration and the main loop.
#include "game.hpp"

#include <chrono>
#include <cstdio>

#include "../core/input.hpp"
#include "../render/gl.h"

namespace aw {

using Clock = std::chrono::steady_clock;

static double nowSeconds() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

bool Game::init(const char* title, int width, int height, bool preferHeadless) {
    // Saved settings (if any) override the requested size. `width`/`height`
    // are the *render* resolution; the window has its own saved size so that
    // changing resolution never resizes or moves it.
    if (settings_.load()) {
        width = settings_.width;
        height = settings_.height;
    } else {
        settings_.width = width;
        settings_.height = height;
        settings_.windowW = width;
        settings_.windowH = height;
    }
    settings_.clamp();

    // Windowed-mode window size: fixed (persisted), only fitted to the screen.
    int winW = settings_.windowW, winH = settings_.windowH;
    if (winW <= 0 || winH <= 0) {
        winW = Settings::kWindowDefaultW;
        winH = Settings::kWindowDefaultH;
    }

    if (!preferHeadless) {
        platform_ = createPlatform();
        if (platform_->init(title, winW, winH)) goto platformReady;
        // No usable display/GPU: fall back to the headless backend.
        platform_->shutdown();
        delete platform_;
    }
    platform_ = createHeadlessPlatform();
    if (!platform_->init(title, width, height)) {
        delete platform_;
        platform_ = nullptr;
        return false;
    }
platformReady:;
    // Keep the windowed window at a size that actually fits the display (the
    // OS clamps windows that do not, and we want the saved size to be the
    // real one). The render resolution is untouched by this.
    {
        int sw = 0, sh = 0;
        platform_->screenSize(sw, sh);
        Settings::fitToScreen(winW, winH, sw, sh, kWindowMarginX, kWindowMarginY);
        settings_.windowW = winW;
        settings_.windowH = winH;
        if (!settings_.fullscreen && !headless()) platform_->resize(winW, winH);
    }
    if (settings_.fullscreen) platform_->setFullscreen(true);

    float platformTop = seedWorld();
    player_.reset(48.0f, platformTop + 0.1f, 0.5f);
    player_.sensitivity = settings_.sensitivity;
    lastChunk_ = brickToChunk(48, floori(platformTop));
    wall_.streamAround(lastChunk_.cx, lastChunk_.cy);

    audio_.init(!headless());
    audio_.setVolume(settings_.volume);
    interaction_.setAudio(&audio_);
    if (!headless()) fprintf(stderr, "[aw] audio: %s\n", audio_.backendName());

    if (!headless()) {
        if (!renderer_.init()) {
            fprintf(stderr, "[aw] renderer init failed\n");
            return false;
        }
        platform_->setCursorCaptured(true);
    }
    lastTime_ = nowSeconds();
    initialized_ = true;
    return true;
}

float Game::seedWorld() {
    // A starting platform the player stands on.
    float platformTop = 0.0f;
    for (int32_t bx = 44; bx <= 51; ++bx) {
        wall_.setBrick(bx, -1, STATE_EXTENDED, 1.0f);
        float top = wall_.brickAABB(bx, -1).mx.y;
        if (top > platformTop) platformTop = top;
    }
    // A short pre-built step path so the scene has visible ledges immediately.
    for (int32_t i = 0; i < 5; ++i)
        wall_.setBrick(48 + (i % 3), i, STATE_EXTENDED, 0.85f);
    return platformTop;
}

void Game::restart() {
    wall_.reset();
    interaction_.clear();
    float platformTop = seedWorld();
    player_.reset(48.0f, platformTop + 0.1f, 0.5f);
    player_.highestBrickY = 0;
    lastChunk_ = brickToChunk(48, floori(platformTop));
    wall_.streamAround(lastChunk_.cx, lastChunk_.cy);
    stats_ = FrameStats{};
    target_ = TargetResult{};
    audio_.play(Sfx::Restart);
}

void Game::shutdown() {
    settings_.save();
    audio_.shutdown();
    renderer_.shutdown();
    if (platform_) { platform_->shutdown(); delete platform_; platform_ = nullptr; }
    initialized_ = false;
}

// Advance the simulation by dt with the given input, without touching the
// platform or renderer. Used by tests and by run() (which supplies real input).
void Game::simulateFrame(const FrameInput& in, float dt) {
    player_.sensitivity = settings_.sensitivity;
    player_.update(in, wall_, dt);

    target_ = interaction_.cast(player_, wall_);
    // Clicks (edge events) start 3 s in/out lerps; update() advances them.
    if (in.mousePressed[MBTN_LEFT] && target_.hit) interaction_.pull(wall_, target_);
    if (in.mousePressed[MBTN_RIGHT] && target_.hit) interaction_.push(wall_, player_, target_);
    interaction_.update(wall_, player_, dt);

    BrickCoord pb = worldToBrick(player_.pos);
    ChunkCoord pc = brickToChunk(pb.x, pb.y);
    if (pc != lastChunk_) {
        lastChunk_ = pc;
        wall_.streamAround(pc.cx, pc.cy);
    }

    stats_.residentChunks = wall_.residentCount();
    stats_.drawnInstances = wall_.residentBrickCount();
    stats_.modifiedBricks = wall_.store().activeCount();
    stats_.playerBrickY = floori(player_.pos.y / BRICK);
}

void Game::run() {
    if (!initialized_) return;

    int frameCount = 0;
    double reportT = nowSeconds();

    while (true) {
        // Headless mode uses a fixed 60 Hz step (deterministic, benchmarkable);
        // interactive mode steps by wall-clock delta time (uncapped frame rate).
        float dt;
        if (headless()) {
            dt = 1.0f / 60.0f;
        } else {
            double now = nowSeconds();
            dt = float(now - lastTime_);
            lastTime_ = now;
            if (dt > 0.1f) dt = 0.1f;
            if (dt <= 0.0f) dt = 1.0f / 60.0f;
        }

        FrameInput in;
        bool alive = platform_->frame(in);
        if (!alive || in.shouldQuit) break;

        // in.width/in.height are the real window client size: the space the
        // cursor is reported in and the space the UI is drawn in. Remember it
        // as the windowed window size (never while fullscreen, where it is the
        // monitor) so it stays consistent across sessions.
        if (!settings_.fullscreen && !headless() &&
            in.width >= 320 && in.height >= 200 &&
            (in.width != settings_.windowW || in.height != settings_.windowH)) {
            settings_.windowW = in.width;
            settings_.windowH = in.height;
        }

        // Esc toggles the settings menu (real window only: windowed or
        // fullscreen); the simulation pauses while it is open.
        bool esc = in.keys[KEY_ESC] != 0;
        if (esc && !prevEsc_ && !headless()) {
            menuOpen_ = !menuOpen_;
            if (menuOpen_) menu_.open();
            else settings_.save();
            platform_->setCursorCaptured(!menuOpen_);
        }
        prevEsc_ = esc;

        if (menuOpen_) {
            menu_.update(in, settings_, audio_, *platform_);
            if (menu_.consumeRestart()) {
                restart();
                menuOpen_ = false;
                settings_.save();
                platform_->setCursorCaptured(true);
            }
            if (menu_.consumeResume()) {
                menuOpen_ = false;
                settings_.save();
                platform_->setCursorCaptured(true);
            }
            if (menu_.consumeQuit()) {
                settings_.save();
                break;
            }
        } else {
            if (headless()) demoDrive(dt);

            bool wasGrounded = player_.grounded;
            simulateFrame(in, dt);
            if (!headless()) {
                if (wasGrounded && in.keys[KEY_SPACE]) audio_.play(Sfx::Jump);
                if (!wasGrounded && player_.grounded) audio_.play(Sfx::Land);
            }
        }

        if (!headless()) {
            // The scene renders at the configured resolution — independent of
            // the window — and is scaled into it; the projection must match
            // that resolution's aspect, not the window's.
            int winW = in.width > 0 ? in.width : settings_.width;
            int winH = in.height > 0 ? in.height : settings_.height;
            renderer_.setRenderSize(settings_.width, settings_.height);
            int rw = winW, rh = winH;
            renderer_.renderSize(winW, winH, rw, rh);
            float aspect = rh > 0 ? float(rw) / float(rh) : 16.0f / 9.0f;
            Mat4 proj = Mat4::perspective(deg2rad(settings_.fov), aspect, NEAR_PLANE, FAR_PLANE);
            Vec3 eye = player_.eye();
            Mat4 view = Mat4::lookAt(eye, eye + player_.forward(), {0, 1, 0});
            Mat4 vp = proj * view;
            stats_.drawnInstances =
                renderer_.render(wall_, player_, vp, settings_.fov, winW, winH, target_.hit);
            if (menuOpen_) {
                // Window pixels: the menu is hit-tested against the same
                // coordinates the platform reports for the cursor.
                renderer_.uiBegin(winW, winH);
                menu_.render(renderer_);
                renderer_.uiEnd();
            }
            platform_->swapBuffers();
        }

        ++frameCount;
        stats_.frame = frameCount;
        stats_.elapsed += dt;

        double t = nowSeconds();
        if (t - reportT >= 1.0) {
            stats_.fps = double(frameCount) / (t - reportT);
            stats_.frameMs = (t - reportT) * 1000.0 / double(frameCount);
            fprintf(stderr,
                    "[aw] fps=%6.1f  ms=%5.2f  chunks=%2d  inst=%6d  mods=%4d  h=%4d\n",
                    stats_.fps, stats_.frameMs, stats_.residentChunks, stats_.drawnInstances,
                    stats_.modifiedBricks, stats_.playerBrickY);
            frameCount = 0;
            reportT = t;
        }
    }
}

// Scripted driving for headless/demo mode: climbs the wall brick-by-brick
// (building ledges under the player, exercising collision + streaming), orbits
// the camera, and periodically pulls/pushes bricks so the dirty instance-upload
// path runs too.
void Game::demoDrive(float dt) {
    static float phase = 0.0f;
    phase += dt;
    player_.yaw += 0.12f * dt;      // orbit
    player_.pitch = std::sin(phase * 0.4f) * 0.6f;

    int32_t tick = stats_.frame;

    // Climb one ledge every 12 frames; extend the brick at the player's feet
    // and snap onto the tallest extended ledge overlapping the footprint so
    // the controller rests on it (grounded) between steps. Mosaic bricks are
    // 1..4 m tall and extended towers can stack above the new brick, so the
    // support height is measured over a vertical window (not assumed): the
    // 12-frame cadence keeps falls between snaps (~0.4 m) below the minimum
    // 1 m step gain, which keeps the climb monotonic.
    if (tick % 12 == 0 && tick > 0) {
        int32_t row = floori(player_.pos.y / BRICK);
        wall_.setBrick(48, row, STATE_EXTENDED, 1.0f);
        float top = wall_.brickAABB(48, row).mx.y;
        for (int32_t bx = 43; bx <= 48; ++bx) {
            for (int32_t by = row - 5; by <= row + 8; ++by) {
                if (wall_.brickDepth(bx, by) <= 0.0f) continue;  // flush: no z overlap
                AABB b = wall_.brickAABB(bx, by);
                if (b.mx.x > 48.0f - PLAYER_HALF_W && b.mn.x < 48.0f + PLAYER_HALF_W &&
                    b.mx.y > top)
                    top = b.mx.y;
            }
        }
        player_.pos = {48.0f, top, 0.5f};
        player_.vel = {0, 0, 0};
    }

    if (tick % 20 == 0) {
        int32_t bx = 40 + (tick * 7) % 40;
        int32_t byy = floori(player_.pos.y) + 2;
        wall_.setBrick(bx, byy, STATE_EXTENDED, 0.8f);
    }
    if (tick % 37 == 0) {
        int32_t bx = 30 + (tick * 11) % 50;
        int32_t byy = floori(player_.pos.y) - 1;
        if (wall_.brickDepth(bx, byy) > 0.0f)
            wall_.setBrick(bx, byy, STATE_REST, 0.0f);
    }
}

}  // namespace aw
