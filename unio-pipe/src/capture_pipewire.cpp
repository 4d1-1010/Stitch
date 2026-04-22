// PipeWire / xdg-desktop-Portal capture backend — Linux Wayland.
//
// Captures the screen via the standard Wayland stack:
//   1. sdbus-c++ calls xdg-desktop-portal ScreenCast API for session mgmt
//   2. libpipewire connects to the daemon and captures frames
//   3. Frames are pushed into SpscRing<CpuFrame> via FrameCallback

#if !defined(__linux__)
#error "capture_pipewire.cpp is Linux-only"
#endif

#include "capture_pipewire.h"
#include "frame.h"
#include "spsc_ring.h"

#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/loop.h>
#include <pipewire/core.h>
#include <pipewire/context.h>
#include <pipewire/properties.h>
#include <pipewire/impl-core.h>

#include <spa/param/param.h>
#include <spa/param/format.h>
#include <spa/param/video/format.h>
#include <spa/buffer/buffer.h>
#include <spa/utils/dict.h>

#ifdef UNIO_PIPE_HAS_SDBUS
#include <sdbus-c++/sdbus-c++.h>
#endif

#include <chrono>
#include <cstring>
#include <cstdio>
#include <memory>
#include <thread>

namespace unio {

namespace {

// D-Bus constants for xdg-desktop-portal ScreenCast API.
constexpr char kPortalService[] = "org.freedesktop.portal.Desktop";
constexpr char kPortalPath[] = "/org/freedesktop/portal/desktop";
constexpr char kPortalInterface[] = "org.freedesktop.portal.ScreenCast";
constexpr char kPortalCreateSession[] = "CreateSession";

// Monotonic clock in nanoseconds.
auto now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// ── Impl ─────────────────────────────────────────────────────────

struct PipeWireCapture::Impl {
    // PipeWire objects (C API).  PipeWire 1.x uses the context-based
    // API: pw_context → pw_impl_core (opaque).
    struct pw_context* ctx = nullptr;
    struct pw_loop* loop = nullptr;
    struct pw_impl_core* impl_core = nullptr;
    struct pw_stream* stream = nullptr;
    spa_hook stream_hook{};

    // Capture state.
    bool running = false;
    PipewireRect rect;
    int fps = 30;
    FrameCallback cb = nullptr;
    void* user = nullptr;
    std::thread capture_thread;

    // Frame buffer — reused across grabs to avoid allocation.
    std::vector<uint8_t> frame_buf;
    uint32_t frame_w = 0;
    uint32_t frame_h = 0;
    uint64_t frame_id = 0;

