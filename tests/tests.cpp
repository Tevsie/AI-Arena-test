// tests.cpp — dependency-free unit tests for the core simulation.
// Exercises grid math, brick sizes, the persistent brick store, 2D chunk
// streaming, the kinematic controller, endless falling, and the click/lerp
// pull/push mechanics. No window/GPU required.
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

// Tallest row-(-1) platform brick top overlapping the XZ footprint at (x,z).
static float platformTopUnder(const Wall& w, float x, float z) {
    float top = -1e30f;
    for (int32_t bx = 43; bx <= 52; ++bx) {
        AABB b = w.brickAABB(bx, -1);
        if (b.mx.x > x - PLAYER_HALF_W && b.mn.x < x + PLAYER_HALF_W &&
            b.mx.z > z - PLAYER_HALF_W && b.mn.z < z + PLAYER_HALF_W)
            if (b.mx.y > top) top = b.mx.y;
    }
    return top;
}

// Which platform brick the footprint at (x,z) stands on (argmax of the above).
static int32_t platformStandBrick(const Wall& w, float x, float z) {
    float top = -1e30f;
    int32_t arg = 48;
    for (int32_t bx = 43; bx <= 52; ++bx) {
        AABB b = w.brickAABB(bx, -1);
        if (b.mx.x > x - PLAYER_HALF_W && b.mn.x < x + PLAYER_HALF_W &&
            b.mx.z > z - PLAYER_HALF_W && b.mn.z < z + PLAYER_HALF_W)
            if (b.mx.y > top) { top = b.mx.y; arg = bx; }
    }
    return arg;
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
    // world coords (corner-anchored, back face at z = -size)
    Vec3 mn = brickMin(3, 7);
    CHECK_NEAR(mn.x, 3.0, 1e-5);
    CHECK_NEAR(mn.y, 7.0, 1e-5);
    CHECK_NEAR(mn.z, -brickSize(3, 7), 1e-5);
}

// ---------------------------------------------------------------------------
static void testBrickSizes() {
    // Every brick draws its size from {1 m, 2.5 m, 5 m}.
    for (int32_t y = -20; y < 20; ++y)
        for (int32_t x = -20; x < 20; ++x) {
            float s = brickSize(x, y);
            CHECK(s == BRICK_SIZE_SMALL || s == BRICK_SIZE_MEDIUM || s == BRICK_SIZE_LARGE);
        }
    // All three classes appear in a modest sample (deterministic hash).
    bool seenS = false, seenM = false, seenL = false;
    for (int32_t y = 0; y < 16; ++y)
        for (int32_t x = 0; x < 16; ++x) {
            float s = brickSize(x, y);
            seenS |= (s == BRICK_SIZE_SMALL);
            seenM |= (s == BRICK_SIZE_MEDIUM);
            seenL |= (s == BRICK_SIZE_LARGE);
        }
    CHECK(seenS && seenM && seenL);

    // Geometry: corner-anchored at the cell minimum, front face at the
    // extension depth.
    Wall w;
    w.streamAround(0, 0);
    int32_t bx = 3, by = 7;
    float s = brickSize(bx, by);
    AABB b = w.brickAABB(bx, by);
    CHECK_NEAR(b.mn.x, float(bx), 1e-6);
    CHECK_NEAR(b.mx.x, float(bx) + s, 1e-6);
    CHECK_NEAR(b.mn.y, float(by), 1e-6);
    CHECK_NEAR(b.mx.y, float(by) + s, 1e-6);
    CHECK_NEAR(b.mx.z, 0.0f, 1e-6);   // flush front face at z=0
    CHECK_NEAR(b.mn.z, -s, 1e-6);
    w.setBrick(bx, by, STATE_EXTENDED, PULL_DEPTH);
    AABB e = w.brickAABB(bx, by);
    CHECK_NEAR(e.mx.z, PULL_DEPTH, 1e-6);
    CHECK_NEAR(e.mn.z, PULL_DEPTH - s, 1e-6);

    // Infinite in X: far-negative / far-outside-the-old-width bricks exist.
    AABB n = w.brickAABB(-100, -50);
    CHECK(n.mx.x > n.mn.x && n.mx.y > n.mn.y && n.mx.z > n.mn.z);
    AABB f = w.brickAABB(100000, 200000);
    CHECK(f.mx.x > f.mn.x && f.mx.y > f.mn.y && f.mx.z > f.mn.z);
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
    w.streamAround(0, 0);
    // 9x9 = 81 resident chunks around the player's chunk.
    CHECK(w.residentCount() == RESIDENT_ROWS * RESIDENT_COLS);
    CHECK(w.residentCount() == 81);
    CHECK(w.find({0, 0}) != nullptr);
    CHECK(w.find({ACTIVE_CHUNK_RANGE, ACTIVE_CHUNK_RANGE}) != nullptr);
    CHECK(w.find({-ACTIVE_CHUNK_RANGE, -ACTIVE_CHUNK_RANGE}) != nullptr);
    CHECK(w.find({ACTIVE_CHUNK_RANGE + 1, 0}) == nullptr);
    CHECK(w.find({0, ACTIVE_CHUNK_RANGE + 1}) == nullptr);

    // move diagonally; the window slides in both axes (x: -2..6, y: -1..7)
    w.streamAround(2, 3);
    CHECK(w.residentCount() == RESIDENT_ROWS * RESIDENT_COLS);
    CHECK(w.find({-4, 0}) == nullptr);   // old left columns gone
    CHECK(w.find({0, -4}) == nullptr);   // old bottom rows gone
    CHECK(w.find({-2, -1}) != nullptr);
    CHECK(w.find({6, 7}) != nullptr);
}

