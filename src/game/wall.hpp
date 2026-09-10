// wall.hpp — the infinite vertical brick wall: chunk residency, streaming, and
// the persistent store of player-modified bricks.
//
// Memory model (all pre-allocated, zero runtime allocation):
//   * a fixed pool of `MAX_RESIDENT_CHUNKS` chunk slots (SoA brick data);
//   * an open-addressing resident map (ChunkCoord -> pool slot);
//   * a fixed-capacity hash store of *modified* bricks (pull/push overrides).
//
// Chunks hold only deterministic base data (shade variation). Brick mutations
// live in the persistent store, so unloaded chunks regenerate correctly and
// player-made ledges survive chunk streaming.
#pragma once

#include <cstdint>
#include <cstdio>

#include "../core/math.hpp"
#include "chunk.hpp"
#include "constants.hpp"
#include "grid.hpp"

namespace aw {

// ---------------------------------------------------------------------------
// Persistent store of modified bricks (packed, fixed capacity, no allocation).
// ---------------------------------------------------------------------------
class BrickStore {
public:
    static constexpr int CAP_BITS = 14;
    static constexpr int CAP = 1 << CAP_BITS;      // 16384 modified bricks
    static constexpr int CAP_MASK = CAP - 1;

    struct Slot {
        uint64_t key = 0;      // packBrick(x,y); 0 == empty
        uint8_t state = STATE_REST;
        float depth = 0.0f;
    };

    static inline uint64_t packBrick(int32_t x, int32_t y) {
        return (uint64_t(uint32_t(x)) << 32) | uint32_t(y);
    }
    static inline void unpackBrick(uint64_t key, int32_t& x, int32_t& y) {
        x = int32_t(uint32_t(key >> 32));
        y = int32_t(uint32_t(key));
    }

    // Returns nullptr if not present.
    Slot* find(int32_t x, int32_t y) {
        uint64_t k = packBrick(x, y);
        size_t i = size_t(mix64(k) & uint64_t(CAP_MASK));
        for (int32_t n = 0; n < CAP; ++n) {
            Slot& s = slots_[i];
            if (s.key == 0) return nullptr;         // empty slot terminates probe
            if (s.key == k) return &s;
            i = (i + 1) & CAP_MASK;
        }
        return nullptr;
    }

    const Slot* find(int32_t x, int32_t y) const {
        return const_cast<BrickStore*>(this)->find(x, y);
    }

    // Insert or update. Returns false only if the store is exhausted (brick
    // state will then simply not persist across chunk reloads).
    bool set(int32_t x, int32_t y, BrickState state, float depth) {
        uint64_t k = packBrick(x, y);
        size_t i = size_t(mix64(k) & uint64_t(CAP_MASK));
        for (int32_t n = 0; n < CAP; ++n) {
            Slot& s = slots_[i];
            if (s.key == 0 || s.key == k) {
                uint8_t oldState = (s.key == k) ? s.state : uint8_t(STATE_REST);
                if (s.key == 0) ++count_;   // new brick, not an update
                s.key = k;
                s.state = uint8_t(state);
                s.depth = depth;
                bool wasActive = oldState != STATE_REST;
                bool nowActive = state != STATE_REST;
                if (!wasActive && nowActive) ++active_;
                else if (wasActive && !nowActive) --active_;
                return true;
            }
            i = (i + 1) & CAP_MASK;
        }
        return false;
    }

    uint8_t state(int32_t x, int32_t y) const {
        const Slot* s = find(x, y);
        return s ? s->state : uint8_t(STATE_REST);
    }
    float depth(int32_t x, int32_t y) const {
        const Slot* s = find(x, y);
        return s ? s->depth : 0.0f;
    }

    int32_t count() const { return count_; }        // distinct bricks ever touched
    int32_t activeCount() const { return active_; } // bricks currently extended

    // Iterate all occupied slots (used when (re)computing a chunk's active count).
    template <typename F>
    void forEach(F&& f) const {
        for (int32_t i = 0; i < CAP; ++i) {
            if (slots_[i].key != 0) {
                int32_t x, y;
                unpackBrick(slots_[i].key, x, y);
                f(x, y, static_cast<BrickState>(slots_[i].state), slots_[i].depth);
            }
        }
    }

private:
    Slot slots_[CAP]{};
    int32_t count_ = 0;
    int32_t active_ = 0;
};

// ---------------------------------------------------------------------------
// The wall: chunk residency + streaming + brick queries.
// ---------------------------------------------------------------------------
class Wall {
public:
    static constexpr int MAP_CAP = 256;
    static constexpr int MAP_MASK = MAP_CAP - 1;

