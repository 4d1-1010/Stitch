/// @file evdev_capture.hpp
/// @brief Linux evdev event capture: opens /dev/input/event*
/// keyboard + pointer nodes and dispatches input as motion /
/// button / scroll / key callbacks. EVIOCGRAB toggles
/// kernel-level exclusivity so the dormant peer's user input
/// doesn't reach local apps via X.
///
/// Replaces the older XInput2-based @c X11RawCapture: capturing
/// at the kernel input layer (rather than via XI raw events on
/// a sibling X connection) avoids the documented conflict with
/// XGrabPointer / XGrabKeyboard, and makes the same mechanism
/// serve both capture and local-suppression — exactly what the
/// Python tree's @c LinuxX11Backend has shipped for years.
///
/// Threading: one reader thread blocks in @c select() on every
/// open evdev fd plus a self-pipe used by @c stop(). EVIOCGRAB
/// transitions are issued from any thread under the internal
/// mutex; the reader thread is unaffected.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace unio_ui::orchestrator::input {

class EvdevCapture {
public:
    /// @brief Mirror of @ref MouseButton. Inline so this header
    /// doesn't include input_backend.hpp — keeps the dependency
    /// surface minimal.
    enum class Button : std::uint8_t {
        Left   = 1,
        Middle = 2,
        Right  = 3,
    };

    using OnMotionFn = std::function<void(std::int32_t dx, std::int32_t dy)>;
    using OnButtonFn = std::function<void(Button button, bool pressed)>;
    using OnScrollFn = std::function<void(std::int32_t dx, std::int32_t dy)>;
    /// @c hid is a USB HID Usage ID (Keyboard/Keypad page 0x07);
    /// see @ref keycodes.hpp. The capture layer translates
    /// evdev → HID at the boundary.
    using OnKeyFn    = std::function<void(std::uint32_t hid, bool pressed)>;

    EvdevCapture()  = default;
    ~EvdevCapture() { stop(); }

    EvdevCapture(const EvdevCapture&)            = delete;
    EvdevCapture& operator=(const EvdevCapture&) = delete;

    /// @brief Discover input devices, open them, spawn the
    /// reader. Returns false if no usable device opened (often
    /// "user not in the input group" — caller logs guidance).
    bool start(OnMotionFn on_motion,
                OnButtonFn on_button,
                OnScrollFn on_scroll,
                OnKeyFn    on_key);

    /// @brief Stop the reader, drop the grab, close every fd.
    /// Idempotent.
    void stop();

    /// @brief Toggle EVIOCGRAB on the open keyboard / pointer
    /// nodes. Pointer + keyboard are independent so the
    /// orchestrator can grab one without the other.
    void set_grabbed(bool pointer_grabbed, bool keyboard_grabbed);

    /// @brief True iff either pointer or keyboard nodes are
    /// currently grabbed. Used by the orchestrator's motion
    /// path to decide between raw-delta forwarding (grabbed:
    /// OS cursor is frozen, no polled signal) and the
    /// polled-cursor forwarder (ungrabbed).
    bool any_grabbed() const;

private:
    /// @brief One open evdev node — we keep its capability
    /// flags so set_grabbed only toggles the right subset.
    struct Device {
        int          fd          = -1;
        bool         is_keyboard = false;
        bool         is_pointer  = false;
        bool         grabbed     = false;
        std::string  path;
    };

    void reader_loop();
    void handle_events(Device& dev);

    std::vector<Device>   devices_;
    std::thread           thread_;
    std::atomic<bool>     running_{false};

    /// @brief Self-pipe used to wake the reader's @c select()
    /// from @ref stop().
    int                   wake_read_fd_  = -1;
    int                   wake_write_fd_ = -1;

    OnMotionFn            on_motion_;
    OnButtonFn            on_button_;
    OnScrollFn            on_scroll_;
    OnKeyFn               on_key_;

    mutable std::mutex    grab_m_;
    bool                  pointer_grabbed_  = false;
    bool                  keyboard_grabbed_ = false;
};

}  // namespace unio_ui::orchestrator::input
