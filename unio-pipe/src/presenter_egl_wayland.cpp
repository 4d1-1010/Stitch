// EGL-on-Wayland presenter (Linux Wayland). Zero-copy NV12 display.

#if defined(__linux__)

#include "presenter.h"
#include "latency_log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <drm/drm_fourcc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

namespace unio {

namespace {

// ── Shared globals ────────────────────────────────────────────

PFNEGLCREATEIMAGEKHRPROC g_eglCreateImageKHR = nullptr;
PFNEGLDESTROYIMAGEKHRPROC g_eglDestroyImageKHR = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_glEGLImageTargetTexture2DOES = nullptr;

const float kQuadVerts[] = {
    -1.f, -1.f, 0.f, 1.f,  1.f, -1.f, 1.f, 1.f,
    -1.f,  1.f, 0.f, 0.f,  1.f,  1.f, 1.f, 0.f,
};

const char* kVertexSrc = R"(
attribute vec2 a_pos; attribute vec2 a_tc; varying vec2 v_tc;
void main() { v_tc = a_tc; gl_Position = vec4(a_pos, 0.0, 1.0); }
)";

const char* kFragmentSrc = R"(
precision mediump float; varying vec2 v_tc;
uniform sampler2D u_y; uniform sampler2D u_uv;
void main() {
    float y = texture2D(u_y, v_tc).r;
    vec2 uv = texture2D(u_uv, v_tc).rg - vec2(0.5);
    y = (y - 16.0/255.0) * (255.0/219.0);
    float r = y + 1.402 * uv.y;
    float g = y - 0.344136 * uv.x - 0.714136 * uv.y;
    float b = y + 1.772 * uv.x;
    gl_FragColor = vec4(r, g, b, 1.0);
}
)";

GLuint CompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]{};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "unio-pipe: shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// ── Presenter class ───────────────────────────────────────────

class EglWaylandPresenter final : public Presenter {
public:
    EglWaylandPresenter() = default;
    ~EglWaylandPresenter() override { Shutdown(); }

    std::optional<std::string> Init(const Config& cfg) override;
    void Present(const DecodedFrame& frame) override;
    std::uint64_t FramesPresented() const override {
        return frames_presented_.load(std::memory_order_relaxed);
    }
    std::string_view Name() const override { return "egl-wayland"; }

private:
    struct CachedSurface {
        VASurfaceID id = 0;
        EGLImageKHR y_image  = EGL_NO_IMAGE_KHR;
        EGLImageKHR uv_image = EGL_NO_IMAGE_KHR;
    };

    std::optional<std::string> RunPresentLoop();
    std::optional<std::string> SetupGl();
    EGLImageKHR ImportPlane(int fd, std::uint32_t fourcc, int width, int height,
                             std::uint32_t offset, std::uint32_t pitch,
                             std::uint64_t modifier);
    bool UploadSurface(const DecodedFrame& frame);
    void RenderFrame(const DecodedFrame& frame);
    void Shutdown();
    bool LoadExtensions();

    Config cfg_{};
    std::thread present_thread_;
    std::atomic<bool> run_{false};
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<DecodedFrame> queue_;
    std::mutex ready_mu_;
    std::condition_variable ready_cv_;
    std::atomic<bool> init_ready_{false};
    std::atomic<bool> init_done_{false};
    std::mutex err_mu_;
    std::string init_error_;

    struct wl_display*  display_  = nullptr;
    struct wl_registry* registry_ = nullptr;
    struct wl_compositor* compositor_ = nullptr;
    struct wl_surface*  surface_  = nullptr;
    struct wl_seat*     seat_     = nullptr;
    struct xdg_wm_base* xdg_wm_   = nullptr;
    struct xdg_surface* xdg_surface_ = nullptr;
    struct xdg_toplevel* xdg_toplevel_ = nullptr;
    struct wl_callback* frame_callback_ = nullptr;

    EGLDisplay egl_dpy_ = EGL_NO_DISPLAY;
    EGLContext egl_ctx_ = EGL_NO_CONTEXT;
    EGLSurface egl_surf_ = EGL_NO_SURFACE;

    GLuint program_ = 0;
    GLuint vbo_ = 0;
    GLuint y_tex_ = 0;
    GLuint uv_tex_ = 0;
    GLint uYTex_loc_ = -1;
    GLint uUVTex_loc_ = -1;

    std::unordered_map<VASurfaceID, CachedSurface> cache_;
    std::atomic<std::uint64_t> frames_presented_{0};

