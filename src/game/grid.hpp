// grid.hpp — mapping between brick grid coordinates and chunk coordinates.
#pragma once

#include <cstdint>

#include "../core/math.hpp"
#include "constants.hpp"

namespace aw {

// Brick grid cell (integer). The wall is infinite in X (horizontal) and
// infinite in Y (vertical).
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

// Deterministic brick mosaic: each chunk is covered by an exact tiling of
// non-overlapping rectangular bricks, so spawned bricks never stack on top of
// each other and never leave gaps. The tiling is a pure function of
// coordinates (chunk = 4x4 macro-cells of 4x4 cells; each macro-cell picks one
// of 8 exact tilings from a spatial hash), so the infinite wall regenerates
// losslessly and every system (collision, raycast, rendering) agrees on the
// layout without storing anything. A brick is identified by its origin: the
// minimum-corner cell of its rectangle (globally unique per brick).
struct BrickRect {
    int32_t ox = 0, oy = 0;   // origin cell (global brick coords)
    int32_t w = 1, h = 1;     // footprint in cells (1..BRICK_MAX_W/H)
};

// One tiling pattern = rect list (x,y,w,h in macro-local cells) + a 4x4 LUT
// mapping each macro-local cell to its rect index. Every pattern tiles its
// 4x4 macro-cell exactly (see testMosaic for the machine-checked proof).
struct MosaicPattern {
    uint8_t count = 0;
    int8_t rects[16][4]{};   // [i] = {x, y, w, h}
    uint8_t lut[16]{};       // [ly*4+lx] = rect index
};

inline constexpr MosaicPattern kMosaic[8] = {
    // P0: single 4x4 block.
    {1, {{0, 0, 4, 4}},
     {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // P1: four 2x2 quadrants.
    {4, {{0, 0, 2, 2}, {2, 0, 2, 2}, {0, 2, 2, 2}, {2, 2, 2, 2}},
     {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3}},
    // P2: sixteen 1x1 bricks.
    {16, {{0, 0, 1, 1}, {1, 0, 1, 1}, {2, 0, 1, 1}, {3, 0, 1, 1},
          {0, 1, 1, 1}, {1, 1, 1, 1}, {2, 1, 1, 1}, {3, 1, 1, 1},
          {0, 2, 1, 1}, {1, 2, 1, 1}, {2, 2, 1, 1}, {3, 2, 1, 1},
          {0, 3, 1, 1}, {1, 3, 1, 1}, {2, 3, 1, 1}, {3, 3, 1, 1}},
     {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}},
    // P3: four horizontal 4x1 stripes.
    {4, {{0, 0, 4, 1}, {0, 1, 4, 1}, {0, 2, 4, 1}, {0, 3, 4, 1}},
     {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3}},
    // P4: four vertical 1x4 stripes.
    {4, {{0, 0, 1, 4}, {1, 0, 1, 4}, {2, 0, 1, 4}, {3, 0, 1, 4}},
     {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3}},
    // P5: two 2x2 blocks over one 4x2 slab.
    {3, {{0, 0, 2, 2}, {2, 0, 2, 2}, {0, 2, 4, 2}},
     {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2}},
    // P6: one 2x4 slab beside two 2x2 blocks.
    {3, {{0, 0, 2, 4}, {2, 0, 2, 2}, {2, 2, 2, 2}},
     {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2}},
    // P7: running bond (offset 2x1 courses with 1x1 closers).
    {10, {{0, 0, 2, 1}, {2, 0, 2, 1}, {0, 1, 1, 1}, {1, 1, 2, 1}, {3, 1, 1, 1},
           {0, 2, 2, 1}, {2, 2, 2, 1}, {0, 3, 1, 1}, {1, 3, 2, 1}, {3, 3, 1, 1}},
     {0, 0, 1, 1, 2, 3, 3, 4, 5, 5, 6, 6, 7, 8, 8, 9}},
};

// The brick containing grid cell (bx,by). Pure function of coordinates.
inline BrickRect brickAt(int32_t bx, int32_t by) {
    ChunkCoord cc = brickToChunk(bx, by);
    int32_t lx = bx - cc.cx * CHUNK_X;   // 0..15 (brickToChunk floors)
    int32_t ly = by - cc.cy * CHUNK_Y;
    int32_t mx = lx / MOSAIC_CELLS;      // macro-cell 0..3 inside the chunk
    int32_t my = ly / MOSAIC_CELLS;
    // Global macro-cell coords select the tiling pattern (stable across
    // chunk (un)loads because each macro-cell tiles itself exactly).
    uint32_t p = hash2d(cc.cx * (CHUNK_X / MOSAIC_CELLS) + mx,
                        cc.cy * (CHUNK_Y / MOSAIC_CELLS) + my) %
                 8u;
    const MosaicPattern& pat = kMosaic[p];
    const int8_t* r = pat.rects[pat.lut[(ly % MOSAIC_CELLS) * MOSAIC_CELLS +
                                       (lx % MOSAIC_CELLS)]];
    BrickRect out;
    out.ox = cc.cx * CHUNK_X + mx * MOSAIC_CELLS + r[0];
    out.oy = cc.cy * CHUNK_Y + my * MOSAIC_CELLS + r[1];
    out.w = r[2];
    out.h = r[3];
    return out;
}

// True if (bx,by) is a brick origin (the minimum-corner cell of its brick).
inline bool isBrickOrigin(int32_t bx, int32_t by) {
    BrickRect r = brickAt(bx, by);
    return r.ox == bx && r.oy == by;
}

// Depth extent of a brick body behind the wall face (larger footprint edge).
inline float brickExtent(const BrickRect& r) {
    return float(r.w > r.h ? r.w : r.h) * BRICK;
}

// World-space position of the containing brick's minimum corner. The wall
// face is at z=0 and the brick material sits BEHIND it (z in [-e, 0] for
// extent e) when flush.
inline Vec3 brickMin(int32_t bx, int32_t by) {
    BrickRect r = brickAt(bx, by);
    return {float(r.ox) * BRICK, float(r.oy) * BRICK, -brickExtent(r)};
}

// World-space AABB of the containing brick at rest (flush with the wall).
inline AABB brickAABB(int32_t bx, int32_t by) {
    BrickRect r = brickAt(bx, by);
    Vec3 mn = brickMin(bx, by);
    float e = brickExtent(r);
    return AABB(mn, {mn.x + float(r.w) * BRICK, mn.y + float(r.h) * BRICK,
                     mn.z + e});
}

// Project world position to containing brick (floor semantics for negative).
inline BrickCoord worldToBrick(const Vec3& p) {
    return {floori(p.x / BRICK), floori(p.y / BRICK)};
}

}  // namespace aw
