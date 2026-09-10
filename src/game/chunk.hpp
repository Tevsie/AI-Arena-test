// chunk.hpp — a wall chunk: CHUNK_X x CHUNK_Y bricks, one deep.
// Flat, POD, allocation-free. Brick state and animation depth live in SoA
// arrays indexed by local brick id (0..CHUNK_BRICKS-1).
#pragma once

#include <cstdint>

#include "constants.hpp"
#include "grid.hpp"

namespace aw {

enum BrickState : uint8_t {
    STATE_REST = 0,      // flush with the wall
    STATE_EXTENDING = 1, // pulling out (+Z)
    STATE_RETRACTING = 2,// pushing back in
    STATE_EXTENDED = 3,  // fully out
};

struct Chunk {
    // Resident-pool slot this chunk currently occupies (-1 = not resident).
    int32_t slot = -1;
    ChunkCoord coord{0, 0};
    uint8_t shade[CHUNK_BRICKS]{};   // precomputed visual variation (0..255)
    uint8_t activeLocal[CHUNK_BRICKS]{};  // true if this brick is modified (non-rest)
    uint16_t activeCount = 0;        // number of active bricks in this chunk
    uint16_t brickCount = 0;         // mosaic bricks in this chunk (<= 256)
    bool dirty = true;               // instance transform data needs re-upload

    void reset(int32_t cx, int32_t cy) {
        coord = {cx, cy};
        dirty = true;
        activeCount = 0;
        brickCount = 0;
        // Procedural per-brick shade from a deterministic spatial hash keyed by
        // the brick's origin cell, so every brick has one uniform color. Only
        // origin cells carry state; doing it at generation time (never at
        // render/update time) keeps hot loops pure.
        for (int32_t i = 0; i < CHUNK_BRICKS; ++i) {
            int32_t bx = coord.cx * CHUNK_X + (i % CHUNK_X);
            int32_t by = coord.cy * CHUNK_Y + (i / CHUNK_X);
            BrickRect r = brickAt(bx, by);
            shade[i] = uint8_t((hash2d(r.ox, r.oy) >> 20) & 0xFF);
            activeLocal[i] = 0;
            if (r.ox == bx && r.oy == by) ++brickCount;
        }
    }
};

}  // namespace aw
