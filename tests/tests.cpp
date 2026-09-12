// tests.cpp — dependency-free unit tests for the core simulation.
// Exercises grid math, the non-overlapping brick mosaic, the persistent brick
// store, 2D chunk streaming, the kinematic controller, endless falling, and
// the click/lerp pull/push mechanics. No window/GPU required.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "src/core/input.hpp"
#include "src/core/platform.hpp"
#include "src/game/constants.hpp"
#include "src/game/game.hpp"
#include "src/game/grid.hpp"
#include "src/game/interaction.hpp"
#include "src/game/player.hpp"
#include "src/game/settings.hpp"
#include "src/game/wall.hpp"
#include "src/render/gl.h"
#include "src/render/renderer.hpp"

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
    // world coords (rest box of the containing mosaic brick)
    BrickRect gr = brickAt(3, 7);
    Vec3 mn = brickMin(3, 7);
    CHECK_NEAR(mn.x, float(gr.ox), 1e-5);
    CHECK_NEAR(mn.y, float(gr.oy), 1e-5);
    CHECK_NEAR(mn.z, -brickExtent(gr), 1e-5);
}

// ---------------------------------------------------------------------------
static void testMosaic() {
    // Every pattern tiles its 4x4 macro-cell exactly, and each pattern's LUT
    // agrees with its rect list (validates the hand-written tables).
    for (int p = 0; p < 8; ++p) {
        const MosaicPattern& pat = kMosaic[p];
        CHECK(pat.count >= 1 && pat.count <= 16);
        int cover[16] = {};
        for (int i = 0; i < pat.count; ++i) {
            int x = pat.rects[i][0], y = pat.rects[i][1];
            int w = pat.rects[i][2], h = pat.rects[i][3];
            CHECK(x >= 0 && y >= 0 && w >= 1 && h >= 1 && x + w <= 4 && y + h <= 4);
            for (int dy = 0; dy < h; ++dy)
                for (int dx = 0; dx < w; ++dx) cover[(y + dy) * 4 + (x + dx)]++;
        }
        for (int i = 0; i < 16; ++i) CHECK(cover[i] == 1);
        for (int ly = 0; ly < 4; ++ly)
            for (int lx = 0; lx < 4; ++lx) {
                uint8_t ri = pat.lut[ly * 4 + lx];
                CHECK(ri < pat.count);
                int x = pat.rects[ri][0], y = pat.rects[ri][1];
                int w = pat.rects[ri][2], h = pat.rects[ri][3];
                CHECK(lx >= x && lx < x + w && ly >= y && ly < y + h);
            }
    }

    // Coverage: every cell (incl. negative coords, across chunk borders) maps
    // to a sane rect containing it; origins are idempotent; bricks never cross
    // chunk boundaries.
    for (int32_t y = -40; y < 40; ++y)
        for (int32_t x = -40; x < 40; ++x) {
            BrickRect r = brickAt(x, y);
            CHECK(r.w >= 1 && r.w <= BRICK_MAX_W && r.h >= 1 && r.h <= BRICK_MAX_H);
            CHECK(x >= r.ox && x < r.ox + r.w && y >= r.oy && y < r.oy + r.h);
            BrickRect o = brickAt(r.ox, r.oy);
            CHECK(o.ox == r.ox && o.oy == r.oy && o.w == r.w && o.h == r.h);
            CHECK(isBrickOrigin(r.ox, r.oy));
            ChunkCoord a = brickToChunk(r.ox, r.oy);
            ChunkCoord b = brickToChunk(r.ox + r.w - 1, r.oy + r.h - 1);
            CHECK(a.cx == b.cx && a.cy == b.cy);
        }

    // No two distinct bricks overlap (the fixed bug), over a 56x56-cell span
    // covering 3x3 chunks including negative coords and chunk borders.
    {
        struct R { int32_t x, y, w, h; };
        static R rects[56 * 56];
        int n = 0;
        for (int32_t y = -36; y < 20; ++y)
            for (int32_t x = -36; x < 20; ++x) {
                if (!isBrickOrigin(x, y)) continue;
                BrickRect r = brickAt(x, y);
                bool seen = false;
                for (int i = 0; i < n; ++i)
                    if (rects[i].x == r.ox && rects[i].y == r.oy) { seen = true; break; }
                if (!seen) rects[n++] = {r.ox, r.oy, r.w, r.h};
            }
        CHECK(n > 100);
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                bool overlap = rects[i].x < rects[j].x + rects[j].w &&
                               rects[j].x < rects[i].x + rects[i].w &&
                               rects[i].y < rects[j].y + rects[j].h &&
                               rects[j].y < rects[i].y + rects[i].h;
                CHECK(!overlap);
            }
    }

    // Geometry: rest box matches the mosaic rect; writes through any cell hit
    // the same brick; extension slides the box +Z.
    Wall w;
    w.streamAround(0, 0);
    {
        BrickRect r = brickAt(3, 7);
        float e = brickExtent(r);
        AABB b = w.brickAABB(3, 7);
        CHECK_NEAR(b.mn.x, float(r.ox), 1e-6);
        CHECK_NEAR(b.mx.x, float(r.ox + r.w), 1e-6);
        CHECK_NEAR(b.mn.y, float(r.oy), 1e-6);
        CHECK_NEAR(b.mx.y, float(r.oy + r.h), 1e-6);
        CHECK_NEAR(b.mx.z, 0.0f, 1e-6);   // flush front face at z=0
        CHECK_NEAR(b.mn.z, -e, 1e-6);
        for (int32_t dy = 0; dy < r.h; ++dy)
            for (int32_t dx = 0; dx < r.w; ++dx) {
                AABB c = w.brickAABB(r.ox + dx, r.oy + dy);
                CHECK_NEAR(c.mn.x, b.mn.x, 1e-6);
                CHECK_NEAR(c.mx.x, b.mx.x, 1e-6);
                CHECK_NEAR(c.mn.y, b.mn.y, 1e-6);
                CHECK_NEAR(c.mx.y, b.mx.y, 1e-6);
            }
        w.setBrick(3, 7, STATE_EXTENDED, PULL_DEPTH);  // non-origin write works
        AABB eb = w.brickAABB(r.ox, r.oy);
        CHECK_NEAR(eb.mx.z, PULL_DEPTH, 1e-6);
        CHECK_NEAR(eb.mn.z, PULL_DEPTH - e, 1e-6);
        CHECK_NEAR(w.brickDepth(3, 7), PULL_DEPTH, 1e-6);
        CHECK(w.brickState(3, 7) == STATE_EXTENDED);
    }

    // Infinite wall: far-negative / far-away bricks exist and are well-formed.
    AABB n = w.brickAABB(-100, -50);
    CHECK(n.mx.x > n.mn.x && n.mx.y > n.mn.y && n.mx.z > n.mn.z);
    AABB f = w.brickAABB(100000, 200000);
    CHECK(f.mx.x > f.mn.x && f.mx.y > f.mn.y && f.mx.z > f.mn.z);

    // Variety: several footprint shapes appear in a modest deterministic sample.
    bool seen1x1 = false, seen4x4 = false, seenTall = false, seenWide = false;
    for (int32_t y = -40; y < 40; ++y)
        for (int32_t x = -40; x < 40; ++x) {
            BrickRect r = brickAt(x, y);
            seen1x1 |= (r.w == 1 && r.h == 1);
            seen4x4 |= (r.w == 4 && r.h == 4);
            seenTall |= (r.w == 1 && r.h == 4);
            seenWide |= (r.w == 4 && r.h == 1);
        }
    CHECK(seen1x1 && seen4x4 && seenTall && seenWide);

    // Chunk brick counts are bounded and vary across the resident window.
    int lo = 1 << 30, hi = 0;
    for (int32_t i = 0; i < MAX_RESIDENT_CHUNKS; ++i) {
        const Chunk& ch = w.poolSlot(i);
        if (ch.slot < 0) continue;
        CHECK(ch.brickCount >= 1 && ch.brickCount <= CHUNK_BRICKS);
        if (ch.brickCount < lo) lo = ch.brickCount;
        if (ch.brickCount > hi) hi = ch.brickCount;
    }
    CHECK(hi > lo);
    CHECK(w.residentBrickCount() > 81 && w.residentBrickCount() <= 81 * CHUNK_BRICKS);
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
    BrickRect pr = brickAt(10, 10);   // state lives on the brick's origin
    int32_t local = brickLocalIndex(pr.ox, pr.oy);
    CHECK(c->activeLocal[local] == 1);
}

