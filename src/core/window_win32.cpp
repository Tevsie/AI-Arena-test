// window_win32.cpp — Win32 + WGL + OpenGL window backend.
// Native Windows backend (compiled with MinGW): creates a window, a WGL 3.3
// core context, loads every GL entry point at runtime, and translates Win32
// input into the engine's FrameInput. Mirrors the X11 backend's behaviour so
// the game code is backend-agnostic.
#include "platform.hpp"

#if defined(_WIN32)

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

        RECT r{0, 0, w, h};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowA("AgainstTheWallWindow", title, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top,
                              nullptr, nullptr, hInstance_, this);
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
        } else {
            ShowCursor(TRUE);
            ReleaseCapture();
        }
    }

    void resize(int w, int h) override {
        if (w <= 0 || h <= 0) return;
        width_ = w; height_ = h;
        mouseX_ = w / 2; mouseY_ = h / 2;
        if (!hwnd_) return;
        RECT r{0, 0, w, h};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER);
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
            HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoA(mon, &mi)) {
                SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top,
                             SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
            }
        } else {
            SetWindowLongA(hwnd_, GWL_STYLE, savedStyle_);
            SetWindowPos(hwnd_, nullptr, savedRect_.left, savedRect_.top,
                         savedRect_.right - savedRect_.left,
                         savedRect_.bottom - savedRect_.top,
                         SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
            ShowWindow(hwnd_, SW_RESTORE);
        }
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
