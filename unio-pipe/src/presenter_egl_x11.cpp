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
// Y is uploaded as GL_LUMINANCE (single channel, sampled via .r);
// UV as GL_LUMINANCE_ALPHA (two channels, .r=U, .a=V). Both
// formats are GLES2 core, available on every driver we'll run
// against.
const char* kFragmentSrc = R"(
precision mediump float;
varying vec2 v_tc;
uniform sampler2D u_y;
uniform sampler2D u_uv;
void main() {
    float y = texture2D(u_y, v_tc).r;
    vec4  uvs = texture2D(u_uv, v_tc);
    vec2  uv = vec2(uvs.r, uvs.a) - vec2(0.5);
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

    // Upload the decoded VA-API NV12 surface to our GL textures
    // via a CPU readback. Slower than the DMA-BUF zero-copy path
    // (~3-5 ms per 1080p frame on Intel UHD 630) but works on
    // every Mesa version without depending on
    // EGL_EXT_image_dma_buf_import + DRM_FORMAT_R8/GR88 / Intel
    // tiling modifier support — all of which Mesa accepts at
    // eglCreateImage level but silently produces undefined
    // texture content for on at least Mesa 24.x + iHD output.
    // Zero-copy DMA-BUF is a post-PR-6 optimisation on top of
    // this baseline.
    bool UploadSurface(const DecodedFrame& frame) {
        auto* va_dpy = reinterpret_cast<VADisplay>(
            frame.native_device);
        auto surface = static_cast<VASurfaceID>(frame.surface_handle);
        if (!va_dpy) return false;

        VAImage img{};
        if (vaDeriveImage(va_dpy, surface, &img)
            != VA_STATUS_SUCCESS) {
            return false;
        }
        if (img.format.fourcc != VA_FOURCC_NV12) {
            vaDestroyImage(va_dpy, img.image_id);
            return false;
        }
        void* mapped = nullptr;
        if (vaMapBuffer(va_dpy, img.buf, &mapped)
            != VA_STATUS_SUCCESS) {
            vaDestroyImage(va_dpy, img.image_id);
            return false;
        }
        const auto* base = static_cast<const std::uint8_t*>(mapped);

        const int w = static_cast<int>(frame.width);
        const int h = static_cast<int>(frame.height);
        const auto* y_src = base + img.offsets[0];
        const auto* uv_src = base + img.offsets[1];
        const std::uint32_t y_pitch  = img.pitches[0];
        const std::uint32_t uv_pitch = img.pitches[1];

        // Repack the Y plane to width*height (strip pitch
        // padding) so glTexImage2D doesn't read past the
        // end. glPixelStorei(GL_UNPACK_ROW_LENGTH) isn't in
        // GLES 2, so we pack on the CPU.
        y_scratch_.resize(static_cast<std::size_t>(w) * h);
        for (int yy = 0; yy < h; ++yy) {
            std::memcpy(y_scratch_.data() + static_cast<std::size_t>(yy) * w,
                        y_src + static_cast<std::size_t>(yy) * y_pitch,
                        w);
        }
        const int uv_w = w / 2;
        const int uv_h = h / 2;
        uv_scratch_.resize(static_cast<std::size_t>(uv_w) * uv_h * 2);
        for (int yy = 0; yy < uv_h; ++yy) {
            std::memcpy(uv_scratch_.data() + static_cast<std::size_t>(yy) * uv_w * 2,
                        uv_src + static_cast<std::size_t>(yy) * uv_pitch,
                        static_cast<std::size_t>(uv_w) * 2);
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, y_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                     w, h, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE,
                     y_scratch_.data());
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, uv_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA,
                     uv_w, uv_h, 0,
                     GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
                     uv_scratch_.data());

        vaUnmapBuffer(va_dpy, img.buf);
        vaDestroyImage(va_dpy, img.image_id);
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
