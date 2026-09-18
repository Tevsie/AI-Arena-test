// window_x11.cpp — X11 + GLX + OpenGL window backend.
// Zero-dependency (no GLFW/SDL): talks to Xlib/GLX directly through dlopen,
// so the engine links only against libdl and libc.
//
// If any step fails (no DISPLAY, no GLX, no GL 3.3), init() returns false and
// the caller falls back to the headless backend.
#if !defined(_WIN32)

#include "platform.hpp"

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "input.hpp"
#include "../render/gl.h"

// ---- Xlib minimal declarations (dlopen'd at runtime) ----------------------
// These mirror the real Xlib ABI; we avoid the system headers so the code
// builds even where X11 dev packages are not installed.
namespace x11 {
using Atom = unsigned long;
using Window = unsigned long;
using Colormap = unsigned long;
using Cursor = unsigned long;
using Pixmap = unsigned long;
using Drawable = unsigned long;
using Display = struct _XDisplay;
using XEvent = void;             // opaque event buffer (we decode known records)
using Time = unsigned long;
using KeySym = unsigned long;
using GLXContext = struct __GLXcontextRec*;
using GLXFBConfig = struct __GLXFBConfigRec*;
using GLXDrawable = unsigned long;
using GLXPixmap = unsigned long;

struct XColor { unsigned long pixel; unsigned short red, green, blue; char flags; char pad; };

// ---- XRandR (hand-declared ABI; libXrandr.so.2 is dlopen'd at runtime) ----
using RROutput = unsigned long;
using RRCrtc = unsigned long;
using RRMode = unsigned long;
using Rotation = unsigned short;

struct XRRModeInfo {
    RRMode id;
    unsigned int width, height;
    unsigned long dotClock;
    unsigned int hSyncStart, hSyncEnd, hTotal, hSkew;
    unsigned int vSyncStart, vSyncEnd, vTotal;
    char* name;
    unsigned int nameLength;
    unsigned long modeFlags;
};

struct XRRScreenResources {
    unsigned long timestamp;
    unsigned long configTimestamp;
    int ncrtc;
    RRCrtc* crtcs;
    int noutput;
    RROutput* outputs;
    int nmode;
    XRRModeInfo* modes;
};

struct XRRCrtcInfo {
    unsigned long timestamp;
    int x, y;
    unsigned int width, height;
    RRMode mode;
    Rotation rotation;
    int noutput;
    RROutput* outputs;
    Rotation rotations;
    int npossible;
    RROutput* possible;
};

struct XRROutputInfo {
    unsigned long timestamp;
    RRCrtc crtc;
    char* name;
    int nameLen;
    unsigned long mmWidth, mmHeight;
    int connection, subpixelOrder;
    int npreferred;
    int ncrtc;
    RRCrtc* crtcs;
    int nclone;
    RROutput* clones;
    int nmode;
    int npreferredmode;
    RRMode* modes;
};

extern "C" {
using XOpenDisplayFn = Display* (*)(const char*);
using XDefaultScreenFn = int (*)(Display*);
using XRootWindowFn = Window (*)(Display*, int);
using XBlackPixelFn = unsigned long (*)(Display*, int);
using XCreateColormapFn = Colormap (*)(Display*, Window, void*, int);
using XCreateSimpleWindowFn = Window (*)(Display*, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long);
using XStoreNameFn = int (*)(Display*, Window, const char*);
using XMapWindowFn = int (*)(Display*, Window);
using XSelectInputFn = int (*)(Display*, Window, long);
using XNextEventFn = int (*)(Display*, XEvent*);
using XPendingFn = int (*)(Display*);
using XLookupKeysymFn = KeySym (*)(void*, int);
using XKeysymToKeycodeFn = unsigned (*)(Display*, KeySym);
using XWarpPointerFn = int (*)(Display*, Window, Window, int, int, unsigned, unsigned, int, int);
using XResizeWindowFn = int (*)(Display*, Window, unsigned, unsigned);
using XMoveWindowFn = int (*)(Display*, Window, int, int);
using XDisplayWidthFn = int (*)(Display*, int);
using XDisplayHeightFn = int (*)(Display*, int);
using XRRGetScreenResourcesCurrentFn = XRRScreenResources* (*)(Display*, Window);
using XRRFreeScreenResourcesFn = void (*)(XRRScreenResources*);
using XRRGetCrtcInfoFn = XRRCrtcInfo* (*)(Display*, XRRScreenResources*, RRCrtc);
using XRRFreeCrtcInfoFn = void (*)(XRRCrtcInfo*);
using XRRGetOutputInfoFn = XRROutputInfo* (*)(Display*, XRRScreenResources*, RROutput);
using XRRFreeOutputInfoFn = void (*)(XRROutputInfo*);
using XRRGetOutputPrimaryFn = RROutput (*)(Display*, Window);
using XRRSetCrtcConfigFn = int (*)(Display*, XRRScreenResources*, RRCrtc, Time, int, int,
                                   RRMode, Rotation, RROutput*, int);
using XSendEventFn = int (*)(Display*, Window, int, long, XEvent*);
using XQueryPointerFn = int (*)(Display*, Window, Window*, Window*, int*, int*, int*, int*, unsigned*);
using XGrabPointerFn = int (*)(Display*, Window, int, unsigned, int, int, Window, Cursor, Time);
using XUngrabPointerFn = int (*)(Display*, Time);
using XCreateFontCursorFn = Cursor (*)(Display*, unsigned);
using XCreatePixmapFn = Pixmap (*)(Display*, Drawable, unsigned, unsigned, unsigned);
using XCreatePixmapCursorFn = Cursor (*)(Display*, Pixmap, Pixmap, XColor*, XColor*, unsigned, unsigned);
using XFreePixmapFn = int (*)(Display*, Pixmap);
using XDefineCursorFn = int (*)(Display*, Window, Cursor);
using XUndefineCursorFn = int (*)(Display*, Window);
using XSyncFn = int (*)(Display*, int);
using XCloseDisplayFn = int (*)(Display*);
using XInternAtomFn = Atom (*)(Display*, const char*, int);
using XSetWMProtocolsFn = int (*)(Display*, Window, Atom*, int);
using XChangePropertyFn = int (*)(Display*, Window, Atom, Atom, int, int, const unsigned char*, int);
using XFreeFn = int (*)(void*);
using XGetWindowAttributesFn = int (*)(Display*, Window, void*);
}

// Xlib constants
constexpr long ExposureMask = 1L << 15;
constexpr long StructureNotifyMask = 1L << 17;
constexpr long KeyPressMask = 1L << 0;
constexpr long KeyReleaseMask = 1L << 1;
constexpr long ButtonPressMask = 1L << 2;
constexpr long ButtonReleaseMask = 1L << 3;
constexpr long PointerMotionMask = 1L << 6;
constexpr unsigned GrabModeAsync = 1;
constexpr int GrabSuccess = 0;

}  // namespace x11