    std::atomic<int> pending_resize_w_{0};
    std::atomic<int> pending_resize_h_{0};

    // Friend declaration: grants the anonymous-namespace callback
    // functions access to private members via g_wayland_self.
    friend void wl_present_xdg_wm_base_cb(void*, struct xdg_wm_base*, uint32_t);
    friend void wl_present_xdg_surface_cb(void*, struct xdg_surface*, uint32_t);
    friend void wl_present_xdg_toplevel_config_cb(void*, struct xdg_toplevel*, int32_t, int32_t, struct wl_array*);
    friend void wl_present_xdg_toplevel_close_cb(void*, struct xdg_toplevel*);
    friend void wl_present_frame_cb(void*, struct wl_callback*, uint32_t);
};

// ── Global pointer for Wayland event callbacks ────────────────

static std::atomic<EglWaylandPresenter*> g_wayland_self{nullptr};

// ── Callback functions (friends of EglWaylandPresenter) ───────

void wl_present_xdg_wm_base_cb(void*, struct xdg_wm_base* wb, uint32_t serial) {
    (void)wb;
    xdg_wm_base_pong(wb, serial);
}

void wl_present_xdg_surface_cb(void*, struct xdg_surface* xdg_surface, uint32_t serial) {
    (void)xdg_surface;
    xdg_surface_ack_configure(xdg_surface, serial);
}

void wl_present_xdg_toplevel_config_cb(void*, struct xdg_toplevel*, int32_t w, int32_t h, struct wl_array*) {
    if (w > 0 && h > 0) {
        auto* self = g_wayland_self.load(std::memory_order_acquire);
        if (self) {
            self->pending_resize_w_.store(w, std::memory_order_relaxed);
            self->pending_resize_h_.store(h, std::memory_order_relaxed);
        }
    }
}

void wl_present_xdg_toplevel_close_cb(void*, struct xdg_toplevel*) {
    // Access g_wayland_self atomically — it's cleared by Shutdown()
    // which joins the presenter thread, so the callback can't fire
    // after g_wayland_self becomes nullptr unless there's a race.
    // The atomic load ensures we don't read a torn pointer.
    auto* self = g_wayland_self.load(std::memory_order_acquire);
    if (self) {
        self->run_.store(false, std::memory_order_release);
    }
}

void wl_present_frame_cb(void*, struct wl_callback* cb, uint32_t) {
    if (cb) wl_callback_destroy(cb);
}

// ── Wayland listener tables (positional init for C++17) ───────

static const struct xdg_wm_base_listener kXdgWmBaseListener = {
    wl_present_xdg_wm_base_cb
};

static const struct xdg_surface_listener kXdgSurfaceListener = {
    wl_present_xdg_surface_cb
};

static const struct xdg_toplevel_listener kXdgToplevelListener = {
    wl_present_xdg_toplevel_config_cb,
    wl_present_xdg_toplevel_close_cb,
    nullptr, nullptr
};

// wl_seat_listener has different function pointer types for each field.
// We use a union to safely store the callback without UB from function
// pointer casts. Both function pointer types have the same size on
// all supported platforms (same as void*).
union SeatCallbackUnion {
    void (*capabilities)(void*, struct wl_seat*, uint32_t);
    void (*name)(void*, struct wl_seat*, const char*);
    void* v;
};

// No-op callback used for both fields. We cast through the union to
// avoid UB from direct function pointer casts between incompatible
// signatures.
static void wl_seat_seat_cb(void* data, struct wl_seat* seat, uint32_t caps) {
    (void)data; (void)seat; (void)caps;
}

static SeatCallbackUnion seat_cb = { wl_seat_seat_cb };

static const struct wl_seat_listener kWlSeatListener = {
    reinterpret_cast<void(*)(void*, struct wl_seat*, uint32_t)>(seat_cb.v),
    reinterpret_cast<void(*)(void*, struct wl_seat*, const char*)>(
        seat_cb.v)
};

static const struct wl_callback_listener kWlCallbackListener = {
    wl_present_frame_cb
};

// ── Presenter method implementations ──────────────────────────

