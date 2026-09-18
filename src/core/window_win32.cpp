// window_win32.cpp — Win32 + WGL + OpenGL window backend.
// Native Windows backend (compiled with MinGW): creates a window, a WGL 3.3
// core context, loads every GL entry point at runtime, and translates Win32
// input into the engine's FrameInput. Mirrors the X11 backend's behaviour so
// the game code is backend-agnostic.
//
// DPI handling: the process declares per-monitor DPI awareness before the first
// window is created. Without it Windows "virtualizes" the window — the app
// (and therefore the GL viewport) works in scaled-down coordinates while the
// window is physically larger — which is what made a 1920x1080 request bigger
// than a 1920x1080 screen and made clicks land away from the cursor. Being
// DPI aware means client pixels are real screen pixels: window sizes, the GL
// viewport and mouse coordinates all agree.
//
// Display modes (see Platform::applyDisplayMode):
//   Windowed   — WS_OVERLAPPEDWINDOW, client area clamped to the work area
//                (screen minus taskbar and decorations) and re-centered on the
//                monitor the window currently lives on.
//   Borderless — WS_POPUP covering the monitor rect 1:1 at its native
//                resolution; no display-mode switch.
//   Exclusive  — EnumerateDisplaySettings reports the modes the driver
//                supports; the requested one is applied with
//                ChangeDisplaySettingsEx (resolution + refresh rate) and the
//                window is put on top of it. The desktop mode is restored when
//                leaving exclusive fullscreen or on shutdown.
#include "platform.hpp"

#if defined(_WIN32)

// Windows 7 API level. Everything newer (DPI awareness contexts, per-monitor
// DPI queries, DPI-aware window metrics) is resolved dynamically below and
// simply skipped on older systems.
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "input.hpp"
#include "../render/gl.h"

namespace aw {
namespace {

using PFNWGLCREATECONTEXTATTRIBSARBPROC = HGLRC(WINAPI*)(HDC, HGLRC, const int*);

// WGL_ARB_create_context constants (not always present in older MinGW headers).
#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#endif
#ifndef WGL_CONTEXT_MINOR_VERSION_ARB
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#endif
#ifndef WGL_CONTEXT_PROFILE_MASK_ARB
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#endif
#ifndef WGL_CONTEXT_CORE_PROFILE_BIT_ARB
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif

// Window messages that older headers may not define (guarded by SDK version).
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_DISPLAYCHANGE
#define WM_DISPLAYCHANGE 0x007E
#endif

// Smallest window the settings allow (mirrors Settings::kMinWindow*).
constexpr int kMinWindowW = 320;
constexpr int kMinWindowH = 200;
// Driver display modes kept for the exclusive-fullscreen picker.
constexpr int kMaxDriverModes = 256;
constexpr int kMinModeW = 640, kMinModeH = 400;

// ---- Dynamic entry points (post-Windows-7 APIs) ----------------------------
using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
using SetProcessDpiAwarenessFn = long(WINAPI*)(int);   // HRESULT(shcore.dll)
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using GetDpiForSystemFn = UINT(WINAPI*)();
using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);

struct WinApi {
    SetProcessDpiAwarenessContextFn setDpiAwarenessContext = nullptr;
    SetProcessDpiAwarenessFn setProcessDpiAwareness = nullptr;
    GetDpiForWindowFn getDpiForWindow = nullptr;
    GetDpiForSystemFn getDpiForSystem = nullptr;
    AdjustWindowRectExForDpiFn adjustWindowRectForDpi = nullptr;
};

const WinApi& winApi() {
    static WinApi api = []() {
        WinApi a;
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (!user32) user32 = LoadLibraryA("user32.dll");
        if (user32) {
            a.setDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
            a.getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
                reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
            a.getDpiForSystem = reinterpret_cast<GetDpiForSystemFn>(
                reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForSystem")));
            a.adjustWindowRectForDpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
                reinterpret_cast<void*>(GetProcAddress(user32, "AdjustWindowRectExForDpi")));
        }
        HMODULE shcore = LoadLibraryA("shcore.dll");
        if (shcore) {
            a.setProcessDpiAwareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
                reinterpret_cast<void*>(GetProcAddress(shcore, "SetProcessDpiAwareness")));
        }
        return a;
    }();
    return api;
}

