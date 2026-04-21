// Linux XComposite capture — C++ port of the Python root-shm
// fast path (see unio/features/capture_xcomposite.py).
//
// Day-2 scope: open the X display, bind XShm, grab the capture
// rect off the root window with one XShmGetImage call per frame,
// memcpy into a CpuFrame, fire the callback. One host-memory
// copy; no per-window composite yet. That's the path the Python
// version already uses when no overlay is mapped.

#if defined(__linux__)

#include "capture_xcomposite.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

namespace unio {

namespace {

std::uint64_t NowNs() {
    // system_clock (Unix epoch) — see encoder_vaapi.cpp for why
    // latency timestamps must be wall clock, not steady_clock.
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(
            system_clock::now().time_since_epoch()).count());
}

int SilentXError(Display*, XErrorEvent*) {
    // XComposite + rapid window destruction race us; transient
    // BadWindow / BadPixmap / BadDrawable must not abort the
    // process the way Xlib's default handler does. Returning 0
    // tells Xlib to continue.
    return 0;
}

}  // namespace

struct XCompositeCapture::Impl {
    Display* dpy = nullptr;
    Window root = 0;
    Visual* visual = nullptr;
    int depth = 0;
    int screen = 0;

    bool shm_ready = false;
    XShmSegmentInfo shm_info{};
    XImage* shm_image = nullptr;
    std::uint32_t shm_w = 0;
    std::uint32_t shm_h = 0;

    std::atomic<bool> running{false};
    std::thread thread;

    CaptureRect rect{};
    int fps = 60;
    FrameCallback callback = nullptr;
    void* user = nullptr;

    // Captured-frame counter. Increments monotonically from 1 so
    // the latency tracer can correlate without treating 0 as
    // "unstamped".
    std::uint64_t frame_id = 0;

    bool EnsureShmImage(int w, int h) {
        if (shm_image
                && static_cast<int>(shm_w) == w
                && static_cast<int>(shm_h) == h) {
            return true;
        }
        ReleaseShm();

        shm_image = XShmCreateImage(dpy, visual, depth, ZPixmap,
                                    nullptr, &shm_info, w, h);
        if (!shm_image) return false;

        int bytes_per_line = shm_image->bytes_per_line;
        std::size_t size =
            static_cast<std::size_t>(bytes_per_line) * h;

        shm_info.shmid = shmget(IPC_PRIVATE, size,
                                IPC_CREAT | 0600);
        if (shm_info.shmid < 0) {
            XDestroyImage(shm_image);
            shm_image = nullptr;
            return false;
        }
        shm_info.shmaddr = reinterpret_cast<char*>(
            shmat(shm_info.shmid, nullptr, 0));
        if (shm_info.shmaddr == reinterpret_cast<char*>(-1)) {
            shmctl(shm_info.shmid, IPC_RMID, nullptr);
            XDestroyImage(shm_image);
            shm_image = nullptr;
            return false;
        }
        shm_image->data = shm_info.shmaddr;
        shm_info.readOnly = False;
        if (!XShmAttach(dpy, &shm_info)) {
            shmdt(shm_info.shmaddr);
            shmctl(shm_info.shmid, IPC_RMID, nullptr);
            XDestroyImage(shm_image);
            shm_image = nullptr;
            return false;
        }
        // Marking the segment for deletion now is safe on Linux
        // SysV shm: it actually disappears once no process still
        // has it mapped, so a crash can't leak the segment.
        shmctl(shm_info.shmid, IPC_RMID, nullptr);
        XSync(dpy, False);

        shm_w = static_cast<std::uint32_t>(w);
        shm_h = static_cast<std::uint32_t>(h);
        shm_ready = true;
        return true;
    }

    void ReleaseShm() {
        if (!shm_ready && !shm_image) return;
        if (shm_ready) {
            XShmDetach(dpy, &shm_info);
        }
        if (shm_image) {
            XDestroyImage(shm_image);
            shm_image = nullptr;
        }
        if (shm_info.shmaddr
                && shm_info.shmaddr != reinterpret_cast<char*>(-1)) {
            shmdt(shm_info.shmaddr);
        }
        shm_info = XShmSegmentInfo{};
        shm_ready = false;
        shm_w = 0;
        shm_h = 0;
    }

