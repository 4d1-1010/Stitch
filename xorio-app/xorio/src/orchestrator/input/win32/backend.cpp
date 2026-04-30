/// @file input_win32.cpp
/// @brief Win32 implementation of @ref IInputBackend — cursor
/// query (GetCursorPos), warp + button + scroll injection
/// (SendInput), key injection (SendInput / KEYEVENTF_SCANCODE),
/// cursor visibility (ShowCursor), and button polling
/// (GetAsyncKeyState). Raw scroll + key capture is owned by a
/// sibling helper (@ref Win32RawCapture) so this TU stays
/// focused on the inject / query side of the interface.
///
/// Coordinate space: SendInput's MOUSEEVENTF_ABSOLUTE expects
/// the dx/dy fields normalised to 0..65535 across the entire
/// virtual desktop. We translate from incoming local screen
/// pixels to that range using the SM_*VIRTUALSCREEN metrics.

#include "orchestrator/input/input_backend.hpp"
#include "orchestrator/input/keycodes.hpp"
#include "orchestrator/input/win32/input_grab.hpp"
#include "orchestrator/input/win32/raw_capture.hpp"

// NOMINMAX before <windows.h> so the Windows headers don't
// define `min` / `max` as macros — clashes with std::min/std::max
// elsewhere in the build.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <mutex>
#include <utility>

namespace xorio_ui::orchestrator::input {

namespace {

/// @brief Convert a screen-pixel coordinate on the virtual
/// desktop to SendInput's normalised 0..65535 range.
LONG normalise(int pixel, int origin, int extent) {
    if (extent <= 0) return 0;
    long long n = (static_cast<long long>(pixel - origin) * 65535LL) / extent;
    if (n < 0)     n = 0;
    if (n > 65535) n = 65535;
    return static_cast<LONG>(n);
}

class Win32InputBackend final : public IInputBackend {
public:
    bool open() override  { return true; }
    void close() override {}

    bool get_cursor_pos(std::int32_t& x, std::int32_t& y) override {
        std::lock_guard lk(m_);
        POINT p{};
        if (!::GetCursorPos(&p)) return false;
        x = p.x;
        y = p.y;
        return true;
    }

    void inject_mouse_move(std::int32_t x, std::int32_t y) override {
        std::lock_guard lk(m_);
        // Arm the raw-capture swallow window first — a
        // SetCursorPos / SendInput cursor write triggers a
        // touchpad-driver phantom-correction echo on the next
        // RawInput tick on Windows laptops, and the echo is
        // shaped identically to real touchpad motion (hDevice
        // == 0, relative flags). The swallow window absorbs
        // it. 100 ms covers the burst tail.
        raw_capture_.arm_warp_swallow(30);
        const int origin_x = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int origin_y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int width    = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int height   = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

        INPUT in{};
        in.type        = INPUT_MOUSE;
        in.mi.dx       = normalise(x, origin_x, width);
        in.mi.dy       = normalise(y, origin_y, height);
        in.mi.dwFlags  = MOUSEEVENTF_MOVE
                       | MOUSEEVENTF_ABSOLUTE
                       | MOUSEEVENTF_VIRTUALDESK;
        ::SendInput(1, &in, sizeof(in));
    }

    void inject_mouse_button(MouseButton button, bool pressed) override {
        std::lock_guard lk(m_);
        DWORD flags = 0;
        switch (button) {
            case MouseButton::Left:
                flags = pressed ? MOUSEEVENTF_LEFTDOWN
                                : MOUSEEVENTF_LEFTUP;
                break;
            case MouseButton::Middle:
                flags = pressed ? MOUSEEVENTF_MIDDLEDOWN
                                : MOUSEEVENTF_MIDDLEUP;
                break;
            case MouseButton::Right:
                flags = pressed ? MOUSEEVENTF_RIGHTDOWN
                                : MOUSEEVENTF_RIGHTUP;
                break;
        }
        if (flags == 0) return;

        INPUT in{};
        in.type       = INPUT_MOUSE;
        in.mi.dwFlags = flags;
        ::SendInput(1, &in, sizeof(in));
    }

