/// @file input_stub.cpp
/// @brief Windows-only no-op @ref IInputBackend implementation.
///
/// Phase B is Linux-only. This stub keeps the Windows build
/// linking by providing a backend whose methods do nothing:
///   * open() returns true (the orchestrator can still construct
///     and store the backend without conditional code).
///   * cursor query reports (0, 0).
///   * inject methods are silent no-ops — incoming mouse frames
///     get logged by the orchestrator but never move Diana's
///     cursor until the real Win32 backend lands.

#include "orchestrator/input/input_backend.hpp"

namespace unio_ui::orchestrator::input {

namespace {

class StubInputBackend final : public IInputBackend {
public:
    bool open() override  { return true; }
    void close() override {}

    bool get_cursor_pos(std::int32_t& x, std::int32_t& y) override {
        x = 0;
        y = 0;
        return false;
    }

    void inject_mouse_move(std::int32_t, std::int32_t) override {}
    void inject_mouse_button(MouseButton, bool) override {}
    void inject_mouse_scroll(std::int32_t, std::int32_t) override {}
    void set_cursor_visible(bool) override {}
    std::uint32_t get_button_mask() override { return 0; }
    void inject_key(std::uint32_t, bool) override {}
    void start_raw_capture(RawInputCallbacks) override {}
    void stop_raw_capture() override {}
    void set_input_grabbed(bool, bool) override {}
};

}  // namespace

std::unique_ptr<IInputBackend> make_default_input_backend() {
    return std::make_unique<StubInputBackend>();
}

}  // namespace unio_ui::orchestrator::input
