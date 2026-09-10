// player.hpp — kinematic character controller + grid-aligned AABB collision.
//
// No physics engine: the player is an AABB capsule (box) integrated with a
// fixed sub-stepped semi-implicit Euler step and resolved against the brick
// grid with axis-by-axis AABB push-out. All queries are O(candidate bricks),
// where candidate bricks are the small grid window the player overlaps.
#pragma once

#include <cstdint>

#include "../core/input.hpp"
#include "../core/math.hpp"
#include "../core/platform.hpp"
#include "constants.hpp"
#include "grid.hpp"
#include "wall.hpp"

namespace aw {

class Player {
public:
    Vec3 pos{0.0f, 0.0f, 0.5f};     // feet center
    Vec3 vel{0, 0, 0};
    bool grounded = false;
    float yaw = 0.0f;               // radians; 0 = looking along +Z (into the wall)
    float pitch = 0.0f;
    float sensitivity = 1.0f;       // mouse look multiplier (settings)
    int32_t highestBrickY = 0;      // peak reached (for stats)

    void reset(float x, float y, float z) {
        pos = {x, y, z};
        vel = {0, 0, 0};
        grounded = false;
        yaw = 0.0f;
        pitch = 0.0f;
    }

    Vec3 eye() const { return {pos.x, pos.y + EYE_HEIGHT, pos.z}; }

    // View direction (unit vector).
    Vec3 forward() const {
        float cy = std::cos(yaw), sy = std::sin(yaw);
        float cp = std::cos(pitch), sp = std::sin(pitch);
        // yaw=0 -> -Z ... we use +X*0 ... keep consistent with camera: forward
        // points along -Z at yaw 0 (toward the wall) if pitch 0.
        return {-sy * cp, sp, -cy * cp};
    }

    void look(float dx, float dy) {
        yaw -= dx * MOUSE_SENS * sensitivity;
        pitch -= dy * MOUSE_SENS * sensitivity;
        constexpr float lim = 1.55f;
        if (pitch > lim) pitch = lim;
        if (pitch < -lim) pitch = -lim;
    }

    // Player's collision box (half-width in X/Z, full height in Y).
    AABB box() const {
        return AABB({pos.x - PLAYER_HALF_W, pos.y, pos.z - PLAYER_HALF_W},
                    {pos.x + PLAYER_HALF_W, pos.y + PLAYER_HEIGHT, pos.z + PLAYER_HALF_W});
    }

    void update(const FrameInput& in, Wall& wall, float dt) {
        // ---- look ----------------------------------------------------------
        look(in.mouseDX, in.mouseDY);

        // ---- movement input (WASD relative to yaw, camera-relative) --------
        float fx = 0.0f, fz = 0.0f;  // world-space wish direction
        if (in.keys[KEY_W]) { fx += 0; fz += 1; }
        if (in.keys[KEY_S]) { fx += 0; fz -= 1; }
        if (in.keys[KEY_A]) { fx -= 1; fz += 0; }
        if (in.keys[KEY_D]) { fx += 1; fz += 0; }

        float sinY = std::sin(yaw), cosY = std::cos(yaw);
        // Camera forward is (-sinY, 0, -cosY); right is (cosY, 0, -sinY).
        // wish = forward * fz + right * fx
        Vec3 wish{-sinY * fz + cosY * fx, 0.0f, -cosY * fz - sinY * fx};
        float len = length(wish);
        if (len > 1e-4f) wish = wish * (1.0f / len);

        float target = MOVE_SPEED;
        // Accelerate horizontal velocity toward the wish direction (velocity
        // blend; frame-rate independent via exponential approach).
        float k = grounded ? (1.0f - std::exp(-GROUND_ACCEL * dt))
                           : std::min(1.0f, AIR_ACCEL * dt);
        vel.x += (wish.x * target - vel.x) * k;
        vel.z += (wish.z * target - vel.z) * k;

        // Cap horizontal speed.
        float hs = std::sqrt(vel.x * vel.x + vel.z * vel.z);
        float maxH = MOVE_SPEED * 1.4f;
        if (hs > maxH) { vel.x *= maxH / hs; vel.z *= maxH / hs; }

        // ---- jump ----------------------------------------------------------
        if (in.keys[KEY_SPACE] && grounded) {
            vel.y = JUMP_VELOCITY;
            grounded = false;
        }

        // ---- gravity -------------------------------------------------------
        vel.y -= GRAVITY * dt;
        if (vel.y < -TERMINAL_VELOCITY) vel.y = -TERMINAL_VELOCITY;

        // ---- integrate with sub-stepping for robust thin-ledge collision ----
        float maxStep = 0.25f;  // max displacement per sub-step (meters)
        float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z) + 1e-6f;
        int steps = int(std::ceil(speed * dt / maxStep));
        if (steps < 1) steps = 1;
        if (steps > 8) steps = 8;
        float sdt = dt / float(steps);

