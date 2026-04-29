/// @file clipboard_x11.cpp
/// @brief Native X11 clipboard implementation, multi-format
/// (UTF-8 text + text/html + image/png). Owns its own Display*
/// + invisible Window* and walks the X selection protocol
/// synchronously: XConvertSelection + wait for SelectionNotify
/// per format on @c get_clipboard(); XSetSelectionOwner +
/// service SelectionRequest events for every format we hold
/// on @c set_clipboard().
///
/// No xclip / xsel shell-out — keeps the runtime dep surface
/// at zero, matching the project's minimal-deps preference.
///
/// Threading: a dedicated event thread polls the X connection
/// FD and drains pending events (SelectionRequest from other
/// apps pasting from us; SelectionClear when we lose
/// ownership; SelectionNotify completing a get_clipboard
/// request). The polling thread inside the orchestrator's
/// ClipboardMonitor calls @c get_clipboard / @c set_clipboard
/// from a separate thread; both APIs synchronise with the
/// event thread via the internal mutex + a condvar that the
/// event thread fires whenever a SelectionNotify lands.
///
/// INCR transfer is not yet implemented — payloads larger
/// than the X server's max-request-size are dropped (mostly
/// affects very large screenshots).

#include "orchestrator/clipboard_backend.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <poll.h>
#include <sys/stat.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace unio_ui::orchestrator {

namespace {

/// @brief Sniff the image MIME from the first few magic bytes
/// of @p bytes. Empty string if no signature matches.
std::string sniff_image_mime(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4) return {};
    if (bytes[0] == 0x89 && bytes[1] == 0x50
        && bytes[2] == 0x4E && bytes[3] == 0x47) {
        return "image/png";
    }
    if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return "image/jpeg";
    }
    if (bytes[0] == 'B' && bytes[1] == 'M') {
        return "image/bmp";
    }
    if (bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F') {
        return "image/gif";
    }
    return {};
}

/// @brief Read every byte of @p path into a vector. Empty
/// vector on any I/O error or if the file is bigger than
/// @c kMaxFileSize (a safety cap so a stray HTML reference
/// pointing at a multi-GB file doesn't OOM us).
std::vector<std::uint8_t> read_file_bytes(const std::string& path) {
    constexpr std::size_t kMaxFileSize = 16 * 1024 * 1024;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return {};
    if (st.st_size <= 0
        || static_cast<std::size_t>(st.st_size) > kMaxFileSize) {
        return {};
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(st.st_size));
    f.read(reinterpret_cast<char*>(bytes.data()),
           static_cast<std::streamsize>(bytes.size()));
    if (!f) return {};
    return bytes;
}

