// tests.cpp — dependency-free unit tests for the core simulation.
// Exercises grid math, the non-overlapping brick mosaic, the persistent brick
// store, 2D chunk streaming, the kinematic controller, endless falling, and
// the click/lerp pull/push mechanics. No window/GPU required.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "src/core/input.hpp"
#include "src/core/platform.hpp"
#include "src/game/constants.hpp"
#include "src/game/game.hpp"
#include "src/game/grid.hpp"
#include "src/game/interaction.hpp"
#include "src/game/player.hpp"
#include "src/game/settings.hpp"
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
    CHECK_NEAR(c.fov, Settings::kFovDefault, 1e-6);

    // field of view: default, roundtrip, clamping
    CHECK_NEAR(Settings().fov, 75.0, 1e-6);
    Settings f;
    f.fov = 95.0f;
    char fbuf[256];
    f.serialize(fbuf, sizeof(fbuf));
    Settings f2;
    CHECK(f2.parse(fbuf));
    CHECK_NEAR(f2.fov, 95.0, 1e-3);
    f.fov = 500.0f; f.clamp();
    CHECK_NEAR(f.fov, Settings::kFovMax, 1e-6);
    f.fov = 0.0f; f.clamp();
    CHECK_NEAR(f.fov, Settings::kFovMin, 1e-6);

    // ---- monitor-aware window sizes ---------------------------------------
    Resolution modes[Settings::kMaxWindowModes];
    // Unknown monitor (headless): every preset is offered.
    int n = Settings::windowModes(modes, Settings::kMaxWindowModes, 0, 0);
    CHECK(n == Settings::kModes);
    // 1080p screen: 1440p is dropped and the usable area equals the 1080p
    // preset, so it is not offered twice.
    n = Settings::windowModes(modes, Settings::kMaxWindowModes, 1920, 1080);
    CHECK(n == 3);
    CHECK(modes[2].w == 1920 && modes[2].h == 1080);
    // Roomier than every preset: presets + the usable area as the MAX entry.
    n = Settings::windowModes(modes, Settings::kMaxWindowModes, 3840, 2080);
    CHECK(n == Settings::kMaxWindowModes);
    CHECK(modes[n - 1].w == 3840 && modes[n - 1].h == 2080);
    // Tiny screen: only the largest usable size remains.
    n = Settings::windowModes(modes, Settings::kMaxWindowModes, 640, 400);
    CHECK(n == 1);
    CHECK(modes[0].w == 640 && modes[0].h == 400);

    // A size saved on a big monitor is snapped down on a small one.
    Settings w;
    w.width = 2560; w.height = 1440;
    w.fitToMonitor(1366, 728);
    CHECK(w.width == 1366 && w.height == 728);
    // Cycling on that small screen skips 900p/1080p/1440p entirely.
    w.width = 1280; w.height = 720;
    w.cycleMode(+1, 1366, 728);
    CHECK(w.width == 1366 && w.height == 728);
    w.cycleMode(+1, 1366, 728);
    CHECK(w.width == 1280 && w.height == 720);   // wraps around
    w.cycleMode(-1, 1366, 728);
    CHECK(w.width == 1366 && w.height == 728);
    for (int i = 0; i < 8; ++i) {
        w.cycleMode(+1, 1366, 728);
        CHECK(w.width <= 1366 && w.height <= 728);   // never larger than the monitor
    }
    // Presets stay reachable on a large monitor.
    w.width = 1280; w.height = 720;
    w.cycleMode(+1, 3840, 2080);
    CHECK(w.width == 1600 && w.height == 900);
    w.cycleMode(-1, 3840, 2080);
    CHECK(w.width == 1280 && w.height == 720);
    // Unknown monitor: plain preset cycling (unchanged legacy behaviour).
    w.width = 1280; w.height = 720;
    w.cycleMode(+1, 0, 0);
    CHECK(w.width == 1600 && w.height == 900);
    w.cycleMode(-1, 0, 0);
    CHECK(w.width == 1280 && w.height == 720);
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
        // Frame input straight from the backend, like the game loop does: this
        // is what carries the real client size into the menu. The cursor is
        // parked off the panel so this stays a keyboard-only test.
        FrameInput in;
        plat->frame(in);
        in.mouseX = -1.0f; in.mouseY = -1.0f;
        if (key) in.keys[key] = 1;
        m.update(in, s, audio, *plat);
    };
    // Down x5 (volume, resolution, fov, sensitivity, fullscreen) -> Restart.
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
    // Resolution cycles + resizes the backend.
    frame(KEY_DOWN); frame(0);
    CHECK(m.selected() == Menu::Resolution);
    int w0 = s.width;
    frame(KEY_RIGHT); frame(0);
    CHECK(s.width != w0);
    FrameInput probe = zeroInput();
    plat->frame(probe);
    CHECK(probe.width == s.width && probe.height == s.height);
    // Field of view: arrows step it and it stays inside the slider range.
    frame(KEY_DOWN); frame(0);
    CHECK(m.selected() == Menu::Fov);
    float f0 = s.fov;
    frame(KEY_RIGHT); frame(0);
    CHECK(s.fov > f0);
    frame(KEY_LEFT); frame(0);
    CHECK_NEAR(s.fov, f0, 1e-6);
    for (int i = 0; i < 30; ++i) { frame(KEY_RIGHT); frame(0); }
    CHECK(s.fov <= Settings::kFovMax);
    for (int i = 0; i < 60; ++i) { frame(KEY_LEFT); frame(0); }
    CHECK(s.fov >= Settings::kFovMin);
    for (int i = 0; i < 20; ++i) { frame(KEY_RIGHT); frame(0); }
    // Fullscreen toggles via Enter and arrows (headless backend ignores it).
    frame(KEY_DOWN); frame(0);  // sensitivity
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
// Windowed backend with a configurable monitor: exercises the monitor-aware
// window sizing (and the menu's resolution row) without a real display.
class StubPlatform final : public Platform {
public:
    bool init(const char*, int w, int h) override {
        width_ = w; height_ = h;
        return true;
    }
    bool frame(FrameInput& in) override {
        for (int i = 0; i < 8; ++i) { in.mousePressed[i] = false; in.mouseReleased[i] = false; }
        std::memset(in.keys, 0, sizeof(in.keys));
        in.mouseDX = in.mouseDY = 0.0f;
        in.mouseX = float(width_ / 2); in.mouseY = float(height_ / 2);
        in.width = width_; in.height = height_;
        in.shouldQuit = false;
        return true;
    }
    void swapBuffers() override {}
    void shutdown() override {}
    void setCursorCaptured(bool) override {}
    // Mirrors the real backends: a request larger than the monitor is clamped.
    void resize(int w, int h) override {
        if (w <= 0 || h <= 0) return;
        if (availW_ > 0 && w > availW_) w = availW_;
        if (availH_ > 0 && h > availH_) h = availH_;
        width_ = w; height_ = h;
    }
    void setFullscreen(bool on) override { fullscreen_ = on; }
    bool maxWindowSize(int& w, int& h) const override {
        if (availW_ <= 0 || availH_ <= 0) return false;
        w = availW_; h = availH_;
        return true;
    }
    bool clientSize(int& w, int& h) const override { w = width_; h = height_; return true; }
    BackendInfo info() const override {
        BackendInfo b;
        b.hasWindow = true;
        b.hasGL = false;
        b.name = "stub";
        return b;
    }
    void* loadGLProc(const char*) override { return nullptr; }

