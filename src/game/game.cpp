// game.cpp — game orchestration and the main loop.
#include "game.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

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
        // Pick a window size the monitor can actually show *before* the window
        // is created, so a saved "2560x1440" never opens a window larger than
        // the screen (even for a moment).
        fitWindowToMonitor();
        if (platform_->init(title, settings_.width, settings_.height)) goto platformReady;
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
    {
        // Apply the saved display configuration (windowed / borderless /
        // exclusive fullscreen). A configuration the backend refuses — e.g. an
        // exclusive mode that no longer exists on this display, or a machine
        // without exclusive support — falls back to a fitting window instead of
        // leaving the user with a black screen. The headless backend accepts
        // everything, so the same code just validates the saved settings there
        // and keeps the "last confirmed configuration" bookkeeping honest.
        DisplayConfig cfg = displayConfigOf(settings_);
        if (!applyDisplayConfig(cfg)) {
            fprintf(stderr, "[aw] display config refused (%s) - falling back to windowed\n",
                    displayModeName(cfg.mode));
            settings_.mode = DisplayMode::Windowed;
            fitWindowToMonitor();
            cfg = displayConfigOf(settings_);
            applyDisplayConfig(cfg);
        }
        // The window may have opened on a different monitor than the primary
        // one: fit (and resize) again now that the backend knows its display.
        fitWindowToMonitor();
        displayStable_ = cfg;
        // A restored exclusive mode is provisional too: the display was switched
        // before the user could see anything, so ask for confirmation. Until it
        // is confirmed, the revert target is a plain window — otherwise the
        // countdown would "revert" to the very mode it is asking about.
        if (needsStartupConfirm(cfg.mode, headless())) {
            displayStable_ = displayConfigOf(settings_);
            displayStable_.mode = DisplayMode::Windowed;
            char label[48];
            describeDisplayConfig(cfg, label, sizeof(label));
            displayConfirm_.begin(Settings::kDisplayConfirmSeconds, label);
        }
    }

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

DisplayConfig Game::displayConfigOf(const Settings& s) {
    DisplayConfig c;
    c.mode = s.mode;
    c.width = s.width; c.height = s.height;
    c.renderWidth = s.renderWidth; c.renderHeight = s.renderHeight;
    c.modeWidth = s.modeWidth; c.modeHeight = s.modeHeight;
    c.modeRefresh = s.modeRefresh;
    return c;
}

void Game::setDisplayConfig(Settings& s, const DisplayConfig& cfg) {
    s.mode = cfg.mode;
    s.width = cfg.width; s.height = cfg.height;
    s.renderWidth = cfg.renderWidth; s.renderHeight = cfg.renderHeight;
    s.modeWidth = cfg.modeWidth; s.modeHeight = cfg.modeHeight;
    s.modeRefresh = cfg.modeRefresh;
    s.clamp();
}

void describeDisplayConfig(const DisplayConfig& cfg, char* out, size_t n) {
    switch (cfg.mode) {
        case DisplayMode::Windowed:
            std::snprintf(out, n, "WINDOW %dx%d", cfg.width, cfg.height);
            break;
        case DisplayMode::Borderless:
            if (cfg.renderWidth > 0 && cfg.renderHeight > 0)
                std::snprintf(out, n, "BORDERLESS RENDER %dx%d", cfg.renderWidth, cfg.renderHeight);
            else
                std::snprintf(out, n, "BORDERLESS NATIVE");
            break;
        case DisplayMode::Exclusive:
        default:
            if (cfg.modeRefresh > 0)
                std::snprintf(out, n, "EXCLUSIVE %dx%d %dHZ", cfg.modeWidth, cfg.modeHeight,
                              cfg.modeRefresh);
            else
                std::snprintf(out, n, "EXCLUSIVE %dx%d", cfg.modeWidth, cfg.modeHeight);
            break;
    }
}

bool Game::needsStartupConfirm(DisplayMode mode, bool headless) {
    // Only a real, hardware-switched mode needs confirming at start-up: there is
    // nothing to accept in a mode the backend never switched.
    return mode == DisplayMode::Exclusive && !headless;
}