std::optional<std::string> EglWaylandPresenter::Init(const Config& cfg) {
    cfg_ = cfg;
    g_wayland_self.store(this, std::memory_order_release);
    run_.store(true, std::memory_order_release);
    present_thread_ = std::thread([this]() {
        if (auto err = RunPresentLoop(); err) {
            std::lock_guard<std::mutex> lk(err_mu_);
            init_error_ = *err;
        }
        {
            std::lock_guard<std::mutex> lk(ready_mu_);
            init_done_.store(true, std::memory_order_release);
            ready_cv_.notify_all();
        }
    });
    std::unique_lock<std::mutex> lk(ready_mu_);
    ready_cv_.wait(lk, [&]() {
        return init_ready_.load() || init_done_.load();
    });
    std::lock_guard<std::mutex> elk(err_mu_);
    if (!init_error_.empty()) return init_error_;
    return std::nullopt;
}

void EglWaylandPresenter::Present(const DecodedFrame& frame) {
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        if (queue_.size() >= 2) queue_.pop_front();
        queue_.push_back(frame);
    }
    queue_cv_.notify_one();
}

std::optional<std::string> EglWaylandPresenter::RunPresentLoop() {
    display_ = wl_display_connect(nullptr);
    if (!display_) return "wl_display_connect failed (is WAYLAND_DISPLAY set?)";
    if (wl_display_roundtrip(display_) < 0) {
        wl_display_disconnect(display_); display_ = nullptr;
        return "wl_display_roundtrip failed";
    }

    registry_ = wl_display_get_registry(display_);
    wl_registry_listener listener{};
    listener.global = [](void* data, struct wl_registry* reg,
                          uint32_t id, const char* iface, uint32_t version) {
        auto* self = static_cast<EglWaylandPresenter*>(data);
        if (std::strcmp(iface, "wl_compositor") == 0 && version >= 4) {
            self->compositor_ = reinterpret_cast<struct wl_compositor*>(
                wl_registry_bind(reg, id, &wl_compositor_interface, 4));
        } else if (std::strcmp(iface, "xdg_wm_base") == 0 && version >= 2) {
            self->xdg_wm_ = reinterpret_cast<struct xdg_wm_base*>(
                wl_registry_bind(reg, id, &xdg_wm_base_interface, 2));
            xdg_wm_base_add_listener(self->xdg_wm_, &kXdgWmBaseListener, self);
        } else if (std::strcmp(iface, "wl_seat") == 0) {
            self->seat_ = reinterpret_cast<struct wl_seat*>(
                wl_registry_bind(reg, id, &wl_seat_interface, 8));
            wl_seat_add_listener(self->seat_, &kWlSeatListener, self);
        }
    };
    listener.global_remove = [](void*, struct wl_registry*, uint32_t) {};
    wl_registry_add_listener(registry_, &listener, this);
    wl_display_roundtrip(display_);

    if (!compositor_ || !xdg_wm_) {
        std::fprintf(stderr, "unio-pipe: Wayland compositor/xdg_wm_base not available\n");
        // Cannot call Shutdown() here — we're on the presenter thread and
        // Shutdown() joins that same thread (deadlock). Clean up manually.
        if (egl_dpy_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(egl_dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (egl_surf_ != EGL_NO_SURFACE) eglDestroySurface(egl_dpy_, egl_surf_);
            if (egl_ctx_ != EGL_NO_CONTEXT) eglDestroyContext(egl_dpy_, egl_ctx_);
            eglTerminate(egl_dpy_);
            egl_dpy_ = EGL_NO_DISPLAY;
        }
        if (xdg_toplevel_) { xdg_toplevel_destroy(xdg_toplevel_); xdg_toplevel_ = nullptr; }
        if (xdg_surface_)  { xdg_surface_destroy(xdg_surface_);  xdg_surface_  = nullptr; }
        if (surface_)      { wl_surface_destroy(surface_);       surface_      = nullptr; }
        if (xdg_wm_)       { xdg_wm_base_destroy(xdg_wm_);       xdg_wm_       = nullptr; }
        if (compositor_)   { wl_compositor_destroy(compositor_); compositor_   = nullptr; }
        if (registry_)     { wl_registry_destroy(registry_);     registry_     = nullptr; }
        if (seat_)         { wl_seat_destroy(seat_);             seat_         = nullptr; }
        if (display_)      { wl_display_disconnect(display_); display_ = nullptr; }
        return "Wayland compositor or xdg_wm_base unavailable";
    }

    surface_ = wl_compositor_create_surface(compositor_);
    if (!surface_) { Shutdown(); return "wl_compositor_create_surface failed"; }
    xdg_surface_ = xdg_wm_base_get_xdg_surface(xdg_wm_, surface_);
    if (!xdg_surface_) { Shutdown(); return "xdg_wm_base_get_xdg_surface failed"; }
    xdg_surface_add_listener(xdg_surface_, &kXdgSurfaceListener, this);
    xdg_toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    if (!xdg_toplevel_) { Shutdown(); return "xdg_surface_get_toplevel failed"; }
    xdg_toplevel_add_listener(xdg_toplevel_, &kXdgToplevelListener, this);
    xdg_toplevel_set_title(xdg_toplevel_, cfg_.window_title.c_str());
    xdg_toplevel_set_app_id(xdg_toplevel_, "unio-pipe");
    xdg_toplevel_set_maximized(xdg_toplevel_);
    wl_surface_commit(surface_);
    wl_display_roundtrip(display_);

    int w = cfg_.width > 0 ? cfg_.width : 1920;
    int h = cfg_.height > 0 ? cfg_.height : 1080;

    egl_dpy_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_dpy_ == EGL_NO_DISPLAY) { Shutdown(); return "eglGetDisplay failed"; }
    EGLint maj = 0, min = 0;
    if (!eglInitialize(egl_dpy_, &maj, &min)) { Shutdown(); return "eglInitialize failed"; }

    const EGLint cfg_attribs[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig ec; EGLint nconfigs = 0;
    if (!eglChooseConfig(egl_dpy_, cfg_attribs, &ec, 1, &nconfigs)
        || nconfigs < 1) { Shutdown(); return "eglChooseConfig found no match"; }

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    egl_ctx_ = eglCreateContext(egl_dpy_, ec, EGL_NO_CONTEXT, ctx_attribs);
    if (egl_ctx_ == EGL_NO_CONTEXT) { Shutdown(); return "eglCreateContext failed"; }

    egl_surf_ = eglCreatePlatformWindowSurface(
        egl_dpy_, ec,
        reinterpret_cast<void*>(surface_), nullptr);
    if (egl_surf_ == EGL_NO_SURFACE) { Shutdown(); return "eglCreatePlatformWindowSurface failed"; }

    if (!eglMakeCurrent(egl_dpy_, egl_surf_, egl_surf_, egl_ctx_)) {
        Shutdown(); return "eglMakeCurrent failed";
    }
    eglSwapInterval(egl_dpy_, 0);

    if (!LoadExtensions()) { Shutdown(); return "EGL DMA-BUF extensions unavailable"; }
    if (auto err = SetupGl(); err) { Shutdown(); return err; }

    {
        std::lock_guard<std::mutex> lk(ready_mu_);
        init_ready_.store(true, std::memory_order_release);
        ready_cv_.notify_all();
    }
    std::fprintf(stderr, "unio-pipe: EGL %d.%d on Wayland %dx%d (DMA-BUF NV12)\n", maj, min, w, h);

    glClearColor(0.05f, 0.05f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(egl_dpy_, egl_surf_);
    glViewport(0, 0, w, h);

    frame_callback_ = wl_surface_frame(surface_);
    if (frame_callback_) {
        wl_callback_add_listener(frame_callback_, &kWlCallbackListener, this);
    }

    while (run_.load(std::memory_order_acquire)) {
        DecodedFrame frame{};
        bool have_frame = false;
        {
            std::unique_lock<std::mutex> lk(queue_mu_);
            queue_cv_.wait_for(lk, std::chrono::milliseconds(16), [&]() {
                return !queue_.empty() || !run_.load();
            });
            if (!queue_.empty()) { frame = queue_.front(); queue_.pop_front(); have_frame = true; }
        }
        if (!have_frame) { wl_display_flush(display_); continue; }

        if (pending_resize_w_.load(std::memory_order_relaxed) > 0 && pending_resize_h_.load(std::memory_order_relaxed) > 0) {
            // Read atomics to avoid torn values on weakly-ordered architectures.
            // The callback (running on the same thread via wl_display_roundtrip)
            // writes atomically, so no mutex is needed — using atomics avoids
            // deadlock that a mutex would cause.
            int rw = pending_resize_w_.load(std::memory_order_relaxed);
            int rh = pending_resize_h_.load(std::memory_order_relaxed);
            if (rw > 0 && rh > 0) {
                w = rw; h = rh;
                pending_resize_w_.store(0, std::memory_order_relaxed);
                pending_resize_h_.store(0, std::memory_order_relaxed);
                glViewport(0, 0, w, h);
            }
        }
        RenderFrame(frame);
    }
    return std::nullopt;
}

std::optional<std::string> EglWaylandPresenter::SetupGl() {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentSrc);
    if (!vs || !fs) return "shader compile failed";
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glBindAttribLocation(program_, 0, "a_pos");
    glBindAttribLocation(program_, 1, "a_tc");
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) return "program link failed";
    uYTex_loc_  = glGetUniformLocation(program_, "u_y");
    uUVTex_loc_ = glGetUniformLocation(program_, "u_uv");
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);
    glGenTextures(1, &y_tex_);
    glGenTextures(1, &uv_tex_);
    for (GLuint t : {y_tex_, uv_tex_}) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return std::nullopt;
}