// Declare DPI awareness before any window exists (idempotent, harmless to call
// repeatedly: Windows rejects later calls once a window exists, we ignore that).
void enableDpiAwareness() {
    static bool done = false;
    if (done) return;
    done = true;
    const WinApi& api = winApi();
    // Per-monitor V2 (Win10 1703+): crisp rendering + WM_DPICHANGED on moves.
    if (api.setDpiAwarenessContext) {
        constexpr INT_PTR kPerMonitorAwareV2 = -4;
        if (api.setDpiAwarenessContext(reinterpret_cast<HANDLE>(kPerMonitorAwareV2))) return;
    }
    // Per-monitor awareness (Win8.1+).
    if (api.setProcessDpiAwareness &&
        api.setProcessDpiAwareness(2 /* PROCESS_PER_MONITOR_DPI_AWARE */) == 0 /* S_OK */)
        return;
    // System DPI awareness (Vista+): still removes the virtualization.
    if (SetProcessDPIAware()) return;
    fprintf(stderr, "[aw] DPI awareness unavailable — the OS may scale the window\n");
}

// Map a Win32 virtual-key code to the engine's keysym-style key code
// (see input.hpp). Letters become lowercase ASCII; special keys use the same
// 0x100+low-byte scheme as the X11 backend.
uint32_t vkToKey(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return uint32_t(vk) + 0x20;
    switch (vk) {
        case VK_SPACE:   return KEY_SPACE;   // 0x20
        case VK_ESCAPE:  return KEY_ESC;     // 0x11B
        case VK_SHIFT:   return KEY_SHIFT;   // 0x1E1
        case VK_CONTROL: return KEY_CTRL;    // 0x1E3
        case VK_TAB:     return KEY_TAB;
        case VK_RETURN:  return KEY_ENTER;
        case VK_LEFT:    return KEY_LEFT;
        case VK_RIGHT:   return KEY_RIGHT;
        case VK_UP:      return KEY_UP;
        case VK_DOWN:    return KEY_DOWN;
        default:         return uint32_t(vk) & 0xFF;
    }
}

}  // namespace

