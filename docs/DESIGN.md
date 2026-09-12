# Design notes — Against the Wall prototype

This document records the architectural decisions behind the prototype and how
each GDD mandate is satisfied.

## Coordinate model

- The wall face lies in the **XY plane at z = 0**; gravity is **−Y**.
- A brick is a rigid box embedded in the wall. Each chunk is covered by an
  exact mosaic tiling of non-overlapping 1×1..4×4-cell bricks (8 tiling
  patterns picked per 4×4 macro-cell from a spatial hash), so spawned bricks
  never overlap and never leave gaps. A brick is identified by its origin
  (minimum-corner cell); any grid cell resolves to its containing brick.
  **Flush** a brick occupies `z ∈ [−e, 0]` (material behind the wall face,
  `e` = larger footprint edge). **Pulled** by depth `d` it slides outward to
  `z ∈ [d−e, d]`.
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
   grid, extended −4/+1 cells (mosaic bricks are up to 4 wide, so a brick
   overlapping the player may originate up to 4 cells away in −X/−Y). Each
   candidate cell resolves to its containing brick; for each, an
   axis-separated AABB overlap test resolves the minimum-penetration axis;
   ground contact is inferred when the player is pushed **up**. A small
   contact epsilon makes resting contact detectable.

This is O(candidate cells) per sub-step — constant and cache-friendly.

## Rendering

- **One cube mesh**, drawn as instances scaled per brick (1×1..4×4 m
  footprints). Each resident chunk owns a fixed range in the instance VBO
  keyed by its pool slot (at most 256 instances: one per mosaic brick).
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

- `Esc` pauses the game and opens the settings overlay (volume bar, render
  resolution picker, sensitivity bar, field-of-view bar, fullscreen toggle,
  Restart / Resume / Quit). The
  menu is driven by
  mouse (hover + click + slider drag; backends report absolute cursor position)
  and keyboard (arrows + Enter), drawn with the renderer's immediate-mode UI
  pass (colored rects + an embedded 5x7 bitmap font atlas — no font files).
- Settings apply live (mixer gain, `Renderer::setRenderSize`, `Settings::fov`
  for the projection and the sky tangent, look scale,
  `Platform::setFullscreen`) and persist to `settings.cfg`. Restart clears
  the brick store, cancels lerps, reseeds the starting platform and respawns
  the player. Fullscreen is EWMH (`_NET_WM_STATE`) on X11, borderless
  monitor-cover on Win32, and a no-op headless; the saved choice is applied
  at startup.

## Resolution vs. window size

The scene never renders "at the window size". It is drawn into an offscreen
target (RGBA8 color + DEPTH_COMPONENT24 renderbuffer) at the configured
resolution and blitted into the window with `glBlitFramebuffer`, scaled to the
largest centered rect that fits while preserving the target's aspect (bars are
cleared first). Consequences:

- **Windowed mode has one consistent size.** `Settings::windowW/H` (fitted to
  the display via `Platform::screenSize` + `Settings::fitToScreen` at startup,
  remembered afterwards) is the only thing that resizes the window. The
  resolution setting never touches the window — in fullscreen either, where it
  used to resize a borderless window with the wrong decoration math.
- A resolution larger than the display is safe: it supersamples instead of
  producing a window that does not fit.
- The projection aspect comes from the render target
  (`Renderer::renderSize`), not the window, so a letterboxed view is never
  stretched.

**Cursor/UI alignment.** The overlay is laid out and drawn in *window* pixels
(`Renderer::uiBegin(width, height)` with the platform's real client size), the
same coordinate space the backends report the cursor in. Previously the menu
sized itself from the *requested* resolution while the drawable could be
something else (OS/WM clamps the window, `AdjustWindowRect` with the wrong
style, a size stored before the resize was applied), so hit boxes drifted away
from the drawn cursor. Both backends now also read the real client area back
after a resize/fullscreen switch (X11 `XGetWindowAttributes`, Win32
`GetClientRect`) and query the actual pointer position instead of assuming the
window centre, and `FrameInput` is filled *after* the event pump so a resize
event processed this frame cannot desync the size from the cursor.
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
  settings (clamp/parse/modes/FOV/window size), menu keyboard navigation, and
  restart. The render path is covered without a GPU by stubbing the `gl`
  function-pointer table with recorders and asserting the scene viewport is the
  render resolution, the blit rect is the letterboxed fit, and the UI ends up
  in window space.