EGLImageKHR EglWaylandPresenter::ImportPlane(int fd, std::uint32_t fourcc, int width, int height,
                              std::uint32_t offset, std::uint32_t pitch,
                              std::uint64_t modifier) {
    EGLint a[32]; int i = 0;
    a[i++] = EGL_WIDTH;  a[i++] = width;
    a[i++] = EGL_HEIGHT; a[i++] = height;
    a[i++] = EGL_LINUX_DRM_FOURCC_EXT;
    a[i++] = static_cast<EGLint>(fourcc);
    a[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;     a[i++] = fd;
    a[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; a[i++] = static_cast<EGLint>(offset);
    a[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  a[i++] = static_cast<EGLint>(pitch);
    if (modifier != 0 && modifier != (1ULL << 56) - 1) {
        a[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        a[i++] = static_cast<EGLint>(modifier & 0xFFFFFFFF);
        a[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        a[i++] = static_cast<EGLint>(modifier >> 32);
    }
    a[i++] = EGL_NONE;
    return g_eglCreateImageKHR(egl_dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a);
}

bool EglWaylandPresenter::UploadSurface(const DecodedFrame& frame) {
    auto* va_dpy = reinterpret_cast<VADisplay>(frame.native_device);
    auto surface = static_cast<VASurfaceID>(frame.surface_handle);
    if (!va_dpy) return false;
    auto it = cache_.find(surface);
    CachedSurface* cs = nullptr;
    if (it != cache_.end()) { cs = &it->second; }
    else {
        VADRMPRIMESurfaceDescriptor desc{};
        VAStatus vs = vaExportSurfaceHandle(va_dpy, surface,
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
        if (vs != VA_STATUS_SUCCESS) return false;
        if (desc.num_layers < 2 || desc.num_objects < 2) {
            for (std::uint32_t i = 0; i < desc.num_objects; ++i) ::close(desc.objects[i].fd);
            return false;
        }
        // Bounds-check layer and object indices — malformed descriptors
        // from misbehaving drivers could cause out-of-bounds reads.
        if (desc.layers[0].object_index[0] >= desc.num_objects
            || desc.layers[1].object_index[0] >= desc.num_objects) {
            for (std::uint32_t i = 0; i < desc.num_objects; ++i) ::close(desc.objects[i].fd);
            return false;
        }
        const auto& ly  = desc.layers[0];
        const auto& luv = desc.layers[1];
        const auto& oy  = desc.objects[ly.object_index[0]];
        const auto& ouv = desc.objects[luv.object_index[0]];
        CachedSurface fresh;
        fresh.id = surface;
        fresh.y_image = ImportPlane(oy.fd, 0x20203852u, static_cast<int>(desc.width),
            static_cast<int>(desc.height), ly.offset[0], ly.pitch[0], oy.drm_format_modifier);
        fresh.uv_image = ImportPlane(ouv.fd, 0x38385247u, static_cast<int>(desc.width) / 2,
            static_cast<int>(desc.height) / 2, luv.offset[0], luv.pitch[0], ouv.drm_format_modifier);
        for (std::uint32_t i = 0; i < desc.num_objects; ++i) ::close(desc.objects[i].fd);
        if (fresh.y_image == EGL_NO_IMAGE_KHR || fresh.uv_image == EGL_NO_IMAGE_KHR) {
            if (fresh.y_image)  g_eglDestroyImageKHR(egl_dpy_, fresh.y_image);
            if (fresh.uv_image) g_eglDestroyImageKHR(egl_dpy_, fresh.uv_image);
            return false;
        }
        // emplace may fail if surface already exists in cache.
        // On failure, fresh is discarded — clean up its EGL images.
        auto [ins, inserted] = cache_.emplace(surface, fresh);
        if (!inserted) {
            // Surface already cached — destroy the newly created images.
            if (fresh.y_image)  g_eglDestroyImageKHR(egl_dpy_, fresh.y_image);
            if (fresh.uv_image) g_eglDestroyImageKHR(egl_dpy_, fresh.uv_image);
        }
        cs = &ins->second;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, y_tex_);
    g_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, cs->y_image);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, uv_tex_);
    g_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, cs->uv_image);
    return true;
}

void EglWaylandPresenter::RenderFrame(const DecodedFrame& frame) {
    if (!UploadSurface(frame)) {
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(egl_dpy_, egl_surf_);
        return;
    }
    glUseProgram(program_);
    glUniform1i(uYTex_loc_, 0);
    glUniform1i(uUVTex_loc_, 1);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Destroy previous frame callback before creating a new one.
    // frame_callback_ may be nullptr if wl_surface_frame() failed
    // (e.g., surface was destroyed by the compositor).
    if (frame_callback_) { wl_callback_destroy(frame_callback_); frame_callback_ = nullptr; }
    frame_callback_ = wl_surface_frame(surface_);
    if (frame_callback_) {
        wl_callback_add_listener(frame_callback_, &kWlCallbackListener, this);
    }

    eglSwapBuffers(egl_dpy_, egl_surf_);
    wl_display_flush(display_);

    const std::uint64_t present_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    frames_presented_.fetch_add(1, std::memory_order_relaxed);
    if (frame.frame_id) {
        LogLatency("UNIO_PIPE_LATENCY_CSV", frame.frame_id,
            frame.capture_monotonic_ns, frame.decode_done_monotonic_ns,
            present_ns, frame.width, frame.height);
    }
}

void EglWaylandPresenter::Shutdown() {
    run_.store(false, std::memory_order_release);
    queue_cv_.notify_all();
    // Don't join if we're on the presenter thread — that would deadlock.
    // This happens when RunPresentLoop() fails early and the destructor
    // runs on the presenter thread itself.
    if (present_thread_.joinable() && present_thread_.get_id() != std::this_thread::get_id()) {
        present_thread_.join();
    }
    g_wayland_self.store(nullptr, std::memory_order_release);

    if (egl_dpy_ != EGL_NO_DISPLAY) {
        if (g_eglDestroyImageKHR) {
            for (auto& [id, cs] : cache_) {
                if (cs.y_image)  g_eglDestroyImageKHR(egl_dpy_, cs.y_image);
                if (cs.uv_image) g_eglDestroyImageKHR(egl_dpy_, cs.uv_image);
            }
        }
        cache_.clear();
        eglMakeCurrent(egl_dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_surf_ != EGL_NO_SURFACE) eglDestroySurface(egl_dpy_, egl_surf_);
        if (egl_ctx_ != EGL_NO_CONTEXT) eglDestroyContext(egl_dpy_, egl_ctx_);
        eglTerminate(egl_dpy_);
        egl_dpy_ = EGL_NO_DISPLAY;
    }
    if (frame_callback_) { wl_callback_destroy(frame_callback_); frame_callback_ = nullptr; }
    if (xdg_toplevel_) { xdg_toplevel_destroy(xdg_toplevel_); xdg_toplevel_ = nullptr; }
    if (xdg_surface_)  { xdg_surface_destroy(xdg_surface_);  xdg_surface_  = nullptr; }
    if (surface_)      { wl_surface_destroy(surface_);       surface_      = nullptr; }
    if (xdg_wm_)       { xdg_wm_base_destroy(xdg_wm_);       xdg_wm_       = nullptr; }
    if (compositor_)   { wl_compositor_destroy(compositor_); compositor_   = nullptr; }
    if (registry_)     { wl_registry_destroy(registry_);     registry_     = nullptr; }
    if (seat_)         { wl_seat_destroy(seat_);             seat_         = nullptr; }
    if (display_)      { wl_display_flush(display_); wl_display_disconnect(display_); display_ = nullptr; }
}

bool EglWaylandPresenter::LoadExtensions() {
    g_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    g_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    g_glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    return g_eglCreateImageKHR && g_eglDestroyImageKHR && g_glEGLImageTargetTexture2DOES;
}

}  // namespace

std::unique_ptr<Presenter> MakeEglWaylandPresenter() {
    return std::make_unique<EglWaylandPresenter>();
}

}  // namespace unio

#else  // !__linux__

#include "presenter.h"
namespace unio {
std::unique_ptr<Presenter> MakeEglWaylandPresenter() { return nullptr; }
}  // namespace unio

#endif  // __linux__