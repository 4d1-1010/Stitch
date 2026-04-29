/// @file clipboard_x11.cpp
/// @brief Native X11 clipboard implementation. Owns its own
/// Display* + invisible Window* and walks the X selection
/// protocol synchronously: XConvertSelection + wait for
/// SelectionNotify on @c get_text(); XSetSelectionOwner +
/// service SelectionRequest events on @c set_text().
///
/// No xclip / xsel shell-out — keeps the runtime dep surface
/// at zero, matching the project's minimal-deps preference.
///
/// Threading: a dedicated event thread polls the X connection
/// FD and drains pending events (SelectionRequest from other
/// apps pasting from us; SelectionClear when we lose
/// ownership; SelectionNotify completing a get_text request).
/// The polling thread inside the orchestrator's
/// ClipboardMonitor calls @c get_text / @c set_text from a
/// separate thread; both APIs synchronise with the event
/// thread via the internal mutex + a condvar that the event
/// thread fires whenever a SelectionNotify lands.
///
/// INCR transfer is not yet implemented — payloads larger
/// than the X server's max-request-size are dropped.

#include "orchestrator/clipboard_backend.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <poll.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace unio_ui::orchestrator {

namespace {

class X11ClipboardBackend final : public IClipboardBackend {
public:
    bool open() override {
        std::lock_guard lk(m_);
        if (display_ != nullptr) return true;
        display_ = XOpenDisplay(nullptr);
        if (display_ == nullptr) {
            std::fprintf(stderr,
                         "clipboard_x11: XOpenDisplay() returned nullptr\n");
            return false;
        }
        const int screen = DefaultScreen(display_);
        // Invisible 1x1 window — selection owner / requestor.
        // PropertyChangeMask so a future INCR receive path can
        // monitor property updates.
        window_ = XCreateSimpleWindow(
            display_, RootWindow(display_, screen),
            0, 0, 1, 1, 0,
            BlackPixel(display_, screen),
            BlackPixel(display_, screen));
        XSelectInput(display_, window_, PropertyChangeMask);

        a_clipboard_ = XInternAtom(display_, "CLIPBOARD",      False);
        a_utf8_      = XInternAtom(display_, "UTF8_STRING",    False);
        a_targets_   = XInternAtom(display_, "TARGETS",        False);
        a_string_    = XInternAtom(display_, "STRING",         False);
        a_text_      = XInternAtom(display_, "TEXT",           False);
        a_prop_      = XInternAtom(display_, "UNIO_CLIP_PROP", False);
        a_incr_      = XInternAtom(display_, "INCR",           False);
        XFlush(display_);

        running_.store(true, std::memory_order_release);
        event_thread_ = std::thread(&X11ClipboardBackend::event_loop, this);
        return true;
    }

    void close() override {
        running_.store(false, std::memory_order_release);
        // Wake the event thread's poll() — push an XNoOp via
        // the X connection by setting an event that flushes
        // through.
        {
            std::lock_guard lk(m_);
            if (display_ != nullptr) {
                XEvent ev{};
                ev.xclient.type         = ClientMessage;
                ev.xclient.window       = window_;
                ev.xclient.message_type = XInternAtom(
                    display_, "UNIO_CLIP_WAKE", False);
                ev.xclient.format       = 32;
                XSendEvent(display_, window_, False,
                            NoEventMask, &ev);
                XFlush(display_);
            }
        }
        if (event_thread_.joinable()) event_thread_.join();

        std::lock_guard lk(m_);
        if (display_ == nullptr) return;
        if (window_ != 0) {
            XDestroyWindow(display_, window_);
            window_ = 0;
        }
        XCloseDisplay(display_);
        display_ = nullptr;
    }

    std::string get_text() override {
        std::unique_lock lk(m_);
        if (display_ == nullptr) return {};
        // Fast path: we already own the clipboard, so the
        // current text is whatever we last wrote. Avoids the
        // round-trip XConvertSelection would do.
        if (have_ownership_) return owned_text_;

        // Ask the current owner to convert into UTF-8 and
        // write the bytes to our scratch property.
        pending_get_done_   = false;
        pending_get_result_.clear();
        XConvertSelection(display_, a_clipboard_, a_utf8_,
                           a_prop_, window_, CurrentTime);
        XFlush(display_);

        // Event thread sees the SelectionNotify, reads the
        // property, fills the result, and signals — wait up
        // to 200 ms (matches Synergy's typical paste latency
        // budget).
        pending_get_cv_.wait_for(
            lk, std::chrono::milliseconds(200),
            [&]{ return pending_get_done_; });
        std::string out;
        out.swap(pending_get_result_);
        pending_get_done_ = false;
        return out;
    }

    void set_text(const std::string& text) override {
        std::lock_guard lk(m_);
        if (display_ == nullptr) return;
        owned_text_ = text;
        XSetSelectionOwner(display_, a_clipboard_,
                            window_, CurrentTime);
        const Window owner = XGetSelectionOwner(display_, a_clipboard_);
        have_ownership_ = (owner == window_);
        XFlush(display_);
    }

private:
    /// @brief Event-loop thread body. Blocks in poll() on the
    /// X connection FD with a short timeout so close() can
    /// notice the running_ flag flipping. Drains every
    /// pending event under the mutex and dispatches.
    void event_loop() {
        int fd = -1;
        {
            std::lock_guard lk(m_);
            if (display_ != nullptr) {
                fd = ConnectionNumber(display_);
            }
        }
        while (running_.load(std::memory_order_acquire)) {
            if (fd >= 0) {
                struct pollfd pfd{};
                pfd.fd     = fd;
                pfd.events = POLLIN;
                ::poll(&pfd, 1, 50);  // 50 ms tick
            }

            std::unique_lock lk(m_);
            if (display_ == nullptr) return;
            while (XPending(display_)) {
                XEvent e;
                XNextEvent(display_, &e);
                dispatch_event_locked(e, lk);
            }
        }
    }

