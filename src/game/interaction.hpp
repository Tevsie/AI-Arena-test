// interaction.hpp — center-screen brick targeting (raycast) and the
// pull/push mechanics: a mouse CLICK starts a 3-second lerp of the target
// brick outward (pull) or back flush (push). In-flight lerps live in a small
// fixed pool (no allocation) and are advanced once per frame by update().
#pragma once

#include <cstdint>

#include "../core/math.hpp"
#include "../audio/audio.hpp"
#include "constants.hpp"
#include "grid.hpp"
#include "player.hpp"
#include "wall.hpp"

namespace aw {

// Slab-method ray vs AABB. Returns true and sets tNear if the ray hits.
inline bool rayAABB(const Ray& ray, const AABB& b, float& tOut) {
    float tmin = 0.0f, tmax = 1e30f;
    auto slab = [&](float o, float d, float mn, float mx) {
        if (std::fabs(d) < 1e-9f) {
            if (o < mn || o > mx) return false;
        } else {
            float inv = 1.0f / d;
            float t1 = (mn - o) * inv, t2 = (mx - o) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
        return true;
    };
    if (!slab(ray.o.x, ray.d.x, b.mn.x, b.mx.x)) return false;
    if (!slab(ray.o.y, ray.d.y, b.mn.y, b.mx.y)) return false;
    if (!slab(ray.o.z, ray.d.z, b.mn.z, b.mx.z)) return false;
    if (tmax < 0.0f) return false;
    tOut = tmin;
    return true;
}

struct TargetResult {
    bool hit = false;
    int32_t bx = 0, by = 0;
    float depth = 0.0f;   // current extension of the target brick
    float dist = 0.0f;    // distance to the hit point
};

class Interaction {
public:
    static constexpr int MAX_LERPS = 256;   // max simultaneous brick lerps

    struct BrickLerp {
        bool active = false;
        int32_t bx = 0, by = 0;
        float from = 0.0f;   // depth when the lerp started
        float to = 0.0f;     // target depth (PULL_DEPTH out, 0 in)
        float t = 0.0f;      // elapsed seconds (done at BRICK_LERP_TIME)
    };

    // Cast the center-screen ray against the brick grid within PULL_REACH.
    // Uses an exact 2D grid DDA over the wall's (x,y) cells (infinite in both
    // axes). Every cell's own brick covers the whole cell (sizes >= 1 m and
    // corner-anchored), so it is tested first; neighbors whose (up to 5 m)
    // bodies overlap the cell are tested too, nearest wins.
    TargetResult cast(const Player& player, const Wall& wall) const {
        TargetResult r;
        Vec3 eye = player.eye();
        Vec3 dir = player.forward();
        Ray ray{eye, dir};

        BrickCoord c = worldToBrick(eye);
        int stepX = dir.x >= 0.0f ? 1 : -1;
        int stepY = dir.y >= 0.0f ? 1 : -1;
        float tDeltaX = std::fabs(dir.x) < 1e-9f ? 1e30f : std::fabs(BRICK / dir.x);
        float tDeltaY = std::fabs(dir.y) < 1e-9f ? 1e30f : std::fabs(BRICK / dir.y);
        float nextX = dir.x >= 0.0f ? (float(c.x) + 1.0f) * BRICK : float(c.x) * BRICK;
        float nextY = dir.y >= 0.0f ? (float(c.y) + 1.0f) * BRICK : float(c.y) * BRICK;
        float tMaxX = std::fabs(dir.x) < 1e-9f ? 1e30f : (nextX - eye.x) / dir.x;
        float tMaxY = std::fabs(dir.y) < 1e-9f ? 1e30f : (nextY - eye.y) / dir.y;

        for (int i = 0; i < 64; ++i) {
            // 1) The brick owning this cell always covers it — test it first so
            //    the cell under the crosshair wins ties against overlapping
            //    large neighbors.
            {
                AABB b = wall.brickAABB(c.x, c.y);
                float tn = 0.0f;
                if (rayAABB(ray, b, tn) && tn <= PULL_REACH) {
                    r.hit = true;
                    r.bx = c.x; r.by = c.y;
                    r.depth = wall.brickDepth(c.x, c.y);
                    r.dist = tn;
                    return r;
                }
            }
            // 2) Neighboring bricks (corner-anchored, up to 5 m) whose bodies
            //    reach into this cell from -X/-Y.
            float bestT = PULL_REACH + 1.0f;
            int32_t hitX = 0, hitY = 0;
            bool found = false;
            for (int32_t oy = -BRICK_SIZE_MAX_CELLS; oy <= 0; ++oy) {
                for (int32_t ox = -BRICK_SIZE_MAX_CELLS; ox <= 0; ++ox) {
                    if (ox == 0 && oy == 0) continue;
                    int32_t bx = c.x + ox, by = c.y + oy;
                    // Quick reject: the neighbor must actually reach this cell.
                    float s = brickSize(bx, by);
                    if (float(bx) + s <= float(c.x) || float(by) + s <= float(c.y))
                        continue;
                    AABB b = wall.brickAABB(bx, by);
                    float tn = 0.0f;
                    if (rayAABB(ray, b, tn) && tn <= PULL_REACH && tn < bestT) {
                        bestT = tn; hitX = bx; hitY = by; found = true;
                    }
                }
            }
            if (found) {
                r.hit = true;
                r.bx = hitX; r.by = hitY;
                r.depth = wall.brickDepth(hitX, hitY);
                r.dist = bestT;
                return r;
            }
            // Advance to the next cell along the nearer axis.
            if (tMaxX < tMaxY) { tMaxX += tDeltaX; c.x += stepX; }
            else { tMaxY += tDeltaY; c.y += stepY; }
        }
        return r;
    }

    // True when the player is standing ON the brick's top face (i.e. its
    // retraction would drop the player). A brick the player merely touches with
    // their body is NOT occupied.
    bool isOccupied(const Player& player, const Wall& wall, int32_t bx, int32_t by) const {
        AABB b = wall.brickAABB(bx, by);
        float s = brickSize(bx, by);
        // The player's feet box.
        AABB feet{{player.pos.x - PLAYER_HALF_W, player.pos.y - 0.05f, player.pos.z - PLAYER_HALF_W},
                  {player.pos.x + PLAYER_HALF_W, player.pos.y + 0.02f, player.pos.z + PLAYER_HALF_W}};
        // The brick's top face (a thin slab at y = by+s).
        AABB top{{float(bx), float(by) + s - 0.02f, b.mn.z},
                 {float(bx) + s, float(by) + s + 0.02f, b.mx.z}};
        return feet.overlaps(top);
    }

    // True while the brick has an in-flight lerp.
    bool isLerping(int32_t bx, int32_t by) const {
        for (int i = 0; i < MAX_LERPS; ++i) {
            const BrickLerp& L = lerps_[i];
            if (L.active && L.bx == bx && L.by == by) return true;
        }
        return false;
    }

    int activeLerps() const {
        int n = 0;
        for (int i = 0; i < MAX_LERPS; ++i)
            if (lerps_[i].active) ++n;
        return n;
    }

    // Cancel every in-flight lerp (restart).
    void clear() {
        for (int i = 0; i < MAX_LERPS; ++i) lerps_[i].active = false;
    }

    // Optional sound effects (null = silent, e.g. tests/headless dummy).
    void setAudio(Audio* audio) { audio_ = audio; }

    // Pull (extend) the target brick: starts a 3 s lerp outward. Clicking a
    // brick that is already lerping retargets it smoothly from its current
    // depth (so a mid-retract click reverses back out). Returns true if a lerp
    // was started. Extending is always allowed (collision pushes/lifts the
    // player out of the way); only retraction is constrained by occupancy.
    bool pull(Wall& wall, const TargetResult& t) {
        if (!t.hit) return false;
        float d = wall.brickDepth(t.bx, t.by);
        if (d >= PULL_DEPTH - 1e-4f && !isLerping(t.bx, t.by)) {
            if (wall.brickState(t.bx, t.by) != STATE_EXTENDED)
                wall.setBrick(t.bx, t.by, STATE_EXTENDED, PULL_DEPTH);
            return false;
        }
        startLerp(t.bx, t.by, d, PULL_DEPTH);
        wall.setBrick(t.bx, t.by, STATE_EXTENDING, d);
        if (audio_) audio_->play(Sfx::Pull);
        return true;
    }

    // Push (retract) the target brick: starts a 3 s lerp back flush. Refused
    // while the player stands on the brick. Returns true if a lerp was started.
    bool push(Wall& wall, const Player& player, const TargetResult& t) {
        if (!t.hit) return false;
        if (isOccupied(player, wall, t.bx, t.by)) return false;
        float d = wall.brickDepth(t.bx, t.by);
        if (d <= 1e-4f && !isLerping(t.bx, t.by)) return false;
        startLerp(t.bx, t.by, d, 0.0f);
        wall.setBrick(t.bx, t.by, STATE_RETRACTING, d);
        if (audio_) audio_->play(Sfx::Push);
        return true;
    }

    // Advance all in-flight lerps by dt (call once per frame). Each lerp moves
    // its brick linearly from `from` to `to` over BRICK_LERP_TIME seconds. A
    // retracting brick pauses while the player stands on it and resumes when
    // they step off.
    void update(Wall& wall, const Player& player, float dt) {
        for (int i = 0; i < MAX_LERPS; ++i) {
            BrickLerp& L = lerps_[i];
            if (!L.active) continue;
            if (L.to < L.from && isOccupied(player, wall, L.bx, L.by)) continue;
            L.t += dt;
            float k = L.t >= BRICK_LERP_TIME ? 1.0f : (L.t / BRICK_LERP_TIME);
            float nd = L.from + (L.to - L.from) * k;
            if (k >= 1.0f) {
                L.active = false;
                BrickState s = L.to <= 1e-4f ? STATE_REST : STATE_EXTENDED;
                wall.setBrick(L.bx, L.by, s, L.to);
                if (audio_) audio_->play(Sfx::Done);
            } else {
                BrickState s = (L.to > L.from) ? STATE_EXTENDING : STATE_RETRACTING;
                wall.setBrick(L.bx, L.by, s, nd);
            }
        }
    }

private:
    void startLerp(int32_t bx, int32_t by, float from, float to) {
        // Retarget an in-flight lerp on the same brick (smooth reversal).
        for (int i = 0; i < MAX_LERPS; ++i) {
            BrickLerp& L = lerps_[i];
            if (L.active && L.bx == bx && L.by == by) {
                L.from = from; L.to = to; L.t = 0.0f;
                return;
            }
        }
        // Otherwise grab a free slot (steal slot 0 if the pool is exhausted).
        for (int i = 0; i < MAX_LERPS; ++i) {
            BrickLerp& L = lerps_[i];
            if (!L.active) {
                L.active = true; L.bx = bx; L.by = by;
                L.from = from; L.to = to; L.t = 0.0f;
                return;
            }
        }
        BrickLerp& L = lerps_[0];
        L.active = true; L.bx = bx; L.by = by;
        L.from = from; L.to = to; L.t = 0.0f;
    }

    BrickLerp lerps_[MAX_LERPS]{};
    Audio* audio_ = nullptr;
};

}  // namespace aw
