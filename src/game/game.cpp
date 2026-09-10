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
    // Saved settings (if any) override the requested window size.
    if (settings_.load()) {
        width = settings_.width;
        height = settings_.height;
    } else {
        settings_.width = width;
        settings_.height = height;
    }

    if (!preferHeadless) {
        platform_ = createPlatform();
        if (platform_->init(title, width, height)) goto platformReady;
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
        float top = float(-1) + brickSize(bx, -1);
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
    stats_.drawnInstances = wall_.residentCount() * CHUNK_BRICKS;
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
            float aspect = in.width > 0 && in.height > 0 ? float(in.width) / float(in.height)
                                                         : 16.0f / 9.0f;
            Mat4 proj = Mat4::perspective(deg2rad(FOV_DEG), aspect, NEAR_PLANE, FAR_PLANE);
            Vec3 eye = player_.eye();
            Mat4 view = Mat4::lookAt(eye, eye + player_.forward(), {0, 1, 0});
            Mat4 vp = proj * view;
            stats_.drawnInstances =
                renderer_.render(wall_, player_, vp, aspect, in.width, in.height, target_.hit);
            if (menuOpen_) {
                renderer_.uiBegin(in.width, in.height);
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

    // Climb one ledge every 24 frames; extend the brick at the player's feet
    // and snap onto the tallest supporting ledge under the footprint so the
    // controller rests on it (grounded) between steps. Brick tops vary with
    // size (1/2.5/5 m) and older large ledges can tower above the new one, so
    // the support height is measured, not assumed (sizes >= 1 m keep the
    // climb monotonic).
    if (tick % 24 == 0 && tick > 0) {
        int32_t row = floori(player_.pos.y / BRICK);
        wall_.setBrick(48, row, STATE_EXTENDED, 1.0f);
        float top = float(row) + brickSize(48, row);
        for (int32_t bx = 43; bx <= 48; ++bx) {
            for (int32_t by = row - 5; by <= row; ++by) {
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
