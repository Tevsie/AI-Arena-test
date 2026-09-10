// main.cpp — entry point for the "Against the Wall" C++20 prototype.
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/platform.hpp"
#include "game/game.hpp"

namespace {

void printUsage() {
    fprintf(stderr,
            "usage: atw [--headless] [--frames N] [--width W] [--height H]\n"
            "  --headless   run without a window/GPU (benchmark/demo mode)\n"
            "  --frames N   exit after N frames (headless default 1200)\n");
}

#if defined(_WIN32)
// The shipped .exe is a GUI-subsystem app, so there is no console. Redirect
// stderr to "atw.log" next to the executable so all diagnostics (GL vendor,
// shader errors, frame stats) are captured for troubleshooting.
void initLogging() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    char* ext = std::strrchr(path, '.');
    char* sep1 = std::strrchr(path, '\\');
    char* sep2 = std::strrchr(path, '/');
    char* sep = sep1 > sep2 ? sep1 : sep2;
    if (ext && sep && ext > sep) *ext = '\0';
    std::strcat(path, ".log");
    FILE* f = freopen(path, "w", stderr);
    if (f) setvbuf(f, nullptr, _IONBF, 0);
    fprintf(stderr, "[aw] Against the Wall — startup log\n");
    fflush(stderr);
}
#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    initLogging();
#endif
    bool forceHeadless = false;
    int width = 1280, height = 720;
    int frames = 1200;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need = [&]() -> const char* {
            if (i + 1 >= argc) { printUsage(); std::exit(2); }
            return argv[++i];
        };
        if (std::strcmp(a, "--headless") == 0) forceHeadless = true;
        else if (std::strcmp(a, "--frames") == 0) frames = std::atoi(need());
        else if (std::strcmp(a, "--width") == 0) width = std::atoi(need());
        else if (std::strcmp(a, "--height") == 0) height = std::atoi(need());
        else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) { printUsage(); return 0; }
        else { printUsage(); return 2; }
    }

    {
        // Configure the headless frame cap through the environment so the
        // headless platform picks it up (used both for --headless runs and the
        // automatic fallback when no display/GPU is available).
        char env[32];
        std::snprintf(env, sizeof(env), "%d", frames);
#if defined(_WIN32)
        _putenv_s("AW_HEADLESS_FRAMES", env);
#else
        ::setenv("AW_HEADLESS_FRAMES", env, 1);
#endif
    }

    aw::Game game;
    if (!game.init("Against the Wall (C++20 prototype)", width, height, forceHeadless)) {
        fprintf(stderr, "[aw] failed to initialize\n");
#if defined(_WIN32)
        MessageBoxA(nullptr,
                    "Against the Wall failed to start.\n\n"
                    "Your system may not have an OpenGL 3.3 graphics driver.\n"
                    "Try updating your GPU driver (Intel / NVIDIA / AMD).",
                    "Against the Wall", MB_OK | MB_ICONERROR);
#endif
        return 1;
    }
    fprintf(stderr, "[aw] backend: %s\n", game.headless() ? "headless" : "windowed");

    game.run();

    const aw::FrameStats s = game.stats();
    double fps = s.elapsed > 0.0 ? double(s.frame) / s.elapsed : 0.0;
    fprintf(stderr, "[aw] done: frames=%d elapsed=%.3fs fps=%.0f  "
                    "residentChunks=%d instances=%d modifiedBricks=%d peakY=%d\n",
            s.frame, s.elapsed, fps, s.residentChunks, s.drawnInstances,
            s.modifiedBricks, s.playerBrickY);

    game.shutdown();
    return 0;
}

#if defined(_WIN32)
// GUI-subsystem entry point (MinGW, -mwindows). __argc/__argv are supplied by
// the MinGW C runtime; we simply forward into the standard main().
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(__argc, __argv);
}
#endif