bool Game::applyDisplayConfigTo(Platform& p, const DisplayConfig& cfg) {
    switch (cfg.mode) {
        case DisplayMode::Windowed:
            return p.applyDisplayMode(DisplayMode::Windowed, cfg.width, cfg.height, 0);
        case DisplayMode::Borderless:
            // The window is locked to the monitor's native resolution; the
            // render resolution lives in the settings (see renderSizeFor).
            return p.applyDisplayMode(DisplayMode::Borderless, 0, 0, 0);
        case DisplayMode::Exclusive:
        default:
            return p.applyDisplayMode(DisplayMode::Exclusive, cfg.modeWidth,
                                      cfg.modeHeight, cfg.modeRefresh);
    }
}

bool Game::applyDisplayConfig(const DisplayConfig& cfg) {
    if (!platform_) return false;
    return applyDisplayConfigTo(*platform_, cfg);
}

// Called when the menu changed a display row: apply it provisionally and ask
// the user to confirm (an unusable mode must never be saved silently).
// Windowed sizes are standard resolutions: a standard value is kept even when
// the window has to be fitted to the screen (the window may then be smaller than
// the setting, that is the point -- the user asked to be able to pick 1080p/4K
// in windowed mode). Only a non-standard value (an old "largest window that
// fits" of a previous build) snaps to the closest standard resolution. No-op for
// the other modes: their window is the monitor, and exclusive has its own pool.
void Game::snapWindowSizeToStandard(Settings& s) {
    if (s.mode != DisplayMode::Windowed) return;
    s.snapToStandard();
}

bool Game::requestDisplayApply() {
    // One dialog at a time: while the user is deciding, the menu cannot start a
    // second change (and once applied, the same configuration is not re-applied,
    // which used to show the confirmation twice for a single key press).
    if (displayConfirm_.active()) {
        fprintf(stderr, "[aw] display change ignored, confirmation pending\n");
        return false;
    }
    // Windowed sizes are standard presets, so pick the largest one this monitor
    // can actually show *before* applying: otherwise the backend would have to
    // clamp the window and the setting (1920x1080) would disagree with the real
    // client area (e.g. 1902x1003).
    snapWindowSizeToStandard(settings_);
    const DisplayConfig want = displayConfigOf(settings_);
    if (want == displayStable_ || want == displayPending_) return true;   // nothing to do
    if (!applyDisplayConfig(want)) {
        setDisplayConfig(settings_, displayStable_);   // refused: undo the edit
        displayPending_ = displayStable_;
        if (menuOpen_) menu_.setNotice("DISPLAY CHANGE NOT SUPPORTED");
        fprintf(stderr, "[aw] display change refused, keeping %s\n",
                displayModeName(displayStable_.mode));
        return false;
    }
    displayPending_ = want;
    char label[48];
    describeDisplayConfig(want, label, sizeof(label));
    displayConfirm_.begin(Settings::kDisplayConfirmSeconds, label, pendingHeldKeys_);
    fprintf(stderr, "[aw] display change applied provisionally: %s\n", label);
    return true;
}

// Opens the settings overlay (pause). The menu mirrors the settings, so it
// always opens on the values that are in effect.
void Game::openMenu() {
    menuOpen_ = true;
    menu_.open();
    if (platform_) platform_->setCursorCaptured(false);
}

void Game::closeMenu() {
    menuOpen_ = false;
    settings_.save();
    if (platform_) platform_->setCursorCaptured(true);
}

// One frame of the settings overlay, after the confirmation dialog has had its
// turn with the input.
void Game::updateMenuFrame(const FrameInput& in, bool modalActive) {
    if (!menuOpen_) return;
    // The dialog is modal: the menu stays visible but frozen below it — it keeps
    // tracking the key/mouse edges though (Menu::syncInput), so the very key
    // that dismisses the dialog cannot be seen as a new press by the menu on the
    // next frame. Without that, one Enter both confirmed the dialog *and* stepped
    // the resolution again: two changes and two dialogs for a single action.
    if (modalActive) menu_.syncInput(in);
    else menu_.update(in, settings_, audio_, *platform_);
    setPendingHeldKeys(in.keys);
    if (menu_.consumeDisplayApply()) requestDisplayApply();
    setPendingHeldKeys(nullptr);

    if (menu_.consumeRestart()) {
        restart();
        closeMenu();
    }
    if (menu_.consumeResume()) closeMenu();
    if (menu_.consumeQuit()) quitRequested_ = true;   // the loop saves and exits
}