    void inject_mouse_scroll(std::int32_t dx, std::int32_t dy) override {
        std::lock_guard lk(m_);
        if (dy != 0) {
            INPUT in{};
            in.type           = INPUT_MOUSE;
            in.mi.dwFlags     = MOUSEEVENTF_WHEEL;
            in.mi.mouseData   = static_cast<DWORD>(dy * WHEEL_DELTA);
            ::SendInput(1, &in, sizeof(in));
        }
        if (dx != 0) {
            INPUT in{};
            in.type           = INPUT_MOUSE;
            in.mi.dwFlags     = MOUSEEVENTF_HWHEEL;
            in.mi.mouseData   = static_cast<DWORD>(dx * WHEEL_DELTA);
            ::SendInput(1, &in, sizeof(in));
        }
    }

    void set_cursor_visible(bool visible) override {
        std::lock_guard lk(m_);
        // ShowCursor's display counter is per-THREAD — calling
        // it from the orchestrator's threads doesn't affect the
        // UI thread's cursor, so the cursor stayed visible even
        // when we tried to hide it. SetSystemCursor / SPI_SETCURSORS
        // work system-wide from any thread, which is what we
        // need for the dormant state. We swap the standard
        // arrow with a 1x1 transparent cursor while dormant and
        // restore the system defaults when going active again.
        if (visible) {
            ::SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
        } else {
            // Build a 1x1 fully-transparent cursor on the fly.
            // SetSystemCursor takes ownership of the HCURSOR so
            // we don't destroy it here; the system frees it on
            // SPI_SETCURSORS.
            BYTE and_mask[]  = { 0xFF };  // 1 = leave background
            BYTE xor_mask[]  = { 0x00 };  // 0 = no foreground draw
            HCURSOR transparent = ::CreateCursor(
                ::GetModuleHandleW(nullptr),
                0, 0, 1, 1, and_mask, xor_mask);
            if (transparent != nullptr) {
                // Replace every flavour of the arrow cursor so
                // the busy / cross / IBeam variants don't pop
                // the cursor back into view mid-handoff.
                // Replace every flavour of the arrow cursor.
                // Listed explicitly because <windows.h> often
                // defines a global `id` macro that breaks
                // range-for / sizeof shorthands here.
                // The OCR_* constants are stable across Windows
                // versions but the msvc-wine SDK we cross-build
                // with ships an older set that omits half of
                // them. Guard each one — populated values match
                // the official Windows SDK numerics.
#               ifndef OCR_NORMAL
#                   define OCR_NORMAL      32512
#               endif
#               ifndef OCR_IBEAM
#                   define OCR_IBEAM       32513
#               endif
#               ifndef OCR_WAIT
#                   define OCR_WAIT        32514
#               endif
#               ifndef OCR_CROSS
#                   define OCR_CROSS       32515
#               endif
#               ifndef OCR_UP
#                   define OCR_UP          32516
#               endif
#               ifndef OCR_SIZENWSE
#                   define OCR_SIZENWSE    32642
#               endif
#               ifndef OCR_SIZENESW
#                   define OCR_SIZENESW    32643
#               endif
#               ifndef OCR_SIZEWE
#                   define OCR_SIZEWE      32644
#               endif
#               ifndef OCR_SIZENS
#                   define OCR_SIZENS      32645
#               endif
#               ifndef OCR_SIZEALL
#                   define OCR_SIZEALL     32646
#               endif
#               ifndef OCR_NO
#                   define OCR_NO          32648
#               endif
#               ifndef OCR_HAND
#                   define OCR_HAND        32649
#               endif
#               ifndef OCR_APPSTARTING
#                   define OCR_APPSTARTING 32650
#               endif
                // Note: CopyCursor is a macro (CopyIcon cast),
                // so a `::` qualifier here breaks the expansion.
                auto replace_cursor = [transparent](DWORD ocr_id) {
                    HCURSOR clone = CopyCursor(transparent);
                    if (clone) ::SetSystemCursor(clone, ocr_id);
                };
                replace_cursor(OCR_NORMAL);
                replace_cursor(OCR_IBEAM);
                replace_cursor(OCR_WAIT);
                replace_cursor(OCR_CROSS);
                replace_cursor(OCR_UP);
                replace_cursor(OCR_SIZENWSE);
                replace_cursor(OCR_SIZENESW);
                replace_cursor(OCR_SIZEWE);
                replace_cursor(OCR_SIZENS);
                replace_cursor(OCR_SIZEALL);
                replace_cursor(OCR_NO);
                replace_cursor(OCR_HAND);
                replace_cursor(OCR_APPSTARTING);
                ::DestroyCursor(transparent);
            }
        }
    }

