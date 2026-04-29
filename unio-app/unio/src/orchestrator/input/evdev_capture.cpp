/// @file evdev_capture.cpp
/// @brief Implementation of @ref EvdevCapture. Heuristics for
/// classifying /dev/input/event* nodes as keyboard / pointer
/// are ported from @c unio/backends/linux_x11.py — same rules
/// the Python tree shipped (low-word key bits >= 12 → keyboard;
/// EV_REL or EV_ABS bit set → pointer).
///
/// Hot-plug: the reader thread rescans /dev/input/event* every
/// 2 s. Devices that disappear (read returns ENODEV after a
/// USB hub reset / replug) are closed; new devices are opened
/// and inherit the current grab state — so unplugging a mouse
/// and plugging it back in just works without restarting
/// unio-ui.

#include "orchestrator/input/evdev_capture.hpp"

#include "orchestrator/input/keycodes.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace unio_ui::orchestrator::input {

namespace {

/// @brief Read /sys/class/input/eventN/device/capabilities/<name>
/// — a whitespace-separated list of 64-bit hex words, MSB-first.
/// Returns the words in MSB-first order (matching the file's
/// layout); the LSB word — the one carrying low-numbered bits —
/// is therefore at the back.
std::vector<std::uint64_t>
read_caps(const std::string& sys_dev, const char* leaf) {
    std::ifstream f(sys_dev + "/capabilities/" + leaf);
    std::vector<std::uint64_t> out;
    if (!f) return out;
    std::string word;
    while (f >> word) {
        out.push_back(std::strtoull(word.c_str(), nullptr, 16));
    }
    return out;
}

/// @brief True iff the device's key capability bitmap looks
/// like a keyboard. Real keyboard keys (KEY_ESC=1 .. KEY_KPDOT
/// =83) all live in the low 64-bit word; mouse buttons
/// (BTN_MOUSE=0x110) sit in higher words. >=12 bits set in the
/// low word qualifies as a keyboard.
bool has_keyboard_keys(const std::vector<std::uint64_t>& key_caps) {
    if (key_caps.empty()) return false;
    return std::popcount(key_caps.back()) >= 12;
}

/// @brief Pointer = device that reports relative or absolute
/// motion. EV_REL = bit 0x02, EV_ABS = bit 0x03 in the EV mask.
bool is_pointer_caps(const std::vector<std::uint64_t>& ev_caps) {
    if (ev_caps.empty()) return false;
    constexpr std::uint64_t kPointerMask =
        (1ULL << EV_REL) | (1ULL << EV_ABS);
    return (ev_caps.back() & kPointerMask) != 0;
}

bool has_ev_key(const std::vector<std::uint64_t>& ev_caps) {
    if (ev_caps.empty()) return false;
    return (ev_caps.back() & (1ULL << EV_KEY)) != 0;
}

/// @brief Read /sys/class/input/eventN/device/name — best-
/// effort, only used in log lines.
std::string read_name(const std::string& sys_dev) {
    std::ifstream f(sys_dev + "/name");
    std::string name;
    std::getline(f, name);
    return name;
}

/// @brief Walk /dev/input/event* and classify each as
/// keyboard / pointer / both / neither. Heuristics match
/// Python's _find_keyboard_devices + _find_pointer_devices.
struct DiscoveredDevice {
    std::string path;
    std::string name;
    bool        is_keyboard;
    bool        is_pointer;
};
std::vector<DiscoveredDevice> discover_devices() {
    std::vector<DiscoveredDevice> out;
    DIR* d = ::opendir("/dev/input");
    if (d == nullptr) return out;
    std::vector<std::string> entries;
    while (auto* ent = ::readdir(d)) {
        if (std::strncmp(ent->d_name, "event", 5) != 0) continue;
        entries.emplace_back(ent->d_name);
    }
    ::closedir(d);
    std::sort(entries.begin(), entries.end());
    for (const auto& name : entries) {
        const std::string sys_dev =
            "/sys/class/input/" + name + "/device";
        const auto ev  = read_caps(sys_dev, "ev");
        const auto key = read_caps(sys_dev, "key");
        if (ev.empty()) continue;

        const bool pointer = is_pointer_caps(ev) && has_ev_key(ev);
        // Pure keyboards report EV_KEY but no EV_REL/EV_ABS;
        // a keyboard-with-buttons-only-not-motion qualifies.
        const bool keyboard = has_keyboard_keys(key) && !pointer;

        if (!keyboard && !pointer) continue;

        DiscoveredDevice dd;
        dd.path        = "/dev/input/" + name;
        dd.name        = read_name(sys_dev);
        dd.is_keyboard = keyboard;
        dd.is_pointer  = pointer;
        out.push_back(std::move(dd));
    }
    return out;
}

}  // namespace

