/// @file app_x11.cpp
/// @brief Xlib window + EGL/OpenGL 3.3 context + ImGui GL3 renderer.

// X11 headers are included before ImGui because ImGui `#undef`s
// `Status` (X11 leaks it as a typedef that collides with one of
// ImGui's struct members). Subsequent X11 headers require that
// symbol, so X11 must be fully parsed first.
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <EGL/egl.h>
#include <GL/gl.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include "orchestrator/orchestrator.hpp"
#include "platform/app.hpp"
#include "theme/logo.hpp"
#include "theme/palette.hpp"
#include "theme/style.hpp"
#include "ui/shell.hpp"
#include "util/image_loader.hpp"

#include "imgui_impl_x11.hpp"
#include "logo_mark_48.h"

#include <cstdint>
#include <cstdio>

namespace unio_ui::platform {

namespace {

/// @brief Bundle of X11 + EGL state owned by the run loop.
struct X11App {
    Display*   dpy       = nullptr;
    Window     win       = 0;
    Atom       wm_delete = 0;
    EGLDisplay egl_dpy   = EGL_NO_DISPLAY;
    EGLContext egl_ctx   = EGL_NO_CONTEXT;
    EGLSurface egl_surf  = EGL_NO_SURFACE;
    int        width        = 0;
    int        height       = 0;
    bool       should_close = false;
};

/// @brief Open a top-level Xlib window and register the
/// WM_DELETE_WINDOW protocol.
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
        0, CopyFromParent, InputOutput, CopyFromParent,
        CWEventMask | CWBackPixel, &swa);

    XStoreName(app.dpy, app.win, cfg.window_title.c_str());

    XSizeHints hints{};
    hints.flags = PMinSize;
    hints.min_width  = cfg.min_width;
    hints.min_height = cfg.min_height;
    XSetWMNormalHints(app.dpy, app.win, &hints);

    app.wm_delete = XInternAtom(app.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app.dpy, app.win, &app.wm_delete, 1);

    XMapWindow(app.dpy, app.win);
    XFlush(app.dpy);

    app.width  = cfg.window_width;
    app.height = cfg.window_height;
    return true;
}

/// @brief Create an OpenGL 3.3 core-profile context via EGL and
/// bind it to the window's EGL surface.
bool init_egl(X11App& app) {
    app.egl_dpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(app.dpy));
    if (app.egl_dpy == EGL_NO_DISPLAY) return false;

    EGLint emajor = 0, eminor = 0;
    if (!eglInitialize(app.egl_dpy, &emajor, &eminor)) return false;
    if (!eglBindAPI(EGL_OPENGL_API)) return false;

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      0,
        EGL_STENCIL_SIZE,    0,
        EGL_NONE,
    };
    EGLConfig egl_cfg = nullptr;
    EGLint    num_cfg = 0;
    if (!eglChooseConfig(app.egl_dpy, cfg_attribs, &egl_cfg, 1, &num_cfg) ||
        num_cfg == 0) {
        return false;
    }

    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
            EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    app.egl_ctx = eglCreateContext(app.egl_dpy, egl_cfg,
                                   EGL_NO_CONTEXT, ctx_attribs);
    if (app.egl_ctx == EGL_NO_CONTEXT) return false;

    app.egl_surf = eglCreateWindowSurface(
        app.egl_dpy, egl_cfg,
        reinterpret_cast<EGLNativeWindowType>(app.win), nullptr);
    if (app.egl_surf == EGL_NO_SURFACE) return false;

    eglMakeCurrent(app.egl_dpy, app.egl_surf, app.egl_surf, app.egl_ctx);
    eglSwapInterval(app.egl_dpy, 1);
    return true;
}

/// @brief Drain the Xlib event queue and forward each event into ImGui.
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
            app.width  = ev.xconfigure.width;
            app.height = ev.xconfigure.height;
        }
        x11::imgui_impl_x11_process_event(ev);
    }
}

/// @brief Run one full ImGui frame + GL clear + present.
void render_frame(X11App& app, ui::Shell& shell) {
    x11::imgui_impl_x11_new_frame();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    shell.render();

    ImGui::Render();

    glViewport(0, 0, app.width, app.height);
    const ImVec4 bg = theme::palette::paper_bg;
    glClearColor(bg.x, bg.y, bg.z, bg.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    eglSwapBuffers(app.egl_dpy, app.egl_surf);
}

/// @brief Release all GPU + window resources in reverse init order.
void shutdown(X11App& app) {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplOpenGL3_Shutdown();
        x11::imgui_impl_x11_shutdown();
        ImGui::DestroyContext();
    }
    if (app.egl_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(app.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (app.egl_surf != EGL_NO_SURFACE) eglDestroySurface(app.egl_dpy, app.egl_surf);
        if (app.egl_ctx  != EGL_NO_CONTEXT) eglDestroyContext(app.egl_dpy, app.egl_ctx);
        eglTerminate(app.egl_dpy);
    }
    if (app.win) XDestroyWindow(app.dpy, app.win);
    if (app.dpy) XCloseDisplay(app.dpy);
}

/// @brief Upload an RGBA8 buffer to a 2D GL texture with linear
/// filtering and clamp-to-edge wrapping.
GLuint upload_rgba_texture(const unsigned char* pixels, int w, int h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

/// @brief Decode and upload the embedded logo; register with the theme.
void load_logo_texture() {
    auto img = unio_ui::decode_image(logo_mark_48_data, logo_mark_48_size);
    if (!img.pixels) return;
    const GLuint tex = upload_rgba_texture(img.pixels, img.width, img.height);
    unio_ui::free_decoded_image(img);
    theme::register_logo_texture(static_cast<ImTextureID>(tex), 48, 48);
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
    theme::load_fonts();
    theme::apply_style();
    if (!x11::imgui_impl_x11_init(app.dpy, app.win) ||
        !ImGui_ImplOpenGL3_Init("#version 330 core")) {
        std::fprintf(stderr, "ImGui backend init failed\n");
        shutdown(app);
        return 1;
    }

    load_logo_texture();
    auto orch = orchestrator::make_stub();
    ui::Shell shell(*orch);
    while (!app.should_close) {
        pump_events(app);
        render_frame(app, shell);
    }

    shutdown(app);
    return 0;
}

}  // namespace unio_ui::platform
