# Design notes — Against the Wall prototype

This document records the architectural decisions behind the prototype and how
each GDD mandate is satisfied.

## Coordinate model

- The wall face lies in the **XY plane at z = 0**; gravity is **−Y**.
- A brick is a rigid cube embedded in the wall, with a deterministic random
  edge length of **1 m (small), 2.5 m (medium) or 5 m (large)** from a spatial
  hash. Bricks are corner-anchored: brick `(bx, by)` with size `s` spans
  `[bx, bx+s] × [by, by+s]`. **Flush** it occupies `z ∈ [−s, 0]` (material
  behind the wall face). **Pulled** by depth `d` it slides outward to
  `z ∈ [d−s, d]`.
- Bricks are indexed by integer grid cells `(x, y)`, unbounded in both axes
  (infinite wall in all directions). A chunk is a `16 × 16` block of bricks;
  chunks are indexed `(cx, cy)` and generated procedurally.

This model makes the wall surface a clean height-field for collision: the only
free surface a player can stand on is a brick's **top face** (`y = by + s`).

## Data layout (DOD, zero runtime allocation)

Every container is a fixed-size pool allocated once at construction:

| Structure            | Layout                                                   | Size (defaults)        |
|----------------------|----------------------------------------------------------|------------------------|
| `Chunk`              | SoA `shade[256]` + `activeLocal[256]` + scalar flags      | ~536 B × 81 slots      |
| `Wall::pool_`        | pre-allocated chunk slots                                 | 81 chunks              |
| `Wall::map_`         | open-addressing resident map (tombstone deletion)         | 256 entries            |
| `BrickStore`         | persistent modified-brick hash store (linear probing)     | 16 384 entries         |
| `Renderer::staging_` | per-chunk instance staging buffer                         | 256 instances          |
| instance VBO         | GPU buffer for the whole resident wall                    | 20 736 instances × 80 B|
| `Interaction` lerps  | fixed pool of in-flight 3 s brick in/out lerps            | 256 entries            |

Hot loops (`Player::update`, `Renderer::render`) only read these pools and never
allocate. Brick modification (`pull`/`push`) is a rare discrete event that does
a constant number of hash-table probes.

## Streaming

`Wall::streamAround(playerCx, playerCy)` is called only when the player's chunk
changes. It acquires every chunk within `ACTIVE_CHUNK_RANGE` in both axes (a
9×9 window) and releases the rest. Acquire populates a free pool slot and
regenerates the chunk from its
deterministic spatial hash; the persistent `BrickStore` then re-applies the
player's previous modifications (with a per-chunk `activeLocal` bitmap + count
recomputed on acquire), so ledges survive unload/reload.

Because the resident window is a fixed 9×9 square, the resident map is small
and the evict-on-demand path (`evictFurthest`) is effectively never hit. There
is no fall respawn: a falling player keeps falling and streaming follows them.

## Collision

`Player` is a box (`PLAYER_HALF_W` × `PLAYER_HEIGHT`). Each frame:

1. Semi-implicit Euler integration with **sub-stepping** (max 0.25 m per step) so
   thin ledges can't be tunneled through at high fall speeds.
2. Per sub-step, the candidate brick window is the player's AABB rounded to the
   grid, extended −5/+1 cells (large corner-anchored bricks may originate up to
   5 cells away in −X/−Y). For each candidate, an axis-separated AABB overlap test resolves
   the minimum-penetration axis; ground contact is inferred when the player is
   pushed **up**. A small contact epsilon makes resting contact detectable.

This is O(candidate cells) per sub-step — constant and cache-friendly.

## Rendering

- **One cube mesh**, drawn as instances scaled per brick (1 / 2.5 / 5 m). Each
  resident chunk owns a fixed range in the instance VBO keyed by its pool slot.
- `glBufferSubData` uploads only **dirty** chunk ranges (brick modified, or chunk
  streamed in). Instanced attributes (4×vec4 model matrix + 1×float shade) use
  `glVertexAttribDivisor`.
- **Frustum culling** is done on the CPU per chunk (extracted from the
  view-projection matrix), so off-screen chunks never issue a draw call. A
  distance-based **LOD** in the vertex shader flattens far bricks and fog
  recedes them; the horizon is a cheap fullscreen-triangle gradient sky.
- Per-chunk draw calls use re-pointed instanced attributes to supply each
  chunk's base instance (the GL 3.3 method, avoiding GL 4.2 `BaseInstance`).

## Platform layer

X11 + GLX + OpenGL are accessed through `dlopen` at runtime; only `libc`,
`libm` and `libdl` are link-time dependencies. A headless backend runs the same
simulation with a fixed 60 Hz step and a scripted climb, which keeps the engine
fully testable and benchmarkable on machines without a display or GPU.

## Settings, menu & audio

- `Esc` pauses the game and opens the settings overlay (volume bar, resolution
  picker, sensitivity bar, fullscreen toggle, Restart / Resume / Quit). The
  menu is driven by
  mouse (hover + click + slider drag; backends report absolute cursor position)
  and keyboard (arrows + Enter), drawn with the renderer's immediate-mode UI
  pass (colored rects + an embedded 5x7 bitmap font atlas — no font files).
- Settings apply live (mixer gain, `Platform::resize`, look scale,
  `Platform::setFullscreen`) and persist to `settings.cfg`. Restart clears
  the brick store, cancels lerps, reseeds the starting platform and respawns
  the player. Fullscreen is EWMH (`_NET_WM_STATE`) on X11, borderless
  monitor-cover on Win32, and a no-op headless; the saved choice is applied
  at startup.
- Audio is a tiny procedural engine: a mixer thread renders up to 8
  synthesized one-shots (48 kHz stereo int16) into a platform backend — WinMM
  on Windows, PulseAudio-simple with an ALSA fallback on Linux (both `dlopen`,
  so link-time deps are unchanged). `play()` is fire-and-forget; with no
  device (or headless) the engine runs silent.

## Determinism / testability

- All procedural generation is a pure function of coordinates (spatial hash).
- The headless platform and the scripted demo driver produce deterministic runs.
- Unit tests cover grid math, the brick store, streaming + persistence, the
  controller (landing/jumping/wall), pull/push including the occupancy rule,
  settings (clamp/parse/modes), menu keyboard navigation, and restart.
