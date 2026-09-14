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

// Applies a display change the menu asked for exactly like Game does, so the
// backend state in the tests follows the real frame loop.
static bool applyPendingDisplay(Menu& m, Settings& s, Platform& p) {
    if (!m.consumeDisplayApply()) return false;
    Game::applyDisplayConfigTo(p, Game::displayConfigOf(s));
    return true;
}

// Navigates the menu to `item` with Down presses (wrap-safe).
static void selectRow(Menu& m, Settings& s, Audio& a, Platform& p, const FrameInput& base, int item) {
    for (int guard = 0; guard < 2 * Menu::Count && m.selected() != item; ++guard) {
        FrameInput in = base;
        in.keys[KEY_DOWN] = 1;
        m.update(in, s, a, p);
        applyPendingDisplay(m, s, p);
        m.update(base, s, a, p);
        applyPendingDisplay(m, s, p);
    }
}

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
    CHECK(s.mode == DisplayMode::Windowed);
    CHECK(s.renderWidth == 0 && s.renderHeight == 0);   // 0 = render at window size
    // clamping
    s.volume = 2.0f; s.sensitivity = -1.0f; s.width = 10; s.height = 99999;
    s.clamp();
    CHECK_NEAR(s.volume, 1.0, 1e-6);
    CHECK_NEAR(s.sensitivity, 0.1, 1e-6);
    CHECK(s.width == 320);
    CHECK(s.height == 4320);
    s.renderWidth = 10; s.renderHeight = 99999; s.clamp();
    CHECK(s.renderWidth == 320 && s.renderHeight == 4320);
    s.renderWidth = 0; s.renderHeight = 0; s.clamp();
    CHECK(s.renderWidth == 0 && s.renderHeight == 0);   // stays "native"
    s.mode = DisplayMode(9); s.clamp();
    CHECK(s.mode == DisplayMode::Windowed);

    // serialize/parse roundtrip (every display field)
    Settings a;
    a.volume = 0.35f; a.sensitivity = 2.5f; a.fov = 95.0f;
    a.mode = DisplayMode::Exclusive;
    a.width = 1920; a.height = 1080;
    a.renderWidth = 2560; a.renderHeight = 1440;
    a.modeWidth = 1920; a.modeHeight = 1080; a.modeRefresh = 144;
    char buf[384];
    a.serialize(buf, sizeof(buf));
    Settings b;
    CHECK(b.parse(buf));
    CHECK_NEAR(b.volume, 0.35, 1e-3);
    CHECK_NEAR(b.sensitivity, 2.5, 1e-3);
    CHECK_NEAR(b.fov, 95.0, 1e-3);
    CHECK(b.mode == DisplayMode::Exclusive);
    CHECK(b.width == 1920 && b.height == 1080);
    CHECK(b.renderWidth == 2560 && b.renderHeight == 1440);
    CHECK(b.modeWidth == 1920 && b.modeHeight == 1080 && b.modeRefresh == 144);
    // legacy config (only a fullscreen flag) maps to borderless fullscreen
    Settings legacy;
    CHECK(legacy.parse("volume=0.5\nwidth=1600\nheight=900\nfullscreen=1\n"));
    CHECK(legacy.mode == DisplayMode::Borderless);
    Settings legacyOff;
    CHECK(legacyOff.parse("fullscreen=0\n"));
    CHECK(legacyOff.mode == DisplayMode::Windowed);
    // an explicit displaymode always wins over the legacy flag
    Settings mixed;
    CHECK(mixed.parse("fullscreen=1\ndisplaymode=0\n"));
    CHECK(mixed.mode == DisplayMode::Windowed);
    // unknown keys ignored, missing keys keep their values
    Settings c;
    CHECK(c.parse("bogus=123\nvolume=0.5\n"));
    CHECK_NEAR(c.volume, 0.5, 1e-6);
    CHECK_NEAR(c.sensitivity, 1.0, 1e-6);
    CHECK_NEAR(c.fov, Settings::kFovDefault, 1e-6);

    // field of view: default, clamping
    CHECK_NEAR(Settings().fov, 75.0, 1e-6);
    Settings f;
    f.fov = 500.0f; f.clamp();
    CHECK_NEAR(f.fov, Settings::kFovMax, 1e-6);
    f.fov = 0.0f; f.clamp();
    CHECK_NEAR(f.fov, Settings::kFovMin, 1e-6);
}

// ---------------------------------------------------------------------------
static void testSettingsModeNames() {
    // The settings file is hand-editable; symbolic display modes are accepted.
    Settings s;
    CHECK(s.parse("displaymode=exclusive\nmodewidth=1920\nmodeheight=1080\n"));
    CHECK(s.mode == DisplayMode::Exclusive);
    CHECK(s.parse("displaymode=borderless\nrenderwidth=2560\nrenderheight=1440\n"));
    CHECK(s.mode == DisplayMode::Borderless);
    CHECK(s.renderWidth == 2560 && s.renderHeight == 1440);
    CHECK(s.parse("displaymode=windowed\n"));
    CHECK(s.mode == DisplayMode::Windowed);
    CHECK(s.parse("displaymode=fullscreen\n"));   // friendly alias
    CHECK(s.mode == DisplayMode::Borderless);
    CHECK(s.parse("displaymode=42\n"));           // out of range -> windowed
    CHECK(s.mode == DisplayMode::Windowed);
    // What we write is what we read back.
    Settings w;
    w.mode = DisplayMode::Exclusive;
    w.modeWidth = 2560; w.modeHeight = 1440; w.modeRefresh = 144;
    char buf[384];
    w.serialize(buf, sizeof(buf));
    Settings r;
    CHECK(r.parse(buf));
    CHECK(r.mode == DisplayMode::Exclusive);
    CHECK(r.modeWidth == 2560 && r.modeHeight == 1440 && r.modeRefresh == 144);
}