/// @brief Scan @p html for the first @c <img src="file://..."> /
/// @c <img src='file://...'> reference, dereference it, and
/// return the file's bytes + sniffed MIME. Empty result if no
/// file-URL is found, the file is unreadable, or the bytes
/// don't look like a known image format.
///
/// LibreOffice on Linux puts ONLY HTML on the clipboard when
/// rich text + image are selected together — the image is
/// referenced via @c file:///tmp/lu...tmp/... pointing at a
/// temp file the LibreOffice process maintains for the
/// lifetime of the selection. The X selection protocol has
/// no image target in that case, so we have to dereference
/// the URL ourselves to give receivers actual image bytes.
std::vector<std::uint8_t>
extract_html_file_image(const std::string& html,
                          std::string& out_mime) {
    out_mime.clear();
    std::size_t pos = 0;
    while (pos < html.size()) {
        const std::size_t img_pos = html.find("<img", pos);
        if (img_pos == std::string::npos) break;
        const std::size_t tag_end = html.find('>', img_pos);
        if (tag_end == std::string::npos) break;
        const std::size_t src_pos = html.find("src=", img_pos);
        if (src_pos == std::string::npos || src_pos > tag_end) {
            pos = tag_end + 1;
            continue;
        }
        const std::size_t quote_pos = src_pos + 4;
        if (quote_pos >= html.size()) break;
        const char qc = html[quote_pos];
        if (qc != '"' && qc != '\'') {
            pos = tag_end + 1;
            continue;
        }
        const std::size_t value_start = quote_pos + 1;
        const std::size_t value_end =
            html.find(qc, value_start);
        if (value_end == std::string::npos
            || value_end > tag_end) {
            pos = tag_end + 1;
            continue;
        }
        const std::string url =
            html.substr(value_start, value_end - value_start);
        if (url.compare(0, 7, "file://") != 0) {
            pos = tag_end + 1;
            continue;
        }
        const std::string path = url.substr(7);
        auto bytes = read_file_bytes(path);
        if (bytes.empty()) {
            pos = tag_end + 1;
            continue;
        }
        out_mime = sniff_image_mime(bytes);
        if (out_mime.empty()) {
            // Unknown format — fall through to next <img>.
            pos = tag_end + 1;
            continue;
        }
        return bytes;
    }
    return {};
}

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

        a_clipboard_ = XInternAtom(display_, "CLIPBOARD",       False);
        a_utf8_      = XInternAtom(display_, "UTF8_STRING",     False);
        a_targets_   = XInternAtom(display_, "TARGETS",         False);
        a_string_    = XInternAtom(display_, "STRING",          False);
        a_text_      = XInternAtom(display_, "TEXT",            False);
        a_html_      = XInternAtom(display_, "text/html",       False);
        a_png_       = XInternAtom(display_, "image/png",       False);
        a_jpeg_      = XInternAtom(display_, "image/jpeg",      False);
        a_bmp_       = XInternAtom(display_, "image/bmp",       False);
        a_prop_      = XInternAtom(display_, "UNIO_CLIP_PROP",  False);
        a_incr_      = XInternAtom(display_, "INCR",            False);
        XFlush(display_);

        running_.store(true, std::memory_order_release);
        event_thread_ = std::thread(&X11ClipboardBackend::event_loop, this);
        return true;
    }

    void close() override {
        running_.store(false, std::memory_order_release);
        // Wake the event thread's poll() — push a no-op
        // ClientMessage through the X connection so XPending
        // returns and the loop checks running_.
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

    ClipboardData get_clipboard() override {
        std::unique_lock lk(m_);
        if (display_ == nullptr) return {};
        // Fast path: we already own the clipboard, so the
        // current state is whatever we last wrote.
        if (have_ownership_) {
            ClipboardData d;
            d.text        = owned_text_;
            d.html        = owned_html_;
            d.image_mime  = owned_image_mime_;
            d.image_bytes = owned_image_bytes_;
            return d;
        }

        ClipboardData out;
        // Try UTF-8 text.
        auto text_bytes = request_target_locked(lk, a_utf8_);
        if (text_bytes) {
            out.text.assign(
                reinterpret_cast<const char*>(text_bytes->data()),
                text_bytes->size());
        }
        // Try HTML.
        auto html_bytes = request_target_locked(lk, a_html_);
        if (html_bytes) {
            out.html.assign(
                reinterpret_cast<const char*>(html_bytes->data()),
                html_bytes->size());
        }
        // Try image formats in priority order. PNG is the
        // ideal target because it's universally renderable;
        // JPEG and BMP are common fallbacks (LibreOffice keeps
        // the source format when an embedded image is JPEG,
        // some screenshot tools emit BMP). Take the first
        // format the owner agrees to convert.
        struct ImgTarget { Atom atom; const char* mime; };
        const ImgTarget kImageTargets[] = {
            { a_png_,  "image/png"  },
            { a_jpeg_, "image/jpeg" },
            { a_bmp_,  "image/bmp"  },
        };
        for (const auto& t : kImageTargets) {
            auto img_bytes = request_target_locked(lk, t.atom);
            if (img_bytes && !img_bytes->empty()) {
                out.image_bytes = std::move(*img_bytes);
                out.image_mime  = t.mime;
                break;
            }
        }

        // LibreOffice fallback: when text+image are selected
        // together, the X selection only exposes text targets
        // — the image lives in a temp file referenced via
        // <img src="file:///tmp/lu...">. Dereference the URL
        // ourselves so receivers get actual image bytes.
        if (out.image_bytes.empty() && !out.html.empty()) {
            std::string sniffed_mime;
            auto file_bytes =
                extract_html_file_image(out.html, sniffed_mime);
            if (!file_bytes.empty()) {
                out.image_bytes = std::move(file_bytes);
                out.image_mime  = std::move(sniffed_mime);
            }
        }
        return out;
    }

    void set_clipboard(const ClipboardData& data) override {
        std::lock_guard lk(m_);
        if (display_ == nullptr) return;
        owned_text_        = data.text;
        owned_html_        = data.html;
        owned_image_mime_  = data.image_mime;
        owned_image_bytes_ = data.image_bytes;
        XSetSelectionOwner(display_, a_clipboard_,
                            window_, CurrentTime);
        const Window owner = XGetSelectionOwner(display_, a_clipboard_);
        have_ownership_ = (owner == window_);
        XFlush(display_);
    }

private:
    /// @brief Issue XConvertSelection for @p target and block
    /// until SelectionNotify (and any subsequent INCR
    /// PropertyNotify chunks) complete or the request times
    /// out. @c m_ is held by the caller (passed as @p lk so
    /// we can wait on the condvar). Returns the property
    /// bytes if the owner agreed; nullopt if the owner
    /// refused, the request timed out, or the property type
    /// didn't match.
    std::optional<std::vector<std::uint8_t>>
    request_target_locked(std::unique_lock<std::mutex>& lk,
                           Atom target) {
        pending_get_done_     = false;
        pending_get_incr_     = false;
        pending_get_target_   = target;
        pending_get_result_.clear();
        XConvertSelection(display_, a_clipboard_, target,
                           a_prop_, window_, CurrentTime);
        XFlush(display_);

        // Two timeouts: the SelectionNotify reply (≤200 ms
        // typical) and, if it indicates INCR, the full chunk
        // sequence (large LibreOffice / GIMP images can take
        // several seconds even on localhost). 5 s covers the
        // realistic max — larger payloads get truncated.
        const auto reply_timeout = std::chrono::milliseconds(500);
        const auto incr_timeout  = std::chrono::seconds(5);
        const auto deadline =
            std::chrono::steady_clock::now() + incr_timeout;
        if (!pending_get_cv_.wait_for(
                lk, reply_timeout,
                [&]{ return pending_get_done_
                          || pending_get_incr_; })) {
            // No reply at all.
            pending_get_target_ = None;
            return std::nullopt;
        }
        // INCR: wait for the chunk sequence to complete (or
        // timeout). Each PropertyNotify chunk re-fires the
        // condvar; the empty-chunk final signal flips
        // pending_get_done_.
        while (pending_get_incr_ && !pending_get_done_) {
            if (pending_get_cv_.wait_until(
                    lk, deadline,
                    [&]{ return pending_get_done_; })) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr,
                             "clipboard_x11: INCR transfer timed "
                             "out — payload truncated\n");
                break;
            }
        }
        const bool was_incr = pending_get_incr_;
        pending_get_done_   = false;
        pending_get_incr_   = false;
        pending_get_target_ = None;
        if (pending_get_result_.empty()) return std::nullopt;
        std::vector<std::uint8_t> out;
        out.swap(pending_get_result_);
        // Suppress unused-variable warning when logging is
        // turned off — keeps the local around for future
        // diagnostic prints.
        (void)was_incr;
        return out;
    }

    /// @brief Event-loop thread body. Blocks in poll() on the
    /// X connection FD with a short timeout so close() can
    /// notice the running_ flag flipping.
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
                ::poll(&pfd, 1, 50);
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
                    owned_html_.clear();
                    owned_image_mime_.clear();
                    owned_image_bytes_.clear();
                }
                break;
            case SelectionNotify:
                handle_selection_notify_locked(e.xselection);
                pending_get_cv_.notify_all();
                (void)lk;
                break;
            case PropertyNotify:
                // INCR chunk delivery: when the owner advertised
                // INCR on the SelectionNotify, every subsequent
                // property write to a_prop_ is one chunk. We
                // read+delete to acknowledge; an empty chunk
                // ends the transfer.
                if (pending_get_incr_
                    && e.xproperty.window   == window_
                    && e.xproperty.atom     == a_prop_
                    && e.xproperty.state    == PropertyNewValue) {
                    handle_incr_chunk_locked();
                    pending_get_cv_.notify_all();
                }
                break;
            default:
                break;
        }
    }

    void handle_selection_notify_locked(
        const XSelectionEvent& ev) {
        if (ev.selection != a_clipboard_) return;
        // Defence: only consume if it matches the target we
        // last requested. SelectionNotifies for other targets
        // (a stale request, an external client racing us) are
        // ignored rather than overwriting our pending result.
        if (ev.target != pending_get_target_) {
            pending_get_result_.clear();
            pending_get_done_ = true;
            return;
        }
        if (ev.property == None) {
            pending_get_result_.clear();
            pending_get_done_ = true;
            return;
        }

        // Peek the property type. INCR means the owner is
        // about to deliver the data in chunks via subsequent
        // property updates; we delete the property to
        // acknowledge and switch into INCR mode.
        Atom            actual_type = None;
        int             actual_fmt  = 0;
        unsigned long   nitems      = 0;
        unsigned long   bytes_after = 0;
        unsigned char*  data        = nullptr;
        if (XGetWindowProperty(display_, window_, ev.property,
                                0, 0, False, AnyPropertyType,
                                &actual_type, &actual_fmt,
                                &nitems, &bytes_after,
                                &data) != Success) {
            pending_get_result_.clear();
            pending_get_done_ = true;
            return;
        }
        if (data != nullptr) XFree(data);

        if (actual_type == a_incr_) {
            pending_get_incr_ = true;
            pending_get_result_.clear();
            // Acknowledge by deleting; owner now starts
            // sending PropertyNotify chunks.
            XDeleteProperty(display_, window_, ev.property);
            XFlush(display_);
            return;
        }

        // Single-shot read for non-INCR replies.
        pending_get_result_ =
            read_property_bytes_locked(ev.property);
        pending_get_done_ = true;
    }

    /// @brief Drain one INCR chunk: read + delete the property
    /// and append its bytes to @ref pending_get_result_. An
    /// empty chunk signals end-of-transfer; we flip
    /// @ref pending_get_done_ so the API caller wakes up with
    /// the accumulated payload.
    void handle_incr_chunk_locked() {
        Atom            actual_type = None;
        int             actual_fmt  = 0;
        unsigned long   nitems      = 0;
        unsigned long   bytes_after = 0;
        unsigned char*  data        = nullptr;
        // First pass to learn the size.
        if (XGetWindowProperty(display_, window_, a_prop_,
                                0, 0, False, AnyPropertyType,
                                &actual_type, &actual_fmt,
                                &nitems, &bytes_after,
                                &data) != Success) {
            pending_get_done_ = true;
            return;
        }
        if (data != nullptr) XFree(data);
        // long_length is the count of 32-bit units to fetch,
        // not sizeof(long); see read_property_bytes_locked.
        const unsigned long total = (bytes_after + 3) / 4;
        if (total == 0) {
            // Empty chunk → end of transfer.
            XDeleteProperty(display_, window_, a_prop_);
            XFlush(display_);
            pending_get_done_ = true;
            return;
        }
        if (XGetWindowProperty(display_, window_, a_prop_,
                                0, static_cast<long>(total),
                                False, AnyPropertyType,
                                &actual_type, &actual_fmt,
                                &nitems, &bytes_after,
                                &data) != Success
            || data == nullptr) {
            XDeleteProperty(display_, window_, a_prop_);
            XFlush(display_);
            pending_get_done_ = true;
            return;
        }
        pending_get_result_.insert(
            pending_get_result_.end(),
            data, data + nitems);
        XFree(data);
        // Acknowledge: delete the property so the owner sends
        // the next chunk.
        XDeleteProperty(display_, window_, a_prop_);
        XFlush(display_);
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

        // Resolve the atom matching our captured image MIME
        // (if any) so we know which target to advertise +
        // respond to. Empty owned_image_mime_ → no image.
        const Atom owned_image_atom =
              owned_image_mime_ == "image/png"  ? a_png_
            : owned_image_mime_ == "image/jpeg" ? a_jpeg_
            : owned_image_mime_ == "image/bmp"  ? a_bmp_
            : None;

        if (req.target == a_targets_) {
            // Advertise only the formats we actually hold.
            std::vector<Atom> targets;
            targets.push_back(a_targets_);
            if (!owned_text_.empty()) {
                targets.push_back(a_utf8_);
                targets.push_back(a_string_);
                targets.push_back(a_text_);
            }
            if (!owned_html_.empty()) {
                targets.push_back(a_html_);
            }
            if (owned_image_atom != None
                && !owned_image_bytes_.empty()) {
                targets.push_back(owned_image_atom);
            }
            XChangeProperty(display_, req.requestor, req.property,
                             XA_ATOM, 32, PropModeReplace,
                             reinterpret_cast<const unsigned char*>(
                                 targets.data()),
                             static_cast<int>(targets.size()));
            reply.property = req.property;
        } else if ((req.target == a_utf8_
                    || req.target == a_string_
                    || req.target == a_text_)
                   && !owned_text_.empty()) {
            const Atom type =
                (req.target == a_utf8_) ? a_utf8_ : a_string_;
            XChangeProperty(display_, req.requestor, req.property,
                             type, 8, PropModeReplace,
                             reinterpret_cast<const unsigned char*>(
                                 owned_text_.data()),
                             static_cast<int>(owned_text_.size()));
            reply.property = req.property;
        } else if (req.target == a_html_ && !owned_html_.empty()) {
            XChangeProperty(display_, req.requestor, req.property,
                             a_html_, 8, PropModeReplace,
                             reinterpret_cast<const unsigned char*>(
                                 owned_html_.data()),
                             static_cast<int>(owned_html_.size()));
            reply.property = req.property;
        } else if (req.target == owned_image_atom
                   && !owned_image_bytes_.empty()) {
            XChangeProperty(display_, req.requestor, req.property,
                             owned_image_atom, 8, PropModeReplace,
                             owned_image_bytes_.data(),
                             static_cast<int>(owned_image_bytes_.size()));
            reply.property = req.property;
        }

        XSendEvent(display_, req.requestor, False, NoEventMask,
                    reinterpret_cast<XEvent*>(&reply));
        XFlush(display_);
    }

    /// @brief Pull the bytes of @p prop off our window into a
    /// raw byte vector and delete the property. Caller holds
    /// the lock. Single-shot reads only; INCR transfer is
    /// recognised earlier in @ref handle_selection_notify_locked
    /// and routed through @ref handle_incr_chunk_locked.
    std::vector<std::uint8_t>
    read_property_bytes_locked(Atom prop) {
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
        // long_length is in 32-bit multiples per the X11
        // protocol — *not* sizeof(long). Using sizeof(long)
        // (8 on 64-bit) reads only half the property bytes
        // and produces silently corrupt PNG / HTML payloads.
        const unsigned long total = (bytes_after + 3) / 4;
        if (XGetWindowProperty(display_, window_, prop,
                                0, static_cast<long>(total),
                                False, AnyPropertyType,
                                &actual_type, &actual_fmt,
                                &nitems, &bytes_after,
                                &data) != Success
            || data == nullptr) {
            return {};
        }
        std::vector<std::uint8_t> out(data,
                                       data + nitems);
        XFree(data);
        XDeleteProperty(display_, window_, prop);
        return out;
    }

    std::mutex                m_;
    Display*                  display_         = nullptr;
    Window                    window_          = 0;
    Atom                      a_clipboard_     = None;
    Atom                      a_utf8_          = None;
    Atom                      a_targets_       = None;
    Atom                      a_string_        = None;
    Atom                      a_text_          = None;
    Atom                      a_html_          = None;
    Atom                      a_png_           = None;
    Atom                      a_jpeg_          = None;
    Atom                      a_bmp_           = None;
    Atom                      a_prop_          = None;
    Atom                      a_incr_          = None;
    bool                      have_ownership_  = false;
    std::string               owned_text_;
    std::string               owned_html_;
    std::string               owned_image_mime_;
    std::vector<std::uint8_t> owned_image_bytes_;

    /// @brief Dedicated event thread: services
    /// SelectionRequest (other apps pasting from us) and
    /// SelectionNotify (our own get_clipboard completion).
    /// Runs from open() to close().
    std::thread               event_thread_;
    std::atomic<bool>         running_{false};

    /// @brief Cross-thread coordination for get_clipboard. The
    /// API caller blocks on @c pending_get_cv_; the event
    /// thread stamps the result into @c pending_get_result_
    /// when SelectionNotify (and any subsequent INCR
    /// PropertyNotify chunks) arrive matching
    /// @c pending_get_target_. @c pending_get_incr_ flips on
    /// when the SelectionNotify property type is INCR — the
    /// caller then waits for an empty chunk before unblocking.
    std::condition_variable   pending_get_cv_;
    bool                      pending_get_done_   = false;
    bool                      pending_get_incr_   = false;
    Atom                      pending_get_target_ = None;
    std::vector<std::uint8_t> pending_get_result_;
};

}  // namespace

std::unique_ptr<IClipboardBackend> make_default_clipboard_backend() {
    return std::make_unique<X11ClipboardBackend>();
}

}  // namespace unio_ui::orchestrator
