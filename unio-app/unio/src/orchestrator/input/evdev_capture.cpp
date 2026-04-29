/// @file evdev_capture.cpp
/// @brief Implementation of @ref EvdevCapture. Heuristics for
/// classifying /dev/input/event* nodes as keyboard / pointer
/// are ported from @c unio/backends/linux_x11.py — same rules
/// the Python tree shipped (low-word key bits >= 12 → keyboard;
/// EV_REL or EV_ABS bit set → pointer).

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

    const auto found = discover_devices();
    int opened_kbd = 0, opened_ptr = 0;
    for (const auto& dd : found) {
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
    std::fprintf(stderr,
                 "evdev: capturing on %d keyboard + %d pointer node(s)\n",
                 opened_kbd, opened_ptr);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&EvdevCapture::reader_loop, this);
    return true;
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
    for (auto& dev : devices_) {
        const bool want_grab =
            (dev.is_pointer  && pointer_grabbed)
         || (dev.is_keyboard && keyboard_grabbed);
        if (want_grab && !dev.grabbed) {
            if (::ioctl(dev.fd, EVIOCGRAB, 1) == 0) {
                dev.grabbed = true;
            } else {
                std::fprintf(stderr,
                             "evdev: EVIOCGRAB %s failed: %s "
                             "(local input may leak through)\n",
                             dev.path.c_str(),
                             std::strerror(errno));
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
    while (running_.load(std::memory_order_acquire)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = wake_read_fd_;
        FD_SET(wake_read_fd_, &rfds);
        for (const auto& dev : devices_) {
            if (dev.fd >= 0) {
                FD_SET(dev.fd, &rfds);
                if (dev.fd > maxfd) maxfd = dev.fd;
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
        for (auto& dev : devices_) {
            if (dev.fd >= 0 && FD_ISSET(dev.fd, &rfds)) {
                handle_events(dev);
            }
        }
    }
}

void EvdevCapture::handle_events(Device& dev) {
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
        if (n <= 0) {
            if (pending_dx != 0 || pending_dy != 0) {
                if (on_motion_) on_motion_(pending_dx, pending_dy);
                pending_dx = pending_dy = 0;
            }
            return;
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
            } else if (ev.type == EV_KEY) {
                // Skip auto-repeat (value == 2) — only press / release.
                if (ev.value != 0 && ev.value != 1) continue;
                const bool pressed = ev.value == 1;
                if (ev.code == BTN_LEFT && on_button_) {
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
