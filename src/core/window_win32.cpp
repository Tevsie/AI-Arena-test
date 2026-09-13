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
// viewport and mouse coordinates all agree. Windowed sizes are additionally
// clamped to the monitor's work area (screen minus taskbar and decorations).
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

        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        // Adopt the size Windows actually gave the client area (DPI rounding).
        syncClientSize();
        fprintf(stderr, "[aw] window: %dx%d client at %d,%d (dpi %d)\n",
                width_, height_, x, y, windowDpi());
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
        if (fullscreen_) return;   // borderless fullscreen always fills the monitor
        // Clamp to what the monitor can show: a windowed window must never be
        // bigger than the screen (the resolution setting asks for a preset, the
        // monitor decides how much of it fits).
        int maxW = 0, maxH = 0;
        if (maxWindowSize(maxW, maxH)) {
            if (w > maxW) w = maxW;
            if (h > maxH) h = maxH;
        }
        width_ = w; height_ = h;
        if (!hwnd_) return;   // before init(): remembered for CreateWindow

        int fw = 0, fh = 0;
        frameSize(windowDpi(), fw, fh);
        RECT wr{};
        if (!GetWindowRect(hwnd_, &wr)) { wr.left = 0; wr.top = 0; }
        RECT target{wr.left, wr.top, wr.left + w + fw, wr.top + h + fh};
        clampWindowRect(target);
        SetWindowPos(hwnd_, nullptr, target.left, target.top,
                     target.right - target.left, target.bottom - target.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // Apply immediately so this frame already renders the new size (the
        // asynchronous WM_SIZE confirms it afterwards).
        syncClientSize();
        keepCursorInside();
    }

    void setFullscreen(bool on) override {
        if (fullscreen_ == on) return;
        fullscreen_ = on;
        if (!hwnd_) return;
        if (on) {
            // Borderless window covering the current monitor (WM_SIZE syncs w/h).
            GetWindowRect(hwnd_, &savedRect_);
            savedStyle_ = GetWindowLongA(hwnd_, GWL_STYLE);
            SetWindowLongA(hwnd_, GWL_STYLE, (savedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoA(monitor(), &mi)) {
                SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top,
                             SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
            }
            syncClientSize();
        } else {
            SetWindowLongA(hwnd_, GWL_STYLE, savedStyle_);
            RECT r = savedRect_;
            if (r.right <= r.left || r.bottom <= r.top) {
                RECT cur{};
                if (GetWindowRect(hwnd_, &cur)) r = cur;
            }
            clampWindowRect(r);
            SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
            ShowWindow(hwnd_, SW_RESTORE);
            syncClientSize();
        }
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

    // Scaled displays: the menu must grow with the DPI (before this backend
    // was DPI aware, Windows did that scaling for us — blurrily — and the
    // fixed-pixel menu would now look half-size on a 200 % display).
    float uiScale() const override {
        return quantizeUiScale(float(windowDpi()) / 96.0f);
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

    // ---- DPI / monitor helpers ---------------------------------------------
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

    // Centre a window of `winW x winH` on the primary monitor's work area.
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
                // the new scale factor (windowed mode only; borderless
                // fullscreen covers the monitor either way).
                if (!fullscreen_) {
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
                // display, taskbar resized): keep the window fully on screen.
                // A maximized window is Windows' own business — it re-fits it —
                // and un-maximizing it here would be a rude surprise.
                if (!fullscreen_) {
                    if (!IsZoomed(hwnd_) && width_ > 0 && height_ > 0) resize(width_, height_);
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
    bool fullscreen_ = false;
    RECT savedRect_{};
    LONG savedStyle_ = 0;
};

Platform* createPlatform() { return new PlatformWin32(); }

}  // namespace aw

#endif  // _WIN32
