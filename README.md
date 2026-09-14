# Against the Wall — C++20 Performance Prototype

A high-performance, first-person vertical-climbing prototype inspired by
*Against the Wall*. You stand on a sheer, procedurally generated brick wall and
climb it by **pulling bricks out** to make ledges and **pushing them back** in.
The engine is written in pure C++20 with **zero third-party dependencies** — no
physics engine, no windowing toolkit, no GL loader library. The only link-time
dependencies are `libc`, `libm` and `libdl` on Linux (X11/GLX/OpenGL are
resolved at runtime through `dlopen`).

---

## ▶️ Get the game (Windows)

A ready-to-run `atw.exe` is built automatically by GitHub Actions every time
code is pushed.

**Option A — GitHub Releases (easiest)**

1. Open the repo's **Releases** page (right sidebar → *Releases*).
2. Download **`atw.exe`** from the latest release.
3. Double-click `atw.exe` and play.

**Option B — Actions artifacts (if there's no release yet)**

1. Open the repo on GitHub and click the **Actions** tab.
2. Click the most recent **build** run (a green ✓).
3. Scroll down to **Artifacts** and download **`against-the-wall`**.
4. Unzip it and double-click **`atw.exe`**.

> `atw.exe` is fully self-contained (x64, no install). It needs a standard
> Windows 10/11 GPU driver with **OpenGL 3.3** support (any Intel / NVIDIA /
> AMD driver). If SmartScreen warns on first run, click **More info →
> Run anyway**. Press `Esc` for the settings menu (volume, display mode, resolution,
> sensitivity, restart, quit).

### Build the .exe yourself on Windows (optional)

With [MSYS2](https://www.msys2.org/) (MINGW64 shell):

```bash
pacman -S mingw-w64-x86_64-gcc make
make windows        # produces ./build/atw.exe
```

### Windows via WSL2 (alternative)

```bash
sudo apt update && sudo apt install -y g++ make
make run            # needs an X server (e.g. VcXsrv) to show the window
```

---

## Linux / macOS-ish dev build

```bash
make            # builds ./build/atw and ./build/atw_tests
make test       # run the unit tests
make run        # launch the game (X11 + OpenGL 3.3)
make demo       # headless benchmark/demo (no window required)
```

or with CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Requirements

- **C++20 compiler** (GCC 12+ / Clang 16+ recommended).
- Interactive windowed mode needs **X11 + GLX + OpenGL 3.3** (Linux) or a
  standard Windows GPU driver (Windows). The engine loads these at runtime, so
  no X11/GL *development* headers are needed to build.
- **Headless mode needs nothing** — it runs the full simulation with a scripted
  camera and prints frame stats, so it works in CI or a container.

If you run the game on a machine without a display/GPU it automatically falls
back to headless mode and prints a status line.

## Controls

| Input            | Action                                  |
|------------------|-----------------------------------------|
| Mouse            | Look around                             |
| `W A S D`        | Move                                    |
| `Space`          | Jump                                    |
| **Left mouse**   | **Pull** target brick outward (click, 3 s lerp) |
| **Right mouse**  | **Push** target brick back flush (click, 3 s lerp) |
| `Esc`            | Settings menu (pause)                   |

The center-screen crosshair turns **green** when a brick is in reach. You
cannot push a brick you are currently standing on.

## Settings (`Esc`)

The in-game menu (mouse or `↑ ↓ ← →` + `Enter`) offers:

| Setting              | What it does                                              |
|----------------------|-----------------------------------------------------------|
| Master volume bar    | 0–100 % loudness for all procedural sound effects         |
| Display mode         | `WINDOWED` / `BORDERLESS` / `EXCLUSIVE` fullscreen        |
| Resolution           | Depends on the mode — see *Display modes* below           |
| Refresh rate         | Exclusive fullscreen only: the rates your driver reports  |
| Field of view        | Vertical FOV, 55–110° (default 75°)                       |
| Mouse sensitivity    | Look speed multiplier, 10–300 % (default 100 %)            |
| **Restart** button   | Clears all brick edits and respawns you on a fresh wall   |
| Resume / Quit        | Close the menu / exit the game                            |

### Display modes

| Mode           | Window                                | Resolution row                                    |
|----------------|---------------------------------------|---------------------------------------------------|
| `WINDOWED`     | Real window with borders, client area = the selected size, re-centred on the monitor **every time you change the resolution** | Window **client size** — always a standard size: 1280×720 (HD), 1600×900 (HD+), 1920×1080 (Full HD), 2560×1440 (QHD), 3840×2160 (4K), filtered to what your screen can show. A leftover non-standard value from an older version (e.g. 1003×986) is snapped to the largest standard size that fits |
| `BORDERLESS`   | Borderless window covering the whole monitor at its native resolution (no mode switch, instant `Alt+Tab`) | **Render resolution**: the 3D scene is drawn at this size and upscaled to the window; the UI, menu and crosshair always render at native window resolution, so text stays crisp. The chosen preset keeps its pixel budget but takes the monitor's shape, so upscaling never stretches the image |
| `EXCLUSIVE`    | Driver mode switch (`ChangeDisplaySettingsEx` / XRandR): real hardware resolution + refresh rate | Strictly the modes your driver reports for this display (lowest → highest) |

Every apply is **confirmed before it sticks**: a modal asks *"Keep these display
settings? Reverting in 15 s"* — `Enter`/`Space`/click **KEEP** saves it to
`settings.cfg`; `Esc`, the timeout, or **REVERT** instantly restore the last
confirmed configuration (a restored exclusive mode at startup is provisional
too), so a mode your monitor cannot show can never leave you with a black
screen. One action is one dialog: the confirmation appears once per change (the
key that dismisses it cannot immediately repeat the change underneath), and no
new change can start while the countdown is running.

Windowed mode always fits your monitor: the picker only offers standard sizes
that the work area (screen minus taskbar and window decorations) can really
show, a saved `settings.cfg` size is fitted down at startup, and the window is
re-fitted — and re-centred — if you move it to another display, change the
desktop resolution, or pick another size (a maximized window is restored first,
because a maximized window cannot be sized or moved). On Windows the process declares
per-monitor DPI awareness *before* the window is created, so the client area,
the GL viewport and the mouse coordinates are all in real screen pixels —
otherwise a scaled (125 %/150 %) display makes windows physically larger than
the screen and makes clicks land away from the cursor. The settings menu uses
that same DPI to scale itself (1× / 1.5× / 2×, shrinking back if it would not
fit the window) so it keeps its apparent size on high-DPI displays.

Settings apply instantly and persist to `settings.cfg` next to the executable.
Sound effects (brick pull/push, jump, land, UI clicks) are synthesized live —
no audio files needed. Output uses WinMM on Windows and PulseAudio (with an
ALSA fallback) on Linux; with no audio device the game simply runs silent.

## How it works (performance architecture)

The whole design is built around the mandates in the GDD:

**Data-oriented, allocation-free core.** Bricks live in contiguous SoA arrays
inside pre-allocated chunk slots (`Chunk`). The persistent store of
player-modified bricks, the chunk resident map, and the instance staging buffer
are all fixed-size pools allocated once at startup. **Nothing is allocated
during a frame** — no `new`, no `malloc`, no growth, no rehashing in any hot
loop.

**GPU instancing.** One unit-cube mesh is drawn as thousands of instances. The
wall is split into 16×16-brick chunks; every chunk owns a fixed 256-instance
range in a single pre-allocated instance buffer, uploaded incrementally with
`glBufferSubData` only when a chunk is marked dirty (a brick was pulled/pushed,
or the chunk streamed in). Only the transformation matrices of modified bricks
are ever re-uploaded.

**Culling & LOD.** Each frame the CPU extracts a frustum from the view-projection
matrix and drops entire chunks that are off-screen (behind the camera, above/
below the frustum, or beyond the far plane) before issuing any draw call.
Distance-based LOD fades far bricks to a flat shade and fog-recedes distant
geometry in the shader. Chunk streaming keeps only a 9×9 window of chunks
resident around the player — the rest are unloaded instantly.

**Custom physics.** A sub-stepped kinematic character controller resolves
collisions against grid-aligned brick AABBs with axis-by-axis push-out — no
external engine. Candidate bricks are a tiny grid window around the player, so
collision cost is effectively constant per frame.

**Numbers (headless demo, 60 Hz simulated step):** 81 resident chunks =
20,736 bricks, drawn as ≤ 81 instanced draw calls (one per visible chunk) on a
single vertex buffer; the wall itself is one mesh.

## Project layout

```
src/
  core/          math.hpp (vec/mat/frustum/ray/hash), platform abstraction,
                 X11+GLX window backend, Win32+WGL window backend,
                 headless backend, input constants
  render/        gl.h/gl.cpp (zero-dependency GL 3.3 loader),
                 renderer.hpp/.cpp (instanced bricks, sky, crosshair, UI overlay),
                 font.hpp (embedded 5x7 menu font)
  game/          constants, grid/chunk/wall (streaming + brick store),
                 player (kinematic controller), interaction (raycast pull/push),
                 settings (display modes + render scaling, FOV, sensitivity,
                 persistence), display_confirm (15 s keep-or-revert modal),
                 menu (pause/settings overlay), game.cpp (loop + demo driver)
  audio/         procedural SFX mixer + synth, WinMM / PulseAudio / ALSA backends
tests/           dependency-free unit tests (grid, store, streaming,
                 persistence, controller, interaction)
.github/workflows/  CI: builds + tests on Linux, cross-compiles atw.exe,
                    attaches it to releases / artifacts
```

## Technical notes

- **Zero runtime allocations**: every container is a fixed pool (see
  `src/game/wall.hpp`).
- **Infinite wall in all directions**: brick/chunk coordinates are unbounded
  in X and Y; procedural appearance (including the non-overlapping brick
  mosaic: 1×1..4×4 m bricks from 8 deterministic tiling patterns) comes from
  a deterministic spatial hash, so a chunk can be regenerated losslessly at
  any time. Player
  modifications persist across unload/reload via the persistent brick store.
- **No fall respawn**: falling means falling forever (streaming follows you).
- **Deterministic demos**: `make demo` (or `./build/atw --headless --frames N`)
  steps at a fixed 60 Hz and prints `fps / frame ms / chunks / instances /
  modified bricks / peak height` every second.
- The renderer intentionally trades visual fidelity (flat-shaded bricks, no
  textures, one directional light, baked AO) for raw throughput, per the GDD.
