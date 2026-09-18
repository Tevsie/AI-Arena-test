// debris.hpp — procedural stone chips and drifting dust, drawn as tiny cubes by
// the *existing* instanced brick program (so they cost no new shader, no asset
// and no new pipeline). Debris is visual-only: nothing here touches collision,
// the brick store or the simulation, and the pool is fixed-size so it can never
// grow into a frame-time cliff.
//
// Determinism: every random number comes from the pool's own xorshift state, so
// a run with a given sequence of spawns always looks the same — but the pool is
// only ever advanced in interactive mode (see Game), which keeps the headless
// benchmark bit-identical.
#pragma once

#include <cmath>
#include <cstdint>

#include "../core/math.hpp"
#include "constants.hpp"
#include "grid.hpp"

namespace aw {

struct DebrisParticle {
    Vec3 pos{};
    Vec3 vel{};
    float size = 0.0f;       // world size at birth (fades out near the end)
    float life = 0.0f;       // seconds lived
    float lifeMax = 1.0f;
    float spin = 0.0f;       // axis-aligned wobble phase (visual only)
    float shade = 128.0f;    // per-chip tone, the shader's shade byte
};

// Particles are visual-only mini-instances: the renderer turns each one into an
// axis-aligned cube (the brick shader's normal transform is scale + translation,
// which is exactly what a tiny chip needs) and picks its stone profile from the
// wall it came from.

class Debris {
public:
    static constexpr int CAP = 640;          // hard cap: never allocate per frame

    void reset() {
        count_ = 0;
        for (int i = 0; i < CAP; ++i) pool_[i] = DebrisParticle{};
        rng_ = 0x51a3c0deu;
    }

    int count() const { return count_; }
    const DebrisParticle& at(int i) const { return pool_[i]; }
    const DebrisParticle* data() const { return pool_; }

    // A puff of dust and chips at a brick face: `n` chips thrown along `dir`
    // (the face normal), dust drifting outward. Used for pulls and pushes.
    void spawnBrickPuff(const Vec3& at, const Vec3& dir, int chips, float scale) {
        for (int i = 0; i < chips; ++i) {
            float a = rand01() * 6.2831853f;
            float r = 0.25f * float(BRICK);
            Vec3 jitter{std::cos(a) * r, std::sin(a) * r, 0.0f};
            Vec3 p = at + jitter;
            // Chips fly out along the face normal with a little spread, and fall.
            float speed = 1.4f + rand01() * 2.6f;
            Vec3 v{dir.x * speed + (rand01() - 0.5f) * 1.6f,
                   dir.y * speed + (rand01() - 0.5f) * 1.6f + 1.2f,
                   dir.z * speed * 0.6f + 0.3f};
            add(p, v, 0.05f + rand01() * 0.10f * scale, 0.9f + rand01() * 1.5f,
                uint8_t(randu() & 0xff));
        }
        // Slower, larger dust motes that hang in the light.
        int motes = chips / 2 + 1;
        for (int i = 0; i < motes; ++i) {
            Vec3 p = at + Vec3{(rand01() - 0.5f) * BRICK, (rand01() - 0.5f) * BRICK,
                               rand01() * 0.4f};
            Vec3 v{dir.x * 0.4f + (rand01() - 0.5f) * 0.5f,
                   dir.y * 0.4f + rand01() * 0.35f,
                   dir.z * 0.3f + 0.15f};
            add(p, v, 0.13f + rand01() * 0.16f * scale, 2.2f + rand01() * 2.6f,
                uint8_t(randu() & 0xff));
        }
    }

    // A ring of dust at the player's feet on landing.
    void spawnLandingPuff(const Vec3& feet, float impactSpeed) {
        float k = clampf(impactSpeed / 12.0f, 0.25f, 1.6f);
        int n = int(6.0f + 10.0f * k);
        for (int i = 0; i < n; ++i) {
            float a = rand01() * 6.2831853f;
            Vec3 p = feet + Vec3{std::cos(a) * 0.35f, 0.06f, std::sin(a) * 0.35f};
            Vec3 v{std::cos(a) * (0.8f + rand01() * 1.4f) * k,
                   0.25f + rand01() * 0.5f,
                   std::sin(a) * (0.8f + rand01() * 1.4f) * k};
            add(p, v, 0.10f + rand01() * 0.18f * k, 1.2f + rand01() * 1.6f,
                uint8_t(randu() & 0xff));
        }
    }

    // Ambient motes: a sparse drift of dust near the camera so the low sun has
    // something to catch. Kept far cheaper than the interaction puffs.
    void spawnAmbientMote(const Vec3& eye, const Vec3& forward) {
        float side = (rand01() - 0.5f) * 9.0f;
        float up = (rand01() - 0.5f) * 6.0f;
        float ahead = 1.5f + rand01() * 7.0f;
        Vec3 p = eye + forward * ahead + Vec3{side, up, 0.4f};
        Vec3 v{(rand01() - 0.5f) * 0.25f, 0.05f + rand01() * 0.18f,
               (rand01() - 0.5f) * 0.25f};
        add(p, v, 0.04f + rand01() * 0.05f, 3.0f + rand01() * 4.0f,
            uint8_t(randu() & 0xff));
    }

    // Advance everything: light gravity, drag, and the death of expired chips.
    void update(float dt) {
        int w = 0;
        for (int i = 0; i < count_; ++i) {
            DebrisParticle p = pool_[i];
            p.life += dt;
            if (p.life >= p.lifeMax) continue;      // expired: drop it
            p.vel.y -= 5.5f * dt;                   // chips are heavier than motes
            float drag = p.size > 0.09f ? 1.1f : 0.35f;   // dust floats, chips do not
            p.vel = p.vel - p.vel * (drag * dt);
            p.pos = p.pos + p.vel * dt;
            // Do not sink through the wall face; bounce a little instead.
            if (p.pos.z < -1.2f) { p.pos.z = -1.2f; p.vel.z = std::fabs(p.vel.z) * 0.25f; }
            p.spin += dt * (2.0f + p.size * 12.0f);
            pool_[w++] = p;
        }
        count_ = w;
    }

private:
    void add(const Vec3& pos, const Vec3& vel, float size, float lifeMax, uint8_t shade) {
        if (count_ >= CAP) return;                  // full: drop the newest, never grow
        DebrisParticle& p = pool_[count_++];
        p.pos = pos;
        p.vel = vel;
        p.size = size;
        p.life = 0.0f;
        p.lifeMax = lifeMax;
        p.spin = rand01() * 6.2831853f;
        p.shade = float(shade);
    }

    uint32_t randu() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return rng_;
    }
    float rand01() { return float(randu() & 0xffffffu) / float(0x1000000u); }

    DebrisParticle pool_[CAP]{};
    int count_ = 0;
    uint32_t rng_ = 0x51a3c0deu;
};

}  // namespace aw