    // Open-addressing map with tombstones so deletions don't break probe chains.
    struct MapEntry {
        int32_t cx = 0, cy = 0;
        int32_t poolIndex = -1;
        uint8_t state = 0;   // 0 = empty, 1 = occupied, 2 = tombstone
    };

    Wall() {
        // Initialize the free-slot stack with every pool slot.
        for (int32_t i = 0; i < MAX_RESIDENT_CHUNKS; ++i) {
            freeSlots_[i] = MAX_RESIDENT_CHUNKS - 1 - i;
            pool_[i].slot = i;
        }
        freeCount_ = MAX_RESIDENT_CHUNKS;
    }

    // ---- chunk residency ---------------------------------------------------

    Chunk* find(ChunkCoord c) {
        size_t i = mapIndex(c);
        for (int32_t n = 0; n < MAP_CAP; ++n) {
            MapEntry& e = map_[i];
            if (e.state == 0) return nullptr;                     // empty -> absent
            if (e.state == 1 && e.cx == c.cx && e.cy == c.cy) return &pool_[e.poolIndex];
            i = (i + 1) & MAP_MASK;
        }
        return nullptr;
    }
    const Chunk* find(ChunkCoord c) const { return const_cast<Wall*>(this)->find(c); }

    Chunk* acquire(ChunkCoord c) {
        if (Chunk* ex = find(c)) return ex;
        if (freeCount_ == 0) {
            // Pool exhausted: evict the furthest chunk (simple safety net; the
            // streaming window is sized so this never triggers in practice).
            evictFurthest(c.cx, c.cy);
        }
        int32_t slot = freeSlots_[--freeCount_];
        Chunk& ch = pool_[slot];
        ch.reset(c.cx, c.cy);
        ch.slot = slot;
        // Recompute the active-brick flags + count from the persistent store so
        // player-made ledges survive chunk streaming.
        int32_t ox = c.cx * CHUNK_X, oy = c.cy * CHUNK_Y;
        store_.forEach([&](int32_t bx, int32_t by, BrickState s, float) {
            if (s != STATE_REST && bx >= ox && bx < ox + CHUNK_X && by >= oy && by < oy + CHUNK_Y) {
                int32_t local = brickLocalIndex(bx, by);
                if (local >= 0 && local < CHUNK_BRICKS && !ch.activeLocal[local]) {
                    ch.activeLocal[local] = 1;
                    ++ch.activeCount;
                }
            }
        });
        ch.dirty = true;
        insertMap(c, slot);
        return &ch;
    }

    void release(ChunkCoord c) {
        size_t i = mapIndex(c);
        for (int32_t n = 0; n < MAP_CAP; ++n) {
            MapEntry& e = map_[i];
            if (e.state == 0) return;   // absent
            if (e.state == 1 && e.cx == c.cx && e.cy == c.cy) {
                e.state = 2;            // tombstone: keep probe chains intact
                freeSlots_[freeCount_++] = e.poolIndex;
                pool_[e.poolIndex].slot = -1;
                return;
            }
            i = (i + 1) & MAP_MASK;
        }
    }

    int32_t residentCount() const { return MAX_RESIDENT_CHUNKS - freeCount_; }

    // Iterate resident chunks in stable map order (used by the renderer to
    // compact instance ranges into a single contiguous draw).
    template <typename F>
    void forEachResident(F&& f) {
        for (int32_t i = 0; i < MAP_CAP; ++i) {
            MapEntry& e = map_[i];
            if (e.state == 1) f(pool_[e.poolIndex]);
        }
    }
    template <typename F>
    void forEachResident(F&& f) const {
        for (int32_t i = 0; i < MAP_CAP; ++i) {
            const MapEntry& e = map_[i];
            if (e.state == 1) f(pool_[e.poolIndex]);
        }
    }

    // Make resident all chunks within ACTIVE_CHUNK_RANGE of the player's chunk
    // (a 9x9 window in X and Y — the wall is infinite in all directions) and
    // release the rest. Cheap when the player stays in the same chunk.
    void streamAround(int32_t playerCx, int32_t playerCy) {
        // Release out-of-range residents FIRST so their pool slots are free
        // for the incoming window. (Acquiring first would force the pool's
        // evict-on-demand path to run mid-slide, where it can evict window
        // chunks whose turn already passed, shrinking the resident set.)
        for (int32_t i = 0; i < MAP_CAP; ++i) {
            MapEntry& e = map_[i];
            if (e.state != 1) continue;
            if (e.cx < playerCx - ACTIVE_CHUNK_RANGE || e.cx > playerCx + ACTIVE_CHUNK_RANGE ||
                e.cy < playerCy - ACTIVE_CHUNK_RANGE || e.cy > playerCy + ACTIVE_CHUNK_RANGE)
                release({e.cx, e.cy});
        }
        for (int32_t dx = -ACTIVE_CHUNK_RANGE; dx <= ACTIVE_CHUNK_RANGE; ++dx) {
            for (int32_t dy = -ACTIVE_CHUNK_RANGE; dy <= ACTIVE_CHUNK_RANGE; ++dy)
                acquire({playerCx + dx, playerCy + dy});
        }
    }

