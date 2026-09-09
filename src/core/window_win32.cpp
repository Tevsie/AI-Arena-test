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
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:    return 0x1F0;       // reserved, unused by the game
        default:         return uint32_t(vk) & 0xFF;
    }
}

}  // namespace

class PlatformWin32 final : public Platform {
public:
    bool init(const char* title, int w, int h) override {
        width_ = w; height_ = h;
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
        if (wglCreateContextAttribsARB) {
            const int attribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                0
            };
            hrc_ = wglCreateContextAttribsARB(hdc_, nullptr, attribs);
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
            fprintf(stderr, "[aw] OpenGL 3.3 core unavailable\n");
            return false;
        }

        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        fprintf(stderr, "[aw] renderer: %s | %s\n",
                gl.GetString ? (const char*)gl.GetString(GL_RENDERER) : "?",
                gl.GetString ? (const char*)gl.GetString(GL_VERSION) : "?");
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
        in.shouldQuit = shouldQuit_;

        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (in.keys[KEY_ESC]) {
            setCursorCaptured(false);
            in.keys[KEY_ESC] = 0;
            in.shouldQuit = true;
            shouldQuit_ = true;
        }
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
            case WM_LBUTTONDOWN: mousePressed_[MBTN_LEFT] = true;   return 0;
            case WM_LBUTTONUP:   mouseReleased_[MBTN_LEFT] = true;  return 0;
            case WM_RBUTTONDOWN: mousePressed_[MBTN_RIGHT] = true;  return 0;
            case WM_RBUTTONUP:   mouseReleased_[MBTN_RIGHT] = true; return 0;
            case WM_MBUTTONDOWN: mousePressed_[MBTN_MIDDLE] = true; return 0;
            case WM_MBUTTONUP:   mouseReleased_[MBTN_MIDDLE] = true; return 0;
            case WM_MOUSEMOVE: {
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
    int centerX_ = 0, centerY_ = 0, screenCX_ = 0, screenCY_ = 0;
    bool captured_ = false, shouldQuit_ = false;
};

Platform* createPlatform() { return new PlatformWin32(); }

}  // namespace aw

#endif  // _WIN32