    // Portal session (D-Bus).
#ifdef UNIO_PIPE_HAS_SDBUS
    std::unique_ptr<sdbus::IConnection> dbus_conn;
    std::string session_path;
    std::string portal_handle;
#endif
};

// ── PipeWire frame handler ───────────────────────────────────────

static void on_stream_data(void* user_data)
{
    auto* impl = static_cast<PipeWireCapture::Impl*>(user_data);
    if (!impl || !impl->stream || !impl->running) return;

    auto* buf = pw_stream_dequeue_buffer(impl->stream);
    if (!buf) return;

    // PipeWire 1.x: buf->buffer is a struct spa_buffer* with
    // n_datas / datas[] instead of the old n_chunks / chunks[].
    // Each spa_data has a spa_chunk* (not inline size).
    auto* spa_buf = buf->buffer;
    if (spa_buf) {
        for (uint32_t i = 0; i < spa_buf->n_datas; i++) {
            auto& d = spa_buf->datas[i];
            if (!d.data || !d.chunk) continue;
            if (d.type != SPA_DATA_MemPtr) continue;
            if (!(d.flags & SPA_DATA_FLAG_READABLE)) continue;

            const auto* data = static_cast<const uint8_t*>(d.data);
            uint32_t size = static_cast<uint32_t>(d.chunk->size);

            // Allocate frame buffer if needed.
            if (impl->frame_buf.size() < size) {
                impl->frame_buf.resize(size);
            }
            std::memcpy(impl->frame_buf.data(), data, size);

            // Create CpuFrame (BGRA layout matches CpuFrame::pixels).
            auto frame = std::make_unique<CpuFrame>();
            frame->width = impl->frame_w;
            frame->height = impl->frame_h;
            frame->stride_bytes = impl->frame_w * 4;
            frame->pixels.assign(impl->frame_buf.begin(),
                                 impl->frame_buf.end());
            frame->frame_id = ++impl->frame_id;
            frame->capture_monotonic_ns = now_ns();

            if (impl->cb) {
                impl->cb(std::move(frame), impl->user);
            }
            break;  // take first readable data slot
        }
    }

    pw_stream_queue_buffer(impl->stream, buf);
}

static void on_stream_state_changed(void* user_data,
                                    enum pw_stream_state old,
                                    enum pw_stream_state state,
                                    const char* error)
{
    auto* impl = static_cast<PipeWireCapture::Impl*>(user_data);
    if (!impl) return;

    (void)old;

    if (state == PW_STREAM_STATE_STREAMING) {
        impl->running = true;
    } else if (state == PW_STREAM_STATE_ERROR) {
        impl->running = false;
        std::fprintf(stderr, "PipeWireCapture: stream error: %s\n",
                     error ? error : "(none)");
    }
}

static const struct pw_stream_events stream_events = {
    .version       = PW_VERSION_STREAM_EVENTS,
    .destroy       = nullptr,
    .state_changed = on_stream_state_changed,
    .control_info  = nullptr,
    .io_changed    = nullptr,
    .param_changed = nullptr,
    .add_buffer    = nullptr,
    .remove_buffer = nullptr,
    .process       = on_stream_data,
    .drained       = nullptr,
    .command       = nullptr,
    .trigger_done  = nullptr,
};

// ── Open / Close ─────────────────────────────────────────────────

bool PipeWireCapture::Open()
{
    impl_ = std::make_unique<Impl>();
    auto* impl = impl_.get();

    // Check for Wayland session.
    if (!std::getenv("WAYLAND_DISPLAY")) {
        std::fprintf(stderr,
                     "PipeWireCapture: no WAYLAND_DISPLAY — not a "
                     "Wayland session\n");
        return false;
    }

    // Initialize libpipewire.
    pw_init(nullptr, nullptr);

    // Create main loop.
    impl->loop = pw_loop_new(nullptr);
    if (!impl->loop) {
        std::fprintf(stderr, "PipeWireCapture: failed to create pw loop\n");
        return false;
    }

    // PipeWire 1.x: create a context then a core from it.
    impl->ctx = pw_context_new(impl->loop, nullptr, 0);
    if (!impl->ctx) {
        std::fprintf(stderr, "PipeWireCapture: failed to create pw context\n");
        pw_loop_destroy(impl->loop);
        impl->loop = nullptr;
        return false;
    }

    impl->impl_core = pw_context_create_core(impl->ctx, nullptr, 0);
    if (!impl->impl_core) {
        std::fprintf(stderr, "PipeWireCapture: failed to create pw core\n");
        pw_context_destroy(impl->ctx);
        impl->ctx = nullptr;
        pw_loop_destroy(impl->loop);
        impl->loop = nullptr;
        return false;
    }

    std::fprintf(stderr, "PipeWireCapture: libpipewire initialized\n");
    return true;
}

void PipeWireCapture::Close()
{
    if (impl_) {
        impl_->running = false;
        if (impl_->stream) {
            pw_stream_destroy(impl_->stream);
            impl_->stream = nullptr;
        }
        if (impl_->ctx) {
            pw_context_destroy(impl_->ctx);
            impl_->ctx = nullptr;
        }
        if (impl_->loop) {
            if (impl_->capture_thread.joinable()) {
                impl_->capture_thread.join();
            }
            pw_loop_destroy(impl_->loop);
            impl_->loop = nullptr;
        }
        impl_ = nullptr;
    }
    // NOTE: pw_deinit() is intentionally NOT called here.
    // libpipewire's deinit is a global teardown that should only
    // run once at process exit. The capture backend may be opened
    // and closed multiple times during the process lifetime.
}

// ── Start ────────────────────────────────────────────────────────

bool PipeWireCapture::Start(PipewireRect rect, int fps,
                            FrameCallback cb, void* user)
{
    if (!impl_ || !impl_->impl_core) return false;

    impl_->rect = rect;
    impl_->fps = fps;
    impl_->cb = cb;
    impl_->user = user;
    impl_->running = false;

    // Create PipeWire stream via the simple API (loop-based).
    // pw_stream_new_simple takes pw_loop* + name + props + events + user.
    impl_->stream = pw_stream_new_simple(impl_->loop,
        "unio-capture", nullptr,
        &stream_events, impl_.get());
    if (!impl_->stream) {
        std::fprintf(stderr, "PipeWireCapture: failed to create stream\n");
        return false;
    }

    // Set stream properties using spa_dict (PipeWire 1.x API).
    struct spa_dict_item items[] = {
        SPA_DICT_ITEM("media.type", "video"),
        SPA_DICT_ITEM("video.format", "BGRA"),
        SPA_DICT_ITEM("video.size", "1920x1080"),
    };
    struct spa_dict dict = SPA_DICT(items, 3);
    pw_stream_update_properties(impl_->stream, &dict);

    // Connect the stream to the portal screen-cast node.
    // PW_ID_ANY tells PipeWire to pick the first available screen-cast source.
    // Note: flags must be cast to enum pw_stream_flags (PipeWire 1.x).
    pw_stream_connect(impl_->stream,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        static_cast<enum pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        nullptr, 0);

    // Start the capture thread — pw_loop_iterate() in a blocking loop.
    // Copy loop pointer into the lambda so the thread does not
    // depend on this object staying alive (Close() sets impl_ to
    // nullptr and the PipeWireCapture is destroyed after join()).
    struct pw_loop* loop = impl_->loop;
    impl_->capture_thread = std::thread([loop]() {
        if (loop) {
            while (pw_loop_iterate(loop, -1) >= 0) {
                /* spin */
            }
        }
    });

    std::fprintf(stderr, "PipeWireCapture: started %dx%d @ %d fps\n",
                 rect.width, rect.height, fps);
    return true;
}

// ── Constructor / Destructor ─────────────────────────────────────

PipeWireCapture::PipeWireCapture() = default;
PipeWireCapture::~PipeWireCapture() = default;

}  // namespace unio