// ---------------------------------------------------------------------------
static void testPersistence() {
    Wall w;
    w.streamAround(0, 0);
    // Modify a brick well above the player.
    w.setBrick(10, 10, STATE_EXTENDED, 0.7f);
    CHECK_NEAR(w.brickDepth(10, 10), 0.7f, 1e-6);
    CHECK(w.find({0, 0})->activeCount == 1);

    // Stream far away (chunk with the brick leaves residency) ...
    w.streamAround(20, 20);
    CHECK(w.find({0, 0}) == nullptr);
    // ... the brick state must survive.
    CHECK_NEAR(w.brickDepth(10, 10), 0.7f, 1e-6);

    // Stream back; the chunk regenerates and recalls the active brick.
    w.streamAround(0, 0);
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
    w.streamAround(3, 0);

    // Landing height: tallest platform brick under the spawn footprint
    // (brick tops vary with size: 0 / 1.5 / 4 m here).
    float top = platformTopUnder(w, 48.0f, 0.5f);

    Player p;
    p.reset(48.0f, top + 5.0f, 0.5f);

    // Fall onto the platform. ~2 seconds of 60 Hz steps.
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i) {
        FrameInput in = zeroInput();
        p.update(in, w, dt);
        if (p.pos.y < top - 10.0f) break;  // fell through — bad
    }
    CHECK(p.grounded);
    CHECK_NEAR(p.pos.y, top, 0.06f);   // feet on the tallest brick below
    CHECK(p.pos.y > top - 0.1f);

    // Jump reaches above the platform.
    FrameInput jump = zeroInput();
    jump.keys[KEY_SPACE] = 1;
    for (int i = 0; i < 10; ++i) p.update(jump, w, dt);
    CHECK(p.vel.y > 0.0f || p.pos.y > top + 0.05f);  // moving up

    // Walk + gravity bring the player back down (never through the wall).
    for (int i = 0; i < 240; ++i) p.update(zeroInput(), w, dt);
    CHECK(p.grounded);
    CHECK(p.pos.y > top - 0.1f);
    CHECK_NEAR(p.pos.y, top, 0.06f);

    // The player should not be able to pass through the wall face (z stays >= 0).
    CHECK(p.pos.z >= -0.05f);
}

