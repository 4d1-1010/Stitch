// EGL-on-X11 presenter. The Linux sink side of the pipeline:
// decoded VA-API surfaces land here and get painted on an
// override-redirect X11 window with the EGL context in
// tear-present mode (EGL_BUFFER_PRESERVED = false, swap interval
// 0) so we stay under the 16 ms glass-to-glass budget.
//
// Scope of this file today (PR 6 Day 7):
//   - Window creation + EGL context
//   - Present thread that drains a surface ring and swaps buffers
//   - Per-frame colour derived from frame_id so a human watching
//     the window can see motion even before DMA-BUF upload lands
//
// Follow-up (Day 7b): replace the frame_id colour with a real
// NV12 texture sourced from the VA-API surface via DMA-BUF
// zero-copy (vaExportSurfaceHandle → EGLImage → GL texture).
// The interface is shaped for that — Present() already gets the
// full DecodedFrame including the surface handle.

#if defined(__linux__)

#include "presenter.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>

namespace unio {

namespace {

class EglX11Presenter final : public Presenter {
public:
    EglX11Presenter() = default;
    ~EglX11Presenter() override { Shutdown(); }

    std::optional<std::string> Init(const Config& cfg) override {
        cfg_ = cfg;
        // X11 display opens on the thread that will own the GL
        // context. Xlib is documented as thread-safe when you call
        // XInitThreads once, but in practice every project that
        // cares about latency pins the X connection to one thread
        // so we do the same — present_thread_ owns dpy_.
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

        // Block on the present thread reaching either "ready" or
        // "init failed" so StartInbound sees the error synchronously
        // rather than as silently-dropped frames.
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
            // Drop-oldest if we fall behind: freshest frame always
            // wins, matches the SPSC ring policy the encoder uses.
            if (queue_.size() >= 2) {
                queue_.pop_front();
            }
            queue_.push_back(frame);
        }
        queue_cv_.notify_one();
    }

    std::uint64_t FramesPresented() const override {
        return frames_presented_.load(std::memory_order_relaxed);
    }

    std::string_view Name() const override { return "egl-x11"; }

private:
    std::optional<std::string> RunPresentLoop() {
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) return "XOpenDisplay failed (is DISPLAY set?)";
        int screen = DefaultScreen(dpy_);
        Window root = RootWindow(dpy_, screen);

        const int w = cfg_.width > 0 ? cfg_.width
                    : DisplayWidth(dpy_, screen);
        const int h = cfg_.height > 0 ? cfg_.height
                    : DisplayHeight(dpy_, screen);

        // Override-redirect = we bypass the window manager. No
        // decorations, no re-parenting, no focus theft. The sink
        // is a live video surface, not a regular window. If the
        // user wants to move it they'll use a dedicated hotkey
        // we'll wire in later.
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
        if (egl_dpy_ == EGL_NO_DISPLAY) {
            return "eglGetDisplay failed";
        }
        EGLint maj = 0, min = 0;
        if (!eglInitialize(egl_dpy_, &maj, &min)) {
            return "eglInitialize failed";
        }
        const EGLint cfg_attribs[] = {
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 0,
            EGL_DEPTH_SIZE, 0,
            EGL_STENCIL_SIZE, 0,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE,
        };
        EGLConfig ec;
        EGLint nconfigs = 0;
        if (!eglChooseConfig(egl_dpy_, cfg_attribs, &ec, 1, &nconfigs)
            || nconfigs < 1) {
            return "eglChooseConfig found no match";
        }
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint ctx_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        egl_ctx_ = eglCreateContext(
            egl_dpy_, ec, EGL_NO_CONTEXT, ctx_attribs);
        if (egl_ctx_ == EGL_NO_CONTEXT) {
            return "eglCreateContext failed";
        }
        egl_surf_ = eglCreateWindowSurface(
            egl_dpy_, ec,
            reinterpret_cast<EGLNativeWindowType>(window_), nullptr);
        if (egl_surf_ == EGL_NO_SURFACE) {
            return "eglCreateWindowSurface failed";
        }
        if (!eglMakeCurrent(
                egl_dpy_, egl_surf_, egl_surf_, egl_ctx_)) {
            return "eglMakeCurrent failed";
        }
        // Tear-present: never wait on vblank. PR 9 will add a
        // vblank-aligned capture timer to hit presents on whole
        // frames without the full-frame latency of SyncInterval=1.
        eglSwapInterval(egl_dpy_, 0);

        // Announce ready so Init() returns; errors after this
        // point surface via status counters, not the RPC reply.
        {
            std::lock_guard<std::mutex> lk(ready_mu_);
            init_ready_.store(true, std::memory_order_release);
            ready_cv_.notify_all();
        }

        std::fprintf(stderr,
            "unio-pipe: EGL %d.%d on X11 %dx%d, window 0x%lx\n",
            maj, min, w, h, static_cast<unsigned long>(window_));

        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);

        // Paint one frame synchronously so the window has real
        // pixels before any caller can observe it — the ProbeRoot
        // path in capture_xcomposite.cpp rejects a compositor
        // whose root pixmap is all-black, and a freshly-mapped
        // but never-drawn X window reads as all-black under an
        // override-redirect stack.
        glClearColor(0.05f, 0.05f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(egl_dpy_, egl_surf_);

        while (run_.load(std::memory_order_acquire)) {
            DecodedFrame frame{};
            bool have_frame = false;
            {
                std::unique_lock<std::mutex> lk(queue_mu_);
                queue_cv_.wait_for(lk,
                    std::chrono::milliseconds(16), [&]() {
                        return !queue_.empty()
                            || !run_.load();
                    });
                if (!queue_.empty()) {
                    frame = queue_.front();
                    queue_.pop_front();
                    have_frame = true;
                }
            }
            if (!have_frame) continue;

            // Day 7a placeholder: derive the clear colour from
            // frame_id so a human watching the window sees motion.
            // Day 7b replaces this with a DMA-BUF NV12 sampler
            // and a BT.601 limited-range shader.
            const float t = static_cast<float>(
                frames_presented_.load() % 120) / 120.0f;
            glClearColor(t, 1.0f - t, 0.5f + 0.5f * t, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(egl_dpy_, egl_surf_);
            frames_presented_.fetch_add(1,
                std::memory_order_relaxed);
            (void)frame;
        }
        return std::nullopt;
    }

    void Shutdown() {
        run_.store(false, std::memory_order_release);
        queue_cv_.notify_all();
        if (present_thread_.joinable()) present_thread_.join();
        if (egl_dpy_ != EGL_NO_DISPLAY) {
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
