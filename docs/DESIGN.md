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

- `Esc` pauses the game and opens the settings overlay (volume bar, display
  mode, resolution picker, refresh rate, field-of-view bar, sensitivity bar,
  Restart / Resume / Quit). The menu is driven by
  mouse (hover + click + slider drag; backends report absolute cursor position)
  and keyboard (arrows + Enter), drawn with the renderer's immediate-mode UI
  pass (colored rects + an embedded 5x7 bitmap font atlas — no font files).
  The resolution row changes meaning with the display mode (`WINDOW SIZE` /
  `RENDER RES` / `DISPLAY RES`) and the refresh row is only live in exclusive
  fullscreen, so the picker can only ever offer combinations the backend really
  supports.
- Display management lives behind `Platform::applyDisplayMode(mode, w, h, hz)`
  with `DisplayMode` = `Windowed` / `Borderless` / `Exclusive` (plus
  `monitorSize`, `displayModeCount/At`, `currentDisplayMode`). The menu builds
  its lists from the backend (`Settings::windowModes`, `renderModes`,
  `exclusiveModes`, `refreshRates`) and only ever edits `Settings`; `Game`
  applies the result, so the UI has no platform-specific code.
  - **Windowed**: monitor aware (`Platform::maxWindowSize`), the picker offers
    only presets the display can show (up to 3840x2160) plus a `MAX` entry for
    the largest window that fits the work area, `Platform::resize` clamps every
    request, and the window is re-centred on the monitor, so windowed mode can
    never produce a window larger than the screen.
  - **Borderless**: the window is locked to the monitor's native resolution
    (`WS_POPUP` + monitor rect on Win32, `_NET_WM_STATE_FULLSCREEN` on X11) and
    the resolution row becomes a *render scale*: `Game::renderSizeFor` feeds the
    3D pass a smaller (or larger, for supersampling) render target which the
    renderer draws into an FBO and upscales with a full-screen blit, while the
    UI pass, menu, crosshair and fonts always run at the native window
    resolution — low-res 3D without blurry text. `Settings::fitRenderAspect`
    re-shapes the chosen preset to the window's aspect ratio at the same pixel
    budget (identity on 16:9, so a 16:10/4:3 monitor cannot stretch).
  - **Exclusive**: `EnumDisplaySettingsExA` / `XRRGetScreenResourcesCurrent`
    enumerate the driver's real modes (deduplicated, sorted by area then
    refresh), `ChangeDisplaySettingsExA(CDS_FULLSCREEN)` / `XRRSetCrtcConfig`
    switch resolution + refresh (with the previous desktop configuration
    remembered and restored on exit), and the picker shows exactly that pool. On Windows the process also declares per-monitor DPI awareness
  before creating the window (otherwise a scaled desktop virtualizes the client
  area: windows come out physically bigger than requested and mouse coordinates
  no longer match what is drawn) and follows `WM_DPICHANGED` /
  `WM_DISPLAYCHANGE` by re-fitting the window. The overlay itself is laid out
  in pixels scaled by `Platform::uiScale` (DPI quantized to 1.0/1.5/2.0 with a
  matching integer bitmap-font scale) so it keeps its apparent size on a
  high-DPI display, dropping back a step when the panel would not fit the
  window.
- Every display change is **provisional**: `Game::requestDisplayApply` applies
  it, then a modal `DisplayConfirm` overlay asks *"keep these display settings?
  reverting in N s"* (`Settings::kDisplayConfirmSeconds` = 15 s). Confirming
  writes `settings.cfg` and promotes the configuration to the new stable state;
  `Esc`, the timeout or **REVERT** call `Game::revertDisplayChange`, which
  restores the last confirmed configuration (falling back to a fitting window
  if even that is refused) — so an unsupported mode can never stick. A refused
  change is undone immediately instead of showing the dialog. A restored
  exclusive mode is provisional at startup too, with a window as the revert
  target until the user accepts it. The dialog swallows the key that opened it
  (a held `Enter` cannot auto-confirm) and freezes the menu below it.
- Settings apply live (mixer gain, display configuration, projection FOV and sky
  half-FOV tangents, look scale) and persist to `settings.cfg` (hand-editable:
  display modes are also accepted as `windowed` / `borderless` / `exclusive`
  names). Restart clears
  the brick store, cancels lerps, reseeds the starting platform and respawns
  the player. The saved display choice is applied at startup; headless is a
  no-op that accepts every mode so the same code paths stay testable.
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
  settings (clamp/parse/mode names/round-trip), the display-mode list builders
  (windowed fit + `MAX` de-duplication, render-mode insertion of the native
  size, exclusive de-duplication, refresh rates + `DEFAULT`), the confirmation
  dialog (countdown, `Enter`/`Esc`, held-key and click edge cases, button
  hit-testing), the apply/confirm/revert flow on the backend, monitor-aware
  window sizing, menu keyboard navigation and restart. Nine mutation tests on
  the display logic (list filtering, `DEFAULT` entry, stepper, countdown,
  `Esc`, startup-confirmation rule, revert and persist paths) are all caught by
  the suite.