    void dispatch_event_locked(const XEvent& e,
                                 std::unique_lock<std::mutex>& lk) {
        switch (e.type) {
            case SelectionRequest:
                handle_selection_request_locked(e.xselectionrequest);
                break;
            case SelectionClear:
                if (e.xselectionclear.selection == a_clipboard_) {
                    have_ownership_ = false;
                    owned_text_.clear();
                }
                break;
            case SelectionNotify:
                handle_selection_notify_locked(e.xselection);
                pending_get_cv_.notify_all();
                (void)lk;  // condvar uses the same lock
                break;
            default:
                break;
        }
    }

    void handle_selection_notify_locked(
        const XSelectionEvent& ev) {
        if (ev.selection != a_clipboard_) return;
        if (ev.property == None) {
            // Owner refused — clipboard empty / non-text.
            pending_get_result_.clear();
            pending_get_done_ = true;
            return;
        }
        pending_get_result_ =
            read_string_property_locked(ev.property);
        pending_get_done_ = true;
    }

    /// @brief Build a reply for an external client's
    /// SelectionRequest on our owned CLIPBOARD selection.
    void handle_selection_request_locked(
        const XSelectionRequestEvent& req) {
        XSelectionEvent reply{};
        reply.type      = SelectionNotify;
        reply.display   = display_;
        reply.requestor = req.requestor;
        reply.selection = req.selection;
        reply.target    = req.target;
        reply.time      = req.time;
        reply.property  = None;  // refusal default

        if (req.target == a_targets_) {
            // Advertise the formats we support.
            const Atom targets[] = {
                a_targets_, a_utf8_, a_string_, a_text_,
            };
            XChangeProperty(display_, req.requestor, req.property,
                             XA_ATOM, 32, PropModeReplace,
                             reinterpret_cast<const unsigned char*>(targets),
                             sizeof(targets) / sizeof(Atom));
            reply.property = req.property;
        } else if (req.target == a_utf8_
                   || req.target == a_string_
                   || req.target == a_text_) {
            const Atom type =
                (req.target == a_utf8_) ? a_utf8_ : a_string_;
            XChangeProperty(display_, req.requestor, req.property,
                             type, 8, PropModeReplace,
                             reinterpret_cast<const unsigned char*>(
                                 owned_text_.data()),
                             static_cast<int>(owned_text_.size()));
            reply.property = req.property;
        }

        XSendEvent(display_, req.requestor, False, NoEventMask,
                    reinterpret_cast<XEvent*>(&reply));
        XFlush(display_);
    }

    /// @brief Pull the bytes of @p prop off our window into a
    /// std::string and delete the property. Caller holds the
    /// lock. Single-shot — INCR transfer not implemented yet
    /// (oversized payloads silently drop).
    std::string read_string_property_locked(Atom prop) {
        Atom            actual_type = None;
        int             actual_fmt  = 0;
        unsigned long   nitems      = 0;
        unsigned long   bytes_after = 0;
        unsigned char*  data        = nullptr;
        if (XGetWindowProperty(display_, window_, prop,
                                0, 0, False, AnyPropertyType,
                                &actual_type, &actual_fmt,
                                &nitems, &bytes_after,
                                &data) != Success) {
            return {};
        }
        if (data != nullptr) XFree(data);
        if (actual_type == a_incr_) {
            std::fprintf(stderr,
                         "clipboard_x11: INCR transfer not yet "
                         "supported; dropping >max-request paste\n");
            XDeleteProperty(display_, window_, prop);
            return {};
        }
        if (actual_type != a_utf8_ && actual_type != a_string_) {
            XDeleteProperty(display_, window_, prop);
            return {};
        }
        const unsigned long total =
            (bytes_after + sizeof(long) - 1) / sizeof(long);
        if (XGetWindowProperty(display_, window_, prop,
                                0, static_cast<long>(total),
                                False, AnyPropertyType,
                                &actual_type, &actual_fmt,
                                &nitems, &bytes_after,
                                &data) != Success
            || data == nullptr) {
            return {};
        }
        std::string out(reinterpret_cast<const char*>(data),
                        static_cast<std::size_t>(nitems));
        XFree(data);
        XDeleteProperty(display_, window_, prop);
        return out;
    }

    std::mutex              m_;
    Display*                display_         = nullptr;
    Window                  window_          = 0;
    Atom                    a_clipboard_     = None;
    Atom                    a_utf8_          = None;
    Atom                    a_targets_       = None;
    Atom                    a_string_        = None;
    Atom                    a_text_          = None;
    Atom                    a_prop_          = None;
    Atom                    a_incr_          = None;
    bool                    have_ownership_  = false;
    std::string             owned_text_;

    /// @brief Dedicated event thread: services
    /// SelectionRequest (other apps pasting from us) and
    /// SelectionNotify (our own get_text completion). Runs
    /// from open() to close().
    std::thread             event_thread_;
    std::atomic<bool>       running_{false};

    /// @brief Cross-thread coordination for get_text. The API
    /// caller blocks on @c pending_get_cv_; the event thread
    /// stamps the result into @c pending_get_result_ when
    /// SelectionNotify arrives.
    std::condition_variable pending_get_cv_;
    bool                    pending_get_done_ = false;
    std::string             pending_get_result_;
};

}  // namespace

std::unique_ptr<IClipboardBackend> make_default_clipboard_backend() {
    return std::make_unique<X11ClipboardBackend>();
}

}  // namespace unio_ui::orchestrator