    float uiScale() const override { return uiScale_; }

    void setMonitor(int w, int h) { availW_ = w; availH_ = h; }
    void setUiScale(float s) { uiScale_ = s; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    int availW_ = 0, availH_ = 0;   // largest usable client size (0 = unknown)
    int width_ = 1280, height_ = 720;
    bool fullscreen_ = false;
    float uiScale_ = 1.0f;
};

// ---------------------------------------------------------------------------
static void testUiScale() {
    // DPI-derived scale is quantized to half steps and clamped (96 dpi -> 1.0,
    // 120/144 dpi -> 1.5, 192 dpi -> 2.0).
    CHECK_NEAR(quantizeUiScale(1.0f), 1.0, 1e-6);
    CHECK_NEAR(quantizeUiScale(1.25f), 1.5, 1e-6);
    CHECK_NEAR(quantizeUiScale(1.5f), 1.5, 1e-6);
    CHECK_NEAR(quantizeUiScale(1.75f), 2.0, 1e-6);
    CHECK_NEAR(quantizeUiScale(2.0f), 2.0, 1e-6);
    CHECK_NEAR(quantizeUiScale(0.5f), 1.0, 1e-6);
    CHECK_NEAR(quantizeUiScale(0.0f), 1.0, 1e-6);
    CHECK_NEAR(quantizeUiScale(4.0f), 2.0, 1e-6);
    // The panel must always fit the window: the scale drops back if needed.
    CHECK_NEAR(Menu::fitUiScale(2.0f, 3840, 2160), 2.0, 1e-6);
    CHECK_NEAR(Menu::fitUiScale(2.0f, 1600, 1200), 2.0, 1e-6);
    CHECK_NEAR(Menu::fitUiScale(2.0f, 1280, 720), 1.0, 1e-6);    // 2.0/1.5 too tall
    CHECK_NEAR(Menu::fitUiScale(1.5f, 2560, 1440), 1.5, 1e-6);
    CHECK_NEAR(Menu::fitUiScale(1.0f, 800, 600), 1.0, 1e-6);
    // Menu clicks still land on the row the cursor is over on a scaled display.
    StubPlatform plat;
    plat.setMonitor(1600, 1200);
    plat.setUiScale(2.0f);
    plat.init("t", 1600, 1200);
    Audio audio;
    audio.init(false);
    Settings s;
    Menu m;
    m.open();
    // With a 2.0 UI scale the panel is 1120x1120, centred in 1600x1200: the
    // Resume button (2nd of 3) sits around y = 600 + 300 + 200 = ...
    auto clickAt = [&](float x, float y) {
        FrameInput in;
        plat.frame(in);
        in.mouseX = x; in.mouseY = y;
        in.mousePressed[MBTN_LEFT] = true;
        m.update(in, s, audio, plat);
        in.mousePressed[MBTN_LEFT] = false;
        plat.frame(in);
        in.mouseX = x; in.mouseY = y;
        m.update(in, s, audio, plat);
    };
    // Buttons are centred horizontally; clicking the middle button's row must
    // select Resume rather than something else.
    const float cx = 1600 * 0.5f;
    const float resumeY = (1200 - 1120) / 2.0f + (92 + 5 * 54 + 18 + (38 + 12)) * 2.0f + 38.0f;
    clickAt(cx, resumeY);
    CHECK(m.selected() == Menu::Resume);
    CHECK(m.consumeResume());
    audio.shutdown();
    plat.shutdown();
}

// ---------------------------------------------------------------------------
static void testWindowSizing() {
    // A 1366x768 laptop: the work area fits 720p and nothing bigger.
    StubPlatform plat;
    plat.setMonitor(1366, 728);
    plat.init("t", 1280, 720);
    Audio audio;
    audio.init(false);
    Settings s;
    Menu m;
    m.open();

    auto frame = [&](uint32_t key) {
        FrameInput in;
        plat.frame(in);
        in.mouseX = -1.0f; in.mouseY = -1.0f;
        if (key) in.keys[key] = 1;
        m.update(in, s, audio, plat);
    };

    frame(KEY_DOWN); frame(0);   // resolution row
    CHECK(m.selected() == Menu::Resolution);
    CHECK(s.width == 1280 && s.height == 720);
    // Stepping up offers the largest window the screen allows, not 1080p.
    frame(KEY_RIGHT); frame(0);
    CHECK(s.width == 1366 && s.height == 728);
    CHECK(plat.width() == 1366 && plat.height() == 728);   // backend resized
    frame(KEY_RIGHT); frame(0);
    CHECK(s.width == 1280 && s.height == 720);             // wraps around
    // Whatever the navigation does, the window never exceeds the monitor.
    for (int i = 0; i < 6; ++i) {
        frame(KEY_RIGHT); frame(0);
        CHECK(s.width <= 1366 && s.height <= 728);
        CHECK(plat.width() <= 1366 && plat.height() <= 728);
    }
    // Fullscreen on: the borderless window already covers the monitor, so the
    // resolution row only records the preference (nothing is resized).
    int keepW = s.width, keepH = s.height;
    for (int i = 0; i < 3; ++i) { frame(KEY_DOWN); frame(0); }
    CHECK(m.selected() == Menu::Fullscreen);
    frame(KEY_ENTER); frame(0);
    CHECK(s.fullscreen);
    CHECK(s.width == keepW && s.height == keepH);
    int platW = plat.width(), platH = plat.height();
    for (int i = 0; i < 3; ++i) { frame(KEY_UP); frame(0); }
    CHECK(m.selected() == Menu::Resolution);
    frame(KEY_RIGHT); frame(0);                    // cycle resolution while fullscreen
    CHECK(s.width <= 1366 && s.height <= 728);
    CHECK(plat.width() == platW && plat.height() == platH);
    // Leaving fullscreen applies the chosen windowed size again.
    for (int i = 0; i < 3; ++i) { frame(KEY_DOWN); frame(0); }
    CHECK(m.selected() == Menu::Fullscreen);
    frame(KEY_LEFT); frame(0);
    CHECK(!s.fullscreen);
    CHECK(plat.width() == s.width && plat.height() == s.height);

    audio.shutdown();
    plat.shutdown();
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
    testSensitivity();
    testMenuNav();
    testUiScale();
    testWindowSizing();
    testRestart();

    fprintf(stderr, "\n[aw-tests] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
