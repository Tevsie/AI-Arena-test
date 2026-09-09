// interaction.hpp — center-screen brick targeting (raycast) and the
// pull/push mechanics with smooth interpolation.
#pragma once

#include <cstdint>

#include "../core/math.hpp"
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
    // Cast the center-screen ray against the brick grid within PULL_REACH.
    // Uses an exact 2D grid DDA over the wall's (x,y) cells; because every
    // brick spans a full unit cell in x/y and the wall is one brick deep, each
    // crossed cell's extended box is tested once — nothing is missed and the
    // cost is proportional to the few cells the ray actually crosses.
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
            if (c.x < 0 || c.x >= WALL_WIDTH_BRICKS) break;
            AABB b = wall.brickAABB(c.x, c.y);
            float tn = 0.0f;
            if (b.mx.x > b.mn.x && rayAABB(ray, b, tn) && tn <= PULL_REACH) {
                r.hit = true;
                r.bx = c.x; r.by = c.y;
                r.depth = wall.brickDepth(c.x, c.y);
                r.dist = tn;
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
        if (b.mx.z <= b.mn.z) return false;
        // The player's feet box.
        AABB feet{{player.pos.x - PLAYER_HALF_W, player.pos.y - 0.05f, player.pos.z - PLAYER_HALF_W},
                  {player.pos.x + PLAYER_HALF_W, player.pos.y + 0.02f, player.pos.z + PLAYER_HALF_W}};
        // The brick's top face (a thin slab at y = by+1).
        AABB top{{float(bx), float(by) + BRICK - 0.02f, b.mn.z},
                 {float(bx) + BRICK, float(by) + BRICK + 0.02f, b.mx.z}};
        return feet.overlaps(top);
    }

    // Pull (extend) the target brick. Returns true if the brick changed.
    // Extending is always allowed (collision pushes/lifts the player out of the
    // way); only retraction is constrained by occupancy.
    bool pull(Wall& wall, const TargetResult& t, float dt) {
        if (!t.hit) return false;
        float d = wall.brickDepth(t.bx, t.by);
        if (d >= PULL_DEPTH - 1e-4f) {
            if (wall.brickState(t.bx, t.by) != STATE_EXTENDED)
                wall.setBrick(t.bx, t.by, STATE_EXTENDED, PULL_DEPTH);
            return false;
        }
        float nd = d + PULL_SPEED * dt;
        if (nd > PULL_DEPTH) nd = PULL_DEPTH;
        BrickState s = nd >= PULL_DEPTH - 1e-4f ? STATE_EXTENDED : STATE_EXTENDING;
        wall.setBrick(t.bx, t.by, s, nd);
        return true;
    }

    // Push (retract) the target brick. Returns true if the brick changed.
    bool push(Wall& wall, const Player& player, const TargetResult& t, float dt) {
        if (!t.hit) return false;
        if (isOccupied(player, wall, t.bx, t.by)) return false;
        float d = wall.brickDepth(t.bx, t.by);
        if (d <= 1e-4f) return false;
        float nd = d - PULL_SPEED * dt;
        if (nd < 0.0f) nd = 0.0f;
        BrickState s = nd <= 1e-4f ? STATE_REST : STATE_RETRACTING;
        wall.setBrick(t.bx, t.by, s, nd);
        return true;
    }
};

}  // namespace aw