// ---- GLX minimal declarations ---------------------------------------------
namespace glx {
using GLXContext = x11::GLXContext;
using GLXFBConfig = x11::GLXFBConfig;
using GLXDrawable = x11::GLXDrawable;
using GLXContextRec = struct _GLXContextRec;
using GLXFBConfigRec = struct _GLXFBConfigRec;

extern "C" {
using glXChooseFBConfigFn = GLXFBConfig* (*)(x11::Display*, int, const int*, int*);
using glXCreateContextAttribsARBFn = GLXContext (*)(x11::Display*, GLXFBConfig, GLXContext, int, const int*);
using glXMakeCurrentFn = int (*)(x11::Display*, GLXDrawable, GLXContext);
using glXSwapBuffersFn = void (*)(x11::Display*, GLXDrawable);
using glXGetProcAddressFn = void* (*)(const unsigned char*);
using glXGetProcAddressARBFn = void* (*)(const unsigned char*);
using glXDestroyContextFn = void (*)(x11::Display*, GLXContext);
// GLX_EXT_swap_control (drawable-based) and the older SGI/MESA spellings.
using glXSwapIntervalEXTFn = void (*)(x11::Display*, GLXDrawable, int);
using glXSwapIntervalFn = int (*)(int);
}

constexpr int GLX_RGBA = 4;
constexpr int GLX_DOUBLEBUFFER = 5;
constexpr int GLX_RED_SIZE = 8;
constexpr int GLX_GREEN_SIZE = 9;
constexpr int GLX_BLUE_SIZE = 10;
constexpr int GLX_DEPTH_SIZE = 12;
constexpr int GLX_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int GLX_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int GLX_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int GLX_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
}  // namespace glx

namespace aw {
namespace {

using namespace x11;   // bring the mask/keysym constants into scope

// Container for the dlsym-resolved Xlib/GLX tables.
struct XLib {
    void* hX11 = nullptr;
    void* hGL = nullptr;

    x11::XOpenDisplayFn XOpenDisplay = nullptr;
    x11::XDefaultScreenFn XDefaultScreen = nullptr;
    x11::XRootWindowFn XRootWindow = nullptr;
    x11::XBlackPixelFn XBlackPixel = nullptr;
    x11::XCreateColormapFn XCreateColormap = nullptr;
    x11::XCreateSimpleWindowFn XCreateSimpleWindow = nullptr;
    x11::XStoreNameFn XStoreName = nullptr;
    x11::XMapWindowFn XMapWindow = nullptr;
    x11::XSelectInputFn XSelectInput = nullptr;
    x11::XNextEventFn XNextEvent = nullptr;
    x11::XPendingFn XPending = nullptr;
    x11::XLookupKeysymFn XLookupKeysym = nullptr;
    x11::XKeysymToKeycodeFn XKeysymToKeycode = nullptr;
    x11::XWarpPointerFn XWarpPointer = nullptr;
    x11::XResizeWindowFn XResizeWindow = nullptr;
    x11::XMoveWindowFn XMoveWindow = nullptr;
    x11::XDisplayWidthFn XDisplayWidth = nullptr;
    x11::XDisplayHeightFn XDisplayHeight = nullptr;
    x11::XSendEventFn XSendEvent = nullptr;
    x11::XQueryPointerFn XQueryPointer = nullptr;
    x11::XGrabPointerFn XGrabPointer = nullptr;
    x11::XUngrabPointerFn XUngrabPointer = nullptr;
    x11::XCreateFontCursorFn XCreateFontCursor = nullptr;
    x11::XCreatePixmapFn XCreatePixmap = nullptr;
    x11::XCreatePixmapCursorFn XCreatePixmapCursor = nullptr;
    x11::XFreePixmapFn XFreePixmap = nullptr;
    x11::XDefineCursorFn XDefineCursor = nullptr;
    x11::XUndefineCursorFn XUndefineCursor = nullptr;
    x11::XSyncFn XSync = nullptr;
    x11::XCloseDisplayFn XCloseDisplay = nullptr;
    x11::XInternAtomFn XInternAtom = nullptr;
    x11::XSetWMProtocolsFn XSetWMProtocols = nullptr;
    x11::XChangePropertyFn XChangeProperty = nullptr;
    x11::XFreeFn XFree = nullptr;