// ---------------------------------------------------------------------------
static void testFallForever() {
    Wall w;
    w.streamAround(0, 0);

    Player p;
    // Far in front of the wall (no ledges out here), no input: free fall.
    p.reset(0.0f, 10.0f, 20.0f);
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 600; ++i) p.update(zeroInput(), w, dt);  // 10 s

    // No respawn: the player just keeps falling (past the old -30 reset line).
    CHECK(p.pos.y < -30.0f);
    CHECK_NEAR(p.pos.x, 0.0f, 1e-3);
    CHECK_NEAR(p.pos.z, 20.0f, 1e-3);
    CHECK(!p.grounded);

    // Streaming follows arbitrarily deep falls (infinite wall).
    BrickCoord pb = worldToBrick(p.pos);
    ChunkCoord pc = brickToChunk(pb.x, pb.y);
    w.streamAround(pc.cx, pc.cy);
    CHECK(w.residentCount() == RESIDENT_ROWS * RESIDENT_COLS);
    CHECK(w.find(pc) != nullptr);
}

// ---------------------------------------------------------------------------
static void testInteraction() {
    Wall w;
    for (int32_t bx = 44; bx <= 51; ++bx) w.setBrick(bx, -1, STATE_EXTENDED, 1.0f);
    w.streamAround(3, 0);

    // Stand on the platform under x=48 (tops vary with brick size).
    float top = platformTopUnder(w, 48.0f, 0.5f);
    int32_t standX = platformStandBrick(w, 48.0f, 0.5f);

    Player p;
    p.reset(48.0f, top, 0.5f);
    p.yaw = 0.0f; p.pitch = 0.0f;
    Interaction it;

    // Center ray looks at -Z from the eye and hits the brick ahead.
    TargetResult t = it.cast(p, w);
    CHECK(t.hit);
    int32_t expectY = floori(p.eye().y / BRICK);
    CHECK(t.bx == 48 && t.by == expectY);
    CHECK_NEAR(t.depth, 0.0f, 1e-5);

    // Click-pull: a 3 s lerp toward PULL_DEPTH (halfway after 1.5 s).
    const float dt = 1.0f / 60.0f;
    CHECK(it.pull(w, t));
    CHECK(it.isLerping(t.bx, t.by));
    for (int i = 0; i < 90; ++i) it.update(w, p, dt);   // 1.5 s
    CHECK_NEAR(w.brickDepth(t.bx, t.by), PULL_DEPTH * 0.5f, 0.03f);
    CHECK(w.brickState(t.bx, t.by) == STATE_EXTENDING);
    for (int i = 0; i < 100; ++i) it.update(w, p, dt);  // past 3 s total
    CHECK_NEAR(w.brickDepth(t.bx, t.by), PULL_DEPTH, 1e-4f);
    CHECK(w.brickState(t.bx, t.by) == STATE_EXTENDED);
    CHECK(!it.isLerping(t.bx, t.by));

    // The brick below the player is occupied -> click-push is refused.
    CHECK(it.isOccupied(p, w, standX, -1));
    TargetResult stood{true, standX, -1, w.brickDepth(standX, -1), 1.0f};
    CHECK(!it.push(w, p, stood));

    // Click-push the extended brick back flush over 3 s.
    t = it.cast(p, w);
    CHECK(t.hit && t.bx == 48 && t.by == expectY);
    CHECK(it.push(w, p, t));
    for (int i = 0; i < 200; ++i) it.update(w, p, dt);
    CHECK_NEAR(w.brickDepth(48, expectY), 0.0f, 1e-4f);
    CHECK(w.brickState(48, expectY) == STATE_REST);

    // Re-clicking mid-lerp retargets smoothly from the current depth.
    t = it.cast(p, w);
    CHECK(it.pull(w, t));
    for (int i = 0; i < 30; ++i) it.update(w, p, dt);  // 0.5 s out
    float mid = w.brickDepth(t.bx, t.by);
    CHECK(mid > 0.0f && mid < PULL_DEPTH);
    t = it.cast(p, w);
    CHECK(it.push(w, p, t));                           // reverse back in
    for (int i = 0; i < 200; ++i) it.update(w, p, dt);
    CHECK_NEAR(w.brickDepth(t.bx, t.by), 0.0f, 1e-4f);
    CHECK(w.brickState(t.bx, t.by) == STATE_REST);
}

// ---------------------------------------------------------------------------
int main() {
    testGrid();
    testBrickSizes();
    testBrickStore();
    testStreaming();
    testPersistence();
    testPlayerPhysics();
    testFallForever();
    testInteraction();

    fprintf(stderr, "\n[aw-tests] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