    std::uint32_t get_button_mask() override {
        std::uint32_t out = 0;
        if (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) out |= (1u << 0);
        if (::GetAsyncKeyState(VK_MBUTTON) & 0x8000) out |= (1u << 1);
        if (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) out |= (1u << 2);
        return out;
    }

    void inject_key(std::uint32_t scancode, bool pressed) override {
        std::lock_guard lk(m_);
        // Wire scancode is HID. Translate to a Windows VK and
        // SendInput by virtual-key — matches the Python tree's
        // approach (no MapVirtualKey hop, layout-aware via the
        // OS keyboard driver).
        const std::uint32_t vk = hid_to_vk(scancode);
        if (vk == 0) return;
        INPUT in{};
        in.type       = INPUT_KEYBOARD;
        in.ki.wVk     = static_cast<WORD>(vk);
        in.ki.dwFlags = pressed ? 0u : KEYEVENTF_KEYUP;
        ::SendInput(1, &in, sizeof(in));
    }

    void start_raw_capture(RawInputCallbacks cbs) override {
        // Wrap the public MouseButton-typed callback into the
        // raw-capture's internal Button enum (which deliberately
        // doesn't depend on input_backend.hpp). The two enums
        // share the same numeric values.
        Win32RawCapture::OnButtonFn raw_button;
        if (cbs.on_button) {
            raw_button = [cb = std::move(cbs.on_button)](
                              Win32RawCapture::Button b, bool pressed) {
                cb(static_cast<MouseButton>(b), pressed);
            };
        }
        // Mouse motion / button / scroll come through RawInput;
        // keyboard goes through the LL hook so the swallow
        // mechanism doesn't suppress its own capture stream.
        input_grab_.set_on_key(std::move(cbs.on_key));
        raw_capture_.start(std::move(cbs.on_motion),
                            std::move(raw_button),
                            std::move(cbs.on_scroll));
    }

    void stop_raw_capture() override {
        raw_capture_.stop();
    }

    void set_input_grabbed(bool pointer_grabbed,
                            bool keyboard_grabbed) override {
        input_grab_.set_grabbed(pointer_grabbed, keyboard_grabbed);
    }

    bool is_input_grabbed() const override {
        // The Win32 LL mouse hook lets motion through (it can
        // only swallow buttons + scroll), so the OS cursor still
        // moves with the user's hardware. From the orchestrator's
        // motion-forwarding perspective though we want the
        // raw-RawInput-delta path (same as Linux EVIOCGRAB) —
        // forwarding raw kernel deltas instead of polled-cursor
        // deltas avoids the pin-warp + invalidate cycle that
        // throttles initial deltas after a fresh edge cross.
        return input_grab_.any_grabbed();
    }

private:
    std::mutex      m_;
    Win32RawCapture raw_capture_;
    Win32InputGrab  input_grab_;
};

}  // namespace

std::unique_ptr<IInputBackend> make_default_input_backend() {
    return std::make_unique<Win32InputBackend>();
}

}  // namespace xorio_ui::orchestrator::input