// ---------------------------------------------------------------------------
static void testRenderAspectFit() {
    // A render preset keeps its pixel budget but takes the window's shape.
    int rw = 0, rh = 0;
    Settings::fitRenderAspect(1280, 720, 1920, 1080, rw, rh);
    CHECK(rw == 1280 && rh == 720);                    // exact 16:9 -> unchanged
    Settings::fitRenderAspect(1280, 720, 2560, 1440, rw, rh);
    CHECK(rw == 1280 && rh == 720);
    Settings::fitRenderAspect(1280, 720, 1680, 1050, rw, rh);   // 16:10 window
    CHECK_NEAR(double(rw) / double(rh), 1.6, 0.01);
    CHECK(rw * rh > 1280 * 720 * 0.95 && rw * rh < 1280 * 720 * 1.05);
    Settings::fitRenderAspect(1280, 720, 1024, 768, rw, rh);    // 4:3 window
    CHECK_NEAR(double(rw) / double(rh), 4.0 / 3.0, 0.01);
    CHECK(rw * rh > 1280 * 720 * 0.95 && rw * rh < 1280 * 720 * 1.05);
    // Degenerate inputs are passed through (no division by zero).
    Settings::fitRenderAspect(1280, 720, 0, 0, rw, rh);
    CHECK(rw == 1280 && rh == 720);
    Settings::fitRenderAspect(0, 0, 1920, 1080, rw, rh);
    CHECK(rw == 0 && rh == 0);
    // Never below the minimum render size.
    Settings::fitRenderAspect(100, 100, 4096, 2160, rw, rh);
    CHECK(rw >= Settings::kMinWindowW && rh >= Settings::kMinWindowH);
}

// ---------------------------------------------------------------------------
static void testDisplayModeLists() {
    Resolution modes[Settings::kMaxModes];

    // ---- windowed: only the standard sizes that fit ------------------------
    int n = Settings::windowModes(modes, Settings::kMaxModes, 0, 0);
    CHECK(n == Settings::kPresetCount);                       // monitor unknown
    CHECK(modes[n - 1].w == 3840 && modes[n - 1].h == 2160);  // up to 4K
    // 4K desktop minus decorations: 4K no longer fits, 1440p is the largest.
    n = Settings::windowModes(modes, Settings::kMaxModes, 3840, 2120);
    CHECK(n == Settings::kPresetCount - 1);
    CHECK(modes[n - 1].w == 2560 && modes[n - 1].h == 1440);
    // 1080p screen with a taskbar and window frame: 1440p/4K are dropped, and
    // because a 1920x1080 *window* cannot fit a 1902x1003 work area the list
    // ends at 1600x900 -- no made-up "1902x1003" entry.
    n = Settings::windowModes(modes, Settings::kMaxModes, 1902, 1003);
    CHECK(n == 2);
    CHECK(modes[n - 1].w == 1600 && modes[n - 1].h == 900);
    for (int i = 0; i < n; ++i) CHECK(Settings::isStandardResolution(modes[i].w, modes[i].h));
    n = Settings::windowModes(modes, Settings::kMaxModes, 1920, 1080);
    CHECK(n == 3);
    CHECK(modes[2].w == 1920 && modes[2].h == 1080);
    // Every entry is a standard size, whatever the work area looks like.
    const int areas[][2] = {{1003, 986}, {1279, 719}, {640, 400}, {1366, 728}, {3840, 2120}};
    for (const auto& a : areas) {
        n = Settings::windowModes(modes, Settings::kMaxModes, a[0], a[1]);
        CHECK(n >= 1);
        for (int i = 0; i < n; ++i) CHECK(Settings::isStandardResolution(modes[i].w, modes[i].h));
    }
    // A screen too small for any preset still offers the smallest standard size
    // (the backend clamps the window to the screen).
    n = Settings::windowModes(modes, Settings::kMaxModes, 640, 400);
    CHECK(n == 1);
    CHECK(modes[0].w == 1280 && modes[0].h == 720);
    // A leftover non-standard saved size snaps onto the standard list.
    Settings s;
    s.width = 1003; s.height = 986;                  // e.g. written by an older build
    s.fitToMonitor(1902, 1003);
    CHECK(s.width == 1600 && s.height == 900);
    s.width = 3840; s.height = 2160;                 // too big for this screen
    s.fitToMonitor(1902, 1003);
    CHECK(s.width == 1600 && s.height == 900);
    s.width = 1280; s.height = 720;                  // a standard size is kept
    s.fitToMonitor(3840, 2120);
    CHECK(s.width == 1280 && s.height == 720);

    // ---- borderless: render resolutions up to 4K + the native resolution ----
    n = Settings::renderModes(modes, Settings::kMaxModes, 1920, 1080);
    CHECK(n == Settings::kPresetCount);       // presets only (up to 4K supersampling)
    CHECK(modes[n - 1].w == 3840 && modes[n - 1].h == 2160);
    int nativeEntry = 0;
    for (int i = 0; i < n; ++i) if (modes[i].w == 1920 && modes[i].h == 1080) ++nativeEntry;
    CHECK(nativeEntry == 1);                  // native is already a preset: no duplicate
    // A native resolution between two presets is inserted in ascending order.
    n = Settings::renderModes(modes, Settings::kMaxModes, 1366, 768);
    CHECK(n == Settings::kPresetCount + 1);
    CHECK(modes[1].w == 1366 && modes[1].h == 768);
    // A virtualised/odd desktop size is not offered as a render resolution; the
    // "native" default (renderWidth = 0) still renders at the real size.
    n = Settings::renderModes(modes, Settings::kMaxModes, 1003, 986);
    CHECK(n == Settings::kPresetCount);
    for (int i = 0; i < n; ++i) CHECK(Settings::isStandardResolution(modes[i].w, modes[i].h));
    // Unknown monitor: presets alone.
    n = Settings::renderModes(modes, Settings::kMaxModes, 0, 0);
    CHECK(n == Settings::kPresetCount);

    // Stepping never lands off-list (and wraps).
    int rw = 1280, rh = 720;
    Settings::stepMode(+1, rw, rh, modes, n);
    CHECK(rw == 1600 && rh == 900);
    Settings::stepMode(-1, rw, rh, modes, n);
    CHECK(rw == 1280 && rh == 720);
    Settings::stepMode(-1, rw, rh, modes, n);          // wraps to the largest
    CHECK(rw == 3840 && rh == 2160);
    Settings::stepMode(+1, rw, rh, modes, n);          // and round to the smallest
    CHECK(rw == 1280 && rh == 720);

    // ---- exclusive: the driver's pool, deduplicated by size ---------------
    DisplayModeInfo pool[8] = {
        {1920, 1080, 60}, {1920, 1080, 144}, {2560, 1440, 60}, {1920, 1080, 60},
        {1280, 720, 60}, {3840, 2160, 60}, {640, 480, 0}, {0, 0, 0},
    };
    n = Settings::exclusiveModes(modes, Settings::kMaxModes, pool, 8);
    CHECK(n == 5);                                     // 480p/720p/1080p/1440p/4K
    CHECK(modes[0].w == 640 && modes[0].h == 480);
    CHECK(modes[n - 1].w == 3840 && modes[n - 1].h == 2160);
    int hz[16];
    n = Settings::refreshRates(hz, 16, pool, 8, 1920, 1080);
    CHECK(n == 3);                                     // 60, 144 and DEFAULT
    CHECK(hz[0] == 60 && hz[1] == 144 && hz[2] == 0);
    n = Settings::refreshRates(hz, 16, pool, 8, 3840, 2160);
    CHECK(n == 2 && hz[0] == 60 && hz[1] == 0);
    n = Settings::refreshRates(hz, 16, pool, 8, 1024, 768);
    CHECK(n == 0);                                     // size not offered
}

