// game.hpp — top-level game orchestration: owns the platform, world, player,
// interaction and renderer, and runs the frame loop (interactive or headless).
#pragma once

#include <cstdint>

#include "../audio/audio.hpp"
#include "../core/math.hpp"
#include "../core/platform.hpp"
#include "../render/renderer.hpp"
#include "constants.hpp"
#include "display_confirm.hpp"
#include "interaction.hpp"
#include "menu.hpp"
#include "player.hpp"
#include "settings.hpp"
#include "wall.hpp"

namespace aw {

struct FrameStats {
    int32_t frame = 0;
    double elapsed = 0.0;        // total wall-clock seconds
    double frameMs = 0.0;        // smoothed frame time
    double fps = 0.0;
    int32_t residentChunks = 0;
    int32_t drawnInstances = 0;
    int32_t drawCalls = 1;       // instanced wall = 1 draw call
    int32_t modifiedBricks = 0;
    int32_t playerBrickY = 0;
    // Frame-time percentiles, filled in by --bench runs (milliseconds).
    double benchP50 = 0.0, benchP95 = 0.0, benchWorst = 0.0;
    int benchFrames = 0;
};

// The display-related part of Settings: what a display change actually toggles.
// Kept as a value so a change can be applied provisionally and reverted exactly.
struct DisplayConfig {
    DisplayMode mode = DisplayMode::Windowed;
    int width = 1280, height = 720;                    // windowed client size
    int renderWidth = 0, renderHeight = 0;             // borderless render resolution
    int modeWidth = 0, modeHeight = 0, modeRefresh = 0; // exclusive display mode

    bool operator==(const DisplayConfig& o) const {
        return mode == o.mode && width == o.width && height == o.height &&
               renderWidth == o.renderWidth && renderHeight == o.renderHeight &&
               modeWidth == o.modeWidth && modeHeight == o.modeHeight &&
               modeRefresh == o.modeRefresh;
    }
    bool operator!=(const DisplayConfig& o) const { return !(*this == o); }
};

// Human-readable description of a configuration (embedded font: no brackets).
void describeDisplayConfig(const DisplayConfig& cfg, char* out, size_t n);

class Game {
public:
    // `preferHeadless` skips the windowed backend entirely (CI / benchmark).
    bool init(const char* title, int width, int height, bool preferHeadless = false);
    void shutdown();

    // Run until the platform asks to quit.
    void run();

    // Advance the simulation only (no platform/render). Testable entry point.
    void simulateFrame(const FrameInput& in, float dt);

    // Restart the run: clears all brick modifications, cancels lerps, reseeds
    // the starting platform and respawns the player (settings are kept).
    void restart();

    FrameStats stats() const { return stats_; }

    // Display mode plumbing (exposed for tests).
    static DisplayConfig displayConfigOf(const Settings& s);
    static void setDisplayConfig(Settings& s, const DisplayConfig& cfg);
    // Applies a configuration to the backend; false when it is refused.
    bool applyDisplayConfig(const DisplayConfig& cfg);
    // Same, for an explicitly supplied backend (used by the tests).
    static bool applyDisplayConfigTo(Platform& p, const DisplayConfig& cfg);
    // A restored exclusive mode must be confirmed by the user before it counts
    // as the stable configuration (see init()).
    static bool needsStartupConfirm(DisplayMode mode, bool headless);
    // Keeps a windowed size on a standard resolution (see snapToStandard).
    static void snapWindowSizeToStandard(Settings& s);
    // Applies the pending display settings and starts the confirm dialog.
    // Returns false when the backend refused the change (settings are restored).
    bool requestDisplayApply();
    // Restores the last confirmed configuration (dialog timeout / Esc).
    void revertDisplayChange();
    // Runs the modal confirmation dialog for one frame; true while it is active
    // (it then owns the input). Called by the main loop.
    bool pollDisplayConfirm(const FrameInput& in, float dt);
    // Settings overlay: open/close (Esc) and one frame of input handling (the
    // modal dialog is polled first, then the menu, then pending display changes).
    void openMenu();
    void closeMenu();
    void updateMenuFrame(const FrameInput& in, bool modalActive);
    // Test/demo hook: the overlay for one frame with a supplied input.
    void stepOverlay(const FrameInput& in, float dt);
    // Key state used when the confirm dialog opens (see DisplayConfirm::begin).
    void setPendingHeldKeys(const uint8_t* keys);
    bool displayConfirmActive() const { return displayConfirm_.active(); }
    const DisplayConfig& stableDisplayConfig() const { return displayStable_; }
    // Internal 3D render resolution for a window of w x h (render scaling).
    void renderSizeFor(int winW, int winH, int& rw, int& rh) const;

    // Headless/demo mode flag.
    bool headless() const { return !platform_->info().hasGL; }

    // ---- benchmarking -------------------------------------------------------
    // `--bench`: run the scripted camera path (the same one headless uses) for a
    // fixed number of frames and report frame-time percentiles. Comparability
    // comes from the fixed path, so two machines (or two settings) can be
    // compared directly. Frame times are still vsync-capped unless the driver
    // swap interval is off.
    void setBenchMode(bool on) { benchMode_ = on; }
    bool benchMode() const { return benchMode_; }
    // Stop after `n` frames in any mode (0 = run until quit).
    void setFrameLimit(int n) { frameLimit_ = n; }
    // Forwarded to the backend (see Platform::setSwapInterval). False when the
    // driver has no swap-control extension.
    bool setSwapInterval(int interval) {
        return platform_ ? platform_->setSwapInterval(interval) : false;
    }

    // ---- look / HUD ---------------------------------------------------------
    // F3 diagnostics overlay (frame times, chunk/instance counts, altitude and
    // the palette in use). Off by default: the shipped look is unobstructed.
    bool hudVisible() const { return hudOn_; }
    void setHudVisible(bool on) { hudOn_ = on; }
    // Renderer access (look toggles, tests).
    Renderer& renderer() { return renderer_; }

    // Exposed for tests.
    Wall& wall() { return wall_; }
    Player& player() { return player_; }
    Interaction& interaction() { return interaction_; }
    Settings& settings() { return settings_; }
    Menu& menu() { return menu_; }

private:
    void demoDrive(float dt);
    float seedWorld();        // (re)builds the starting platform; returns its top
    void fitWindowToMonitor(); // clamp the windowed size to the monitor
    void drawHud();            // F3 diagnostics overlay

    Platform* platform_ = nullptr;
    Wall wall_;
    Player player_;
    Interaction interaction_;
    Renderer renderer_;
    Settings settings_;
    Audio audio_;
    Menu menu_;
    FrameStats stats_;
    // Display state: the last configuration the user confirmed, plus the modal
    // dialog shown while a new one is provisional.
    DisplayConfig displayStable_;
    DisplayConfig displayPending_;   // provisionally applied, awaiting confirmation
    DisplayConfirm displayConfirm_;
    uint8_t pendingHeldKeys_[512]{};
    bool quitRequested_ = false;

    double lastTime_ = 0.0;
    ChunkCoord lastChunk_{0, 0};
    bool initialized_ = false;
    bool menuOpen_ = false;
    bool prevEsc_ = false;
    bool hudOn_ = false;
    bool benchMode_ = false;
    int frameLimit_ = 0;
    bool prevF3_ = false;
    bool prevF4_ = false;
    TargetResult target_;
};

}  // namespace aw
