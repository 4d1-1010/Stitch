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

namespace xorio_ui::orchestrator::input {

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

        /// @brief Touchpad state. When the device emits EV_ABS
        /// (touchpads, touchscreens) rather than EV_REL, we
        /// compute relative deltas from successive absolute
        /// finger positions. @c finger_down tracks BTN_TOUCH so
        /// a lift+re-touch doesn't surface as a phantom delta;
        /// @c last_valid is set once we've seen at least one
        /// ABS_X / ABS_Y after a touch starts.
        bool        emits_abs    = false;
        bool        finger_down  = false;
        bool        last_valid   = false;
        std::int32_t last_abs_x  = 0;
        std::int32_t last_abs_y  = 0;
        /// @brief Multiplier device-units → pixel-units, derived
        /// from EVIOCGABS at open. We scale a full-traverse
        /// (max-min) of the device to 1920 px so a finger
        /// crossing the whole pad moves the cursor across one
        /// monitor. Touchpads typically report ~3000 device units.
        float       abs_scale_x  = 1.0f;
        float       abs_scale_y  = 1.0f;
    };

    void reader_loop();
    /// @brief Read a batch of events from one device. Returns
    /// false when the device is dead (read error / EOF) and
    /// the caller should drop it from @ref devices_.
    bool handle_events(Device& dev);

    /// @brief Walk /dev/input/event*, open any node we don't
    /// already have an fd for, apply the current grab state.
    /// Closes fds that hit ENODEV during a previous read or
    /// EVIOCGRAB. Called periodically by the reader thread so
    /// the user can hot-plug a USB mouse / keyboard / monitor
    /// without restarting xorio-ui.
    void rescan_devices();

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

}  // namespace xorio_ui::orchestrator::input