bool EvdevCapture::start(OnMotionFn on_motion,
                          OnButtonFn on_button,
                          OnScrollFn on_scroll,
                          OnKeyFn    on_key) {
    if (running_.load(std::memory_order_acquire)) return true;

    on_motion_ = std::move(on_motion);
    on_button_ = std::move(on_button);
    on_scroll_ = std::move(on_scroll);
    on_key_    = std::move(on_key);

    int wake_pipe[2] = {-1, -1};
    if (::pipe(wake_pipe) != 0) {
        std::fprintf(stderr,
                     "evdev: pipe() failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    wake_read_fd_  = wake_pipe[0];
    wake_write_fd_ = wake_pipe[1];

    rescan_devices();
    if (devices_.empty()) {
        std::fprintf(stderr,
                     "evdev: no input devices openable — "
                     "input forwarding will be a no-op.\n");
        ::close(wake_read_fd_);
        ::close(wake_write_fd_);
        wake_read_fd_  = -1;
        wake_write_fd_ = -1;
        return false;
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&EvdevCapture::reader_loop, this);
    return true;
}

void EvdevCapture::rescan_devices() {
    std::lock_guard lk(grab_m_);
    // Open any /dev/input/event* node we don't already hold —
    // covers both first-time startup (called from start()) and
    // hot-plug recovery after a USB replug. We don't close
    // existing fds here; fds confirmed dead via read /
    // EVIOCGRAB ENODEV are dropped by the reader loop before
    // it calls us.
    const auto found = discover_devices();
    int opened_kbd = 0, opened_ptr = 0;
    for (const auto& dd : found) {
        bool already_open = false;
        for (const auto& existing : devices_) {
            if (existing.path == dd.path) {
                already_open = true;
                break;
            }
        }
        if (already_open) continue;
        const int fd = ::open(dd.path.c_str(),
                               O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            std::fprintf(stderr,
                         "evdev: open %s failed: %s — make sure your "
                         "user is in the 'input' group "
                         "(`sudo usermod -aG input $USER`).\n",
                         dd.path.c_str(),
                         std::strerror(errno));
            continue;
        }
        Device dev;
        dev.fd          = fd;
        dev.is_keyboard = dd.is_keyboard;
        dev.is_pointer  = dd.is_pointer;
        dev.path        = dd.path;
        // Touchpads / touchscreens report absolute finger
        // positions via EV_ABS, not relative deltas via
        // EV_REL. Detect that and pre-cache the device's
        // absolute-axis range so we can scale device units
        // to pixel deltas on every motion event.
        struct input_absinfo absinfo{};
        if (::ioctl(fd, EVIOCGABS(ABS_X), &absinfo) == 0
            && absinfo.maximum > absinfo.minimum) {
            dev.emits_abs   = true;
            const float range_x =
                static_cast<float>(absinfo.maximum - absinfo.minimum);
            dev.abs_scale_x = 1920.0f / range_x;
        }
        if (dev.emits_abs
            && ::ioctl(fd, EVIOCGABS(ABS_Y), &absinfo) == 0
            && absinfo.maximum > absinfo.minimum) {
            const float range_y =
                static_cast<float>(absinfo.maximum - absinfo.minimum);
            dev.abs_scale_y = 1080.0f / range_y;
        }
        // Inherit the current grab state — if we're dormant
        // (EVIOCGRAB engaged on the existing devices) the
        // freshly-plugged-in device must grab too, otherwise
        // its events would leak to local apps.
        const bool want_grab =
            (dev.is_pointer  && pointer_grabbed_)
         || (dev.is_keyboard && keyboard_grabbed_);
        if (want_grab) {
            if (::ioctl(fd, EVIOCGRAB, 1) == 0) {
                dev.grabbed = true;
            } else {
                std::fprintf(stderr,
                             "evdev: EVIOCGRAB %s failed on rescan: %s\n",
                             dd.path.c_str(),
                             std::strerror(errno));
            }
        }
        if (dev.is_keyboard) ++opened_kbd;
        if (dev.is_pointer)  ++opened_ptr;
        std::fprintf(stderr,
                     "evdev: opened %s (%s)%s%s\n",
                     dd.path.c_str(),
                     dd.name.empty() ? "?" : dd.name.c_str(),
                     dd.is_keyboard ? " [kbd]" : "",
                     dd.is_pointer  ? " [ptr]" : "");
        devices_.push_back(std::move(dev));
    }
    if (opened_kbd > 0 || opened_ptr > 0) {
        std::fprintf(stderr,
                     "evdev: now capturing on %zu device(s) "
                     "(+ %d kbd + %d ptr this scan)\n",
                     devices_.size(), opened_kbd, opened_ptr);
    }
}

void EvdevCapture::stop() {
    if (!running_.load(std::memory_order_acquire)
        && devices_.empty()) {
        return;
    }
    running_.store(false, std::memory_order_release);
    if (wake_write_fd_ >= 0) {
        const char b = 0;
        const auto written = ::write(wake_write_fd_, &b, 1);
        (void)written;  // best-effort wake; reader times out at 50ms anyway.
    }
    if (thread_.joinable()) thread_.join();

    {
        std::lock_guard lk(grab_m_);
        for (auto& dev : devices_) {
            if (dev.grabbed) {
                ::ioctl(dev.fd, EVIOCGRAB, 0);
                dev.grabbed = false;
            }
        }
        pointer_grabbed_  = false;
        keyboard_grabbed_ = false;
    }
    for (auto& dev : devices_) {
        if (dev.fd >= 0) ::close(dev.fd);
    }
    devices_.clear();
    if (wake_read_fd_  >= 0) ::close(wake_read_fd_);
    if (wake_write_fd_ >= 0) ::close(wake_write_fd_);
    wake_read_fd_  = -1;
    wake_write_fd_ = -1;

    on_motion_ = {};
    on_button_ = {};
    on_scroll_ = {};
    on_key_    = {};
}

void EvdevCapture::set_grabbed(bool pointer_grabbed,
                                bool keyboard_grabbed) {
    std::lock_guard lk(grab_m_);
    pointer_grabbed_  = pointer_grabbed;
    keyboard_grabbed_ = keyboard_grabbed;
    // Walk in reverse so we can drop dead entries without
    // invalidating iteration. ENODEV means the underlying
    // device was unplugged — close the fd and let the reader
    // thread's next rescan_devices() pick up its replacement.
    for (std::size_t i = devices_.size(); i-- > 0; ) {
        auto& dev = devices_[i];
        const bool want_grab =
            (dev.is_pointer  && pointer_grabbed)
         || (dev.is_keyboard && keyboard_grabbed);
        if (want_grab && !dev.grabbed) {
            if (::ioctl(dev.fd, EVIOCGRAB, 1) == 0) {
                dev.grabbed = true;
            } else {
                const int e = errno;
                std::fprintf(stderr,
                             "evdev: EVIOCGRAB %s failed: %s "
                             "(local input may leak through)\n",
                             dev.path.c_str(),
                             std::strerror(e));
                if (e == ENODEV) {
                    ::close(dev.fd);
                    devices_.erase(devices_.begin() + i);
                }
            }
        } else if (!want_grab && dev.grabbed) {
            ::ioctl(dev.fd, EVIOCGRAB, 0);
            dev.grabbed = false;
        }
    }
}

bool EvdevCapture::any_grabbed() const {
    std::lock_guard lk(grab_m_);
    return pointer_grabbed_ || keyboard_grabbed_;
}

void EvdevCapture::reader_loop() {
    auto last_rescan = std::chrono::steady_clock::now();
    constexpr auto kRescanInterval = std::chrono::seconds(2);
    while (running_.load(std::memory_order_acquire)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = wake_read_fd_;
        FD_SET(wake_read_fd_, &rfds);
        {
            std::lock_guard lk(grab_m_);
            for (const auto& dev : devices_) {
                if (dev.fd >= 0) {
                    FD_SET(dev.fd, &rfds);
                    if (dev.fd > maxfd) maxfd = dev.fd;
                }
            }
        }
        // 50 ms timeout matches Python's _kbd_read_loop. Lets us
        // notice a stop() that raced past the wake_pipe write.
        struct timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 50 * 1000;
        const int n = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (FD_ISSET(wake_read_fd_, &rfds)) {
            char buf[16];
            const auto drained = ::read(wake_read_fd_, buf, sizeof(buf));
            (void)drained;
        }
        // Walk a snapshot of fds — handle_events releases the
        // lock internally if it drops a dead device, so we
        // can't iterate devices_ directly here.
        std::vector<int> ready_fds;
        {
            std::lock_guard lk(grab_m_);
            ready_fds.reserve(devices_.size());
            for (const auto& dev : devices_) {
                if (dev.fd >= 0 && FD_ISSET(dev.fd, &rfds)) {
                    ready_fds.push_back(dev.fd);
                }
            }
        }
        for (int fd : ready_fds) {
            // Snapshot the device's metadata under the lock,
            // release the lock, run handle_events. Holding
            // grab_m_ across the user callbacks (which fan
            // out to send_mouse_rel / inject_key / cursor
            // router methods) is a deadlock hazard — anything
            // that ends up calling back into set_grabbed
            // would block forever. The fd/path/flags are
            // stable for the lifetime of the device entry,
            // and we re-acquire the lock to clean up if the
            // device turns out to be dead.
            Device snapshot{};
            bool   found = false;
            {
                std::lock_guard lk(grab_m_);
                for (const auto& dev : devices_) {
                    if (dev.fd == fd) {
                        snapshot = dev;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) continue;
            const bool alive = handle_events(snapshot);
            if (alive) {
                // Persist the touchpad-state mutations
                // (finger_down, last_valid, last_abs_x/y) back
                // to the live Device. Other fields don't change
                // inside handle_events.
                std::lock_guard lk(grab_m_);
                for (auto& dev : devices_) {
                    if (dev.fd != fd) continue;
                    dev.finger_down = snapshot.finger_down;
                    dev.last_valid  = snapshot.last_valid;
                    dev.last_abs_x  = snapshot.last_abs_x;
                    dev.last_abs_y  = snapshot.last_abs_y;
                    break;
                }
                continue;
            }
            std::lock_guard lk(grab_m_);
            for (std::size_t i = 0; i < devices_.size(); ++i) {
                if (devices_[i].fd != fd) continue;
                std::fprintf(stderr,
                             "evdev: dropping %s (read error)\n",
                             devices_[i].path.c_str());
                if (devices_[i].grabbed) {
                    ::ioctl(devices_[i].fd, EVIOCGRAB, 0);
                }
                ::close(devices_[i].fd);
                devices_.erase(devices_.begin() + i);
                break;
            }
        }
        // Periodic rescan picks up devices that vanished mid-
        // session (USB hub reset, replug) and brings their
        // replacements online, applying the current grab
        // state so dormant-mode forwarding survives a replug.
        const auto now = std::chrono::steady_clock::now();
        if (now - last_rescan >= kRescanInterval) {
            last_rescan = now;
            rescan_devices();
        }
    }
}

bool EvdevCapture::handle_events(Device& dev) {
    input_event evs[64];
    // Motion deltas accumulate across REL_X / REL_Y events
    // until SYN_REPORT marks the end of one input frame, then
    // fire as a single combined call. Without this batching,
    // the receiver sees motion as a stair-step (X-only step
    // followed by Y-only step) which renders as visible noise
    // when the source is moving diagonally — same shape the
    // HID device originally reported but split across two
    // packets.
    std::int32_t pending_dx = 0;
    std::int32_t pending_dy = 0;
    while (true) {
        const ssize_t n = ::read(dev.fd, evs, sizeof(evs));
        if (n < 0) {
            // EAGAIN/EWOULDBLOCK is the normal "no data" path
            // on a non-blocking fd; anything else (ENODEV from
            // a hot-unplug, EIO, etc.) means the fd is unusable
            // and the caller should drop it.
            if (pending_dx != 0 || pending_dy != 0) {
                if (on_motion_) on_motion_(pending_dx, pending_dy);
            }
            return errno == EAGAIN || errno == EWOULDBLOCK
                || errno == EINTR;
        }
        if (n == 0) {
            // EOF on an evdev node = device gone.
            if (pending_dx != 0 || pending_dy != 0) {
                if (on_motion_) on_motion_(pending_dx, pending_dy);
            }
            return false;
        }
        const std::size_t count =
            static_cast<std::size_t>(n) / sizeof(input_event);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& ev = evs[i];
            if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                if ((pending_dx != 0 || pending_dy != 0)
                    && on_motion_) {
                    on_motion_(pending_dx, pending_dy);
                }
                pending_dx = pending_dy = 0;
                continue;
            }
            if (ev.type == EV_REL) {
                if (!dev.is_pointer) continue;
                if (ev.code == REL_X) {
                    pending_dx += static_cast<std::int32_t>(ev.value);
                } else if (ev.code == REL_Y) {
                    pending_dy += static_cast<std::int32_t>(ev.value);
                } else if (ev.code == REL_WHEEL && on_scroll_) {
                    // Each notch = ±1; positive = up.
                    on_scroll_(0, static_cast<std::int32_t>(ev.value));
                } else if (ev.code == REL_HWHEEL && on_scroll_) {
                    on_scroll_(static_cast<std::int32_t>(ev.value), 0);
                }
            } else if (ev.type == EV_ABS && dev.emits_abs) {
                // Touchpad / touchscreen path. Convert successive
                // absolute positions to relative deltas; suppress
                // the delta from a finger lift→re-touch jump by
                // tracking BTN_TOUCH and re-baselining last_abs.
                if (ev.code == ABS_X) {
                    const std::int32_t v =
                        static_cast<std::int32_t>(ev.value);
                    if (dev.finger_down && dev.last_valid) {
                        const float d =
                            (v - dev.last_abs_x) * dev.abs_scale_x;
                        pending_dx += static_cast<std::int32_t>(d);
                    }
                    dev.last_abs_x = v;
                } else if (ev.code == ABS_Y) {
                    const std::int32_t v =
                        static_cast<std::int32_t>(ev.value);
                    if (dev.finger_down && dev.last_valid) {
                        const float d =
                            (v - dev.last_abs_y) * dev.abs_scale_y;
                        pending_dy += static_cast<std::int32_t>(d);
                    }
                    dev.last_abs_y = v;
                    // Mark valid once we've seen both axes so the
                    // very-first finger touch doesn't fire a delta
                    // computed against zero-initialised state.
                    if (dev.finger_down) dev.last_valid = true;
                }
            } else if (ev.type == EV_KEY) {
                // Skip auto-repeat (value == 2) — only press / release.
                if (ev.value != 0 && ev.value != 1) continue;
                const bool pressed = ev.value == 1;
                if (ev.code == BTN_TOUCH && dev.emits_abs) {
                    // Finger lift invalidates the last-known
                    // absolute position so the next re-touch
                    // doesn't synthesize a phantom delta.
                    dev.finger_down = pressed;
                    if (!pressed) dev.last_valid = false;
                } else if (ev.code == BTN_LEFT && on_button_) {
                    on_button_(Button::Left,   pressed);
                } else if (ev.code == BTN_RIGHT && on_button_) {
                    on_button_(Button::Right,  pressed);
                } else if (ev.code == BTN_MIDDLE && on_button_) {
                    on_button_(Button::Middle, pressed);
                } else if (ev.code < BTN_MISC) {
                    // Real keyboard key — translate to HID.
                    const std::uint32_t hid =
                        evdev_to_hid(static_cast<std::uint32_t>(ev.code));
                    if (hid != 0 && on_key_) on_key_(hid, pressed);
                }
            }
        }
    }
}

}  // namespace unio_ui::orchestrator::input
