// tests.cpp — dependency-free unit tests for the core simulation.
// Exercises grid math, the persistent brick store, chunk streaming, the
// kinematic controller, and the pull/push mechanics. No window/GPU required.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "src/core/input.hpp"
#include "src/core/platform.hpp"
#include "src/game/constants.hpp"
#include "src/game/grid.hpp"
#include "src/game/interaction.hpp"
#include "src/game/player.hpp"
#include "src/game/wall.hpp"

using namespace aw;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) {                                                          \
            ++g_pass;                                                        \
        } else {                                                             \
            ++g_fail;                                                        \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                \
    do {                                                                     \
        double _a = (a), _b = (b);                                           \
        if (std::fabs(_a - _b) <= (eps)) {                                   \
            ++g_pass;                                                        \
        } else {                                                             \
            ++g_fail;                                                        \
            fprintf(stderr, "FAIL %s:%d  |%s - %s| = %f > %f\n", __FILE__,   \
                    __LINE__, #a, #b, std::fabs(_a - _b), double(eps));      \
        }                                                                    \
    } while (0)

static FrameInput zeroInput() {
    FrameInput in{};
    in.width = 1280; in.height = 720;
    return in;
}

// ---------------------------------------------------------------------------
static void testGrid() {
    // brick -> chunk -> origin round trips
    CHECK(brickToChunk(0, 0).cx == 0 && brickToChunk(0, 0).cy == 0);
    CHECK(brickToChunk(15, 15).cx == 0 && brickToChunk(15, 15).cy == 0);
    CHECK(brickToChunk(16, 0).cx == 1 && brickToChunk(16, 0).cy == 0);
    CHECK(brickToChunk(-1, 0).cx == -1);
    CHECK(brickToChunk(-1, -1).cx == -1 && brickToChunk(-1, -1).cy == -1);
    CHECK(brickToChunk(0, -16).cy == -1);
    // local index
    CHECK(brickLocalIndex(0, 0) == 0);
    CHECK(brickLocalIndex(15, 15) == 255);
    CHECK(brickLocalIndex(16, 0) == 0);   // belongs to chunk (1,0), local (0,0)
    // world coords
    Vec3 mn = brickMin(3, 7);
    CHECK_NEAR(mn.x, 3.0, 1e-5);
    CHECK_NEAR(mn.y, 7.0, 1e-5);
}

// ---------------------------------------------------------------------------
static void testBrickStore() {
    BrickStore s;
    CHECK(s.find(1, 2) == nullptr);
    CHECK(s.set(1, 2, STATE_EXTENDING, 0.5f));
    CHECK(s.find(1, 2) != nullptr);
    CHECK_NEAR(s.depth(1, 2), 0.5f, 1e-6);
    CHECK(s.state(1, 2) == STATE_EXTENDING);
    // update in place
    CHECK(s.set(1, 2, STATE_EXTENDED, 0.65f));
    CHECK_NEAR(s.depth(1, 2), 0.65f, 1e-6);
    // absent bricks report rest/zero
    CHECK(s.state(5, 5) == STATE_REST);
    CHECK_NEAR(s.depth(5, 5), 0.0, 1e-6);
    // many insertions (no collision/overflow) across a spread
    for (int32_t i = 0; i < 1000; ++i) CHECK(s.set(i * 3, -i * 2, STATE_EXTENDED, 0.4f));
    CHECK_NEAR(s.depth(999 * 3, -999 * 2), 0.4f, 1e-6);
    CHECK_NEAR(s.depth(1, 2), 0.65f, 1e-6);  // untouched
}

// ---------------------------------------------------------------------------
static void testStreaming() {
    Wall w;
    w.streamAround(0);
    // 6 columns x 9 rows = 54 resident chunks
    CHECK(w.residentCount() == RESIDENT_ROWS * CHUNKS_X);
    CHECK(w.find({0, 0}) != nullptr);
    CHECK(w.find({0, ACTIVE_CHUNK_RANGE}) != nullptr);
    CHECK(w.find({0, ACTIVE_CHUNK_RANGE + 1}) == nullptr);

    // move up a few rows; window slides
    w.streamAround(3);
    CHECK(w.residentCount() == RESIDENT_ROWS * CHUNKS_X);
    CHECK(w.find({0, 0 - ACTIVE_CHUNK_RANGE}) == nullptr);   // old bottom gone
    CHECK(w.find({0, 3 + ACTIVE_CHUNK_RANGE}) != nullptr);   // new top present
}

// ---------------------------------------------------------------------------
static void testPersistence() {
    Wall w;
    w.streamAround(0);
    // Modify a brick well above the player.
    w.setBrick(10, 10, STATE_EXTENDED, 0.7f);
    CHECK_NEAR(w.brickDepth(10, 10), 0.7f, 1e-6);
    CHECK(w.find({0, 0})->activeCount == 1);

    // Stream far away (chunk with the brick leaves residency) ...
    w.streamAround(20);
    CHECK(w.find({0, 0}) == nullptr);
    // ... the brick state must survive.
    CHECK_NEAR(w.brickDepth(10, 10), 0.7f, 1e-6);

    // Stream back; the chunk regenerates and recalls the active brick.
    w.streamAround(0);
    Chunk* c = w.find({0, 0});
    CHECK(c != nullptr);
    CHECK(c->activeCount == 1);
    int32_t local = brickLocalIndex(10, 10);
    CHECK(c->activeLocal[local] == 1);
}

// ---------------------------------------------------------------------------
static void testPlayerPhysics() {
    Wall w;
    // Spawn platform: bricks (44..51, -1) extended fully.
    for (int32_t bx = 44; bx <= 51; ++bx) w.setBrick(bx, -1, STATE_EXTENDED, 1.0f);
    w.streamAround(0);

    Player p;
    p.reset(48.0f, 5.0f, 0.5f);

    // Fall onto the platform. ~2 seconds of 60 Hz steps.
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i) {
        FrameInput in = zeroInput();
        p.update(in, w, dt);
        if (p.pos.y < -10.0f) break;  // fell through — bad
    }
    CHECK(p.grounded);
    CHECK_NEAR(p.pos.y, 0.0f, 0.06f);   // feet on top of brick y=-1 (top at 0)
    CHECK(p.pos.y > -0.1f);

    // Jump reaches above the platform.
    FrameInput jump = zeroInput();
    jump.keys[KEY_SPACE] = 1;
    for (int i = 0; i < 10; ++i) p.update(jump, w, dt);
    CHECK(p.vel.y > 0.0f || p.pos.y > 0.05f);  // moving up

    // Walk + gravity bring the player back down (never through the wall).
    for (int i = 0; i < 240; ++i) p.update(zeroInput(), w, dt);
    CHECK(p.grounded);
    CHECK(p.pos.y > -0.1f);

    // The player should not be able to pass through the wall face (z stays >= 0).
    CHECK(p.pos.z >= -0.05f);
}

// ---------------------------------------------------------------------------
static void testInteraction() {
    Wall w;
    for (int32_t bx = 44; bx <= 51; ++bx) w.setBrick(bx, -1, STATE_EXTENDED, 1.0f);
    w.streamAround(0);

    Player p;
    p.reset(48.0f, 0.0f, 0.5f);
    p.yaw = 0.0f; p.pitch = 0.0f;
    Interaction it;

    // Center ray looks at -Z from the eye and hits the flush brick ahead.
    TargetResult t = it.cast(p, w);
    CHECK(t.hit);
    CHECK(t.bx == 48 && t.by == 1);   // eye y=1.62 -> brick row 1
    CHECK_NEAR(t.depth, 0.0f, 1e-5);

    // Pull: depth grows toward PULL_DEPTH.
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 60; ++i) { t = it.cast(p, w); it.pull(w, t, dt); }
    CHECK_NEAR(w.brickDepth(48, 1), PULL_DEPTH, 0.02f);
    CHECK(w.brickState(48, 1) == STATE_EXTENDED);

    // The brick below the player is occupied -> cannot be retracted.
    CHECK(it.isOccupied(p, w, 48, -1));

    // Push the extended brick back flush.
    for (int i = 0; i < 60; ++i) { t = it.cast(p, w); it.push(w, p, t, dt); }
    CHECK_NEAR(w.brickDepth(48, 1), 0.0f, 1e-4);
    CHECK(w.brickState(48, 1) == STATE_REST);
}

// ---------------------------------------------------------------------------
int main() {
    testGrid();
    testBrickStore();
    testStreaming();
    testPersistence();
    testPlayerPhysics();
    testInteraction();

    fprintf(stderr, "\n[aw-tests] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
