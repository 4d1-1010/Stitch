/// @file local_probe_win32.cpp
/// @brief `EnumDisplayMonitors` implementation of
/// @ref ILocalProbeAdapter on Windows.
///
/// Scope: enumerate active monitors via the Win32 API. Encoder
/// / decoder / presenter / capture-backend lists are intentionally
/// left empty — populating those is the unio-pipe probe's job once
/// the pipe layer folds into the same binary.
///
/// Failures (no monitors, GDI not initialised) surface as an
/// empty `displays` vector; the rest of the app keeps working.

#include "orchestrator/local_probe.hpp"

#include <windows.h>

#include <memory>
#include <string>

namespace unio_ui::orchestrator {

namespace {

/// @brief Capture the host's NetBIOS name. The orchestrator
/// overwrites the resulting machine_id again on publish — kept
/// here for symmetry with the X11 twin.
std::string host_label() {
    char  buf[256] = {};
    DWORD len      = sizeof(buf);
    if (::GetComputerNameA(buf, &len)) return std::string(buf, len);
    return "unknown-host";
}

/// @brief Per-monitor accumulator passed to EnumDisplayMonitors.
struct EnumState {
    CapsRecord* out;
    int         next_number;
};

BOOL CALLBACK monitor_proc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* st = reinterpret_cast<EnumState*>(lp);

    MONITORINFOEXA info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoA(mon, &info)) return TRUE;

    const LONG w = info.rcMonitor.right  - info.rcMonitor.left;
    const LONG h = info.rcMonitor.bottom - info.rcMonitor.top;
    if (w <= 0 || h <= 0) return TRUE;

    Display d;
    d.machine_id = st->out->machine_id;
    d.monitor_id = info.szDevice[0] ? std::string(info.szDevice)
                                     : "DISPLAY"
                                     + std::to_string(st->next_number);
    d.global_x   = static_cast<std::int32_t>(info.rcMonitor.left);
    d.global_y   = static_cast<std::int32_t>(info.rcMonitor.top);
    d.width      = static_cast<std::int32_t>(w);
    d.height     = static_cast<std::int32_t>(h);
    d.number     = st->next_number++;
    st->out->displays.push_back(std::move(d));

    return TRUE;
}

class Win32LocalProbe final : public ILocalProbeAdapter {
public:
    CapsRecord probe() const override {
        CapsRecord r;
        r.machine_id   = host_label();
        r.display_name = r.machine_id;

        EnumState st{ &r, 1 };
        ::EnumDisplayMonitors(
            nullptr, nullptr, &monitor_proc,
            reinterpret_cast<LPARAM>(&st));
        return r;
    }
};

}  // namespace

std::unique_ptr<ILocalProbeAdapter> make_local_probe() {
    return std::make_unique<Win32LocalProbe>();
}

}  // namespace unio_ui::orchestrator