// ---------------------------------------------------------------------------
static void testDisplayConfirm() {
    // The modal "keep these display settings?" dialog: countdown, keep, revert,
    // Escape, mouse and the automatic revert when nobody answers.
    auto input = [](int w, int h) {
        FrameInput in;
        in.width = w; in.height = h;
        in.mouseX = -1.0f; in.mouseY = -1.0f;
        return in;
    };
    DisplayConfirm d;
    CHECK(!d.active());
    d.begin(Settings::kDisplayConfirmSeconds, "EXCLUSIVE 1920x1080 60HZ");
    CHECK(d.active());
    CHECK_NEAR(d.remaining(), Settings::kDisplayConfirmSeconds, 1e-6);
    // Countdown ticks down and is still active until it runs out.
    for (int i = 0; i < 14; ++i) {
        FrameInput in = input(1920, 1080);
        CHECK(d.update(in, 1.0f, 1.0f) == DisplayConfirm::None);
    }
    CHECK(d.active());
    // ...and reverts by itself when the timer expires.
    FrameInput in = input(1920, 1080);
    CHECK(d.update(in, 2.0f, 1.0f) == DisplayConfirm::Revert);
    CHECK(!d.active());                       // closed: no second decision
    CHECK(d.update(in, 1.0f, 1.0f) == DisplayConfirm::None);

    // Enter keeps, Escape reverts.
    d.begin(15.0f, "TEST");
    in = input(1920, 1080);
    in.keys[KEY_ENTER] = 1;
    CHECK(d.update(in, 0.1f, 1.0f) == DisplayConfirm::Keep);
    CHECK(!d.active());
    d.begin(15.0f, "TEST");
    in = input(1920, 1080);
    in.keys[KEY_ESC] = 1;
    CHECK(d.update(in, 0.1f, 1.0f) == DisplayConfirm::Revert);
    CHECK(!d.active());

    // A dialog opened while Enter is still held (the user just pressed it in
    // the menu) must not confirm itself: the key has to be pressed again.
    in = input(1920, 1080);
    in.keys[KEY_ENTER] = 1;
    d.begin(15.0f, "TEST", in.keys);
    CHECK(d.update(in, 0.1f, 1.0f) == DisplayConfirm::None);
    CHECK(d.active());
    in.keys[KEY_ENTER] = 0;                     // released...
    CHECK(d.update(in, 0.1f, 1.0f) == DisplayConfirm::None);
    in.keys[KEY_ENTER] = 1;                     // ...and pressed again
    CHECK(d.update(in, 0.1f, 1.0f) == DisplayConfirm::Keep);
    // Mouse: clicking KEEP confirms, clicking REVERT reverts, nowhere does nothing.
    // Layout at UI scale 1.0 in a 1920x1080 window: panel 640x250 centered at
    // (640,415) with a 210x44 button pair starting at y=569.
    d.begin(15.0f, "TEST");
    in = input(1920, 1080);
    in.mouseX = 843.0f; in.mouseY = 591.0f;      // KEEP button centre
    in.mousePressed[MBTN_LEFT] = true;
    CHECK(d.update(in, 0.1f, 1.0f) == DisplayConfirm::Keep);
    d.begin(15.0f, "TEST");
    in = input(1920, 1080);
    in.mouseX = 1077.0f; in.mouseY = 591.0f;     // REVERT button centre
    in.mousePressed[MBTN_LEFT] = true;
    CHECK(d.update(in, 0.1f, 1.0f) == DisplayConfirm::Revert);
    d.begin(15.0f, "TEST");
    in = input(1920, 1080);
    in.mouseX = 100.0f; in.mouseY = 100.0f;      // outside the buttons
    in.mousePressed[MBTN_LEFT] = true;
    CHECK(d.update(in, 0.1f, 1.0f) == DisplayConfirm::None);
    CHECK(d.active());
    // cancel() closes without a decision (used after a refused change).
    d.cancel();
    CHECK(!d.active());
    CHECK(d.update(input(1920, 1080), 1.0f, 1.0f) == DisplayConfirm::None);
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
    int displayApplies = 0;

    auto frame = [&](uint32_t key) {
        // Frame input straight from the backend, like the game loop does: this
        // is what carries the real client size into the menu. The cursor is
        // parked off the panel so this stays a keyboard-only test.
        FrameInput in;
        plat->frame(in);
        in.mouseX = -1.0f; in.mouseY = -1.0f;
        if (key) in.keys[key] = 1;
        m.update(in, s, audio, *plat);
        // Mirror the game loop: a display change is applied to the backend.
        if (applyPendingDisplay(m, s, *plat)) ++displayApplies;
    };
    // Down x6 (volume, mode, resolution, refresh, fov, sensitivity) -> Restart.
    for (int i = 0; i < 6; ++i) { frame(KEY_DOWN); frame(0); }
    CHECK(m.selected() == Menu::Restart);
    frame(KEY_ENTER); frame(0);
    CHECK(m.consumeRestart());
    CHECK(!m.consumeRestart());
    // Up wraps to the top (volume); Right raises the volume bar.
    for (int i = 0; i < 6; ++i) { frame(KEY_UP); frame(0); }
    CHECK(m.selected() == Menu::Volume);
    float v0 = s.volume;
    frame(KEY_RIGHT); frame(0);
    CHECK(s.volume > v0);
    frame(KEY_LEFT); frame(0);
    CHECK_NEAR(s.volume, v0, 1e-6);
    // Window size cycles, resizes the backend, and the change is provisional:
    // the menu asks the game to apply and confirm it.
    selectRow(m, s, audio, *plat, zeroInput(), Menu::Resolution);
    CHECK(m.selected() == Menu::Resolution);
    int w0 = s.width, applies0 = displayApplies;
    frame(KEY_RIGHT); frame(0);
    CHECK(s.width != w0);
    CHECK(displayApplies == applies0 + 1);   // exactly one request per change
    CHECK(plat->currentDisplayMode() == DisplayMode::Windowed);
    FrameInput probe = zeroInput();
    plat->frame(probe);
    CHECK(probe.width == s.width && probe.height == s.height);
    // Field of view: arrows step it and it stays inside the slider range.
    selectRow(m, s, audio, *plat, zeroInput(), Menu::Fov);
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
    // Sensitivity is reachable and steps.
    selectRow(m, s, audio, *plat, zeroInput(), Menu::Sensitivity);
    CHECK(m.selected() == Menu::Sensitivity);
    float sens0 = s.sensitivity;
    frame(KEY_RIGHT); frame(0);
    CHECK(s.sensitivity > sens0);
    // Display mode cycles windowed -> borderless -> exclusive -> windowed, and
    // each change is applied to the backend (checkboxes are provisional).
    selectRow(m, s, audio, *plat, zeroInput(), Menu::Mode);
    CHECK(m.selected() == Menu::Mode);
    CHECK(s.mode == DisplayMode::Windowed);
    frame(KEY_RIGHT); frame(0);
    CHECK(s.mode == DisplayMode::Borderless);
    CHECK(applies0 + 2 == displayApplies);
    CHECK(plat->currentDisplayMode() == DisplayMode::Borderless);
    frame(KEY_RIGHT); frame(0);
    CHECK(s.mode == DisplayMode::Exclusive);
    frame(KEY_RIGHT); frame(0);
    CHECK(s.mode == DisplayMode::Windowed);
    frame(KEY_LEFT); frame(0);
    CHECK(s.mode == DisplayMode::Exclusive);
    // Quit via keyboard.
    selectRow(m, s, audio, *plat, zeroInput(), Menu::Quit);
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
    // Display modes: a fake driver pool + recording of applied configurations.
    bool applyDisplayMode(DisplayMode mode, int w, int h, int hz) override {
        if (mode == DisplayMode::Exclusive) {
            if (exclusiveSupported_ == 0) return false;
            bool found = false;
            for (int i = 0; i < driverCount_ && !found; ++i)
                found = driver_[i].width == w && driver_[i].height == h &&
                        (hz <= 0 || driver_[i].refreshHz == hz);
            if (!found) return false;
            desktopW_ = w; desktopH_ = h;
            width_ = w; height_ = h;
        } else if (mode == DisplayMode::Borderless) {
            width_ = desktopW_ > 0 ? desktopW_ : width_;
            height_ = desktopH_ > 0 ? desktopH_ : height_;
        } else {
            resize(w, h);
        }
        mode_ = mode;
        lastW_ = w; lastH_ = h; lastHz_ = hz;
        ++applyCount_;
        return true;
    }
    DisplayMode currentDisplayMode() const override { return mode_; }
    bool monitorSize(int& w, int& h) const override {
        if (desktopW_ <= 0 || desktopH_ <= 0) return false;
        w = desktopW_; h = desktopH_;
        return true;
    }
    int displayModeCount() const override { return driverCount_; }
    bool displayModeAt(int index, DisplayModeInfo& out) const override {
        if (index < 0 || index >= driverCount_) return false;
        out = driver_[index];
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
    void setDesktop(int w, int h) { desktopW_ = w; desktopH_ = h; }
    void setUiScale(float s) { uiScale_ = s; }
    // Fake driver pool (exclusive fullscreen modes).
    void addDriverMode(int w, int h, int hz) {
        if (driverCount_ < 32) driver_[driverCount_++] = DisplayModeInfo{w, h, hz};
    }
    void setExclusiveSupported(bool on) { exclusiveSupported_ = on ? 1 : 0; }
    int width() const { return width_; }
    int height() const { return height_; }
    DisplayMode mode() const { return mode_; }
    int applyCount() const { return applyCount_; }
    int lastW() const { return lastW_; }
    int lastH() const { return lastH_; }
    int lastHz() const { return lastHz_; }

private:
    int availW_ = 0, availH_ = 0;   // largest usable client size (0 = unknown)
    int desktopW_ = 0, desktopH_ = 0;   // native monitor size (0 = unknown)
    int width_ = 1280, height_ = 720;
    float uiScale_ = 1.0f;
    DisplayMode mode_ = DisplayMode::Windowed;
    DisplayModeInfo driver_[32]{};
    int driverCount_ = 0;
    int exclusiveSupported_ = 1;
    int applyCount_ = 0, lastW_ = 0, lastH_ = 0, lastHz_ = 0;
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
    CHECK_NEAR(Menu::fitUiScale(2.0f, 1600, 1300), 2.0, 1e-6);
    CHECK_NEAR(Menu::fitUiScale(2.0f, 1600, 1200), 1.5, 1e-6);   // panel is 620*2 tall
    CHECK_NEAR(Menu::fitUiScale(2.0f, 1280, 720), 1.0, 1e-6);
    CHECK_NEAR(Menu::fitUiScale(1.5f, 2560, 1440), 1.5, 1e-6);
    CHECK_NEAR(Menu::fitUiScale(1.0f, 800, 600), 1.0, 1e-6);
    // Menu clicks still land on the row/button the cursor is over on a scaled
    // display: ask the menu where it drew the RESUME button, then click there.
    StubPlatform plat;
    plat.setMonitor(1600, 1300);
    plat.setDesktop(1920, 1080);
    plat.setUiScale(2.0f);
    plat.init("t", 1600, 1300);
    Audio audio2;
    audio2.init(false);
    Settings s2;
    Menu m2;
    m2.open();
    auto baseInput = [&]() {
        FrameInput in;
        plat.frame(in);
        in.mouseX = -1.0f; in.mouseY = -1.0f;
        return in;
    };
    m2.update(baseInput(), s2, audio2, plat);          // compute the layout
    CHECK(plat.uiScale() == 2.0f);                     // panel laid out at 2x
    float bx = 0.0f, by = 0.0f;
    m2.itemCenter(Menu::Resume, bx, by);
    CHECK(bx > 0.0f && by > 0.0f && bx < 1600.0f && by < 1300.0f);
    auto clickAt = [&](float x, float y) {
        FrameInput in = baseInput();
        in.mouseX = x; in.mouseY = y;
        in.mousePressed[MBTN_LEFT] = true;
        m2.update(in, s2, audio2, plat);
        applyPendingDisplay(m2, s2, plat);
        m2.update(baseInput(), s2, audio2, plat);
        applyPendingDisplay(m2, s2, plat);
    };
    clickAt(bx, by);
    CHECK(m2.selected() == Menu::Resume);
    CHECK(m2.consumeResume());
    CHECK(!m2.consumeResume());   // one click, one activation
    audio2.shutdown();
    plat.shutdown();
}

// ---------------------------------------------------------------------------
static void testWindowSizing() {
    // A 1920x1080 laptop whose usable area (taskbar + window frame removed) is
    // 1902x1003, the exact situation that used to produce a "1902x1003" entry.
    StubPlatform plat;
    plat.setMonitor(1902, 1003);
    plat.setDesktop(1920, 1080);
    plat.addDriverMode(1280, 720, 60);
    plat.addDriverMode(1920, 1080, 60);
    plat.init("t", 1280, 720);
    Audio audio;
    audio.init(false);
    Settings s;
    Menu m;
    m.open();
    int displayApplies = 0;

    auto baseInput = [&]() {
        FrameInput in;
        plat.frame(in);
        in.mouseX = -1.0f; in.mouseY = -1.0f;
        return in;
    };
    auto frame = [&](uint32_t key) {
        FrameInput in = baseInput();
        if (key) in.keys[key] = 1;
        m.update(in, s, audio, plat);
        if (applyPendingDisplay(m, s, plat)) ++displayApplies;   // like the game loop
    };
    auto navTo = [&](int item) {
        for (int guard = 0; guard < 2 * Menu::Count && m.selected() != item; ++guard) {
            frame(KEY_DOWN);
            frame(0);
        }
        CHECK(m.selected() == item);
    };

    // ---- windowed: standard client sizes that fit the work area ------------
    navTo(Menu::Resolution);
    CHECK(s.width == 1280 && s.height == 720);
    frame(KEY_RIGHT);
    frame(0);
    CHECK(s.width == 1600 && s.height == 900);                 // 1080p needs 1920 wide
    CHECK(plat.width() == 1600 && plat.height() == 900);       // backend resized
    CHECK(Settings::isStandardResolution(s.width, s.height));
    frame(KEY_RIGHT);
    frame(0);
    CHECK(s.width == 1280 && s.height == 720);                 // only 2 entries: wraps
    CHECK(Settings::isStandardResolution(s.width, s.height));
    // A whole cycle never leaves the standard list and never exceeds the screen.
    const int applies0 = displayApplies;
    for (int i = 0; i < 6; ++i) {
        frame(KEY_RIGHT);
        frame(0);
        CHECK(Settings::isStandardResolution(s.width, s.height));
        CHECK(s.width <= 1902 && s.height <= 1003);
        CHECK(plat.width() == s.width && plat.height() == s.height);
    }
    CHECK(displayApplies == applies0 + 6);                     // one apply per press

    // The window size in the settings stays standard even though the backend
    // clamps/drags the window: no "1003x986"-style value is ever produced.
    for (int i = 0; i < 4; ++i) {
        FrameInput in = baseInput();
        in.width = 1003; in.height = 986;                      // as if the user dragged
        m.update(in, s, audio, plat);
        CHECK(Settings::isStandardResolution(s.width, s.height));
    }
    CHECK(displayApplies == applies0 + 6);                      // ...and no re-apply

    // ---- borderless: render resolution list has no odd entry either --------
    navTo(Menu::Mode);
    frame(KEY_RIGHT);
    frame(0);
    CHECK(s.mode == DisplayMode::Borderless);
    CHECK(plat.mode() == DisplayMode::Borderless);
    CHECK(plat.width() == 1920 && plat.height() == 1080);       // monitor size
    navTo(Menu::Resolution);
    CHECK(s.renderWidth == 0 && s.renderHeight == 0);           // native
    frame(KEY_RIGHT);
    frame(0);
    CHECK(s.renderWidth > 0 && s.renderHeight > 0);
    for (int i = 0; i < 8; ++i) {
        frame(KEY_RIGHT);
        frame(0);
        CHECK(s.renderWidth >= 1280 && s.renderWidth <= 3840);
        CHECK(Settings::isStandardResolution(s.renderWidth, s.renderHeight));
    }
    CHECK(plat.width() == 1920 && plat.height() == 1080);       // window untouched

    // ---- exclusive: the list is the driver's pool --------------------------
    navTo(Menu::Mode);
    frame(KEY_RIGHT);
    frame(0);
    CHECK(s.mode == DisplayMode::Exclusive);
    navTo(Menu::Resolution);
    s.modeWidth = 1920; s.modeHeight = 1080; s.modeRefresh = 0;
    frame(KEY_RIGHT);
    frame(0);
    CHECK(s.modeWidth == 1280 && s.modeHeight == 720);          // largest -> wraps
    CHECK(plat.mode() == DisplayMode::Exclusive);
    CHECK(plat.lastW() == 1280 && plat.lastH() == 720);
    navTo(Menu::Refresh);
    s.modeRefresh = 0;
    frame(KEY_RIGHT);
    frame(0);
    CHECK(s.modeRefresh == 60);
    frame(KEY_RIGHT);
    frame(0);
    CHECK(s.modeRefresh == 0);                                  // DEFAULT

    audio.shutdown();
    plat.shutdown();
}

// ---------------------------------------------------------------------------
// Regression tests for the "changing the resolution asks twice" report: one
// user action must produce exactly one backend change and exactly one dialog,
// and the key that dismisses the dialog must not act on the menu underneath it.
// This drives the real Game overlay path (Game::stepOverlay), not a copy of it.
static void testDisplayConfirmOnce() {
    const char* kCfg = "settings_once_test.cfg";
    FILE* pre = std::fopen(kCfg, "rb");
    bool hadSettings = (pre != nullptr);
    if (pre) std::fclose(pre);
    Settings::setPathForTests(kCfg);

    {
        Settings saved;
        saved.mode = DisplayMode::Windowed;
        saved.width = 1280; saved.height = 720;
        saved.save();
        Game g;
        CHECK(g.init("t", 1280, 720, true));    // headless backend
        Settings& s = g.settings();
        Menu& m = g.menu();
        g.openMenu();

        auto input = [](uint32_t key) {
            FrameInput in;
            std::memset(in.keys, 0, sizeof(in.keys));
            if (key) in.keys[key] = 1;
            in.width = 1280; in.height = 720;
            in.mouseX = -1.0f; in.mouseY = -1.0f;
            return in;
        };
        // Counts the changes and the dialogs the user would see.
        int changes = 0, dialogs = 0;
        bool dialogWasActive = false;
        int lastW = s.width, lastH = s.height;
        auto step = [&](uint32_t key) {
            FrameInput in = input(key);
            g.stepOverlay(in, 1.0f / 60.0f);
            if (s.width != lastW || s.height != lastH) { ++changes; lastW = s.width; lastH = s.height; }
            bool active = g.displayConfirmActive();
            if (active && !dialogWasActive) ++dialogs;
            dialogWasActive = active;
        };

        // Menu Down presses need a release between them (edge triggered).
        for (int i = 0; i < 2; ++i) { step(KEY_DOWN); step(0); }
        CHECK(m.selected() == Menu::Resolution);
        CHECK(changes == 0 && dialogs == 0);

        // One Right press: exactly one change and one dialog.
        const int w0 = s.width;
        step(KEY_RIGHT);
        CHECK(s.width != w0);
        CHECK(changes == 1);
        CHECK(dialogs == 1);
        CHECK(g.displayConfirmActive());
        CHECK(Settings::isStandardResolution(s.width, s.height));

        // Right stays held while the dialog is up: the menu must not keep
        // stepping and the dialog must not be re-opened.
        for (int i = 0; i < 5; ++i) step(KEY_RIGHT);
        CHECK(changes == 1 && dialogs == 1);
        const int w1 = s.width;

        // A windowed size that no longer fits the screen (e.g. saved on a bigger
        // monitor) snaps to the largest standard preset that does -- the setting
        // and the real client area never disagree, and no odd size is applied.
        {
            Settings onDisk;
            onDisk.mode = DisplayMode::Windowed;
            onDisk.save();
            Game g2;
            CHECK(g2.init("t", 1280, 720, true));
            CHECK(Settings::isStandardResolution(g2.settings().width, g2.settings().height));
        }
        {
            // With a backend that reports the real work area, the snap happens
            // before the window is even asked for that size.
            StubPlatform plat;
            plat.setMonitor(1902, 1003);           // 1920x1080 screen + taskbar/frame
            plat.setDesktop(1920, 1080);
            plat.init("t", 1280, 720);
            Settings s2;
            s2.mode = DisplayMode::Windowed;
            s2.width = 3840; s2.height = 2160;     // saved on a 4K monitor, not here
            Game::fitWindowSizeToMonitor(s2, plat);
            CHECK(s2.width == 1600 && s2.height == 900);
            CHECK(Settings::isStandardResolution(s2.width, s2.height));
            CHECK(Game::applyDisplayConfigTo(plat, Game::displayConfigOf(s2)));
            CHECK(plat.width() == 1600 && plat.height() == 900);   // nothing clamped
            // A standard size that still fits is kept as-is.
            s2.width = 1280; s2.height = 720;
            Game::fitWindowSizeToMonitor(s2, plat);
            CHECK(s2.width == 1280 && s2.height == 720);
            // Other modes are untouched (their window is the monitor).
            s2.mode = DisplayMode::Borderless;
            s2.width = 3840;
            Game::fitWindowSizeToMonitor(s2, plat);
            CHECK(s2.width == 3840);
            plat.shutdown();
        }

        // A second change arriving while the user is still deciding is refused
        // outright instead of stacking another dialog on top of the first.
        const int hz0 = s.height;
        s.width = 1600; s.height = 900;
        CHECK(!g.requestDisplayApply());
        CHECK(g.displayConfirmActive());
        CHECK(dialogs == 1 && changes == 1);
        s.width = w1; s.height = hz0;          // what the dialog is asking about
        step(0);
        CHECK(dialogs == 1);

        // Confirm with Enter and keep it held for a while: the dialog closes
        // once and the held key does not act on the menu below it.
        step(0);                     // release Right
        step(KEY_ENTER);             // confirm
        CHECK(!g.displayConfirmActive());
        for (int i = 0; i < 8; ++i) step(KEY_ENTER);
        CHECK(changes == 1);         // no second change
        CHECK(dialogs == 1);         // no second dialog
        CHECK(s.width == w1);

        // Releasing and pressing again really does change it (once).
        step(0);
        step(KEY_RIGHT);
        CHECK(changes == 2);
        CHECK(dialogs == 2);
        CHECK(g.displayConfirmActive());

        // Escape reverts that provisional change back to the confirmed state.
        step(0);
        step(KEY_ESC);
        CHECK(changes == 3);                       // back to w1
        CHECK(s.width == w1);
        CHECK(!g.displayConfirmActive());
        CHECK(g.stableDisplayConfig().width == w1);

        // 20 idle frames do not produce another dialog or a hidden change.
        for (int i = 0; i < 20; ++i) step(0);
        CHECK(changes == 3 && dialogs == 2);
        CHECK(!g.displayConfirmActive());
        g.shutdown();
    }

    Settings::setPathForTests(nullptr);
    if (!hadSettings) std::remove(kCfg);
}

// ---------------------------------------------------------------------------
// Window centering math (the backends call this after every resize; they cannot
// be exercised in CI, so the shared helper is covered here).
static void testWindowCentering() {
    WorkArea work;
    work.x = 0; work.y = 0; work.width = 1920; work.height = 1040;   // taskbar
    int x = 0, y = 0;
    centerWindowIn(work, 1280, 720, x, y);
    CHECK(x == 320 && y == 160);                       // exactly centered
    centerWindowIn(work, 1600, 900, x, y);
    CHECK(x == 160 && y == 70);
    // Odd remainders round down, never off-screen.
    centerWindowIn(work, 1281, 721, x, y);
    CHECK(x == 319 && y == 159);
    // A window as big as the work area (or bigger) is pinned inside it.
    centerWindowIn(work, 1920, 1040, x, y);
    CHECK(x == 0 && y == 0);
    centerWindowIn(work, 2000, 1200, x, y);
    CHECK(x == 0 && y == 0 && x + 2000 >= work.right() && y + 1200 >= work.bottom());
    // A secondary monitor offset by its origin.
    WorkArea second;
    second.x = 1920; second.y = -200; second.width = 1600; second.height = 900;
    centerWindowIn(second, 1280, 720, x, y);
    CHECK(x == 1920 + 160 && y == -200 + 90);
    // Degenerate input never produces a negative/garbage origin.
    centerWindowIn(work, 0, 0, x, y);
    CHECK(x == 0 && y == 0);
    centerWindowIn(WorkArea{}, 1280, 720, x, y);
    CHECK(x == 0 && y == 0);
    // The window must always end up fully inside the usable area.
    for (int w = 640; w <= 2600; w += 137) {
        for (int h = 480; h <= 1600; h += 91) {
            centerWindowIn(work, w, h, x, y);
            CHECK(x >= work.x && y >= work.y);
            if (w <= work.width) CHECK(x + w <= work.right());
            if (h <= work.height) CHECK(y + h <= work.bottom());
        }
    }
}

// ---------------------------------------------------------------------------
static void testDisplayModeApply() {
    // End-to-end display handling through Game with a stub backend: provisional
    // apply, confirmation, automatic revert and the "last confirmed" state.
    StubPlatform plat;   // not owned by Game (only DisplayConfirm/menu need it)
    plat.setMonitor(1920, 1040);
    plat.setDesktop(1920, 1080);
    plat.setUiScale(1.0f);
    plat.addDriverMode(1920, 1080, 60);
    plat.addDriverMode(1920, 1080, 144);
    plat.addDriverMode(1280, 720, 60);
    plat.init("t", 1280, 720);

    Settings s;
    // displayConfigOf / setDisplayConfig are pure conversions of the settings.
    s.mode = DisplayMode::Borderless;
    s.renderWidth = 1920; s.renderHeight = 1080;
    s.width = 1600; s.height = 900;
    s.modeWidth = 1920; s.modeHeight = 1080; s.modeRefresh = 144;
    DisplayConfig cfg = Game::displayConfigOf(s);
    CHECK(cfg.mode == DisplayMode::Borderless);
    CHECK(cfg.renderWidth == 1920 && cfg.renderHeight == 1080);
    CHECK(cfg.width == 1600 && cfg.height == 900);
    CHECK(cfg.modeWidth == 1920 && cfg.modeHeight == 1080 && cfg.modeRefresh == 144);
    Settings back;
    Game::setDisplayConfig(back, cfg);
    CHECK(back.mode == DisplayMode::Borderless);
    CHECK(back.renderWidth == 1920 && back.renderHeight == 1080);
    CHECK(back.modeWidth == 1920 && back.modeRefresh == 144);
    CHECK(Game::displayConfigOf(back) == cfg);

    // A refused exclusive mode keeps the requested values in the pending config
    // (Game restores the stable one) and never touches the display.
    s.mode = DisplayMode::Exclusive;
    s.modeWidth = 1024; s.modeHeight = 768; s.modeRefresh = 60;
    plat.setExclusiveSupported(false);
    CHECK(!plat.applyDisplayMode(s.mode, s.modeWidth, s.modeHeight, s.modeRefresh));
    plat.setExclusiveSupported(true);
    CHECK(plat.applyDisplayMode(DisplayMode::Exclusive, 1920, 1080, 144));
    CHECK(plat.mode() == DisplayMode::Exclusive);
    CHECK(plat.lastW() == 1920 && plat.lastH() == 1080 && plat.lastHz() == 144);
    // A mode outside the driver pool is refused too.
    CHECK(!plat.applyDisplayMode(DisplayMode::Exclusive, 1024, 768, 60));

    // Headless: neutral (every configuration is accepted so CI/tests never fail
    // on a saved display mode).
    Platform* headless = createHeadlessPlatform();
    CHECK(headless->init("t", 1280, 720));
    CHECK(headless->applyDisplayMode(DisplayMode::Exclusive, 3840, 2160, 60));
    CHECK(headless->currentDisplayMode() == DisplayMode::Exclusive);
    CHECK(headless->displayModeCount() == 0);
    int mw = 0, mh = 0;
    CHECK(!headless->monitorSize(mw, mh));
    // Headless mode lists fall back to the presets so the menu still works.
    Resolution modes[Settings::kMaxModes];
    int n = Settings::renderModes(modes, Settings::kMaxModes, mw, mh);
    CHECK(n == Settings::kPresetCount);
    headless->shutdown();
    delete headless;
}

// ---------------------------------------------------------------------------
static void testGameDisplayFlow() {
    // The full flow against the headless backend (which accepts every mode):
    // a saved configuration is restored, an edit is applied *provisionally*,
    // the confirmation dialog decides, and Escape/timeout restore the previous
    // stable configuration.
    const char* kCfg = "settings_display_test.cfg";
    FILE* pre = std::fopen(kCfg, "rb");
    bool hadSettings = (pre != nullptr);
    if (pre) std::fclose(pre);
    Settings::setPathForTests(kCfg);

    auto input = [](int w, int h) {
        FrameInput in;
        std::memset(in.keys, 0, sizeof(in.keys));
        in.width = w; in.height = h;
        in.mouseX = -1.0f; in.mouseY = -1.0f;
        return in;
    };

    {
        Settings saved;
        saved.mode = DisplayMode::Borderless;
        saved.renderWidth = 1920; saved.renderHeight = 1080;
        saved.save();
        Game g;
        CHECK(g.init("t", 1280, 720, true));   // headless
        CHECK(g.settings().mode == DisplayMode::Borderless);
        CHECK(g.settings().renderWidth == 1920 && g.settings().renderHeight == 1080);
        CHECK(!g.displayConfirmActive());
        // Render resolution follows the borderless configuration; the window
        // (and therefore the UI size) is untouched.
        int rw = 0, rh = 0;
        g.renderSizeFor(1920, 1080, rw, rh);
        CHECK(rw == 1920 && rh == 1080);
        g.renderSizeFor(2560, 1440, rw, rh);
        CHECK(rw == 1920 && rh == 1080);    // render scale, not window size
        // The render resolution adopts the window's aspect ratio (same pixel
        // budget), so a 16:9 preset on a 16:10 or 4:3 monitor does not stretch.
        g.renderSizeFor(1600, 900, rw, rh);
        CHECK(rw == 1920 && rh == 1080);          // 16:9 preset on a 16:9 window
        g.renderSizeFor(1920, 1200, rw, rh);      // 16:10 window
        CHECK_NEAR(double(rw) / double(rh), 1.6, 0.01);
        CHECK(rw * rh > 1920 * 1080 * 0.95 && rw * rh < 1920 * 1080 * 1.05);
        g.renderSizeFor(1600, 1200, rw, rh);      // 4:3 window
        CHECK_NEAR(double(rw) / double(rh), 4.0 / 3.0, 0.01);
        CHECK(rw * rh > 1920 * 1080 * 0.95 && rw * rh < 1920 * 1080 * 1.05);
        // Windowed mode renders at the window size.
        g.settings().mode = DisplayMode::Windowed;
        g.renderSizeFor(1600, 900, rw, rh);
        CHECK(rw == 1600 && rh == 900);

        // An edit starts the countdown, and the stable state is still the
        // restored (confirmed) one until the user accepts the change.
        g.settings().mode = DisplayMode::Borderless;
        g.settings().renderWidth = 1280; g.settings().renderHeight = 720;
        CHECK(g.requestDisplayApply());
        CHECK(g.displayConfirmActive());
        CHECK(g.stableDisplayConfig().mode == DisplayMode::Borderless);
        CHECK(g.stableDisplayConfig().renderWidth == 1920);   // the saved one
        // Enter confirms: the configuration becomes stable and is persisted.
        FrameInput in = input(1280, 720);
        in.keys[KEY_ENTER] = 1;
        CHECK(g.pollDisplayConfirm(in, 0.1f));          // active this frame
        CHECK(!g.pollDisplayConfirm(in, 0.1f));         // dialog closed
        CHECK(!g.displayConfirmActive());
        CHECK(g.stableDisplayConfig().mode == DisplayMode::Borderless);
        CHECK(g.stableDisplayConfig().renderWidth == 1280);
        g.shutdown();
        Settings reread;
        CHECK(reread.load());
        CHECK(reread.mode == DisplayMode::Borderless);
        CHECK(reread.renderWidth == 1280 && reread.renderHeight == 720);

        // A second change is reverted with Escape: the confirmed settings (and
        // the settings file) go back to the previous stable state.
        Game g2;
        CHECK(g2.init("t", 1280, 720, true));
        CHECK(g2.settings().mode == DisplayMode::Borderless);
        g2.settings().renderWidth = 2560; g2.settings().renderHeight = 1440;
        CHECK(g2.requestDisplayApply());
        CHECK(g2.displayConfirmActive());
        FrameInput esc = input(1280, 720);
        esc.keys[KEY_ESC] = 1;
        CHECK(g2.pollDisplayConfirm(esc, 0.1f));
        CHECK(!g2.displayConfirmActive());
        CHECK(g2.settings().renderWidth == 1280 && g2.settings().renderHeight == 720);
        CHECK(g2.stableDisplayConfig().renderWidth == 1280);
        // and the timeout reverts too (the countdown is 15 s by default).
        g2.settings().mode = DisplayMode::Windowed;
        g2.settings().width = 1600; g2.settings().height = 900;
        CHECK(g2.requestDisplayApply());
        CHECK(g2.displayConfirmActive());
        for (int i = 0; i < 20 && g2.displayConfirmActive(); ++i)
            g2.pollDisplayConfirm(input(1280, 720), 1.0f);
        CHECK(!g2.displayConfirmActive());
        CHECK(g2.settings().mode == DisplayMode::Borderless);   // reverted
        // A change to the *same* configuration needs no confirmation.
        const DisplayConfig before = g2.stableDisplayConfig();
        CHECK(g2.requestDisplayApply());
        CHECK(!g2.displayConfirmActive());
        CHECK(g2.stableDisplayConfig() == before);
        g2.shutdown();

        // A saved exclusive mode is re-applied on start-up and, being
        // provisional, asks for confirmation (the user must not be dropped into
        // an unconfirmed video mode).
        Settings ex;
        ex.mode = DisplayMode::Exclusive;
        ex.modeWidth = 1920; ex.modeHeight = 1080; ex.modeRefresh = 60;
        ex.save();
        // (Only a real display switch is confirmed at start-up; the headless
        // backend has nothing to accept, so the rule is unit tested directly.)
        CHECK(Game::needsStartupConfirm(DisplayMode::Exclusive, false));
        CHECK(!Game::needsStartupConfirm(DisplayMode::Exclusive, true));
        CHECK(!Game::needsStartupConfirm(DisplayMode::Borderless, false));
        CHECK(!Game::needsStartupConfirm(DisplayMode::Windowed, false));
        Game g3;
        CHECK(g3.init("t", 1280, 720, true));
        CHECK(g3.settings().mode == DisplayMode::Exclusive);
        CHECK(!g3.displayConfirmActive());                 // headless: no switch
        CHECK(g3.stableDisplayConfig().mode == DisplayMode::Exclusive);
        g3.shutdown();
        Settings after;
        CHECK(after.load());
        CHECK(after.mode == DisplayMode::Exclusive);       // still the saved mode
    }

    Settings::setPathForTests(nullptr);
    if (!hadSettings) std::remove(kCfg);
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
    testSettingsModeNames();
    testRenderAspectFit();
    testDisplayModeLists();
    testWindowCentering();
    testDisplayConfirmOnce();
    testDisplayConfirm();
    testSensitivity();
    testMenuNav();
    testUiScale();
    testWindowSizing();
    testDisplayModeApply();
    testGameDisplayFlow();
    testRestart();

    fprintf(stderr, "\n[aw-tests] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