// ---------------------------------------------------------------------------
static void testPlayerPhysics() {
    Wall w;
    // Spawn platform: bricks (44..51, -1) extended fully.
    for (int32_t bx = 44; bx <= 51; ++bx) w.setBrick(bx, -1, STATE_EXTENDED, 1.0f);
    w.streamAround(3, 0);

    // Landing height: tallest platform brick under the spawn footprint
    // (brick tops vary with the mosaic here).
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

    // Center ray looks at -Z from the eye and hits the brick ahead: the
    // reported target is the origin of the mosaic brick under the crosshair.
    TargetResult t = it.cast(p, w);
    CHECK(t.hit);
    int32_t expectY = floori(p.eye().y / BRICK);
    BrickRect tr = brickAt(48, expectY);
    CHECK(t.bx == tr.ox && t.by == tr.oy);
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
    CHECK(t.hit && t.bx == tr.ox && t.by == tr.oy);
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
static void testSettings() {
    Settings s;
    CHECK_NEAR(s.volume, 0.8, 1e-6);
    CHECK_NEAR(s.sensitivity, 1.0, 1e-6);
    // clamping
    s.volume = 2.0f; s.sensitivity = -1.0f; s.width = 10; s.height = 99999;
    s.clamp();
    CHECK_NEAR(s.volume, 1.0, 1e-6);
    CHECK_NEAR(s.sensitivity, 0.1, 1e-6);
    CHECK(s.width == 320);
    CHECK(s.height == 4320);
    // serialize/parse roundtrip
    Settings a;
    a.volume = 0.35f; a.sensitivity = 2.5f; a.width = 1920; a.height = 1080;
    a.fullscreen = true;
    char buf[256];
    a.serialize(buf, sizeof(buf));
    Settings b;
    CHECK(b.parse(buf));
    CHECK_NEAR(b.volume, 0.35, 1e-3);
    CHECK_NEAR(b.sensitivity, 2.5, 1e-3);
    CHECK(b.width == 1920 && b.height == 1080);
    CHECK(b.fullscreen);
    CHECK(!Settings().fullscreen);
    // unknown keys ignored, missing keys keep their values
    Settings c;
    CHECK(c.parse("bogus=123\nvolume=0.5\n"));
    CHECK_NEAR(c.volume, 0.5, 1e-6);
    CHECK_NEAR(c.sensitivity, 1.0, 1e-6);
    // resolution modes
    CHECK(a.modeIndex() == 2);
    a.setMode(0);
    CHECK(a.width == 1280 && a.height == 720);
    a.cycleMode(1);
    CHECK(a.width == 1600 && a.height == 900);
    a.cycleMode(-1);
    CHECK(a.width == 1280 && a.height == 720);
    // FOV: default, clamp and serialize/parse round-trip
    CHECK_NEAR(Settings().fov, Settings::kFovDefault, 1e-6);
    CHECK_NEAR(Settings::kFovDefault, FOV_DEG, 1e-6);
    Settings d;
    d.fov = 103.0f; d.width = 1920; d.height = 1080; d.windowW = 1600; d.windowH = 900;
    d.fullscreen = true;
    char dbuf[256];
    d.serialize(dbuf, sizeof(dbuf));
    Settings e;
    CHECK(e.parse(dbuf));
    CHECK_NEAR(e.fov, 103.0, 1e-3);
    CHECK(e.width == 1920 && e.height == 1080);
    CHECK(e.windowW == 1600 && e.windowH == 900);
    CHECK(e.fullscreen);
    Settings f;
    f.fov = 500.0f; f.clamp();
    CHECK_NEAR(f.fov, Settings::kFovMax, 1e-6);
    f.fov = -500.0f; f.clamp();
    CHECK_NEAR(f.fov, Settings::kFovMin, 1e-6);
    // An older settings.cfg (no fov / window keys) keeps today's defaults.
    Settings old;
    CHECK(old.parse("volume=0.4\nwidth=800\nheight=600\nfullscreen=1\n"));
    CHECK_NEAR(old.fov, Settings::kFovDefault, 1e-6);
    CHECK(old.windowW == Settings::kWindowDefaultW && old.windowH == Settings::kWindowDefaultH);
    CHECK(old.width == 800 && old.height == 600);
    CHECK(old.fullscreen);
}

// ---------------------------------------------------------------------------
static void testWindowFit() {
    int w = 1280, h = 720;
    Settings::fitToScreen(w, h, 1920, 1080, 64, 96);     // plenty of room
    CHECK(w == 1280 && h == 720);
    Settings::fitToScreen(w, h, 0, 0, 64, 96);           // unknown screen: no-op
    CHECK(w == 1280 && h == 720);
    w = 1280; h = 720;
    Settings::fitToScreen(w, h, 1366, 768, 64, 96);      // too tall: scale down
    CHECK(w == 1194 && h == 672);
    CHECK_NEAR(float(w) / float(h), 1280.0f / 720.0f, 0.01);   // aspect kept
    w = 2560; h = 1440;
    Settings::fitToScreen(w, h, 1920, 1080, 64, 96);
    CHECK(w <= 1920 - 64 && h <= 1080 - 96);
    CHECK_NEAR(float(w) / float(h), 2560.0f / 1440.0f, 0.01);
    // Ridiculously small screens still yield a usable window.
    w = 1280; h = 720;
    Settings::fitToScreen(w, h, 200, 150, 0, 0);
    CHECK(w >= 320 && h >= 200);
}

// ---------------------------------------------------------------------------
static void testSensitivity() {
    Player p;
    p.sensitivity = 1.0f;
    p.look(100.0f, 0.0f);
    CHECK_NEAR(p.yaw, -100.0f * MOUSE_SENS, 1e-6);
    p.yaw = 0.0f;
    p.sensitivity = 2.0f;
    p.look(100.0f, 0.0f);
    CHECK_NEAR(p.yaw, -100.0f * MOUSE_SENS * 2.0f, 1e-6);
}

// ---------------------------------------------------------------------------
static void testMenuNav() {
    // Keyboard-only menu logic (no GL needed): selection, adjust, actions.
    Platform* plat = createHeadlessPlatform();
    plat->init("t", 1280, 720);
    Audio audio;
    audio.init(false);  // silent dummy
    Settings s;
    Menu m;
    m.open();

    auto frame = [&](uint32_t key) {
        FrameInput in = zeroInput();
        if (key) in.keys[key] = 1;
        m.update(in, s, audio, *plat);
    };
    // Down x5 -> Restart; Enter -> restart flag.
    for (int i = 0; i < 5; ++i) { frame(KEY_DOWN); frame(0); }
    CHECK(m.selected() == Menu::Restart);
    frame(KEY_ENTER); frame(0);
    CHECK(m.consumeRestart());
    CHECK(!m.consumeRestart());
    // Up wraps to the top (volume); Right raises the volume bar.
    for (int i = 0; i < 5; ++i) { frame(KEY_UP); frame(0); }
    CHECK(m.selected() == Menu::Volume);
    float v0 = s.volume;
    frame(KEY_RIGHT); frame(0);
    CHECK(s.volume > v0);
    frame(KEY_LEFT); frame(0);
    CHECK_NEAR(s.volume, v0, 1e-6);
    // Resolution cycles the *render* resolution and leaves the window alone:
    // a window that changes size is what used to desync the cursor from the
    // UI hit boxes.
    frame(KEY_DOWN); frame(0);
    CHECK(m.selected() == Menu::Resolution);
    int w0 = s.width;
    frame(KEY_RIGHT); frame(0);
    CHECK(s.width != w0);
    FrameInput probe = zeroInput();
    plat->frame(probe);
    CHECK(probe.width == 1280 && probe.height == 720);        // window untouched
    CHECK(probe.width != s.width || probe.height != s.height);
    // Sensitivity.
    frame(KEY_DOWN); frame(0);
    CHECK(m.selected() == Menu::Sensitivity);
    float sens0 = s.sensitivity;
    frame(KEY_RIGHT); frame(0);
    CHECK(s.sensitivity > sens0);
    frame(KEY_LEFT); frame(0);
    CHECK_NEAR(s.sensitivity, sens0, 1e-6);
    // FOV: arrows move it and it stays inside its clamp.
    frame(KEY_DOWN); frame(0);
    CHECK(m.selected() == Menu::Fov);
    float fov0 = s.fov;
    CHECK_NEAR(fov0, Settings::kFovDefault, 1e-6);
    frame(KEY_RIGHT); frame(0);
    CHECK(s.fov > fov0);
    frame(KEY_LEFT); frame(0);
    CHECK_NEAR(s.fov, fov0, 1e-6);
    s.fov = 500.0f; s.clamp();
    CHECK_NEAR(s.fov, Settings::kFovMax, 1e-6);
    s.fov = -500.0f; s.clamp();
    CHECK_NEAR(s.fov, Settings::kFovMin, 1e-6);
    s.fov = fov0;
    // Fullscreen toggles via Enter and arrows (headless backend ignores it).
    frame(KEY_DOWN); frame(0);  // fullscreen
    CHECK(m.selected() == Menu::Fullscreen);
    CHECK(!s.fullscreen);
    frame(KEY_ENTER); frame(0);
    CHECK(s.fullscreen);
    frame(KEY_LEFT); frame(0);
    CHECK(!s.fullscreen);
    frame(KEY_RIGHT); frame(0);
    CHECK(s.fullscreen);
    // Quit via keyboard.
    frame(KEY_DOWN); frame(0);  // restart
    frame(KEY_DOWN); frame(0);  // resume
    frame(KEY_DOWN); frame(0);  // quit
    CHECK(m.selected() == Menu::Quit);
    frame(KEY_ENTER); frame(0);
    CHECK(m.consumeQuit());

    audio.shutdown();
    plat->shutdown();
    delete plat;
}

// ---------------------------------------------------------------------------
// Recording GL stub: the renderer is CPU state plus GL calls, so filling the
// function table with recorders lets the tests verify the render path with no
// GPU — which resolution the scene is drawn at, how it is presented into the
// window, and that the UI overlay ends up in the window's own pixel space
// (the same space the cursor is reported in, i.e. where clicks land).
static struct {
    int viewports = 0;
    int vpW[8]{}, vpH[8]{};
    unsigned boundFb = 0;
    int blits = 0;
    int blitSrc[4]{}, blitDst[4]{};
    unsigned blitFilter = 0;
    int texW = 0, texH = 0, rbW = 0, rbH = 0;
    bool fbComplete = true;
    int u2n = 0;
    float u2[16][2]{};
    unsigned nextId = 1;
    void reset() {
        static const std::remove_reference_t<decltype(*this)> fresh{};
        *this = fresh;
    }
} g_gl;

static void installGlSpy() {
    g_gl.reset();
    gl = GL{};
    gl.ready = true;
    auto gen = [](GLsizei n, GLuint* p) { for (GLsizei i = 0; i < n; ++i) p[i] = g_gl.nextId++; };
    gl.GenBuffers = gen;
    gl.GenVertexArrays = gen;
    gl.GenTextures = gen;
    gl.GenFramebuffers = gen;
    gl.GenRenderbuffers = gen;
    gl.DeleteBuffers = [](GLsizei, const GLuint*) {};
    gl.DeleteVertexArrays = [](GLsizei, const GLuint*) {};
    gl.DeleteTextures = [](GLsizei, const GLuint*) {};
    gl.DeleteFramebuffers = [](GLsizei, const GLuint*) {};
    gl.DeleteRenderbuffers = [](GLsizei, const GLuint*) {};
    gl.BindBuffer = [](GLenum, GLuint) {};
    gl.BindVertexArray = [](GLuint) {};
    gl.BindTexture = [](GLenum, GLuint) {};
    gl.BindRenderbuffer = [](GLenum, GLuint) {};
    gl.BindFramebuffer = [](GLenum, GLuint fb) { g_gl.boundFb = fb; };
    gl.BufferData = [](GLenum, GLsizeiptr, const void*, GLenum) {};
    gl.BufferSubData = [](GLenum, GLintptr, GLsizeiptr, const void*) {};
    gl.Enable = [](GLenum) {};
    gl.Disable = [](GLenum) {};
    gl.DepthFunc = [](GLenum) {};
    gl.DepthMask = [](GLboolean) {};
    gl.BlendFunc = [](GLenum, GLenum) {};
    gl.ClearColor = [](GLfloat, GLfloat, GLfloat, GLfloat) {};
    gl.Clear = [](GLbitfield) {};
    gl.Viewport = [](GLint, GLint, GLsizei w, GLsizei h) {
        if (g_gl.viewports < 8) { g_gl.vpW[g_gl.viewports] = w; g_gl.vpH[g_gl.viewports] = h; }
        ++g_gl.viewports;
    };
    gl.TexImage2D = [](GLenum, GLint, GLint, GLsizei w, GLsizei h, GLint, GLenum, GLenum,
                       const void*) { g_gl.texW = w; g_gl.texH = h; };
    gl.TexParameteri = [](GLenum, GLenum, GLint) {};
    gl.ActiveTexture = [](GLenum) {};
    gl.FramebufferTexture2D = [](GLenum, GLenum, GLenum, GLuint, GLint) {};
    gl.RenderbufferStorage = [](GLenum, GLenum, GLsizei w, GLsizei h) { g_gl.rbW = w; g_gl.rbH = h; };
    gl.FramebufferRenderbuffer = [](GLenum, GLenum, GLenum, GLuint) {};
    gl.CheckFramebufferStatus = [](GLenum) -> GLenum {
        return g_gl.fbComplete ? GLenum(GL_FRAMEBUFFER_COMPLETE) : GLenum(0);
    };
    gl.BlitFramebuffer = [](GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
                            GLint dx1, GLint dy1, GLbitfield, GLenum filter) {
        g_gl.blitSrc[0] = sx0; g_gl.blitSrc[1] = sy0; g_gl.blitSrc[2] = sx1; g_gl.blitSrc[3] = sy1;
        g_gl.blitDst[0] = dx0; g_gl.blitDst[1] = dy0; g_gl.blitDst[2] = dx1; g_gl.blitDst[3] = dy1;
        g_gl.blitFilter = filter;
        ++g_gl.blits;
    };
    gl.CreateShader = [](GLenum) -> GLuint { return 1; };
    gl.ShaderSource = [](GLuint, GLsizei, const GLchar* const*, const GLint*) {};
    gl.CompileShader = [](GLuint) {};
    gl.GetShaderiv = [](GLuint, GLenum, GLint* v) { *v = 1; };
    gl.GetShaderInfoLog = [](GLuint, GLsizei, GLsizei*, GLchar*) {};
    gl.DeleteShader = [](GLuint) {};
    gl.CreateProgram = []() -> GLuint { return 2; };
    gl.AttachShader = [](GLuint, GLuint) {};
    gl.LinkProgram = [](GLuint) {};
    gl.GetProgramiv = [](GLuint, GLenum, GLint* v) { *v = 1; };
    gl.GetProgramInfoLog = [](GLuint, GLsizei, GLsizei*, GLchar*) {};
    gl.DeleteProgram = [](GLuint) {};
    gl.UseProgram = [](GLuint) {};
    gl.GetUniformLocation = [](GLuint, const GLchar*) -> GLint { return 0; };
    gl.EnableVertexAttribArray = [](GLuint) {};
    gl.VertexAttribPointer = [](GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {};
    gl.VertexAttribDivisorARB = [](GLuint, GLuint) {};
    gl.DrawArrays = [](GLenum, GLint, GLsizei) {};
    gl.DrawArraysInstancedARB = [](GLenum, GLint, GLsizei, GLsizei) {};
    gl.Uniform1i = [](GLint, GLint) {};
    gl.Uniform1f = [](GLint, GLfloat) {};
    gl.Uniform3f = [](GLint, GLfloat, GLfloat, GLfloat) {};
    gl.Uniform4f = [](GLint, GLfloat, GLfloat, GLfloat, GLfloat) {};
    gl.UniformMatrix4fv = [](GLint, GLsizei, GLboolean, const GLfloat*) {};
    gl.Uniform2f = [](GLint, GLfloat x, GLfloat y) {
        if (g_gl.u2n < 16) { g_gl.u2[g_gl.u2n][0] = x; g_gl.u2[g_gl.u2n][1] = y; ++g_gl.u2n; }
    };
}

static void testRenderTarget() {
    installGlSpy();
    Wall w;
    w.streamAround(0, 0);
    Player p;
    const Mat4 vp = Mat4::identity();

    Renderer r;
    CHECK(r.init());
    CHECK(r.hasRenderTarget());

    // Window 1280x720, render resolution 1600x900: the scene is drawn at the
    // resolution and scaled into the window — the window itself is untouched.
    CHECK(r.setRenderSize(1600, 900));
    CHECK(g_gl.texW == 1600 && g_gl.texH == 900);
    CHECK(g_gl.rbW == 1600 && g_gl.rbH == 900);
    int rw = 0, rh = 0;
    r.renderSize(1280, 720, rw, rh);
    CHECK(rw == 1600 && rh == 900);
    CHECK(r.setRenderSize(1600, 900));        // same size: no reallocation

    g_gl.viewports = 0; g_gl.blits = 0; g_gl.u2n = 0;
    r.render(w, p, vp, 90.0f, 1280, 720, false);
    CHECK(g_gl.vpW[0] == 1600 && g_gl.vpH[0] == 900);   // scene viewport = resolution
    CHECK(g_gl.blits == 1);
    CHECK(g_gl.blitFilter == GLenum(GL_LINEAR));
    CHECK(g_gl.blitSrc[0] == 0 && g_gl.blitSrc[1] == 0);
    CHECK(g_gl.blitSrc[2] == 1600 && g_gl.blitSrc[3] == 900);
    // Same 16:9 aspect => the image fills the window exactly.
    CHECK(g_gl.blitDst[0] == 0 && g_gl.blitDst[1] == 0);
    CHECK(g_gl.blitDst[2] == 1280 && g_gl.blitDst[3] == 720);
    // The live FOV reaches the shader (sky tangent = tan(fov/2)).
    bool fovSeen = false;
    for (int i = 0; i < g_gl.u2n; ++i)
        if (std::fabs(g_gl.u2[i][1] - std::tan(deg2rad(90.0f) * 0.5f)) < 1e-3f) fovSeen = true;
    CHECK(fovSeen);

    // The overlay is drawn in window pixels: the very space the cursor is
    // reported in, so hit-testing cannot drift when the resolution changes.
    g_gl.viewports = 0;
    r.uiBegin(1280, 720);
    CHECK(g_gl.boundFb == 0);
    CHECK(g_gl.vpW[0] == 1280 && g_gl.vpH[0] == 720);
    r.uiEnd();

    // A window with a different aspect is letterboxed, never stretched.
    g_gl.viewports = 0; g_gl.blits = 0;
    r.render(w, p, vp, 90.0f, 1000, 1000, false);
    CHECK(g_gl.blits == 1);
    const int dh = int(1000.0f / (1600.0f / 900.0f) + 0.5f);   // 16:9 into a square
    CHECK(g_gl.blitDst[0] == 0 && g_gl.blitDst[2] == 1000);     // full width
    CHECK(g_gl.blitDst[3] - g_gl.blitDst[1] == dh);             // height keeps aspect
    CHECK(g_gl.blitDst[1] == (1000 - dh) / 2);                  // centred: bars top+bottom
    CHECK(g_gl.vpW[0] == 1600 && g_gl.vpH[0] == 900);         // still the resolution

    // Switching resolution changes only the render target: the reported
    // window size is whatever the platform says, always.
    CHECK(r.setRenderSize(1920, 1080));
    r.renderSize(1280, 720, rw, rh);
    CHECK(rw == 1920 && rh == 1080);
    g_gl.viewports = 0;
    r.render(w, p, vp, 75.0f, 1280, 720, false);
    CHECK(g_gl.vpW[0] == 1920 && g_gl.vpH[0] == 1080);
    r.shutdown();

    // Without framebuffer support the scene simply follows the window, and
    // nothing else changes.
    g_gl.reset();
    g_gl.fbComplete = false;
    Renderer r2;
    CHECK(r2.init());
    CHECK(!r2.hasRenderTarget());
    CHECK(!r2.setRenderSize(1600, 900));
    r2.renderSize(1280, 720, rw, rh);
    CHECK(rw == 1280 && rh == 720);
    g_gl.viewports = 0; g_gl.blits = 0;
    r2.render(w, p, vp, 90.0f, 1280, 720, false);
    CHECK(g_gl.blits == 0);
    CHECK(g_gl.vpW[0] == 1280 && g_gl.vpH[0] == 720);
    r2.shutdown();

    gl = GL{};   // leave no stub behind for the other tests
}

// ---------------------------------------------------------------------------
static void testRestart() {
    // init()/shutdown() may read/write settings.cfg — only remove it if we made it.
    FILE* pre = std::fopen("settings.cfg", "rb");
    bool hadSettings = (pre != nullptr);
    if (pre) std::fclose(pre);

    Game g;
    CHECK(g.init("t", 1280, 720, true));
    // Dirty the world, then restart.
    g.wall().setBrick(60, 60, STATE_EXTENDED, PULL_DEPTH);
    CHECK(g.wall().store().activeCount() == 8);  // 7 seeded + 1
    g.player().pos = {100.0f, 200.0f, 30.0f};
    g.restart();
    // Modifications cleared, only the 7 seeded bricks remain.
    CHECK(g.wall().store().activeCount() == 7);
    CHECK_NEAR(g.wall().brickDepth(60, 60), 0.0f, 1e-6);
    CHECK(g.wall().residentCount() == 81);
    // Player respawned on the platform.
    float top = -1e30f;
    for (int32_t bx = 44; bx <= 51; ++bx) {
        float t = g.wall().brickAABB(bx, -1).mx.y;
        if (t > top) top = t;
    }
    CHECK_NEAR(g.player().pos.x, 48.0f, 1e-6);
    CHECK_NEAR(g.player().pos.y, top + 0.1f, 1e-6);
    CHECK_NEAR(g.player().pos.z, 0.5f, 1e-6);
    g.shutdown();

    if (!hadSettings) std::remove("settings.cfg");
}

// ---------------------------------------------------------------------------
int main() {
    testGrid();
    testMosaic();
    testBrickStore();
    testStreaming();
    testPersistence();
    testPlayerPhysics();
    testFallForever();
    testInteraction();
    testSettings();
    testWindowFit();
    testSensitivity();
    testMenuNav();
    testRenderTarget();
    testRestart();

    fprintf(stderr, "\n[aw-tests] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
