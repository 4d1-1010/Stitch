// EGL-on-X11 presenter (Linux). Zero-copy NV12 display pipeline:
//
//   VA-API decoded surface  ──► vaExportSurfaceHandle (DRM-PRIME)
//                                      │
//                            DMA-BUF fds + strides + modifier
//                                      │
//                    eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)
//                                      │
//                           glEGLImageTargetTexture2DOES on
//                           GL_TEXTURE_EXTERNAL_OES (driver
//                           handles YUV→RGB BT.601 per the
//                           EGL_YUV_COLOR_SPACE_HINT we set).
//                                      │
//                        Full-screen quad, SyncInterval=0.
//
// No CPU readback. No colour-space conversion in our shader —
// the samplerExternalOES path lets the GL driver emit the
// hardware's native YUV sampler, which is what every mainline
// driver (Intel iHD + Mesa radeonsi + NVIDIA) optimises for.
//
// EGLImages are cached by VASurfaceID so steady-state cost per
// frame is: bind texture, swap. Teardown destroys images before
// decoder destroys surfaces — InboundStream::~Stream already
// sequences the resets correctly.

#if defined(__linux__)

#include "presenter.h"

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
#include <X11/Xlib.h>
#include <drm/drm_fourcc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

namespace unio {

namespace {

// EGL extension entry points. Loaded once at Init via
// eglGetProcAddress because they aren't guaranteed to be present
// at link time — GL_OES_EGL_image_external needs runtime
// dispatch even though we pull gl2ext.h.
using PFNEGLCREATEIMAGEKHRPROC_T =
    EGLImageKHR (EGLAPIENTRYP)(EGLDisplay, EGLContext, EGLenum,
                                EGLClientBuffer, const EGLint*);
using PFNEGLDESTROYIMAGEKHRPROC_T =
    EGLBoolean (EGLAPIENTRYP)(EGLDisplay, EGLImageKHR);
using PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_T =
    void (GL_APIENTRYP)(GLenum, GLeglImageOES);

PFNEGLCREATEIMAGEKHRPROC_T g_eglCreateImageKHR = nullptr;
PFNEGLDESTROYIMAGEKHRPROC_T g_eglDestroyImageKHR = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_T g_glEGLImageTargetTexture2DOES
    = nullptr;

bool LoadExtensions() {
    g_eglCreateImageKHR =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC_T>(
            eglGetProcAddress("eglCreateImageKHR"));
    g_eglDestroyImageKHR =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC_T>(
            eglGetProcAddress("eglDestroyImageKHR"));
    g_glEGLImageTargetTexture2DOES =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_T>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    return g_eglCreateImageKHR && g_eglDestroyImageKHR
        && g_glEGLImageTargetTexture2DOES;
}

// Full-screen quad in clip-space with matching tex-coords. Y is
// flipped for GL vs H.264/VA-API convention (origin at top).
const float kQuadVerts[] = {
    //  x,     y,   s,   t
    -1.f, -1.f, 0.f, 1.f,
     1.f, -1.f, 1.f, 1.f,
    -1.f,  1.f, 0.f, 0.f,
     1.f,  1.f, 1.f, 0.f,
};

const char* kVertexSrc = R"(
attribute vec2 a_pos;
attribute vec2 a_tc;
varying vec2 v_tc;
void main() {
    v_tc = a_tc;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

// Two-plane NV12 sampler + BT.601 limited-range YUV→RGB.
//
// Initially we used samplerExternalOES on a COMPOSED_LAYERS
// DMA-BUF import, relying on the driver's native YUV conversion.
// Visual validation (Linux loopback, Windows → Linux) showed Mesa
// was returning the Y plane only — gray output. The symptom is
// well-known; the samplerExternalOES path on Mesa's Intel driver
// honours neither EGL_YUV_COLOR_SPACE_HINT nor the NV12 layout
// reliably as of Mesa 24.x. Separate-plane imports + a manual
// matrix dodge the whole class of drivers-ignore-the-hint bugs.
//
// R8 for Y (one channel) and GR88 for UV (two channels) are the
// DRM_FORMAT values Intel iHD hands back when we ask for
// SEPARATE_LAYERS, which every Mesa release supports as
// EGLImage fourcc-es.
// Two-plane NV12 sampler + BT.601 limited-range YUV→RGB.
// Y plane is imported as DRM_FORMAT_R8 → GL samples .r as the
// single Y channel in 0..1. UV plane is DRM_FORMAT_GR88, which
// Mesa maps to its internal R8G8_UNORM format: byte 0 → R
// channel, byte 1 → G channel. NV12 pairs bytes as (U, V), so
// .r samples U and .g samples V.
const char* kFragmentSrc = R"(
precision mediump float;
varying vec2 v_tc;
uniform sampler2D u_y;
uniform sampler2D u_uv;
void main() {
    float y = texture2D(u_y, v_tc).r;
    vec2  uv = texture2D(u_uv, v_tc).rg - vec2(0.5); // .r=U, .g=V
    y = (y - 16.0/255.0) * (255.0/219.0);
    float r = y + 1.402    * uv.y;
    float g = y - 0.344136 * uv.x - 0.714136 * uv.y;
    float b = y + 1.772    * uv.x;
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
        std::fprintf(stderr, "unio-pipe: shader compile failed: %s\n",
                     log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

class EglX11Presenter final : public Presenter {
public:
    EglX11Presenter() = default;
    ~EglX11Presenter() override { Shutdown(); }

    std::optional<std::string> Init(const Config& cfg) override {
        cfg_ = cfg;
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

    void Present(const DecodedFrame& frame) override {
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            if (queue_.size() >= 2) queue_.pop_front();
            queue_.push_back(frame);
        }
        queue_cv_.notify_one();
    }

    std::uint64_t FramesPresented() const override {
        return frames_presented_.load(std::memory_order_relaxed);
    }

    std::string_view Name() const override { return "egl-x11"; }

private:
    struct CachedSurface {
        VASurfaceID id = 0;
        EGLImageKHR y_image  = EGL_NO_IMAGE_KHR;
        EGLImageKHR uv_image = EGL_NO_IMAGE_KHR;
    };

    std::optional<std::string> RunPresentLoop() {
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) return "XOpenDisplay failed (is DISPLAY set?)";
        int screen = DefaultScreen(dpy_);
        Window root = RootWindow(dpy_, screen);

        const int w = cfg_.width > 0 ? cfg_.width
                    : DisplayWidth(dpy_, screen);
        const int h = cfg_.height > 0 ? cfg_.height
                    : DisplayHeight(dpy_, screen);

        XSetWindowAttributes wa{};
        wa.override_redirect = True;
        wa.event_mask = ExposureMask | StructureNotifyMask;
        wa.background_pixel = BlackPixel(dpy_, screen);
        window_ = XCreateWindow(
            dpy_, root, cfg_.x, cfg_.y, w, h, 0,
            CopyFromParent, InputOutput, CopyFromParent,
            CWOverrideRedirect | CWEventMask | CWBackPixel, &wa);
        XStoreName(dpy_, window_, cfg_.window_title.c_str());
        XMapWindow(dpy_, window_);
        XFlush(dpy_);

        egl_dpy_ = eglGetDisplay(
            reinterpret_cast<EGLNativeDisplayType>(dpy_));
        if (egl_dpy_ == EGL_NO_DISPLAY) return "eglGetDisplay failed";
        EGLint maj = 0, min = 0;
        if (!eglInitialize(egl_dpy_, &maj, &min)) {
            return "eglInitialize failed";
        }
        const EGLint cfg_attribs[] = {
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 0,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE};
        EGLConfig ec;
        EGLint nconfigs = 0;
        if (!eglChooseConfig(egl_dpy_, cfg_attribs, &ec, 1,
                              &nconfigs)
            || nconfigs < 1) {
            return "eglChooseConfig found no match";
        }
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint ctx_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        egl_ctx_ = eglCreateContext(egl_dpy_, ec,
                                     EGL_NO_CONTEXT, ctx_attribs);
        if (egl_ctx_ == EGL_NO_CONTEXT) {
            return "eglCreateContext failed";
        }
        egl_surf_ = eglCreateWindowSurface(
            egl_dpy_, ec,
            reinterpret_cast<EGLNativeWindowType>(window_), nullptr);
        if (egl_surf_ == EGL_NO_SURFACE) {
            return "eglCreateWindowSurface failed";
        }
        if (!eglMakeCurrent(egl_dpy_, egl_surf_,
                             egl_surf_, egl_ctx_)) {
            return "eglMakeCurrent failed";
        }
        eglSwapInterval(egl_dpy_, 0);

        if (!LoadExtensions()) {
            return "EGL_KHR_image_base + "
                   "GL_OES_EGL_image_external unavailable";
        }
        if (auto err = SetupGl(); err) return err;

        {
            std::lock_guard<std::mutex> lk(ready_mu_);
            init_ready_.store(true, std::memory_order_release);
            ready_cv_.notify_all();
        }
        std::fprintf(stderr,
            "unio-pipe: EGL %d.%d on X11 %dx%d, window 0x%lx "
            "(DMA-BUF NV12 display)\n",
            maj, min, w, h, static_cast<unsigned long>(window_));

        // Non-black paint on init so the ProbeRoot in the colocated
        // source loop sees content (see Day 7a commit message).
        glClearColor(0.05f, 0.05f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(egl_dpy_, egl_surf_);
        glViewport(0, 0, w, h);

        while (run_.load(std::memory_order_acquire)) {
            DecodedFrame frame{};
            bool have_frame = false;
            {
                std::unique_lock<std::mutex> lk(queue_mu_);
                queue_cv_.wait_for(lk,
                    std::chrono::milliseconds(16), [&]() {
                        return !queue_.empty() || !run_.load();
                    });
                if (!queue_.empty()) {
                    frame = queue_.front();
                    queue_.pop_front();
                    have_frame = true;
                }
            }
            if (!have_frame) continue;
            RenderFrame(frame);
        }
        return std::nullopt;
    }

    std::optional<std::string> SetupGl() {
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
        glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts),
                     kQuadVerts, GL_STATIC_DRAW);

        // One texture per plane. Both stay bound across frames —
        // only the EGLImage behind each swaps per-frame.
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

    // DRM fourcc constants — libdrm's drm/drm_fourcc.h isn't
    // always on the EGL-dev package's include path, so inline
    // the two we need. ASCII fourcc built little-endian:
    // 'R8  ' = 'R','8',' ',' ' → 0x20203852;
    // 'GR88' = 'G','R','8','8' → 0x38385247.
    static constexpr std::uint32_t kFourccR8   = 0x20203852u;
    static constexpr std::uint32_t kFourccGR88 = 0x38385247u;

    EGLImageKHR ImportPlane(int fd, std::uint32_t fourcc,
                             int width, int height,
                             std::uint32_t offset,
                             std::uint32_t pitch,
                             std::uint64_t modifier) {
        EGLint a[32];
        int i = 0;
        a[i++] = EGL_WIDTH;  a[i++] = width;
        a[i++] = EGL_HEIGHT; a[i++] = height;
        a[i++] = EGL_LINUX_DRM_FOURCC_EXT;
        a[i++] = static_cast<EGLint>(fourcc);
        a[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;     a[i++] = fd;
        a[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        a[i++] = static_cast<EGLint>(offset);
        a[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        a[i++] = static_cast<EGLint>(pitch);
        if (modifier != 0 &&
            modifier != (1ULL << 56) - 1 /* DRM_FORMAT_MOD_INVALID */) {
            a[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            a[i++] = static_cast<EGLint>(modifier & 0xFFFFFFFF);
            a[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            a[i++] = static_cast<EGLint>(modifier >> 32);
        }
        a[i++] = EGL_NONE;
        return g_eglCreateImageKHR(
            egl_dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
            nullptr, a);
    }

    // Per-frame (first-time-per-surface): export the VA-API
    // surface as two DMA-BUF planes (Y as DRM_FORMAT_R8, UV as
    // DRM_FORMAT_GR88), wrap each as an EGLImage. Cache both
    // by VASurfaceID. Steady state cost per frame is two
    // glEGLImageTargetTexture2DOES bind calls — no CPU copy.
    //
    // This path was attempted in Day 7b but produced gray
    // output. Root cause turned out to be the VA-API decoder
    // silently emitting fill-value surfaces (Day 8 commit); now
    // that the decoder actually writes pixels, the DMA-BUF
    // import should carry them through. If it still doesn't on
    // some drivers, CPU-readback fallback stays in git history.
    bool UploadSurface(const DecodedFrame& frame) {
        auto* va_dpy = reinterpret_cast<VADisplay>(
            frame.native_device);
        auto surface = static_cast<VASurfaceID>(frame.surface_handle);
        if (!va_dpy) return false;

        auto it = cache_.find(surface);
        CachedSurface* cs = nullptr;
        if (it != cache_.end()) {
            cs = &it->second;
        } else {
            VADRMPRIMESurfaceDescriptor desc{};
            VAStatus vs = vaExportSurfaceHandle(
                va_dpy, surface,
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_ONLY
                    | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                &desc);
            if (vs != VA_STATUS_SUCCESS) return false;
            if (desc.num_layers < 2) {
                for (std::uint32_t i = 0; i < desc.num_objects; ++i) {
                    ::close(desc.objects[i].fd);
                }
                return false;
            }
            const auto& ly  = desc.layers[0];
            const auto& luv = desc.layers[1];
            const auto& oy  = desc.objects[ly.object_index[0]];
            const auto& ouv = desc.objects[luv.object_index[0]];
            CachedSurface fresh;
            fresh.id = surface;
            fresh.y_image = ImportPlane(oy.fd, kFourccR8,
                static_cast<int>(desc.width),
                static_cast<int>(desc.height),
                ly.offset[0], ly.pitch[0],
                oy.drm_format_modifier);
            fresh.uv_image = ImportPlane(ouv.fd, kFourccGR88,
                static_cast<int>(desc.width) / 2,
                static_cast<int>(desc.height) / 2,
                luv.offset[0], luv.pitch[0],
                ouv.drm_format_modifier);
            for (std::uint32_t i = 0; i < desc.num_objects; ++i) {
                ::close(desc.objects[i].fd);
            }
            if (fresh.y_image == EGL_NO_IMAGE_KHR
                || fresh.uv_image == EGL_NO_IMAGE_KHR) {
                if (fresh.y_image)  g_eglDestroyImageKHR(egl_dpy_, fresh.y_image);
                if (fresh.uv_image) g_eglDestroyImageKHR(egl_dpy_, fresh.uv_image);
                return false;
            }
            auto [ins, _] = cache_.emplace(surface, fresh);
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

    void RenderFrame(const DecodedFrame& frame) {
        if (!UploadSurface(frame)) {
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(egl_dpy_, egl_surf_);
            return;
        }

        glUseProgram(program_);
        glUniform1i(uYTex_loc_,  0);
        glUniform1i(uUVTex_loc_, 1);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float),
                              reinterpret_cast<void*>(
                                  2 * sizeof(float)));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        eglSwapBuffers(egl_dpy_, egl_surf_);
        frames_presented_.fetch_add(1, std::memory_order_relaxed);
    }

    void Shutdown() {
        run_.store(false, std::memory_order_release);
        queue_cv_.notify_all();
        if (present_thread_.joinable()) present_thread_.join();
        if (egl_dpy_ != EGL_NO_DISPLAY) {
            if (g_eglDestroyImageKHR) {
                for (auto& [id, cs] : cache_) {
                    if (cs.y_image)  g_eglDestroyImageKHR(egl_dpy_, cs.y_image);
                    if (cs.uv_image) g_eglDestroyImageKHR(egl_dpy_, cs.uv_image);
                }
            }
            cache_.clear();
            eglMakeCurrent(egl_dpy_, EGL_NO_SURFACE,
                            EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (egl_surf_ != EGL_NO_SURFACE) {
                eglDestroySurface(egl_dpy_, egl_surf_);
            }
            if (egl_ctx_ != EGL_NO_CONTEXT) {
                eglDestroyContext(egl_dpy_, egl_ctx_);
            }
            eglTerminate(egl_dpy_);
        }
        if (window_ && dpy_) {
            XDestroyWindow(dpy_, window_);
            XFlush(dpy_);
        }
        if (dpy_) XCloseDisplay(dpy_);
    }

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

    Display* dpy_ = nullptr;
    Window window_ = 0;
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
    bool bind_error_logged_ = false;
    std::vector<std::uint8_t> y_scratch_;
    std::vector<std::uint8_t> uv_scratch_;

    std::atomic<std::uint64_t> frames_presented_{0};
};

}  // namespace

std::unique_ptr<Presenter> MakeEglX11Presenter() {
    return std::make_unique<EglX11Presenter>();
}

}  // namespace unio

#else  // !__linux__

#include "presenter.h"
namespace unio {
std::unique_ptr<Presenter> MakeEglX11Presenter() { return nullptr; }
}  // namespace unio

#endif  // __linux__