// Test/demo hook: runs the overlay for one frame with a supplied input instead
// of one read from the platform (the real loop calls the same two steps).
void Game::stepOverlay(const FrameInput& in, float dt) {
    const bool modalActive = pollDisplayConfirm(in, dt);
    updateMenuFrame(in, modalActive);
}

// Drives the modal display-confirmation dialog. Returns true while it owns the
// input (the menu and the Esc toggle are frozen). Keep persists the settings,
// Escape/timeout restore the last confirmed configuration.
bool Game::pollDisplayConfirm(const FrameInput& in, float dt) {
    if (!displayConfirm_.active()) return false;
    DisplayConfirm::Decision d = displayConfirm_.update(in, dt, platform_->uiScale());
    if (d == DisplayConfirm::Keep) {
        displayStable_ = displayConfigOf(settings_);
        displayPending_ = displayStable_;
        settings_.save();              // confirmed -> persist
        fprintf(stderr, "[aw] display settings confirmed and saved\n");
    } else if (d == DisplayConfirm::Revert) {
        revertDisplayChange();
    }
    return true;
}

// The game loop hands the current keyboard state to a dialog it opens, so the
// key that triggered the change cannot confirm it by accident.
void Game::setPendingHeldKeys(const uint8_t* keys) {
    if (keys) std::memcpy(pendingHeldKeys_, keys, sizeof(pendingHeldKeys_));
    else std::memset(pendingHeldKeys_, 0, sizeof(pendingHeldKeys_));
}

void Game::revertDisplayChange() {
    setDisplayConfig(settings_, displayStable_);
    if (!applyDisplayConfig(displayStable_)) {
        // Last resort: fall back to a plain window rather than stay in a mode
        // this backend cannot leave.
        settings_.mode = DisplayMode::Windowed;
        fitWindowToMonitor();
        applyDisplayConfig(displayConfigOf(settings_));
        displayStable_ = displayConfigOf(settings_);
    }
    displayPending_ = displayStable_;
    if (menuOpen_) menu_.setNotice("DISPLAY SETTINGS REVERTED");
    settings_.save();              // the file follows the display again
    fprintf(stderr, "[aw] display settings reverted\n");
}

// Internal 3D render resolution: the window size, except in borderless
// fullscreen where the user picks a render resolution that is upscaled to the
// screen (the UI always stays at the window resolution).
void Game::renderSizeFor(int winW, int winH, int& rw, int& rh) const {
    rw = winW;
    rh = winH;
    if (settings_.mode == DisplayMode::Borderless &&
        settings_.renderWidth > 0 && settings_.renderHeight > 0) {
        // The chosen render resolution keeps its pixel budget but adopts the
        // window's aspect ratio, so the upscale never stretches the image.
        Settings::fitRenderAspect(settings_.renderWidth, settings_.renderHeight, winW, winH, rw, rh);
    }
    if (rw < Settings::kMinWindowW) rw = Settings::kMinWindowW;
    if (rh < Settings::kMinWindowH) rh = Settings::kMinWindowH;
    if (rw > Settings::kMaxRenderW) rw = Settings::kMaxRenderW;
    if (rh > Settings::kMaxRenderH) rh = Settings::kMaxRenderH;
}

