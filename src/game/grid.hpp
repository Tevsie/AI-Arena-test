// grid.hpp — mapping between brick grid coordinates and chunk coordinates.
#pragma once

#include <cstdint>

#include "../core/math.hpp"

namespace aw {

// Brick grid cell (integer). The wall is finite in X, infinite in Y (vertical).
struct BrickCoord {
    int32_t x = 0, y = 0;

    bool operator==(const BrickCoord& o) const { return x == o.x && y == o.y; }
    bool operator!=(const BrickCoord& o) const { return !(*this == o); }
    bool operator<(const BrickCoord& o) const {
        return x != o.x ? x < o.x : y < o.y;
    }
};

// Chunk coordinate (row along Y, column along X). A chunk covers
// CHUNK_X x CHUNK_Y bricks starting at brick (cx*CHUNK_X, cy*CHUNK_Y).
struct ChunkCoord {
    int32_t cx = 0, cy = 0;
    bool operator==(const ChunkCoord& o) const { return cx == o.cx && cy == o.cy; }
    bool operator!=(const ChunkCoord& o) const { return !(*this == o); }
};

// Column of chunks along the wall (fixed X, infinite Y).
inline ChunkCoord brickToChunk(int32_t bx, int32_t by) {
    int32_t cx = bx >= 0 ? bx / CHUNK_X : (bx - (CHUNK_X - 1)) / CHUNK_X;
    int32_t cy = by >= 0 ? by / CHUNK_Y : (by - (CHUNK_Y - 1)) / CHUNK_Y;
    return {cx, cy};
}

inline BrickCoord chunkOrigin(ChunkCoord c) { return {c.cx * CHUNK_X, c.cy * CHUNK_Y}; }

// Local brick index inside a chunk (0..CHUNK_BRICKS-1).
inline int32_t brickLocalIndex(int32_t bx, int32_t by) {
    int32_t lx = bx - (brickToChunk(bx, by).cx * CHUNK_X);
    int32_t ly = by - (brickToChunk(bx, by).cy * CHUNK_Y);
    return ly * CHUNK_X + lx;
}

// World-space position of a brick's minimum (back) corner. The wall face is at
// z=0 and the brick material sits BEHIND it (z in [-1, 0]) when flush.
inline Vec3 brickMin(int32_t bx, int32_t by) {
    return {float(bx) * BRICK, float(by) * BRICK, -BRICK};
}

// World-space AABB of a brick at rest (flush with the wall): z in [-1, 0].
inline AABB brickAABB(int32_t bx, int32_t by) {
    Vec3 mn = brickMin(bx, by);
    return AABB(mn, {mn.x + BRICK, mn.y + BRICK, mn.z + BRICK});
}

// Project world position to containing brick (floor semantics for negative).
inline BrickCoord worldToBrick(const Vec3& p) {
    return {floori(p.x / BRICK), floori(p.y / BRICK)};
}

}  // namespace aw
