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
#include "platform/identify_overlay.hpp"
#include "theme/logo.hpp"
#include "theme/palette.hpp"
#include "theme/style.hpp"
#include "ui/shell.hpp"
#include "util/image_loader.hpp"

#include "imgui_impl_x11.hpp"
#include "logo_mark.h"

#include <cstdint>
#include <cstdio>

namespace xorio::platform {

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
    // background_pixmap = None disables X-server-side window
    // clears entirely. Without this, every resize-driven Expose
    // first paints the newly-exposed strip with the window's
    // background pixel (black, by default) before our render
    // thread's next eglSwapBuffers lands — which the user sees
    // as a black band along the resized edge.
    swa.background_pixmap = None;

    app.win = XCreateWindow(
        app.dpy, root,
        0, 0, cfg.window_width, cfg.window_height,
        0, CopyFromParent, InputOutput, CopyFromParent,
        CWEventMask | CWBackPixmap, &swa);

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

    // Vsync OFF. With vsync ON, eglSwapBuffers blocks up to one
    // vblank (~16 ms at 60 Hz) — during that window the
    // compositor shows the resized window with the old, smaller
    // front buffer + a black gutter, which the user sees as the
    // resize flicker. The static UI's render cost is small enough
    // that running uncapped is fine; tearing on a typical desktop
    // composited X session is invisible because the compositor
    // re-syncs on its own cadence.
    eglSwapInterval(app.egl_dpy, 0);

    // Ask the driver to keep the back buffer's contents after a
    // swap. If the config supports it the next-frame render
    // starts from a valid image instead of "undefined", which
    // covers the gap between resize ticks before our re-render
    // lands. Drivers that don't support it silently no-op the
    // call (the EGL spec lets eglSurfaceAttrib fail without
    // breaking the surface).
    eglSurfaceAttrib(app.egl_dpy, app.egl_surf,
                     EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED);
    return true;
}

/// @brief Drain the Xlib event queue and forward each event into
/// ImGui. Renders a fresh frame synchronously on every
/// ConfigureNotify so a fast resize never leaves the
/// newly-exposed strip showing the un-rendered EGL backbuffer
/// (the source of the black flicker the user saw).
template <typename RenderFn>
void pump_events(X11App& app, RenderFn&& render_now) {
    while (XPending(app.dpy) > 0) {
        XEvent ev;
        XNextEvent(app.dpy, &ev);
        if (ev.type == ClientMessage &&
            static_cast<Atom>(ev.xclient.data.l[0]) == app.wm_delete) {
            app.should_close = true;
            continue;
        }
        if (ev.type == ConfigureNotify) {
            const int new_w = ev.xconfigure.width;
            const int new_h = ev.xconfigure.height;
            if (new_w != app.width || new_h != app.height) {
                app.width  = new_w;
                app.height = new_h;
                x11::imgui_impl_x11_process_event(ev);
                render_now();
                continue;
            }
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
    auto img = xorio::decode_image(logo_mark_data, logo_mark_size);
    if (!img.pixels) return;
    const GLuint tex = upload_rgba_texture(img.pixels, img.width, img.height);
    const int w = img.width, h = img.height;
    xorio::free_decoded_image(img);
    theme::register_logo_texture(static_cast<ImTextureID>(tex), w, h);
}

}  // namespace

/// @brief Process-wide non-fatal X11 error handler. Xlib's
/// default handler calls `exit(1)` when *any* protocol error
/// reaches the per-Display error queue, which means an
/// asynchronous BadWindow / BadDrawable from a teardown race in
/// the identify-overlay worker tears the whole app down. We log
/// the error string + return 0 so the app keeps running.
int x_error_handler(::Display* dpy, XErrorEvent* ev) {
    char buf[256] = {};
    XGetErrorText(dpy, ev->error_code, buf, sizeof(buf));
    std::fprintf(stderr,
                 "X11 error: %s (request=%u, minor=%u, resource=0x%lx)\n",
                 buf, static_cast<unsigned>(ev->request_code),
                 static_cast<unsigned>(ev->minor_code),
                 ev->resourceid);
    return 0;
}

/// @brief Process-wide non-fatal X11 I/O error handler. The
/// default exits with status 1; ours logs + returns so the loss
/// of a worker's Display connection (server hiccup, broken pipe)
/// doesn't kill the main UI thread.
int x_io_error_handler(::Display* /*dpy*/) {
    std::fprintf(stderr, "X11 I/O error — connection dropped on a worker\n");
    return 0;
}

int run(const AppConfig& cfg) {
    // XInitThreads() must be the very first Xlib call. The
    // identify-overlay subsystem opens its own Display* on a
    // worker thread; without this, Xlib's shared internal state
    // (atom cache, error handler list, lock chains) corrupts
    // when used concurrently with the main thread's Display.
    XInitThreads();

    // Defang Xlib's lethal error handlers — see the doc-comments
    // above each. Without this, any async BadWindow from the
    // identify worker's teardown sequence brings the whole app
    // down because Xlib's default handler calls exit(1).
    XSetErrorHandler(x_error_handler);
    XSetIOErrorHandler(x_io_error_handler);

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

    // Identify overlays fire from a single callback so a local
    // click and a remote-peer-bump take the exact same code path.
    std::unique_ptr<orchestrator::IOrchestrator> orch;
    orchestrator::OrchestratorCallbacks cbs;
    cbs.on_identify_request = [&orch]() {
        if (!orch) return;
        const std::string local_mid = orch->local_machine_id();
        std::vector<platform::IdentifyOverlay> overlays;
        for (const auto& d : orch->displays()) {
            if (d.machine_id != local_mid) continue;
            platform::IdentifyOverlay o;
            o.x      = d.global_x;
            o.y      = d.global_y;
            o.width  = d.width;
            o.height = d.height;
            o.number = d.number;
            o.label  = d.machine_id;
            overlays.push_back(std::move(o));
        }
        platform::show_identify_overlays(overlays, 3000);
    };
    orch = orchestrator::make_mock(cbs);
    ui::Shell shell(*orch);
    while (!app.should_close) {
        pump_events(app, [&] { render_frame(app, shell); });
        render_frame(app, shell);
    }

    shutdown(app);
    return 0;
}

}  // namespace xorio::platform