    // XRandR (optional: exclusive fullscreen / display-mode enumeration)
    void* hRandR = nullptr;
    x11::XRRGetScreenResourcesCurrentFn XRRGetScreenResourcesCurrent = nullptr;
    x11::XRRFreeScreenResourcesFn XRRFreeScreenResources = nullptr;
    x11::XRRGetCrtcInfoFn XRRGetCrtcInfo = nullptr;
    x11::XRRFreeCrtcInfoFn XRRFreeCrtcInfo = nullptr;
    x11::XRRGetOutputInfoFn XRRGetOutputInfo = nullptr;
    x11::XRRFreeOutputInfoFn XRRFreeOutputInfo = nullptr;
    x11::XRRGetOutputPrimaryFn XRRGetOutputPrimary = nullptr;
    x11::XRRSetCrtcConfigFn XRRSetCrtcConfig = nullptr;

    bool hasRandR() const {
        return XRRGetScreenResourcesCurrent && XRRFreeScreenResources && XRRGetCrtcInfo &&
               XRRFreeCrtcInfo && XRRSetCrtcConfig;
    }

    glx::glXChooseFBConfigFn glXChooseFBConfig = nullptr;
    glx::glXCreateContextAttribsARBFn glXCreateContextAttribsARB = nullptr;
    glx::glXMakeCurrentFn glXMakeCurrent = nullptr;
    glx::glXSwapBuffersFn glXSwapBuffers = nullptr;
    glx::glXGetProcAddressFn glXGetProcAddress = nullptr;
    glx::glXDestroyContextFn glXDestroyContext = nullptr;
    glx::glXSwapIntervalEXTFn glXSwapIntervalEXT = nullptr;
    glx::glXSwapIntervalFn glXSwapIntervalMESA = nullptr;
    glx::glXSwapIntervalFn glXSwapIntervalSGI = nullptr;

    bool loadAll() {
        // X11
        hX11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL);
        if (!hX11) { fprintf(stderr, "[aw] libX11 not found\n"); return false; }
#define LD(lib, field, sym) field = reinterpret_cast<decltype(field)>(dlsym(lib, sym)); if (!field) { fprintf(stderr, "[aw] missing symbol %s\n", sym); return false; }
        LD(hX11, XOpenDisplay, "XOpenDisplay");
        LD(hX11, XDefaultScreen, "XDefaultScreen");
        LD(hX11, XRootWindow, "XRootWindow");
        LD(hX11, XBlackPixel, "XBlackPixel");
        LD(hX11, XCreateColormap, "XCreateColormap");
        LD(hX11, XCreateSimpleWindow, "XCreateSimpleWindow");
        LD(hX11, XStoreName, "XStoreName");
        LD(hX11, XMapWindow, "XMapWindow");
        LD(hX11, XSelectInput, "XSelectInput");
        LD(hX11, XNextEvent, "XNextEvent");
        LD(hX11, XPending, "XPending");
        LD(hX11, XLookupKeysym, "XLookupKeysym");
        LD(hX11, XKeysymToKeycode, "XKeysymToKeycode");
        LD(hX11, XWarpPointer, "XWarpPointer");
        LD(hX11, XResizeWindow, "XResizeWindow");
        LD(hX11, XMoveWindow, "XMoveWindow");
        LD(hX11, XDisplayWidth, "XDisplayWidth");
        LD(hX11, XDisplayHeight, "XDisplayHeight");
        LD(hX11, XSendEvent, "XSendEvent");
        LD(hX11, XQueryPointer, "XQueryPointer");
        LD(hX11, XGrabPointer, "XGrabPointer");
        LD(hX11, XUngrabPointer, "XUngrabPointer");
        LD(hX11, XCreateFontCursor, "XCreateFontCursor");
        LD(hX11, XCreatePixmap, "XCreatePixmap");
        LD(hX11, XCreatePixmapCursor, "XCreatePixmapCursor");
        LD(hX11, XFreePixmap, "XFreePixmap");
        LD(hX11, XDefineCursor, "XDefineCursor");
        LD(hX11, XUndefineCursor, "XUndefineCursor");
        LD(hX11, XSync, "XSync");
        LD(hX11, XCloseDisplay, "XCloseDisplay");
        LD(hX11, XInternAtom, "XInternAtom");
        LD(hX11, XSetWMProtocols, "XSetWMProtocols");
        LD(hX11, XChangeProperty, "XChangeProperty");
        LD(hX11, XFree, "XFree");
#undef LD
        // GLX / GL
        hGL = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
        if (hGL) {
#define LD2(lib, field, sym) field = reinterpret_cast<decltype(field)>(dlsym(lib, sym));
            LD2(hGL, glXChooseFBConfig, "glXChooseFBConfig");
            LD2(hGL, glXCreateContextAttribsARB, "glXCreateContextAttribsARB");
            LD2(hGL, glXMakeCurrent, "glXMakeCurrent");
            LD2(hGL, glXSwapBuffers, "glXSwapBuffers");
            LD2(hGL, glXGetProcAddress, "glXGetProcAddress");
            LD2(hGL, glXDestroyContext, "glXDestroyContext");
            LD2(hGL, glXSwapIntervalEXT, "glXSwapIntervalEXT");
            LD2(hGL, glXSwapIntervalMESA, "glXSwapIntervalMESA");
            LD2(hGL, glXSwapIntervalSGI, "glXSwapIntervalSGI");
#undef LD2
        }
        // XRandR (optional): without it the game falls back to borderless
        // fullscreen and only offers render resolutions.
        hRandR = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_GLOBAL);
        if (hRandR) {
#define LD3(field, sym) field = reinterpret_cast<decltype(field)>(dlsym(hRandR, sym));
            LD3(XRRGetScreenResourcesCurrent, "XRRGetScreenResourcesCurrent");
            LD3(XRRFreeScreenResources, "XRRFreeScreenResources");
            LD3(XRRGetCrtcInfo, "XRRGetCrtcInfo");
            LD3(XRRFreeCrtcInfo, "XRRFreeCrtcInfo");
            LD3(XRRGetOutputInfo, "XRRGetOutputInfo");
            LD3(XRRFreeOutputInfo, "XRRFreeOutputInfo");
            LD3(XRRGetOutputPrimary, "XRRGetOutputPrimary");
            LD3(XRRSetCrtcConfig, "XRRSetCrtcConfig");
#undef LD3
        }
        // glXGetProcAddress may live in libGLX.so.1 on some setups; tolerate its absence
        // only if the core GLX symbols resolved.
        return glXChooseFBConfig && glXMakeCurrent && glXSwapBuffers;
    }
};

