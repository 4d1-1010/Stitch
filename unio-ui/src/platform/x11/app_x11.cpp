/*! @file app_x11.cpp
 *  @brief X11 + EGL + OpenGL 3.3 platform backend.
 *
 *  Opens a window, stands up an EGL + GL3 context, drives ImGui
 *  via our in-house `imgui_impl_x11` + the upstream
 *  `imgui_impl_opengl3` renderer, and renders the ImGui demo
 *  window every frame as a smoke test.
 *
 *  Dependencies: libX11, libEGL, libGL.
 */

// X11 headers FIRST — see the note in imgui_impl_x11.cpp for why.
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <EGL/egl.h>
#include <GL/gl.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include "../app.hpp"
#include "../../screens/shell.hpp"
#include "../../theme.hpp"
#include "imgui_impl_x11.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace unio_ui::platform {

namespace {

struct X11App {
    Display* dpy = nullptr;
    Window win = 0;
    Atom wm_delete = 0;
    EGLDisplay egl_dpy = EGL_NO_DISPLAY;
    EGLContext egl_ctx = EGL_NO_CONTEXT;
    EGLSurface egl_surf = EGL_NO_SURFACE;
    int width = 0;
    int height = 0;
    bool should_close = false;
};

bool init_x11(X11App& app, const AppConfig& cfg) {
    app.dpy = XOpenDisplay(nullptr);
    if (!app.dpy) {
        std::fprintf(stderr, "XOpenDisplay failed (no DISPLAY?)\n");
        return false;
    }

    const int screen = DefaultScreen(app.dpy);
    const Window root = RootWindow(app.dpy, screen);

    XSetWindowAttributes swa{};
    swa.event_mask = ExposureMask | StructureNotifyMask |
                     KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | FocusChangeMask |
                     EnterWindowMask | LeaveWindowMask;
    swa.background_pixel = 0;

    app.win = XCreateWindow(
        app.dpy, root,
        0, 0, cfg.window_width, cfg.window_height,
        0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWEventMask | CWBackPixel, &swa);

    XStoreName(app.dpy, app.win, cfg.window_title.c_str());

    XSizeHints hints{};
    hints.flags = PMinSize;
    hints.min_width = cfg.min_width;
    hints.min_height = cfg.min_height;
    XSetWMNormalHints(app.dpy, app.win, &hints);

    app.wm_delete = XInternAtom(app.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app.dpy, app.win, &app.wm_delete, 1);

    XMapWindow(app.dpy, app.win);
    XFlush(app.dpy);

    app.width = cfg.window_width;
    app.height = cfg.window_height;
    return true;
}

bool init_egl(X11App& app) {
    app.egl_dpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(app.dpy));
    if (app.egl_dpy == EGL_NO_DISPLAY) {
        std::fprintf(stderr, "eglGetDisplay failed\n");
        return false;
    }
    EGLint emajor = 0, eminor = 0;
    if (!eglInitialize(app.egl_dpy, &emajor, &eminor)) {
        std::fprintf(stderr, "eglInitialize failed\n");
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        std::fprintf(stderr, "eglBindAPI(OPENGL) failed\n");
        return false;
    }

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,     8,
        EGL_GREEN_SIZE,   8,
        EGL_BLUE_SIZE,    8,
        EGL_ALPHA_SIZE,   8,
        EGL_DEPTH_SIZE,   0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE,
    };
    EGLConfig egl_cfg = nullptr;
    EGLint num_cfg = 0;
    if (!eglChooseConfig(app.egl_dpy, cfg_attribs, &egl_cfg, 1, &num_cfg) ||
        num_cfg == 0) {
        std::fprintf(stderr, "eglChooseConfig found no matching config\n");
        return false;
    }

    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
            EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    app.egl_ctx = eglCreateContext(app.egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (app.egl_ctx == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "eglCreateContext failed (no GL 3.3 core?)\n");
        return false;
    }

    app.egl_surf = eglCreateWindowSurface(
        app.egl_dpy, egl_cfg,
        reinterpret_cast<EGLNativeWindowType>(app.win), nullptr);
    if (app.egl_surf == EGL_NO_SURFACE) {
        std::fprintf(stderr, "eglCreateWindowSurface failed\n");
        return false;
    }
    eglMakeCurrent(app.egl_dpy, app.egl_surf, app.egl_surf, app.egl_ctx);
    eglSwapInterval(app.egl_dpy, 1);
    return true;
}

void pump_events(X11App& app) {
    while (XPending(app.dpy) > 0) {
        XEvent ev;
        XNextEvent(app.dpy, &ev);
        if (ev.type == ClientMessage &&
            static_cast<Atom>(ev.xclient.data.l[0]) == app.wm_delete) {
            app.should_close = true;
            continue;
        }
        if (ev.type == ConfigureNotify) {
            app.width = ev.xconfigure.width;
            app.height = ev.xconfigure.height;
        }
        unio_ui::platform::x11::imgui_impl_x11_process_event(ev);
    }
}

void render_frame(X11App& app, unio_ui::screens::Shell& shell) {
    unio_ui::platform::x11::imgui_impl_x11_new_frame();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    shell.render();

    ImGui::Render();

    glViewport(0, 0, app.width, app.height);
    // Paper background from ui_theme.py — PAPER_BG = "#f6f3ec".
    glClearColor(0xf6 / 255.f, 0xf3 / 255.f, 0xec / 255.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    eglSwapBuffers(app.egl_dpy, app.egl_surf);
}

void shutdown(X11App& app) {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplOpenGL3_Shutdown();
        unio_ui::platform::x11::imgui_impl_x11_shutdown();
        ImGui::DestroyContext();
    }
    if (app.egl_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(app.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (app.egl_surf != EGL_NO_SURFACE) {
            eglDestroySurface(app.egl_dpy, app.egl_surf);
        }
        if (app.egl_ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(app.egl_dpy, app.egl_ctx);
        }
        eglTerminate(app.egl_dpy);
    }
    if (app.win) {
        XDestroyWindow(app.dpy, app.win);
    }
    if (app.dpy) {
        XCloseDisplay(app.dpy);
    }
}

}  // namespace

int run(const AppConfig& cfg) {
    X11App app;
    if (!init_x11(app, cfg) || !init_egl(app)) {
        shutdown(app);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    unio_ui::theme::apply_style();
    if (!unio_ui::platform::x11::imgui_impl_x11_init(app.dpy, app.win) ||
        !ImGui_ImplOpenGL3_Init("#version 330 core")) {
        std::fprintf(stderr, "ImGui backend init failed\n");
        shutdown(app);
        return 1;
    }

    unio_ui::screens::Shell shell;
    while (!app.should_close) {
        pump_events(app);
        render_frame(app, shell);
    }

    shutdown(app);
    return 0;
}

}  // namespace unio_ui::platform