// Snap the requested window size onto a standard resolution and apply it.
// Windowed mode only (borderless/exclusive fullscreen always cover the monitor).
void Game::fitWindowToMonitor() {
    if (!platform_ || settings_.mode != DisplayMode::Windowed) return;
    int w = settings_.width, h = settings_.height;
    settings_.snapToStandard();
    if (settings_.width != w || settings_.height != h)
        platform_->resize(settings_.width, settings_.height);
    // The borderless render resolution must be a supported entry too.
    int nw = 0, nh = 0;
    if (settings_.mode == DisplayMode::Borderless && platform_->monitorSize(nw, nh)) {
        int rw = settings_.renderWidth, rh = settings_.renderHeight;
        settings_.fitRenderToMonitor(nw, nh);
        if (settings_.renderWidth != rw || settings_.renderHeight != rh)
            fprintf(stderr, "[aw] render resolution fitted to %dx%d\n",
                    settings_.renderWidth, settings_.renderHeight);
    }
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
    // --bench collects raw per-frame times (see setBenchMode).
    std::vector<double> benchMs;
    if (benchMode_) benchMs.reserve(4096);
    double benchPrev = nowSeconds();

    // Returns true when the run is over (frame limit reached).
    auto frameLimitReached = [&]() {
        return frameLimit_ > 0 && stats_.frame >= frameLimit_;
    };

    while (true) {
        if (frameLimitReached()) break;
        if (benchMode_) {
            double t = nowSeconds();
            if (stats_.frame > 0) benchMs.push_back((t - benchPrev) * 1000.0);
            benchPrev = t;
        }
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

        // A provisional display change keeps the modal confirmation dialog on
        // screen: no input confirms it, Escape/timeout reverts to the last
        // confirmed configuration (see display_confirm.hpp).
        const bool modalActive = pollDisplayConfirm(in, dt);

        // Esc toggles the settings menu (real window only: windowed or
        // fullscreen); the simulation pauses while it is open.
        bool esc = in.keys[KEY_ESC] != 0;
        if (esc && !prevEsc_ && !headless() && !modalActive) {
            if (menuOpen_) closeMenu();
            else openMenu();
        }
        prevEsc_ = esc;

        // F3 toggles the diagnostics overlay, F4 the bloom/tonemap post pass
        // (a quick A/B between the graded and the raw image).
        if (!headless() && !modalActive) {
            bool f3 = in.keys[KEY_F3] != 0;
            if (f3 && !prevF3_) {
                hudOn_ = !hudOn_;
                fprintf(stderr, "[aw] hud %s\n", hudOn_ ? "on" : "off");
            }
            prevF3_ = f3;
            bool f4 = in.keys[KEY_F4] != 0;
            if (f4 && !prevF4_) {
                renderer_.setPostEnabled(!renderer_.postEnabled());
                fprintf(stderr, "[aw] post-processing %s\n",
                        renderer_.postEnabled() ? "on" : "off");
            }
            prevF4_ = f4;
        }

        const bool menuWasOpen = menuOpen_;
        if (menuWasOpen) {
            updateMenuFrame(in, modalActive);
            // The menu can resize the window (resolution setting): pick the new
            // client size up right away so this frame renders the size the
            // window actually has.
            int cw = 0, ch = 0;
            if (platform_->clientSize(cw, ch)) { in.width = cw; in.height = ch; }
        } else {
            // The bench drives the same scripted path as headless mode, so runs
            // on different machines (or settings) stay comparable.
            if (headless() || benchMode_) demoDrive(dt);

            bool wasGrounded = player_.grounded;
            simulateFrame(in, dt);
            if (!headless()) {
                if (wasGrounded && in.keys[KEY_SPACE]) audio_.play(Sfx::Jump);
                if (!wasGrounded && player_.grounded) audio_.play(Sfx::Land);
            }
        }
        if (quitRequested_) {
            settings_.save();
            break;
        }

        if (!headless()) {
            float aspect = in.width > 0 && in.height > 0 ? float(in.width) / float(in.height)
                                                         : 16.0f / 9.0f;
            float fov = settings_.fov;
            Mat4 proj = Mat4::perspective(deg2rad(fov), aspect, NEAR_PLANE, FAR_PLANE);
            Vec3 eye = player_.eye();
            Mat4 view = Mat4::lookAt(eye, eye + player_.forward(), {0, 1, 0});
            Mat4 vp = proj * view;
            int renderW = 0, renderH = 0;
            renderSizeFor(in.width, in.height, renderW, renderH);   // render scaling
            renderer_.setTime(float(stats_.elapsed));   // drifting clouds, twinkling stars
            stats_.drawnInstances = renderer_.render(wall_, player_, vp, aspect, in.width,
                                                     in.height, renderW, renderH, fov);
            // Overlays (crosshair, menu, modal dialog) always draw at the window
            // resolution, so text stays crisp at any render scale.
            renderer_.uiBegin(in.width, in.height);
            renderer_.uiCrosshair(platform_->uiScale(), target_.hit);
            if (hudOn_) drawHud();
            if (menuOpen_) menu_.render(renderer_);
            displayConfirm_.render(renderer_);
            renderer_.uiEnd();
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

    if (benchMode_ && !benchMs.empty()) {
        std::vector<double> sorted = benchMs;
        std::sort(sorted.begin(), sorted.end());
        auto pct = [&](double p) {
            size_t i = size_t(p * double(sorted.size() - 1) + 0.5);
            return sorted[i < sorted.size() ? i : sorted.size() - 1];
        };
        double sum = 0.0;
        for (double v : benchMs) sum += v;
        double avg = sum / double(benchMs.size());
        stats_.benchP50 = pct(0.50);
        stats_.benchP95 = pct(0.95);
        stats_.benchWorst = sorted.back();
        stats_.benchFrames = int(benchMs.size());
        fprintf(stderr,
                "[aw] bench: frames=%d  avg=%6.2f ms (%5.1f fps)  p50=%6.2f  p95=%6.2f  "
                "worst=%6.2f ms\n",
                stats_.benchFrames, avg, avg > 0.0 ? 1000.0 / avg : 0.0, stats_.benchP50,
                stats_.benchP95, stats_.benchWorst);
        fprintf(stderr,
                "[aw] bench: note — vsync/window compositing caps frame times; use "
                "--novsync for uncapped numbers\n");
    }
}

// ---- F3 diagnostics overlay ------------------------------------------------
// Small, unobtrusive, and drawn with the embedded font only (no assets). It is
// the window into what the look pipeline is doing: frame time, how much of the
// wall is on screen, the altitude and the palette that altitude picked.
void Game::drawHud() {
    const Look& L = renderer_.look();
    const int W = 0;   // placeholder to keep the padding below obvious
    (void)W;
    char line[8][64];
    float sunDeg = std::asin(clampf(L.sunDir.y, -1.0f, 1.0f)) * 57.29578f;
    std::snprintf(line[0], sizeof(line[0]), "FPS %5.1f  %5.2f MS",
                  stats_.fps, stats_.frameMs);
    std::snprintf(line[1], sizeof(line[1]), "CHUNKS %2d  INST %6d",
                  stats_.residentChunks, stats_.drawnInstances);
    std::snprintf(line[2], sizeof(line[2]), "ALT %5.0f  SUN %4.1f DEG",
                  double(player_.eye().y), double(sunDeg));
    std::snprintf(line[3], sizeof(line[3]), "FOG %.4f  STARS %.2f",
                  double(L.fogDensity), double(L.starAmount));
    std::snprintf(line[4], sizeof(line[4]), "LOOK %s  POST %s",
                  renderer_.lookPipeline() ? "GOLDEN" : "LEGACY",
                  renderer_.postEnabled() ? "ON" : "OFF");
    std::snprintf(line[5], sizeof(line[5]), "BRICKS %4d  MODS %4d",
                  stats_.residentChunks > 0 ? stats_.residentChunks * CHUNK_BRICKS : 0,
                  stats_.modifiedBricks);
    std::snprintf(line[6], sizeof(line[6]), "GRID %4d %4d",
                  int(player_.pos.x), int(player_.pos.y));
    std::snprintf(line[7], sizeof(line[7]), "F3 HUD   F4 POST   ESC MENU");

    int scale = platform_->uiScale() >= 1.5f ? 2 : 1;
    const float cw = 6.0f * float(scale), ch = 8.0f * float(scale);
    float x = 10.0f, y = 10.0f;
    float w = 0.0f;
    for (int i = 0; i < 8; ++i) {
        float lw = float(std::strlen(line[i])) * cw;
        if (lw > w) w = lw;
    }
    renderer_.uiRect(x - 6.0f, y - 6.0f, w + 12.0f, ch * 8.0f + 12.0f, 0.02f, 0.02f, 0.03f, 0.55f);
    for (int i = 0; i < 8; ++i) {
        float a = i == 7 ? 0.55f : 0.92f;
        renderer_.uiText(x, y + float(i) * (ch + 3.0f), scale, 0.95f, 0.90f, 0.78f, a, line[i]);
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