    bool ProbeRoot() {
        // Mirror the Python _probe_root_grab: 64x64 grab, count
        // non-black samples. On Mutter X11 with a composited
        // desktop this always has content; on older GNOME Shell
        // (bypasses root pixmap) it's all-black, and we'd need
        // the per-window composite fallback — not ported yet.
        if (!EnsureShmImage(64, 64)) return false;
        if (!XShmGetImage(dpy, root, shm_image, 0, 0, AllPlanes)) {
            return false;
        }
        XSync(dpy, False);
        auto* data = reinterpret_cast<std::uint8_t*>(
            shm_image->data);
        int samples = 0;
        int non_black = 0;
        for (int y = 0; y < 64; y += 4) {
            for (int x = 0; x < 64; x += 4) {
                int off = y * shm_image->bytes_per_line + x * 4;
                if (data[off] || data[off + 1] || data[off + 2]) {
                    ++non_black;
                }
                ++samples;
            }
        }
        (void)samples;
        return non_black >= 3;
    }

    void CaptureLoop() {
        const int w = rect.width;
        const int h = rect.height;
        if (!EnsureShmImage(w, h)) {
            std::fprintf(stderr, "unio-pipe: EnsureShmImage failed "
                                 "(%dx%d)\n", w, h);
            return;
        }
        const auto period = std::chrono::nanoseconds(
            static_cast<std::int64_t>(1e9 / fps));
        auto next_tick = std::chrono::steady_clock::now();
        while (running.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();
            if (now < next_tick) {
                std::this_thread::sleep_for(next_tick - now);
            }
            next_tick += period;

            if (!XShmGetImage(dpy, root, shm_image,
                              rect.x, rect.y, AllPlanes)) {
                std::fprintf(stderr,
                             "unio-pipe: XShmGetImage failed\n");
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(50));
                continue;
            }
            XSync(dpy, False);

            // Copy shm buffer into a CpuFrame. The shm segment is
            // reused every tick, so we MUST copy before the next
            // grab — moving the buffer out would race with the
            // X server's write. 8 MB memcpy at 1080p, ~0.5 ms on
            // modern RAM.
            auto frame = std::make_unique<CpuFrame>();
            frame->width = static_cast<std::uint32_t>(w);
            frame->height = static_cast<std::uint32_t>(h);
            frame->stride_bytes = static_cast<std::uint32_t>(
                shm_image->bytes_per_line);
            frame->capture_monotonic_ns = NowNs();
            frame->frame_id = ++frame_id;
            const std::size_t total =
                static_cast<std::size_t>(frame->stride_bytes) * h;
            frame->pixels.assign(
                reinterpret_cast<std::uint8_t*>(shm_image->data),
                reinterpret_cast<std::uint8_t*>(shm_image->data)
                    + total);
            if (callback) {
                callback(std::move(frame), user);
            }
        }
    }
};

XCompositeCapture::XCompositeCapture()
    : impl_(std::make_unique<Impl>()) {}

XCompositeCapture::~XCompositeCapture() {
    Close();
    if (impl_->dpy) {
        impl_->ReleaseShm();
        XCloseDisplay(impl_->dpy);
        impl_->dpy = nullptr;
    }
}

bool XCompositeCapture::Open() {
    XSetErrorHandler(SilentXError);
    impl_->dpy = XOpenDisplay(nullptr);
    if (!impl_->dpy) return false;
    impl_->screen = DefaultScreen(impl_->dpy);
    impl_->root = RootWindow(impl_->dpy, impl_->screen);
    impl_->visual = DefaultVisual(impl_->dpy, impl_->screen);
    impl_->depth = DefaultDepth(impl_->dpy, impl_->screen);

    int major = 0;
    int event = 0;
    int error = 0;
    if (!XQueryExtension(impl_->dpy, "MIT-SHM", &major,
                         &event, &error)) {
        XCloseDisplay(impl_->dpy);
        impl_->dpy = nullptr;
        return false;
    }
    if (!impl_->ProbeRoot()) {
        // Root pixmap is all-black — WM doesn't composite to
        // root. The per-window composite fallback will land
        // when we port it; for Day 2 we fail loud instead.
        std::fprintf(stderr,
                     "unio-pipe: root-pixmap probe returned "
                     "all-black; this compositor isn't supported "
                     "yet. Stay on the Python capture path.\n");
        return false;
    }
    return true;
}

bool XCompositeCapture::Start(CaptureRect rect, int fps,
                              FrameCallback cb, void* user) {
    if (!impl_->dpy) return false;
    if (impl_->running.load()) return false;
    impl_->rect = rect;
    impl_->fps = fps > 0 ? fps : 60;
    impl_->callback = cb;
    impl_->user = user;
    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([this]() { impl_->CaptureLoop(); });
    return true;
}

void XCompositeCapture::Close() {
    if (!impl_) return;
    impl_->running.store(false, std::memory_order_release);
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

}  // namespace unio

#endif  // __linux__
