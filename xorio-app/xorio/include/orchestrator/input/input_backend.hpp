/// @file input_backend.hpp
/// @brief Per-OS abstraction for cursor queries + mouse event
/// injection.
///
/// Scope: declares @ref IInputBackend — the only interface the
/// cursor-router talks to. Each platform supplies its own
/// implementation behind a factory function (POSIX X11 today,
/// Win32 in a follow-up). Capture / grab APIs are intentionally
/// not part of this interface yet — Phase B does single-direction
/// polling + injection; full pointer grab arrives in a later
/// phase together with edge-handoff.
///
/// Threading:
///   * Backend is constructed and used from the orchestrator's
///     control-channel thread (for inject) + a dedicated polling
///     thread (for cursor query, when added). Each X11 connection
///     is single-thread, so platforms that need cross-thread
///     access should serialise internally.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace xorio_ui::orchestrator::input {

/// @brief Mouse button identifiers.  1=left, 2=middle, 3=right;
/// 4 / 5 are the legacy X11 scroll-up / scroll-down convention
/// but real scrolls go through @ref IInputBackend::inject_scroll.
enum class MouseButton : std::uint8_t {
    Left   = 1,
    Middle = 2,
    Right  = 3,
};

class IInputBackend {
public:
    virtual ~IInputBackend() = default;

    /// @brief Open the platform connection (X11 display etc.).
    /// Returns false if the connection couldn't be established —
    /// the caller falls back to no-op behaviour.
    virtual bool open() = 0;

    /// @brief Tear down the platform connection.
    virtual void close() = 0;

    /// @brief Read the current cursor position in *local* OS
    /// virtual screen pixels. Returns false on failure (caller
    /// should treat the position as unknown).
    virtual bool get_cursor_pos(std::int32_t& x, std::int32_t& y) = 0;

    /// @brief Move the cursor to (@p x, @p y) in *local* OS
    /// virtual screen pixels. Synthesises a motion event so apps
    /// see the move; on platforms that distinguish "warp" from
    /// "drag" we emit the warp variant.
    virtual void inject_mouse_move(std::int32_t x, std::int32_t y) = 0;

    /// @brief Synthesise a mouse button press or release at the
    /// cursor's current position.
    virtual void inject_mouse_button(MouseButton button, bool pressed) = 0;

    /// @brief Synthesise a scroll wheel delta. @p dy > 0 = scroll
    /// up; @p dx > 0 = scroll right.
    virtual void inject_mouse_scroll(std::int32_t dx, std::int32_t dy) = 0;

    /// @brief Show or hide the local OS cursor. Used by the
    /// orchestrator to make the dormant peer's cursor disappear
    /// while another peer owns input — same UX the Python tree
    /// shipped, and matches Synergy / Barrier conventions.
    virtual void set_cursor_visible(bool visible) = 0;

    /// @brief Snapshot the mouse button state as a bitmask:
    /// bit 0 = Left, bit 1 = Middle, bit 2 = Right. Polled by
    /// the cursor poller so a button transition can be forwarded
    /// to the active peer while we're dormant.
    virtual std::uint32_t get_button_mask() = 0;

    /// @brief Inject a key press / release using a USB HID Usage
    /// ID (Keyboard/Keypad page 0x07) — see @ref keycodes.hpp.
    /// Linux maps HID → evdev → X11 keycode via XTestFakeKeyEvent;
    /// Windows maps HID → VK and SendInputs by virtual key.
    virtual void inject_key(std::uint32_t scancode, bool pressed) = 0;

    /// @brief Callbacks fired by the platform-specific raw event
    /// listener (XInput2 on Linux, RawInput on Windows). The
    /// dormant peer wires these to the orchestrator so motion +
    /// scroll + button + key events the user produces locally
    /// can be forwarded to the active peer. Both backends
    /// filter for real-hardware origin (XInput2 RawMotion only
    /// surfaces hardware events; Win32 RawInput drops events
    /// with hDevice == 0), so touchpad-driver phantom-correction
    /// events that move the OS cursor without being real user
    /// input never reach these callbacks.
    struct RawInputCallbacks {
        /// Hardware-origin relative motion delta. Fired only
        /// for real HID input — our own SetCursorPos /
        /// SendInput injections and driver-fired correction
        /// events are filtered out at the raw-capture layer.
        std::function<void(std::int32_t dx, std::int32_t dy)> on_motion;

        /// Mouse button press / release. Used for forwarding
        /// clicks while dormant — the LL hook (Win32) and
        /// XGrabButton (X11) suppress button events from
        /// reaching local apps, so the OS-level button-mask
        /// pollers (GetAsyncKeyState / XQueryPointer) miss
        /// them. Raw-capture sees buttons at the device layer
        /// before any suppression.
        std::function<void(MouseButton button, bool pressed)>
            on_button;

        /// Scroll wheel "clicks" — +y up, +x right. Fired on
        /// each detent; a fast scroll comes through as multiple
        /// callbacks.
        std::function<void(std::int32_t dx, std::int32_t dy)> on_scroll;

        /// Key press / release. @p scancode is in the wire
        /// format above; @p pressed is true on make, false on
        /// break.
        std::function<void(std::uint32_t scancode, bool pressed)> on_key;
    };

    /// @brief Begin delivering raw motion + button + scroll +
    /// key events to @p cbs. Idempotent — calling start while
    /// already active is a no-op. The backend owns its own
    /// event-pump thread.
    virtual void start_raw_capture(RawInputCallbacks cbs) = 0;

    /// @brief Stop the raw event stream and join the pump
    /// thread. Idempotent.
    virtual void stop_raw_capture() = 0;

    /// @brief Grab local mouse / keyboard events independently
    /// so the dormant peer can swallow only the streams the
    /// workspace's checkboxes opted into. Pointer + keyboard
    /// gates are split because the Cursor and Keyboard
    /// per-member flags are independent — a peer can forward
    /// mouse but type locally, or vice-versa.
    /// Linux opens /dev/input/event* and uses EVIOCGRAB at the
    /// kernel input layer; Windows installs WH_MOUSE_LL /
    /// WH_KEYBOARD_LL low-level hooks.
    virtual void set_input_grabbed(bool pointer_grabbed,
                                    bool keyboard_grabbed) = 0;

    /// @brief True iff the local OS cursor is *frozen* by the
    /// current grab — i.e. hardware motion no longer moves the
    /// X cursor. Linux EVIOCGRAB freezes the X cursor so the
    /// orchestrator's polled-cursor forwarder can't see motion
    /// and the on_motion raw delta has to be forwarded
    /// directly. Win32 LL hooks pass motion through and return
    /// false. Default false for backends that don't grab.
    virtual bool is_input_grabbed() const { return false; }
};

/// @brief Build the default backend for the current platform.
/// Linux returns the X11 implementation; Windows returns a stub
/// that swallows every call (until a real Win32 impl lands).
std::unique_ptr<IInputBackend> make_default_input_backend();

}  // namespace xorio_ui::orchestrator::input
