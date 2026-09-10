// constants.hpp — world geometry + gameplay tuning constants.
#pragma once

#include <cstdint>

namespace aw {

// Brick grid: 1.0 unit cubes; the wall face lies in the XY plane at z=0 and
// bricks extend +Z (outward). The wall plane is vertical, gravity is -Y.
constexpr float BRICK = 1.0f;

// Chunk grid: CHUNK_X x CHUNK_Y bricks per chunk (one brick deep). Chunks are
// the unit of generation/streaming/culling and map 1:1 to instanced draw ranges.
constexpr int32_t CHUNK_X = 16;
constexpr int32_t CHUNK_Y = 16;
constexpr int32_t CHUNK_BRICKS = CHUNK_X * CHUNK_Y;          // 256 bricks / chunk
constexpr float CHUNK_WORLD_W = float(CHUNK_X) * BRICK;      // 16 m
constexpr float CHUNK_WORLD_H = float(CHUNK_Y) * BRICK;      // 16 m
constexpr int32_t WALL_WIDTH_BRICKS = 96;                    // total wall width (fixed)
constexpr int32_t CHUNKS_X = WALL_WIDTH_BRICKS / CHUNK_X;    // 6 chunks wide

// Streaming window (in chunk rows). Only chunks within this distance of the
// player's current chunk are resident; everything else is unloaded.
constexpr int32_t ACTIVE_CHUNK_RANGE = 4;                    // rows above/below player
constexpr int32_t RESIDENT_ROWS = ACTIVE_CHUNK_RANGE * 2 + 1; // 9 rows resident
constexpr int32_t MAX_RESIDENT_CHUNKS = RESIDENT_ROWS * CHUNKS_X;  // 54 chunks
constexpr int32_t MAX_RESIDENT_BRICKS = MAX_RESIDENT_CHUNKS * CHUNK_BRICKS;  // 13,824

// Interaction
constexpr float PULL_REACH = 6.5f;       // max distance for brick targeting
constexpr float PULL_DEPTH = 0.65f;      // max extension of a pulled brick (+Z)
constexpr float PULL_SPEED = 3.4f;       // units/sec of extension while animating

// Player
constexpr float EYE_HEIGHT = 1.62f;      // camera height above feet
constexpr float PLAYER_HALF_W = 0.28f;   // cylinder radius (collision)
constexpr float PLAYER_HEIGHT = 1.80f;   // total height (feet .. head)
constexpr float GRAVITY = 22.0f;         // -Y acceleration
constexpr float JUMP_VELOCITY = 7.6f;
constexpr float MOVE_SPEED = 5.2f;       // ground move speed
constexpr float AIR_ACCEL = 14.0f;
constexpr float GROUND_ACCEL = 60.0f;
constexpr float GROUND_FRICTION = 12.0f;
constexpr float TERMINAL_VELOCITY = 46.0f;
constexpr float SNAP_DIST = 0.18f;        // ground snap distance (sticky feel)

// Camera
constexpr float MOUSE_SENS = 0.0022f;
constexpr float FOV_DEG = 75.0f;
constexpr float NEAR_PLANE = 0.06f;
constexpr float FAR_PLANE = 320.0f;

// Rendering / LOD
constexpr float LOD_DIST_NEAR = 64.0f;    // full-resolution brick color per brick
constexpr float LOD_DIST_FAR = 160.0f;    // beyond this chunks use flat shading

}  // namespace aw