        for (int s = 0; s < steps; ++s) {
            pos += vel * sdt;
            resolveCollisions(wall);
        }

        // Track peak height.
        int32_t by = floori(pos.y / BRICK);
        if (by > highestBrickY) highestBrickY = by;

        // No fall respawn: if the player falls they keep falling forever. The
        // wall is infinite and chunk streaming follows the player, so there
        // is always wall alongside (and any protruding ledge can break a fall).
    }

private:
    static constexpr float EPS = 0.002f;

    // Resolve the player box against nearby bricks (axis-separated push-out).
    void resolveCollisions(Wall& wall) {
        grounded = false;
        AABB pb = box();

        // Candidate brick grid window. Mosaic bricks are up to BRICK_MAX_CELLS
        // wide, so a brick overlapping the player may originate that many
        // cells in -X/-Y; +1 cell of margin covers +X/+Y. Each cell resolves
        // to its containing brick (wall.brickAABB canonicalizes), so bricks
        // spanning several cells are simply tested more than once.
        int32_t x0 = floori(pb.mn.x / BRICK) - BRICK_MAX_CELLS;
        int32_t x1 = floori(pb.mx.x / BRICK) + 1;
        int32_t y0 = floori(pb.mn.y / BRICK) - BRICK_MAX_CELLS;
        int32_t y1 = floori(pb.mx.y / BRICK) + 1;

        for (int pass = 0; pass < 3; ++pass) {
            bool any = false;
            for (int32_t by = y0; by <= y1; ++by) {
                for (int32_t bx = x0; bx <= x1; ++bx) {
                    AABB b = wall.brickAABB(bx, by);
                    if (b.mx.x <= b.mn.x) continue;  // out of wall width
                    // Expand slightly so resting contact is detected reliably.
                    b.mn.x -= EPS; b.mn.y -= EPS; b.mn.z -= EPS;
                    b.mx.x += EPS; b.mx.y += EPS; b.mx.z += EPS;
                    if (!pb.overlaps(b)) continue;

                    // Penetration per axis.
                    float px = std::min(pb.mx.x - b.mn.x, b.mx.x - pb.mn.x);
                    float py = std::min(pb.mx.y - b.mn.y, b.mx.y - pb.mn.y);
                    float pz = std::min(pb.mx.z - b.mn.z, b.mx.z - pb.mn.z);

                    if (px < py && px < pz) {
                        pos.x += (pb.mx.x - b.mn.x < b.mx.x - pb.mn.x) ? -px : px;
                        vel.x = 0.0f;
                    } else if (pz < py) {
                        pos.z += (pb.mx.z - b.mn.z < b.mx.z - pb.mn.z) ? -pz : pz;
                        vel.z = 0.0f;
                    } else {
                        // Resolve along Y toward the side the player is on.
                        float pc = (pb.mn.y + pb.mx.y) * 0.5f;
                        float bc = (b.mn.y + b.mx.y) * 0.5f;
                        if (pc > bc) { pos.y += py; grounded = true; }
                        else pos.y -= py;
                        vel.y = 0.0f;
                    }
                    pb = box();
                    any = true;
                }
            }
            if (!any) break;
        }
    }
};

}  // namespace aw