    // ---- brick state / geometry -------------------------------------------

    // Current extension depth of a brick (0 if flush).
    float brickDepth(int32_t bx, int32_t by) const { return store_.depth(bx, by); }
    BrickState brickState(int32_t bx, int32_t by) const {
        return static_cast<BrickState>(store_.state(bx, by));
    }

    // World AABB of a brick, accounting for extension. A rigid cube of the
    // brick's size class that slides outward: flush occupies z in [-s, 0];
    // extended by depth d occupies z in [d-s, d]. The wall is infinite, so
    // every (bx,by) cell holds a brick (corner-anchored at its cell minimum).
    AABB brickAABB(int32_t bx, int32_t by) const {
        float s = brickSize(bx, by);
        float d = store_.depth(bx, by);
        float x0 = float(bx), y0 = float(by);
        return AABB({x0, y0, d - s}, {x0 + s, y0 + s, d});
    }

    // Returns the resident chunk containing brick (bx,by), or nullptr.
    Chunk* chunkAtBrick(int32_t bx, int32_t by) {
        return find(brickToChunk(bx, by));
    }

    // Record a brick modification (pull/push). Keeps the owning chunk in sync.
    void setBrick(int32_t bx, int32_t by, BrickState state, float depth) {
        store_.set(bx, by, state, depth);
        if (Chunk* ch = chunkAtBrick(bx, by)) {
            ch->dirty = true;
            // Track the active-brick count incrementally (used by fast paths).
            int32_t local = brickLocalIndex(bx, by);
            bool wasActive = ch->activeLocal[local];
            bool nowActive = (state != STATE_REST);
            if (nowActive != wasActive) {
                ch->activeLocal[local] = nowActive;
                ch->activeCount += nowActive ? 1 : -1;
            }
        }
    }

    BrickStore& store() { return store_; }
    const BrickStore& store() const { return store_; }
    Chunk& poolSlot(int32_t i) { return pool_[i]; }
    const Chunk& poolSlot(int32_t i) const { return pool_[i]; }

private:
    static size_t mapIndex(ChunkCoord c) {
        return size_t(mix64(uint64_t(uint32_t(c.cy)) * 0x9e3779b97f4a7c15ull ^
                            uint64_t(uint32_t(c.cx)) * 0xbf58476d1ce4e5b9ull) &
                      uint64_t(MAP_MASK));
    }

    void insertMap(ChunkCoord c, int32_t poolIndex) {
        size_t i = mapIndex(c);
        for (int32_t n = 0; n < MAP_CAP; ++n) {
            MapEntry& e = map_[i];
            if (e.state != 1) {   // empty or tombstone slot is reusable
                e.state = 1; e.cx = c.cx; e.cy = c.cy; e.poolIndex = poolIndex;
                return;
            }
            i = (i + 1) & MAP_MASK;
        }
        // Should be unreachable with MAP_CAP >> resident count.
        fprintf(stderr, "[aw] resident map exhausted\n");
    }

    void evictFurthest(int32_t playerCx, int32_t playerCy) {
        int32_t bestIdx = -1, bestDist = -1;
        for (int32_t i = 0; i < MAP_CAP; ++i) {
            MapEntry& e = map_[i];
            if (e.state != 1) continue;
            int32_t dx = e.cx - playerCx; if (dx < 0) dx = -dx;
            int32_t dy = e.cy - playerCy; if (dy < 0) dy = -dy;
            int32_t d = dx > dy ? dx : dy;   // Chebyshev distance on the chunk grid
            if (d > bestDist) { bestDist = d; bestIdx = i; }
        }
        if (bestIdx >= 0) release({map_[bestIdx].cx, map_[bestIdx].cy});
    }

    Chunk pool_[MAX_RESIDENT_CHUNKS]{};
    int32_t freeSlots_[MAX_RESIDENT_CHUNKS]{};
    int32_t freeCount_ = 0;
    MapEntry map_[MAP_CAP]{};
    BrickStore store_;
};

}  // namespace aw