class PlatformWin32 final : public Platform {
public:
    bool init(const char* title, int w, int h) override {
        // Must happen before the first window is created; on a scaled display
        // this is what keeps window sizes and mouse coordinates in real pixels.
        enableDpiAwareness();

        // Never create a window that is larger than the monitor can show.
        int maxW = 0, maxH = 0;
        if (maxWindowSize(maxW, maxH)) {
            if (w > maxW) w = maxW;
            if (h > maxH) h = maxH;
        }
        width_ = w; height_ = h;
        mouseX_ = w / 2; mouseY_ = h / 2;
        hInstance_ = GetModuleHandleA(nullptr);
        hGL_ = LoadLibraryA("opengl32.dll");
        if (!hGL_) { fprintf(stderr, "[aw] opengl32.dll not found\n"); return false; }

        WNDCLASSA wc{};
        wc.lpfnWndProc = &PlatformWin32::wndProcThunk;
        wc.hInstance = hInstance_;
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.lpszClassName = "AgainstTheWallWindow";
        wc.style = CS_OWNDC;
        if (!RegisterClassA(&wc)) return false;

        // Outer size for the requested client area at the monitor's DPI, then
        // centre it on the work area so it is fully visible from the start.
        int fw = 0, fh = 0;
        // The requested client size is what the settings say (a standard
        // resolution); the window itself is fitted to the screen, so a 4K setting
        // on a 1080p display opens a maximally large window instead of one that
        // hangs off the desktop.
        reqW_ = w;
        reqH_ = h;
        int maxW = 0, maxH = 0;
        if (maxWindowSize(maxW, maxH)) {
            if (w > maxW) w = maxW;
            if (h > maxH) h = maxH;
        }
        frameSize(systemDpi(), fw, fh);
        int winW = w + fw, winH = h + fh;
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        centerOnPrimaryMonitor(winW, winH, x, y);
        hwnd_ = CreateWindowA("AgainstTheWallWindow", title, WS_OVERLAPPEDWINDOW,
                              x, y, winW, winH, nullptr, nullptr, hInstance_, this);
        if (!hwnd_) return false;

        hdc_ = GetDC(hwnd_);
        if (!setupPixelFormat()) { fprintf(stderr, "[aw] pixel format setup failed\n"); return false; }

        // Bootstrap a temporary legacy context to resolve wglCreateContextAttribsARB.
        HGLRC tmp = wglCreateContext(hdc_);
        if (!tmp) { fprintf(stderr, "[aw] wglCreateContext failed\n"); return false; }
        wglMakeCurrent(hdc_, tmp);

        auto wglCreateContextAttribsARB =
            reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
                wglGetProcAddress("wglCreateContextAttribsARB"));
        fprintf(stderr, "[aw] wglCreateContextAttribsARB: %s\n",
                wglCreateContextAttribsARB ? "available" : "MISSING");
        if (wglCreateContextAttribsARB) {
            const int attribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                0
            };
            hrc_ = wglCreateContextAttribsARB(hdc_, nullptr, attribs);
            fprintf(stderr, "[aw] GL 3.3 core context: %s\n", hrc_ ? "created" : "FAILED");
        } else {
            fprintf(stderr, "[aw] no ARB_create_context — cannot request GL 3.3\n");
        }
        if (hrc_) {
            wglMakeCurrent(hdc_, hrc_);
            wglDeleteContext(tmp);
        } else {
            hrc_ = tmp;  // legacy fallback; loadGL() will fail without 3.3
        }

        auto proc = [this](const char* name) -> void* {
            void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
            if (!p && hGL_) p = reinterpret_cast<void*>(GetProcAddress(hGL_, name));
            return p;
        };
        if (!loadGL(proc)) {
            fprintf(stderr, "[aw] OpenGL 3.3 core entry points unavailable\n");
            fprintf(stderr, "[aw] -> your GPU driver is too old (needs OpenGL 3.3)\n");
            return false;
        }

        // Apply whatever mode the game requested before the window existed.
        if (pendingMode_ != mode_) applyDisplayMode(pendingMode_, pendingW_, pendingH_, 0);

        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        // Adopt the size Windows actually gave the client area (DPI rounding).
        syncClientSize();
        refreshMonitorInfo();
        fprintf(stderr, "[aw] window: %dx%d client at %d,%d (dpi %d, desktop %dx%d)\n",
                width_, height_, x, y, windowDpi(), desktopW_, desktopH_);
        fprintf(stderr, "[aw] renderer: %s | %s | GLSL %s\n",
                gl.GetString ? (const char*)gl.GetString(GL_RENDERER) : "?",
                gl.GetString ? (const char*)gl.GetString(GL_VERSION) : "?",
                gl.GetString ? (const char*)gl.GetString(GL_SHADING_LANGUAGE_VERSION) : "?");
        return true;
    }

    bool frame(FrameInput& in) override {
        // Copy accumulated input, then reset the per-frame accumulators.
        std::memcpy(in.keys, keys_, sizeof(keys_));
        in.mouseDX = mouseDX_; in.mouseDY = mouseDY_;
        mouseDX_ = mouseDY_ = 0.0f;
        for (int i = 0; i < 8; ++i) {
            in.mousePressed[i] = mousePressed_[i];
            in.mouseReleased[i] = mouseReleased_[i];
            mousePressed_[i] = mouseReleased_[i] = false;
        }
        in.width = width_; in.height = height_;
        in.mouseX = float(mouseX_); in.mouseY = float(mouseY_);
        in.shouldQuit = shouldQuit_;

        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        // Note: Escape is a normal key now (the game toggles the settings menu
        // on it); only the window-close button sets shouldQuit.
        return !in.shouldQuit;
    }

    void swapBuffers() override { if (hdc_) SwapBuffers(hdc_); }

    void shutdown() override {
        setCursorCaptured(false);
        // Never leave the desktop in a mode we switched to (Windows would also
        // restore it on exit, but this keeps multi-monitor setups tidy).
        if (mode_ == DisplayMode::Exclusive) restoreDesktopMode();
        mode_ = DisplayMode::Windowed;
        if (hrc_) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(hrc_); hrc_ = nullptr; }
        if (hdc_ && hwnd_) ReleaseDC(hwnd_, hdc_);
        hdc_ = nullptr;
        if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
        if (hGL_) { FreeLibrary(hGL_); hGL_ = nullptr; }
    }

    BackendInfo info() const override {
        BackendInfo b;
        b.hasWindow = hwnd_ != nullptr;
        b.hasGL = gl.ready;
        b.name = "win32+wgl";
        return b;
    }

    void setCursorCaptured(bool captured) override {
        if (!hwnd_ || captured_ == captured) return;
        captured_ = captured;
        if (captured) {
            ShowCursor(FALSE);
            SetCapture(hwnd_);
            centerX_ = width_ / 2; centerY_ = height_ / 2;
            POINT pt{centerX_, centerY_};
            ClientToScreen(hwnd_, &pt);
            screenCX_ = pt.x; screenCY_ = pt.y;
            SetCursorPos(pt.x, pt.y);
            // Adopt the position we actually got: if the OS clamped the warp,
            // the per-frame look deltas below stay correct.
            POINT cur{};
            if (GetCursorPos(&cur)) {
                POINT cp = cur;
                if (ScreenToClient(hwnd_, &cp)) {
                    screenCX_ = cur.x; screenCY_ = cur.y;
                    centerX_ = cp.x; centerY_ = cp.y;
                }
            }
        } else {
            ShowCursor(TRUE);
            ReleaseCapture();
        }
    }

    void resize(int w, int h) override {
        if (w <= 0 || h <= 0) return;
        if (mode_ != DisplayMode::Windowed) { pendingW_ = w; pendingH_ = h; return; }
        // A maximized window ignores size changes: leave the maximized state
        // first, otherwise the new resolution would not be applied (and the
        // window could not be centered).
        if (hwnd_ && IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        // Remember what was asked for (e.g. 1920x1080) and fit the *window* to
        // what the monitor can show: a windowed window must never be bigger than
        // the screen. The setting keeps the standard resolution, so moving the
        // window to a bigger monitor re-fits it up to the requested size.
        reqW_ = w;
        reqH_ = h;
        int maxW = 0, maxH = 0;
        if (maxWindowSize(maxW, maxH)) {
            if (w > maxW) w = maxW;
            if (h > maxH) h = maxH;
        }
        width_ = w; height_ = h;
        pendingW_ = w; pendingH_ = h;
        if (!hwnd_) return;   // before init(): remembered for CreateWindow

        // Re-center on the monitor the window currently lives on every time the
        // size changes (the frame is size-dependent, so it is measured here).
        placeWindowed(w, h);
    }

    // ---- display modes -----------------------------------------------------

    bool applyDisplayMode(DisplayMode mode, int w, int h, int refreshHz) override {
        pendingMode_ = mode;
        pendingW_ = w > 0 ? w : pendingW_;
        pendingH_ = h > 0 ? h : pendingH_;
        if (!hwnd_) return true;    // remembered; applied during init()

        switch (mode) {
            case DisplayMode::Windowed: {
                bool wasFullscreen = mode_ != DisplayMode::Windowed;
                restoreDesktopMode();          // undo an exclusive switch first
                mode_ = DisplayMode::Windowed;
                refreshMonitorInfo();
                setBorderless(false);          // frame wins before measuring it
                // Leaving fullscreen keeps the current size but must still
                // center the window; a resolution change centers it too.
                resize(w > 0 ? w : (wasFullscreen ? pendingW_ : width_),
                       h > 0 ? h : (wasFullscreen ? pendingH_ : height_));
                centerOnMonitor();
                return true;
            }
            case DisplayMode::Borderless: {
                restoreDesktopMode();
                mode_ = DisplayMode::Borderless;
                refreshMonitorInfo();
                setBorderless(true);
                fillMonitorRect();
                return true;
            }
            case DisplayMode::Exclusive:
            default: {
                DEVMODEA dm{};
                if (!findDriverMode(w, h, refreshHz, dm)) {
                    fprintf(stderr, "[aw] display mode %dx%d@%dHz not supported by the driver\n",
                            w, h, refreshHz);
                    if (mode_ != DisplayMode::Exclusive) restoreDesktopMode();
                    return false;
                }
                if (!switchDisplayMode(dm)) {
                    restoreDesktopMode();
                    return false;
                }
                mode_ = DisplayMode::Exclusive;
                desktopW_ = int(dm.dmPelsWidth);    // keep the cache coherent while
                desktopH_ = int(dm.dmPelsHeight);   // we are in the switched mode
                setBorderless(true);
                fillMonitorRect();
                fprintf(stderr, "[aw] exclusive fullscreen %lux%lu @ %luHz\n",
                        (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
                        (unsigned long)dm.dmDisplayFrequency);
                return true;
            }
        }
    }

    DisplayMode currentDisplayMode() const override { return mode_; }

    bool monitorSize(int& w, int& h) const override {
        w = desktopW_; h = desktopH_;
        return w > 0 && h > 0;
    }

    int displayModeCount() const override { return enumerateModes(); }

    bool displayModeAt(int index, DisplayModeInfo& out) const override {
        int n = enumerateModes();
        if (index < 0 || index >= n) return false;
        out = modeCache_[index];
        return true;
    }

    bool maxWindowSize(int& w, int& h) const override {
        w = 0; h = 0;
        // Also called before init() (to pick the first window size): make sure
        // the monitor metrics below are not DPI-virtualized.
        enableDpiAwareness();
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoA(monitor(), &mi)) return false;
        int fw = 0, fh = 0;
        frameSize(windowDpi(), fw, fh);
        w = (mi.rcWork.right - mi.rcWork.left) - fw;
        h = (mi.rcWork.bottom - mi.rcWork.top) - fh;
        if (w < kMinWindowW) w = kMinWindowW;
        if (h < kMinWindowH) h = kMinWindowH;
        return true;
    }

    bool clientSize(int& w, int& h) const override {
        if (hwnd_) {
            RECT cr{};
            if (GetClientRect(hwnd_, &cr) && cr.right > 0 && cr.bottom > 0) {
                w = int(cr.right);
                h = int(cr.bottom);
                return true;
            }
        }
        w = width_; h = height_;
        return w > 0 && h > 0;
    }

    // Scaled displays: the menu must grow with the DPI (before this backend
    // was DPI aware, Windows did that scaling for us — blurrily — and the
    // fixed-pixel menu would now look half-size on a 200 % display).
    float uiScale() const override {
        return quantizeUiScale(float(windowDpi()) / 96.0f);
    }

    void* loadGLProc(const char* name) override {
        void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
        if (!p && hGL_) p = reinterpret_cast<void*>(GetProcAddress(hGL_, name));
        return p;
    }

private:
    bool setupPixelFormat() {
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 24;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        pfd.iLayerType = PFD_MAIN_PLANE;
        int pf = ChoosePixelFormat(hdc_, &pfd);
        if (!pf) return false;
        return SetPixelFormat(hdc_, pf, &pfd) != FALSE;
    }

    // ---- display-mode helpers ----------------------------------------------
    // Driver modes for the window's monitor (cached; recomputed after a display
    // change or a mode switch).
    int enumerateModes() const {
        if (modeCacheCount_ >= 0) return modeCacheCount_;
        modeCacheCount_ = 0;
        const char* dev = deviceName_[0] ? deviceName_ : nullptr;
        DEVMODEA dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsExA(dev, i, &dm, 0); ++i) {
            if (dm.dmPelsWidth < kMinModeW || dm.dmPelsHeight < kMinModeH) continue;
            if (dm.dmBitsPerPel != 0 && dm.dmBitsPerPel < 24) continue;
            DisplayModeInfo m;
            m.width = int(dm.dmPelsWidth);
            m.height = int(dm.dmPelsHeight);
            m.refreshHz = int(dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0);
            bool dup = false;
            for (int j = 0; j < modeCacheCount_; ++j) {
                if (modeCache_[j].width == m.width && modeCache_[j].height == m.height &&
                    modeCache_[j].refreshHz == m.refreshHz) { dup = true; break; }
            }
            if (dup || modeCacheCount_ >= kMaxDriverModes) continue;
            modeCache_[modeCacheCount_++] = m;
        }
        // Ascending by area, then by refresh rate.
        for (int i = 1; i < modeCacheCount_; ++i) {
            DisplayModeInfo key = modeCache_[i];
            int j = i - 1;
            while (j >= 0 && (modeCache_[j].width * modeCache_[j].height >
                                  key.width * key.height ||
                              (modeCache_[j].width * modeCache_[j].height ==
                                   key.width * key.height &&
                               modeCache_[j].refreshHz > key.refreshHz))) {
                modeCache_[j + 1] = modeCache_[j];
                --j;
            }
            modeCache_[j + 1] = key;
        }
        return modeCacheCount_;
    }

    // DEVMODE for `w x h` (and `hz` when > 0; otherwise the highest refresh).
    bool findDriverMode(int w, int h, int hz, DEVMODEA& out) const {
        int n = enumerateModes();
        int best = -1;
        for (int i = 0; i < n; ++i) {
            if (modeCache_[i].width != w || modeCache_[i].height != h) continue;
            if (hz > 0 && modeCache_[i].refreshHz != hz) continue;
            if (hz <= 0 && best >= 0 && modeCache_[best].refreshHz >= modeCache_[i].refreshHz)
                continue;
            best = i;
        }
        if (best < 0) return false;
        const char* dev = deviceName_[0] ? deviceName_ : nullptr;
        // Re-enumerate to get the full DEVMODE for the chosen entry.
        DEVMODEA dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsExA(dev, i, &dm, 0); ++i) {
            if (int(dm.dmPelsWidth) != modeCache_[best].width ||
                int(dm.dmPelsHeight) != modeCache_[best].height)
                continue;
            if (dm.dmDisplayFrequency != DWORD(modeCache_[best].refreshHz)) continue;
            out = dm;
            return true;
        }
        return false;
    }

    bool switchDisplayMode(const DEVMODEA& dm) {
        const char* dev = deviceName_[0] ? deviceName_ : nullptr;
        DEVMODEA want = dm;
        want.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
        LONG r = ChangeDisplaySettingsExA(dev, &want, nullptr, CDS_FULLSCREEN, nullptr);
        if (r != DISP_CHANGE_SUCCESSFUL) {
            fprintf(stderr, "[aw] ChangeDisplaySettingsEx failed (%ld)\n", (long)r);
            return false;
        }
        modeCacheCount_ = -1;
        return true;
    }

    void restoreDesktopMode() {
        if (mode_ != DisplayMode::Exclusive) return;
        const char* dev = deviceName_[0] ? deviceName_ : nullptr;
        ChangeDisplaySettingsExA(dev, nullptr, nullptr, 0, nullptr);   // registry mode
        modeCacheCount_ = -1;
        mode_ = DisplayMode::Windowed;   // the caller re-applies the real one
        // The desktop size is restored: re-read it.
        refreshMonitorInfo();
    }

    void setBorderless(bool borderless) {
        LONG_PTR style = GetWindowLongPtrA(hwnd_, GWL_STYLE);
        if (borderless) {
            if (!(style & WS_POPUP)) savedStyle_ = LONG(style);
            style = (style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW) | WS_POPUP;
        } else {
            LONG base = savedStyle_ ? savedStyle_ : LONG(style);
            style = (base & ~(LONG_PTR)WS_POPUP) | WS_OVERLAPPEDWINDOW;
        }
        SetWindowLongPtrA(hwnd_, GWL_STYLE, style);
    }

    // Put the window exactly over its monitor's rectangle (borderless /
    // exclusive fullscreen: 1:1 with the display, no borders).
    void fillMonitorRect() {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoA(monitor(), &mi)) return;
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        syncClientSize();
        keepCursorInside();
    }

    // ---- DPI / monitor helpers ---------------------------------------------
    void refreshMonitorInfo() {
        if (mode_ == DisplayMode::Exclusive) return;   // keep the desktop cache
        // MONITORINFOEXA (not MONITORINFO) carries szDevice, which is what the
        // display-mode APIs need to address this monitor's driver modes.
        MONITORINFOEXA mi{};   // szDevice: which display these modes belong to
        mi.cbSize = sizeof(mi);
        HMONITOR mon = monitor();
        // MONITORINFOEX is a MONITORINFO with a device name appended, and
        // GetMonitorInfo documents that either may be passed; the cast keeps
        // strict C++ header checks (MinGW/MSVC) happy.
        if (!mon || !GetMonitorInfoA(mon, reinterpret_cast<MONITORINFO*>(&mi))) return;
        desktopW_ = int(mi.rcMonitor.right - mi.rcMonitor.left);
        desktopH_ = int(mi.rcMonitor.bottom - mi.rcMonitor.top);
        if (mi.szDevice[0]) {
            if (std::strncmp(deviceName_, mi.szDevice, sizeof(deviceName_)) != 0)
                modeCacheCount_ = -1;   // modes belong to a device
            std::snprintf(deviceName_, sizeof(deviceName_), "%s", mi.szDevice);
        }
    }

    // DPI of the window's monitor (or of the system, before the window exists).
    int windowDpi() const {
        const WinApi& api = winApi();
        if (hwnd_ && api.getDpiForWindow) {
            UINT d = api.getDpiForWindow(hwnd_);
            if (d >= 48) return int(d);
        }
        return systemDpi();
    }

    int systemDpi() const {
        const WinApi& api = winApi();
        if (api.getDpiForSystem) {
            UINT d = api.getDpiForSystem();
            if (d >= 48) return int(d);
        }
        HDC dc = GetDC(nullptr);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(nullptr, dc);
        return dpi >= 48 ? dpi : 96;
    }

    // Non-client (frame) size of a standard window at `dpi`, in pixels.
    void frameSize(int dpi, int& fw, int& fh) const {
        constexpr LONG kProbe = 200;
        RECT r{0, 0, kProbe, kProbe};
        const WinApi& api = winApi();
        bool ok = api.adjustWindowRectForDpi &&
                  api.adjustWindowRectForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, UINT(dpi)) != FALSE;
        if (!ok) {
            r = RECT{0, 0, kProbe, kProbe};
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        }
        fw = int((r.right - r.left) - kProbe);
        fh = int((r.bottom - r.top) - kProbe);
        if (fw < 0) fw = 0;
        if (fh < 0) fh = 0;
    }

    HMONITOR monitor() const {
        if (hwnd_) return MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        POINT origin{0, 0};
        return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }

    // Centre a window of `winW x winH` on the monitor the window is on (the
    // requirement: a resized window is re-centered on the active monitor).
    // Usable area of the window's monitor (fallback: the primary monitor).
    WorkArea workAreaOfMonitor() const {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        WorkArea work;
        if (!GetMonitorInfoA(monitor(), &mi)) return work;
        work.x = int(mi.rcWork.left);
        work.y = int(mi.rcWork.top);
        work.width = int(mi.rcWork.right - mi.rcWork.left);
        work.height = int(mi.rcWork.bottom - mi.rcWork.top);
        return work;
    }

    void centerWindowOnMonitor(int winW, int winH, int& x, int& y) const {
        WorkArea work = workAreaOfMonitor();
        if (work.width <= 0 || work.height <= 0) {
            x = CW_USEDEFAULT; y = CW_USEDEFAULT;
            return;
        }
        centerWindowIn(work, winW, winH, x, y);
    }

    // Centers the window on its monitor without changing its size (used when
    // the size is already right, e.g. leaving fullscreen).
    void centerOnMonitor() {
        if (!hwnd_ || mode_ != DisplayMode::Windowed) return;
        RECT wr{};
        if (!GetWindowRect(hwnd_, &wr)) return;
        int x = 0, y = 0;
        centerWindowOnMonitor(int(wr.right - wr.left), int(wr.bottom - wr.top), x, y);
        if (x == CW_USEDEFAULT) return;
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        keepCursorInside();
    }

    // Moves (and centers) an existing window for a `w x h` client area. Called
    // after every windowed change so the window is centered on the monitor each
    // time the resolution changes.
    void placeWindowed(int w, int h) {
        if (!hwnd_) return;
        int fw = 0, fh = 0;
        frameSize(windowDpi(), fw, fh);
        int x = 0, y = 0;
        centerWindowOnMonitor(w + fw, h + fh, x, y);
        SetWindowPos(hwnd_, nullptr, x, y, w + fw, h + fh,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        syncClientSize();
        // Windows may have adjusted the client rect (min/max tracking); re-center
        // once more against the size we really got so the window ends up exactly
        // in the middle instead of drifting when it grows/shrinks.
        if (width_ != w || height_ != h) {
            centerWindowOnMonitor(width_ + fw, height_ + fh, x, y);
            SetWindowPos(hwnd_, nullptr, x, y, width_ + fw, height_ + fh,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            syncClientSize();
        }
        keepCursorInside();
    }

    void centerOnPrimaryMonitor(int winW, int winH, int& x, int& y) const {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        POINT origin{0, 0};
        HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        if (!mon || !GetMonitorInfoA(mon, &mi)) { x = CW_USEDEFAULT; y = CW_USEDEFAULT; return; }
        const RECT& wa = mi.rcWork;
        int availW = wa.right - wa.left, availH = wa.bottom - wa.top;
        x = wa.left + (availW - winW) / 2;
        y = wa.top + (availH - winH) / 2;
        if (x + winW > wa.right) x = wa.right - winW;
        if (y + winH > wa.bottom) y = wa.bottom - winH;
        if (x < wa.left) x = wa.left;
        if (y < wa.top) y = wa.top;
    }

    // Shrink/move a window rect so that it stays inside the work area.
    void clampWindowRect(RECT& wr) const {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoA(monitor(), &mi)) return;
        const RECT& wa = mi.rcWork;
        int w = int(wr.right - wr.left), h = int(wr.bottom - wr.top);
        int availW = wa.right - wa.left, availH = wa.bottom - wa.top;
        if (w > availW) { w = availW; wr.left = wa.left; }
        if (h > availH) { h = availH; wr.top = wa.top; }
        if (wr.left + w > wa.right) wr.left = wa.right - w;
        if (wr.top + h > wa.bottom) wr.top = wa.bottom - h;
        if (wr.left < wa.left) wr.left = wa.left;
        if (wr.top < wa.top) wr.top = wa.top;
        wr.right = wr.left + w;
        wr.bottom = wr.top + h;
    }

    void syncClientSize() {
        RECT cr{};
        if (hwnd_ && GetClientRect(hwnd_, &cr) && cr.right > 0 && cr.bottom > 0) {
            width_ = int(cr.right);
            height_ = int(cr.bottom);
        }
    }

    // After a resize the cursor can end up outside the smaller window (it would
    // then click on whatever is behind it): bring it back into the client area.
    void keepCursorInside() {
        if (!hwnd_ || captured_ || width_ <= 0 || height_ <= 0) return;
        POINT p{};
        if (!GetCursorPos(&p)) return;
        POINT c = p;
        if (!ScreenToClient(hwnd_, &c)) return;
        if (c.x >= 0 && c.y >= 0 && c.x < width_ && c.y < height_) return;
        c.x = width_ / 2; c.y = height_ / 2;
        if (!ClientToScreen(hwnd_, &c)) return;
        SetCursorPos(c.x, c.y);
        mouseX_ = width_ / 2; mouseY_ = height_ / 2;
    }

    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        PlatformWin32* self =
            reinterpret_cast<PlatformWin32*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTA*>(lp);
            self = static_cast<PlatformWin32*>(cs->lpCreateParams);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcA(hwnd, msg, wp, lp);
        return self->wndProc(hwnd, msg, wp, lp);
    }

    LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_KEYDOWN: { uint32_t k = vkToKey(wp); if (k < 512) keys_[k] = 1; return 0; }
            case WM_KEYUP:   { uint32_t k = vkToKey(wp); if (k < 512) keys_[k] = 0; return 0; }
            case WM_LBUTTONDOWN: mouseX_ = GET_X_LPARAM(lp); mouseY_ = GET_Y_LPARAM(lp); mousePressed_[MBTN_LEFT] = true;   return 0;
            case WM_LBUTTONUP:   mouseX_ = GET_X_LPARAM(lp); mouseY_ = GET_Y_LPARAM(lp); mouseReleased_[MBTN_LEFT] = true;  return 0;
            case WM_RBUTTONDOWN: mouseX_ = GET_X_LPARAM(lp); mouseY_ = GET_Y_LPARAM(lp); mousePressed_[MBTN_RIGHT] = true;  return 0;
            case WM_RBUTTONUP:   mouseX_ = GET_X_LPARAM(lp); mouseY_ = GET_Y_LPARAM(lp); mouseReleased_[MBTN_RIGHT] = true; return 0;
            case WM_MBUTTONDOWN: mouseX_ = GET_X_LPARAM(lp); mouseY_ = GET_Y_LPARAM(lp); mousePressed_[MBTN_MIDDLE] = true; return 0;
            case WM_MBUTTONUP:   mouseX_ = GET_X_LPARAM(lp); mouseY_ = GET_Y_LPARAM(lp); mouseReleased_[MBTN_MIDDLE] = true; return 0;
            case WM_MOUSEMOVE: {
                mouseX_ = GET_X_LPARAM(lp);
                mouseY_ = GET_Y_LPARAM(lp);
                if (captured_) {
                    mouseDX_ += float(GET_X_LPARAM(lp) - centerX_);
                    mouseDY_ += float(GET_Y_LPARAM(lp) - centerY_);
                    SetCursorPos(screenCX_, screenCY_);
                }
                return 0;
            }
            case WM_SIZE:
                width_ = LOWORD(lp);
                height_ = HIWORD(lp);
                return 0;
            case WM_DPICHANGED: {
                // Per-monitor DPI awareness: adopt the size Windows suggests for
                // the new scale factor (windowed mode only; borderless and
                // exclusive fullscreen cover the monitor either way).
                if (mode_ == DisplayMode::Windowed) {
                    const RECT* sug = reinterpret_cast<const RECT*>(lp);
                    if (sug && sug->right > sug->left && sug->bottom > sug->top) {
                        RECT r = *sug;
                        clampWindowRect(r);
                        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left,
                                     r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
                        syncClientSize();
                    }
                }
                return 0;
            }
            case WM_DISPLAYCHANGE:
                // Monitor layout/resolution changed (undocked laptop, scaled
                // display, taskbar resized): the driver mode list and the
                // desktop size may both be different now.
                modeCacheCount_ = -1;
                refreshMonitorInfo();
                // Fullscreen presentations are re-fitted to the monitor; a
                // maximized window is Windows' own business — it re-fits it —
                // and un-maximizing it here would be a rude surprise.
                if (mode_ == DisplayMode::Borderless) {
                    fillMonitorRect();
                } else if (mode_ == DisplayMode::Windowed) {
                    if (!IsZoomed(hwnd_))
                        resize(reqW_ > 0 ? reqW_ : width_, reqH_ > 0 ? reqH_ : height_);
                    else syncClientSize();
                }
                return 0;
            case WM_CLOSE:
                shouldQuit_ = true;
                return 0;
            case WM_DESTROY:
                shouldQuit_ = true;
                return 0;
        }
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    HDC hdc_ = nullptr;
    HGLRC hrc_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    HMODULE hGL_ = nullptr;
    uint8_t keys_[512]{};
    bool mousePressed_[8]{}, mouseReleased_[8]{};
    float mouseDX_ = 0.0f, mouseDY_ = 0.0f;
    int width_ = 0, height_ = 0;
    int mouseX_ = 0, mouseY_ = 0;
    int centerX_ = 0, centerY_ = 0, screenCX_ = 0, screenCY_ = 0;
    bool captured_ = false, shouldQuit_ = false;
    // Display state.
    DisplayMode mode_ = DisplayMode::Windowed;
    DisplayMode pendingMode_ = DisplayMode::Windowed;   // requested before init()
    int pendingW_ = 0, pendingH_ = 0;
    char deviceName_[32]{};
    // Windowed client size the settings asked for (may be larger than the fitted
    // window: the standard resolution is kept, the window is fitted to the screen).
    int reqW_ = 0, reqH_ = 0;
    int desktopW_ = 0, desktopH_ = 0;
    RECT savedRect_{};
    LONG savedStyle_ = 0;
    mutable DisplayModeInfo modeCache_[kMaxDriverModes]{};
    mutable int modeCacheCount_ = -1;
};

Platform* createPlatform() { return new PlatformWin32(); }

}  // namespace aw

#endif  // _WIN32
