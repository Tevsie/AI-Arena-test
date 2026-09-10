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
> Run anyway**. Press `Esc` to quit.

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
| **Left mouse**   | **Pull** target brick outward (ledge)   |
| **Right mouse**  | **Push** target brick back flush        |
| `Esc`            | Quit                                    |

The center-screen crosshair turns **green** when a brick is in reach. You
cannot push a brick you are currently standing on.

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
geometry in the shader. Chunk streaming keeps only a 9-row band of chunks
resident around the player — the rest are unloaded instantly.

**Custom physics.** A sub-stepped kinematic character controller resolves
collisions against grid-aligned brick AABBs with axis-by-axis push-out — no
external engine. Candidate bricks are a tiny grid window around the player, so
collision cost is effectively constant per frame.

**Numbers (headless demo, 60 Hz simulated step):** 54 resident chunks =
13,824 bricks, drawn as ≤ 54 instanced draw calls (one per visible chunk) on a
single vertex buffer; the wall itself is one mesh.

## Project layout

```
src/
  core/          math.hpp (vec/mat/frustum/ray/hash), platform abstraction,
                 X11+GLX window backend, Win32+WGL window backend,
                 headless backend, input constants
  render/        gl.h/gl.cpp (zero-dependency GL 3.3 loader),
                 renderer.hpp/.cpp (instanced bricks, sky, crosshair)
  game/          constants, grid/chunk/wall (streaming + brick store),
                 player (kinematic controller), interaction (raycast pull/push),
                 game.cpp (loop + scripted demo driver)
tests/           dependency-free unit tests (grid, store, streaming,
                 persistence, controller, interaction)
.github/workflows/  CI: builds + tests on Linux, cross-compiles atw.exe,
                    attaches it to releases / artifacts
```

## Technical notes

- **Zero runtime allocations**: every container is a fixed pool (see
  `src/game/wall.hpp`).
- **Infinite vertical wall**: brick/chunk coordinates are unbounded in Y;
  procedural appearance comes from a deterministic spatial hash, so a chunk can
  be regenerated losslessly at any time. Player modifications persist across
  unload/reload via the persistent brick store.
- **Deterministic demos**: `make demo` (or `./build/atw --headless --frames N`)
  steps at a fixed 60 Hz and prints `fps / frame ms / chunks / instances /
  modified bricks / peak height` every second.
- The renderer intentionally trades visual fidelity (flat-shaded bricks, no
  textures, one directional light, baked AO) for raw throughput, per the GDD.