// ---------------------------------------------------------------------------
// X11 event decode (without the system Xlib headers we hand-roll the layout):
//   XEvent = union of event structs; every event begins with "int type".
// We only ever read `type` and a few known fields of the key/button/motion
// records via a fixed, stable ABI layout below.
// ---------------------------------------------------------------------------
struct XKeyEvent { int type; unsigned long serial; int send_event; x11::Display* display;
                   x11::Window window, root, subwindow; x11::Time time; int x, y, x_root, y_root;
                   unsigned state; unsigned keycode; int same_screen; };
struct XButtonEvent { int type; unsigned long serial; int send_event; x11::Display* display;
                      x11::Window window, root, subwindow; x11::Time time; int x, y, x_root, y_root;
                      unsigned state; unsigned button; int same_screen; };
struct XMotionEvent { int type; unsigned long serial; int send_event; x11::Display* display;
                      x11::Window window, root, subwindow; x11::Time time; int x, y, x_root, y_root;
                      unsigned state; char is_hint; int same_screen; };

constexpr int KeyPress = 2, KeyRelease = 3, ButtonPress = 4, ButtonRelease = 5, MotionNotify = 6;
constexpr int ClientMessage = 33;
constexpr int ConfigureNotify = 22;

// XClientMessageEvent ABI (only the fields we read).
struct XClientMessageEvent {
    int type; unsigned long serial; int send_event; x11::Display* display;
    x11::Window window; x11::Atom message_type; int format; long data[5];
};

// XConfigureEvent ABI (window resize/move; only width/height are read).
struct XConfigureEvent {
    int type; unsigned long serial; int send_event; x11::Display* display;
    x11::Window event, window; int x, y, width, height, border_width;
    x11::Window above; int override_redirect;
};

}  // namespace

