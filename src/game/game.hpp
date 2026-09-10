// game.hpp — top-level game orchestration: owns the platform, world, player,
// interaction and renderer, and runs the frame loop (interactive or headless).
#pragma once

#include <cstdint>

#include "../core/math.hpp"
#include "../core/platform.hpp"
#include "../render/renderer.hpp"
#include "constants.hpp"
#include "interaction.hpp"
#include "player.hpp"
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
};

class Game {
public:
    // `preferHeadless` skips the windowed backend entirely (CI / benchmark).
    bool init(const char* title, int width, int height, bool preferHeadless = false);
    void shutdown();

    // Run until the platform asks to quit.
    void run();

    // Advance the simulation only (no platform/render). Testable entry point.
    void simulateFrame(const FrameInput& in, float dt);

    FrameStats stats() const { return stats_; }

    // Headless/demo mode flag.
    bool headless() const { return !platform_->info().hasGL; }

    // Exposed for tests.
    Wall& wall() { return wall_; }
    Player& player() { return player_; }
    Interaction& interaction() { return interaction_; }

private:
    void demoDrive(float dt);

    Platform* platform_ = nullptr;
    Wall wall_;
    Player player_;
    Interaction interaction_;
    Renderer renderer_;
    FrameStats stats_;

    double lastTime_ = 0.0;
    ChunkCoord lastChunk_{0, 0};
    bool initialized_ = false;
    TargetResult target_;
};

}  // namespace aw