// ---------------------------------------------------------------------------
class PlatformX11 final : public Platform {
public:
    bool init(const char* title, int w, int h) override {
        width_ = w; height_ = h;
        mouseX_ = w / 2; mouseY_ = h / 2;
        if (!x_.loadAll()) return false;

        dpy_ = x_.XOpenDisplay(nullptr);
        if (!dpy_) { fprintf(stderr, "[aw] no DISPLAY available\n"); return false; }

        int screen = x_.XDefaultScreen(dpy_);
        x11::Window root = x_.XRootWindow(dpy_, screen);

        // Choose a double-buffered RGBA + depth FB config.
        int visAttribs[] = {
            glx::GLX_RGBA, glx::GLX_DOUBLEBUFFER,
            glx::GLX_RED_SIZE, 8, glx::GLX_GREEN_SIZE, 8, glx::GLX_BLUE_SIZE, 8,
            glx::GLX_DEPTH_SIZE, 24, 0
        };
        int ncfg = 0;
        glx::GLXFBConfig* cfgs = x_.glXChooseFBConfig(dpy_, screen, visAttribs, &ncfg);
        if (!cfgs || ncfg == 0) { fprintf(stderr, "[aw] no suitable GLX fbconfig\n"); x_.XCloseDisplay(dpy_); dpy_ = nullptr; return false; }
        fbcfg_ = cfgs[0];
        if (x_.XFree) x_.XFree(cfgs);

        // GL 3.3 core context.
        int ctxAttribs[] = {
            glx::GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            glx::GLX_CONTEXT_MINOR_VERSION_ARB, 3,
            glx::GLX_CONTEXT_PROFILE_MASK_ARB, glx::GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
        ctx_ = x_.glXCreateContextAttribsARB ? x_.glXCreateContextAttribsARB(dpy_, fbcfg_, nullptr, 1, ctxAttribs) : nullptr;
        if (!ctx_) { fprintf(stderr, "[aw] GL 3.3 core context unavailable\n"); x_.XCloseDisplay(dpy_); dpy_ = nullptr; return false; }

        win_ = x_.XCreateSimpleWindow(dpy_, root, 0, 0, (unsigned)w, (unsigned)h, 0,
                                      x_.XBlackPixel(dpy_, screen), x_.XBlackPixel(dpy_, screen));
        x_.XStoreName(dpy_, win_, title);
        long mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                    ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
        x_.XSelectInput(dpy_, win_, mask);

        // WM_DELETE_WINDOW -> clean close.
        wmDelete_ = x_.XInternAtom ? x_.XInternAtom(dpy_, "WM_DELETE_WINDOW", 0) : 0;
        if (wmDelete_) {
            x11::Atom protocols[1] = {wmDelete_};
            x_.XSetWMProtocols(dpy_, win_, protocols, 1);
        }
        x_.XMapWindow(dpy_, win_);

        if (!x_.glXMakeCurrent(dpy_, (glx::GLXDrawable)win_, ctx_)) {
            fprintf(stderr, "[aw] glXMakeCurrent failed\n");
            return false;
        }

        // Resolve GL entry points.
        auto proc = [this](const char* name) -> void* {
            void* p = nullptr;
            if (x_.glXGetProcAddress) p = x_.glXGetProcAddress(reinterpret_cast<const unsigned char*>(name));
            if (!p && x_.hGL) p = dlsym(x_.hGL, name);
            return p;
        };
        if (!loadGL(proc)) {
            fprintf(stderr, "[aw] failed to load OpenGL 3.3 core symbols\n");
            return false;
        }
        refreshMonitorInfo();
        fprintf(stderr, "[aw] window: %dx%d client, desktop %dx%d, xrandr %s\n",
                width_, height_, desktopW_, desktopH_, x_.hasRandR() ? "yes" : "no");
        fprintf(stderr, "[aw] renderer: %s | %s\n",
                gl.GetString ? (const char*)gl.GetString(GL_RENDERER) : "?",
                gl.GetString ? (const char*)gl.GetString(GL_VERSION) : "?");
        return true;
    }

    bool frame(FrameInput& in) override {
        if (!dpy_) return false;
        // Clear edge-triggered mouse events from last frame.
        for (int i = 0; i < 8; ++i) { in.mousePressed[i] = false; in.mouseReleased[i] = false; }
        in.mouseDX = 0.0f; in.mouseDY = 0.0f;
        in.width = width_; in.height = height_;

        // Drain the event queue. XEvent is opaque here; we keep a raw buffer of
        // sufficient size (XEvent is 192 bytes on 64-bit) and decode the known
        // record layouts above.
        alignas(8) char evbuf[256];
        while (x_.XPending(dpy_) > 0) {
            x11::XEvent* ev = reinterpret_cast<x11::XEvent*>(evbuf);
            x_.XNextEvent(dpy_, ev);
            int type = *reinterpret_cast<const int*>(evbuf);
            switch (type) {
                case KeyPress: {
                    auto* ke = reinterpret_cast<XKeyEvent*>(evbuf);
                    x11::KeySym ks = x_.XLookupKeysym(ke, 0);
                    in.keys[mapKeysym(uint32_t(ks))] = 1;
                    break;
                }
                case KeyRelease: {
                    auto* ke = reinterpret_cast<XKeyEvent*>(evbuf);
                    x11::KeySym ks = x_.XLookupKeysym(ke, 0);
                    in.keys[mapKeysym(uint32_t(ks))] = 0;
                    break;
                }
                case ButtonPress: {
                    auto* be = reinterpret_cast<XButtonEvent*>(evbuf);
                    mouseX_ = be->x; mouseY_ = be->y;
                    if (be->button >= 1 && be->button <= 8) in.mousePressed[be->button - 1] = true;
                    break;
                }
                case ButtonRelease: {
                    auto* be = reinterpret_cast<XButtonEvent*>(evbuf);
                    mouseX_ = be->x; mouseY_ = be->y;
                    if (be->button >= 1 && be->button <= 8) in.mouseReleased[be->button - 1] = true;
                    break;
                }
                case MotionNotify: {
                    auto* me = reinterpret_cast<XMotionEvent*>(evbuf);
                    mouseX_ = me->x; mouseY_ = me->y;
                    if (captured_) {
                        // Window-relative coordinates: (x, y) describe the
                        // pointer relative to the event window (the game
                        // window, due to the pointer grab), so the delta from
                        // the centre we warp to is correct no matter where the
                        // window sits on the screen.
                        int cx = width_ / 2, cy = height_ / 2;
                        in.mouseDX += float(me->x - warpX_);
                        in.mouseDY += float(me->y - warpY_);
                        warpX_ = cx; warpY_ = cy;
                        x_.XWarpPointer(dpy_, 0, win_, 0, 0, 0, 0, cx, cy);
                    }
                    break;
                }
                case ClientMessage: {
                    auto* cm = reinterpret_cast<XClientMessageEvent*>(evbuf);
                    if (wmDelete_ && static_cast<unsigned long>(cm->data[0]) == wmDelete_)
                        in.shouldQuit = true;
                    break;
                }
                case ConfigureNotify: {
                    auto* ce = reinterpret_cast<XConfigureEvent*>(evbuf);
                    if (ce->width > 0 && ce->height > 0) {
                        width_ = ce->width;
                        height_ = ce->height;
                    }
                    break;
                }
                default: break;
            }
        }

        in.mouseX = float(mouseX_);
        in.mouseY = float(mouseY_);
        // Note: Escape is a normal key now (the game toggles the settings menu
        // on it); only the window-close button sets shouldQuit.
        return !in.shouldQuit;
    }

    void swapBuffers() override {
        if (dpy_ && x_.glXSwapBuffers) x_.glXSwapBuffers(dpy_, (glx::GLXDrawable)win_);
    }

    // vsync control for benchmarking: "0" asks the driver to present as fast as
    // it can. Any of the three extension spellings will do.
    bool setSwapInterval(int interval) override {
        if (!dpy_) return false;
        if (x_.glXSwapIntervalEXT) {
            x_.glXSwapIntervalEXT(dpy_, (glx::GLXDrawable)win_, interval);
            return true;
        }
        if (x_.glXSwapIntervalMESA) return x_.glXSwapIntervalMESA(interval) == 0;
        if (x_.glXSwapIntervalSGI) return x_.glXSwapIntervalSGI(interval) == 0;
        return false;
    }

    void shutdown() override {
        setCursorCaptured(false);
        if (dpy_) {
            if (ctx_ && x_.glXDestroyContext) x_.glXDestroyContext(dpy_, ctx_);
            if (x_.XCloseDisplay) x_.XCloseDisplay(dpy_);
        }
        dpy_ = nullptr; ctx_ = nullptr;
        if (x_.hRandR) { dlclose(x_.hRandR); x_.hRandR = nullptr; }
        if (x_.hGL) { dlclose(x_.hGL); x_.hGL = nullptr; }
        if (x_.hX11) { dlclose(x_.hX11); x_.hX11 = nullptr; }
    }

    BackendInfo info() const override {
        BackendInfo b;
        b.hasWindow = dpy_ != nullptr;
        b.hasGL = gl.ready;
        b.name = "x11+opengl";
        return b;
    }

    void setCursorCaptured(bool captured) override {
        if (!dpy_ || captured_ == captured) return;
        captured_ = captured;
        if (captured) {
            // Hide the cursor with a 1x1 blank pixmap cursor and grab the pointer
            // so we receive motion events even while warping it back to center.
            x11::Cursor none = 0;
            if (x_.XCreatePixmap && x_.XCreatePixmapCursor) {
                x11::Pixmap pm = x_.XCreatePixmap(dpy_, win_, 1, 1, 1);
                static x11::XColor black{};
                none = x_.XCreatePixmapCursor(dpy_, pm, pm, &black, &black, 0, 0);
                if (pm && x_.XFreePixmap) x_.XFreePixmap(dpy_, pm);
            } else if (x_.XCreateFontCursor) {
                none = x_.XCreateFontCursor(dpy_, 0);
            }
            if (none && x_.XDefineCursor) x_.XDefineCursor(dpy_, win_, none);
            if (x_.XGrabPointer) x_.XGrabPointer(dpy_, win_, 0,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, win_, none, 0 /*CurrentTime*/);
            warpX_ = width_ / 2; warpY_ = height_ / 2;
            if (x_.XWarpPointer) x_.XWarpPointer(dpy_, 0, win_, 0, 0, 0, 0, warpX_, warpY_);
        } else {
            if (x_.XUngrabPointer) x_.XUngrabPointer(dpy_, 0 /*CurrentTime*/);
            if (x_.XUndefineCursor) x_.XUndefineCursor(dpy_, win_);
        }
        if (x_.XSync) x_.XSync(dpy_, 0);
    }

    void resize(int w, int h) override {
        if (w <= 0 || h <= 0) return;
        // A windowed window must never be bigger than the screen (the
        // resolution setting asks for a preset, the display decides how much
        // of it fits).
        int maxW = 0, maxH = 0;
        if (maxWindowSize(maxW, maxH)) {
            if (w > maxW) w = maxW;
            if (h > maxH) h = maxH;
        }
        // Remember what was asked for (e.g. 1920x1080) while the window itself
        // gets what the screen can show, so moving to a bigger display re-fits it
        // up to the requested standard resolution.
        reqW_ = w;
        reqH_ = h;
        // Apply immediately so the next frame already uses the new size (the
        // async ConfigureNotify confirms it afterwards).
        width_ = w; height_ = h;
        pendingW_ = w; pendingH_ = h;
        mouseX_ = w / 2; mouseY_ = h / 2;
        if (dpy_ && x_.XResizeWindow) {
            x_.XResizeWindow(dpy_, win_, (unsigned)w, (unsigned)h);
            // Re-center on the (active) screen every time the size changes.
            if (mode_ == DisplayMode::Windowed && x_.XMoveWindow) {
                WorkArea work;
                work.width = desktopW_;
                work.height = desktopH_;
                int mx = 0, my = 0;
                centerWindowIn(work, w, h, mx, my);
                x_.XMoveWindow(dpy_, win_, mx, my);
            }
            if (x_.XSync) x_.XSync(dpy_, 0);
        }
    }

    // Largest windowed client size that still fits on the screen. X11 exposes
    // the raw screen size (not the work area); subtract a conservative
    // allowance for window decorations, which the WM adds on top of the client
    // area and whose size is only known after the window is mapped.
    bool maxWindowSize(int& w, int& h) const override {
        w = 0; h = 0;
        if (!dpy_ || !x_.XDisplayWidth || !x_.XDisplayHeight) return false;
        int screen = x_.XDefaultScreen(dpy_);
        int sw = x_.XDisplayWidth(dpy_, screen) - 8;
        int sh = x_.XDisplayHeight(dpy_, screen) - 48;
        if (sw <= 0 || sh <= 0) return false;
        w = sw > 320 ? sw : 320;
        h = sh > 200 ? sh : 200;
        return true;
    }

    bool clientSize(int& w, int& h) const override {
        w = width_; h = height_;
        return w > 0 && h > 0;
    }

    // ---- display modes -----------------------------------------------------
    bool applyDisplayMode(DisplayMode mode, int w, int h, int refreshHz) override {
        pendingMode_ = mode;
        if (!dpy_) return true;
        switch (mode) {
            case DisplayMode::Windowed: {
                restoreDesktopMode();
                setFullscreenHint(false);
                mode_ = DisplayMode::Windowed;
                refreshMonitorInfo();
                resize(w > 0 ? w : (width_ > 0 ? width_ : pendingW_),
                       h > 0 ? h : (height_ > 0 ? height_ : pendingH_));
                return true;
            }
            case DisplayMode::Borderless: {
                restoreDesktopMode();
                mode_ = DisplayMode::Borderless;
                refreshMonitorInfo();
                setFullscreenHint(true);
                fillScreenRect();
                return true;
            }
            case DisplayMode::Exclusive:
            default: {
                if (!x_.hasRandR()) {
                    fprintf(stderr, "[aw] XRandR unavailable - no exclusive fullscreen\n");
                    return false;
                }
                if (!switchDisplayMode(w, h, refreshHz)) {
                    restoreDesktopMode();
                    return false;
                }
                mode_ = DisplayMode::Exclusive;
                desktopW_ = w; desktopH_ = h;   // coherent while we are switched
                setFullscreenHint(true);
                fillScreenRect();
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

    void* loadGLProc(const char* name) override {
        void* p = nullptr;
        if (x_.glXGetProcAddress) p = x_.glXGetProcAddress(reinterpret_cast<const unsigned char*>(name));
        if (!p && x_.hGL) p = dlsym(x_.hGL, name);
        return p;
    }

private:
    // ---- display-mode helpers ----------------------------------------------
    void refreshMonitorInfo() {
        if (mode_ == DisplayMode::Exclusive) return;   // keep the desktop cache
        if (!dpy_ || !x_.XDisplayWidth || !x_.XDisplayHeight) return;
        int screen = x_.XDefaultScreen(dpy_);
        int sw = x_.XDisplayWidth(dpy_, screen);
        int sh = x_.XDisplayHeight(dpy_, screen);
        if (sw > 0 && sh > 0) { desktopW_ = sw; desktopH_ = sh; }
        modeCacheCount_ = -1;
    }

    // EWMH fullscreen (borderless / exclusive both cover the whole screen; the
    // difference is whether the display mode itself was switched).
    void setFullscreenHint(bool on) {
        if (fullscreenHint_ == on) return;
        if (!x_.XSendEvent || !x_.XInternAtom) return;
        x11::Atom wmState = x_.XInternAtom(dpy_, "_NET_WM_STATE", 0);
        x11::Atom fs = x_.XInternAtom(dpy_, "_NET_WM_STATE_FULLSCREEN", 0);
        if (!wmState || !fs) return;
        struct FullscreenMsg {
            int type; unsigned long serial; int send_event; x11::Display* display;
            x11::Window window; x11::Atom message_type; int format; long data[5];
        } msg{};
        msg.type = ClientMessage;
        msg.window = win_;
        msg.message_type = wmState;
        msg.format = 32;
        msg.data[0] = on ? 1 : 0;   // _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE
        msg.data[1] = (long)fs;
        int screen = x_.XDefaultScreen(dpy_);
        x11::Window root = x_.XRootWindow(dpy_, screen);
        constexpr long kSubstructure = (1L << 20) | (1L << 21);  // Redirect|Notify
        x_.XSendEvent(dpy_, root, 0, kSubstructure, reinterpret_cast<x11::XEvent*>(&msg));
        int sw = desktopW_, sh = desktopH_;
        if (on && x_.XResizeWindow && sw > 0 && sh > 0)
            x_.XResizeWindow(dpy_, win_, (unsigned)sw, (unsigned)sh);
        if (x_.XSync) x_.XSync(dpy_, 0);
        fullscreenHint_ = on;
    }

    void fillScreenRect() {
        if (x_.XResizeWindow && desktopW_ > 0 && desktopH_ > 0)
            x_.XResizeWindow(dpy_, win_, (unsigned)desktopW_, (unsigned)desktopH_);
        if (x_.XMoveWindow) x_.XMoveWindow(dpy_, win_, 0, 0);
        if (x_.XSync) x_.XSync(dpy_, 0);
        width_ = desktopW_ > 0 ? desktopW_ : width_;
        height_ = desktopH_ > 0 ? desktopH_ : height_;
    }

    // Display modes the driver reports through XRandR (deduplicated, cached).
    int enumerateModes() const {
        if (modeCacheCount_ >= 0) return modeCacheCount_;
        modeCacheCount_ = 0;
        if (!dpy_ || !x_.hasRandR()) return modeCacheCount_;
        int screen = x_.XDefaultScreen(dpy_);
        x11::Window root = x_.XRootWindow(dpy_, screen);
        x11::XRRScreenResources* res = x_.XRRGetScreenResourcesCurrent(dpy_, root);
        if (!res) return modeCacheCount_;
        for (int i = 0; i < res->nmode && modeCacheCount_ < kMaxDriverModes; ++i) {
            const x11::XRRModeInfo& m = res->modes[i];
            if (m.width < kMinModeW || m.height < kMinModeH) continue;
            if (m.hTotal == 0 || m.vTotal == 0) continue;
            double hz = double(m.dotClock) / (double(m.hTotal) * double(m.vTotal));
            DisplayModeInfo info;
            info.width = int(m.width);
            info.height = int(m.height);
            info.refreshHz = int(hz + 0.5);
            bool dup = false;
            for (int j = 0; j < modeCacheCount_; ++j) {
                if (modeCache_[j].width == info.width && modeCache_[j].height == info.height &&
                    modeCache_[j].refreshHz == info.refreshHz) { dup = true; break; }
            }
            if (dup) continue;
            modeCache_[modeCacheCount_++] = info;
        }
        x_.XRRFreeScreenResources(res);
        // Ascending by area, then refresh rate.
        for (int i = 1; i < modeCacheCount_; ++i) {
            DisplayModeInfo key = modeCache_[i];
            int j = i - 1;
            while (j >= 0 && (modeCache_[j].width * modeCache_[j].height > key.width * key.height ||
                              (modeCache_[j].width * modeCache_[j].height == key.width * key.height &&
                               modeCache_[j].refreshHz > key.refreshHz))) {
                modeCache_[j + 1] = modeCache_[j];
                --j;
            }
            modeCache_[j + 1] = key;
        }
        return modeCacheCount_;
    }

    // The crtc driving the primary output (falls back to the first one).
    x11::RRCrtc primaryCrtc(x11::XRRScreenResources* res, x11::Window root) const {
        x11::RROutput out = x_.XRRGetOutputPrimary ? x_.XRRGetOutputPrimary(dpy_, root) : 0;
        if (out && x_.XRRGetOutputInfo && x_.XRRFreeOutputInfo) {
            x11::XRROutputInfo* oi = x_.XRRGetOutputInfo(dpy_, res, out);
            if (oi) {
                x11::RRCrtc crtc = oi->crtc;
                x_.XRRFreeOutputInfo(oi);
                if (crtc) return crtc;
            }
        }
        return res->ncrtc > 0 ? res->crtcs[0] : 0;
    }

    bool switchDisplayMode(int w, int h, int refreshHz) {
        int screen = x_.XDefaultScreen(dpy_);
        x11::Window root = x_.XRootWindow(dpy_, screen);
        x11::XRRScreenResources* res = x_.XRRGetScreenResourcesCurrent(dpy_, root);
        if (!res) return false;
        x11::RRCrtc crtc = primaryCrtc(res, root);
        if (!crtc) { x_.XRRFreeScreenResources(res); return false; }
        x11::XRRCrtcInfo* ci = x_.XRRGetCrtcInfo(dpy_, res, crtc);
        if (!ci) { x_.XRRFreeScreenResources(res); return false; }

        // Remember the desktop configuration once, for restoreDesktopMode().
        if (!modeSaved_) {
            savedCrtc_ = crtc;
            savedMode_ = ci->mode;
            savedRotation_ = ci->rotation;
            savedX_ = ci->x;
            savedY_ = ci->y;
            // The outputs attached to the crtc have to be handed back to
            // XRRSetCrtcConfig when restoring: passing none would switch the
            // display off instead of restoring the desktop.
            savedOutput_ = (ci->noutput > 0 && ci->outputs) ? ci->outputs[0] : 0;
            modeSaved_ = true;
        }

        // Pick the requested mode (closest refresh when 0 = default).
        x11::RRMode pick = 0;
        int bestHz = -1;
        for (int i = 0; i < res->nmode; ++i) {
            const x11::XRRModeInfo& m = res->modes[i];
            if (int(m.width) != w || int(m.height) != h) continue;
            int hz = (m.hTotal && m.vTotal)
                         ? int(double(m.dotClock) / (double(m.hTotal) * double(m.vTotal)) + 0.5)
                         : 0;
            if (refreshHz > 0 && hz != refreshHz) continue;
            if (hz <= bestHz) continue;
            bestHz = hz;
            pick = m.id;
        }
        if (!pick) {
            x_.XRRFreeCrtcInfo(ci);
            x_.XRRFreeScreenResources(res);
            fprintf(stderr, "[aw] XRandR has no mode %dx%d@%d\n", w, h, refreshHz);
            return false;
        }
        x11::RROutput outputs[1] = {0};
        int noutputs = 0;
        if (ci->noutput > 0 && ci->outputs) { outputs[0] = ci->outputs[0]; noutputs = 1; }
        int ok = x_.XRRSetCrtcConfig(dpy_, res, crtc, 0 /*CurrentTime*/, ci->x, ci->y,
                                     pick, ci->rotation, outputs, noutputs);
        x_.XRRFreeCrtcInfo(ci);
        x_.XRRFreeScreenResources(res);
        if (ok != 0 /* Success */) {
            fprintf(stderr, "[aw] XRRSetCrtcConfig failed\n");
            return false;
        }
        if (x_.XSync) x_.XSync(dpy_, 0);
        modeCacheCount_ = -1;
        return true;
    }

    void restoreDesktopMode() {
        if (mode_ != DisplayMode::Exclusive || !modeSaved_ || !dpy_ || !x_.hasRandR()) {
            if (mode_ == DisplayMode::Exclusive) mode_ = DisplayMode::Windowed;
            return;
        }
        int screen = x_.XDefaultScreen(dpy_);
        x11::Window root = x_.XRootWindow(dpy_, screen);
        x11::XRRScreenResources* res = x_.XRRGetScreenResourcesCurrent(dpy_, root);
        if (res) {
            x11::RROutput outputs[1] = {savedOutput_};
            x_.XRRSetCrtcConfig(dpy_, res, savedCrtc_, 0, savedX_, savedY_, savedMode_,
                                savedRotation_, outputs, savedOutput_ ? 1 : 0);
            x_.XRRFreeScreenResources(res);
            if (x_.XSync) x_.XSync(dpy_, 0);
        }
        mode_ = DisplayMode::Windowed;   // the caller re-applies the real one
        modeCacheCount_ = -1;
        refreshMonitorInfo();
    }

    XLib x_;
    x11::Display* dpy_ = nullptr;
    x11::Window win_ = 0;
    glx::GLXFBConfig fbcfg_ = nullptr;
    glx::GLXContext ctx_ = nullptr;
    x11::Atom wmDelete_ = 0;
    int width_ = 0, height_ = 0;
    int warpX_ = 0, warpY_ = 0;
    int mouseX_ = 0, mouseY_ = 0;
    bool captured_ = false;
    // Display state.
    static constexpr int kMaxDriverModes = 256;
    static constexpr int kMinModeW = 640, kMinModeH = 400;
    DisplayMode mode_ = DisplayMode::Windowed;
    DisplayMode pendingMode_ = DisplayMode::Windowed;
    int pendingW_ = 1280, pendingH_ = 720;
    // Client size the settings asked for (may exceed the fitted window).
    int reqW_ = 0, reqH_ = 0;
    bool fullscreenHint_ = false;
    int desktopW_ = 0, desktopH_ = 0;
    bool modeSaved_ = false;
    x11::RRCrtc savedCrtc_ = 0;
    x11::RRMode savedMode_ = 0;
    x11::Rotation savedRotation_ = 1;   // RR_Rotate_0
    int savedX_ = 0, savedY_ = 0;
    x11::RROutput savedOutput_ = 0;     // output to re-attach when restoring
    mutable DisplayModeInfo modeCache_[kMaxDriverModes]{};
    mutable int modeCacheCount_ = -1;
};

Platform* createPlatform() { return new PlatformX11(); }

}  // namespace aw

#endif  // !_WIN32
