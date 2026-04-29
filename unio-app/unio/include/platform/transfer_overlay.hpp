/// @file transfer_overlay.hpp
/// @brief Floating, always-on-top progress overlay for active
/// file transfers. Shows one row per in-flight transfer with
/// a percent-complete bar, the peer name, and the
/// human-readable label (selection roots).
///
/// Auto-show when the orchestrator's progress fetcher returns
/// non-empty; auto-hide when it returns empty. Lives in its
/// own native window (X11 override-redirect / Win32
/// WS_EX_TOPMOST WS_EX_TOOLWINDOW) so the user sees it
/// regardless of whether the main unio-ui window is open.
///
/// Implementations run an internal refresh thread that polls
/// the progress fetcher at ~10 Hz — fast enough that the
/// progress bar visibly animates, slow enough to not burn
/// CPU during long transfers.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace unio_ui::platform {

/// @brief One row in the overlay. Direction picks the title
/// (Sending → / Receiving from) and the colouring; the rest
/// drives the progress bar + label.
struct TransferOverlayItem {
    enum class Direction : std::uint8_t {
        Sending,
        Receiving,
    };

    Direction     direction        = Direction::Sending;
    /// @brief Sender-assigned transfer id; the cancel
    /// callback dispatches on this.
    std::uint64_t transfer_id      = 0;
    /// @brief Peer's @c machine_id — used as the
    /// "Sending → <peer>" / "Receiving from <peer>" label.
    std::string   peer_name;
    /// @brief Free-form selection summary (e.g. "myfolder",
    /// "3 files"); rendered alongside the peer name.
    std::string   label;
    std::uint64_t bytes_done       = 0;
    std::uint64_t bytes_total      = 0;
    std::uint32_t current_file_idx = 0;
    std::uint32_t file_count       = 0;
};

class ITransferOverlay {
public:
    virtual ~ITransferOverlay() = default;

    /// @brief Fetch the current transfer set. Implementations
    /// call this from their refresh thread; should be fast
    /// (mutex-snapshot, no I/O).
    using ProgressFetchFn =
        std::function<std::vector<TransferOverlayItem>()>;

    /// @brief Fired when the user clicks the cancel button on
    /// a row. The orchestrator routes the id to its sender
    /// map (outbound) or its receiver's active set (inbound)
    /// and tears down the matching transfer.
    using CancelFn =
        std::function<void(std::uint64_t transfer_id)>;

    /// @brief Wire the fetcher. Replaces any previous fetcher.
    /// Setting an empty function pauses the refresh loop
    /// (still hides any visible window).
    virtual void set_progress_fetcher(ProgressFetchFn fetch) = 0;

    /// @brief Wire the cancel handler. Optional; when unset,
    /// the per-row cancel button is hidden.
    virtual void set_cancel_handler(CancelFn cancel) = 0;

    /// @brief Open the overlay window + start the refresh
    /// thread. Idempotent; harmless to call before any
    /// transfer is active (the window stays unmapped /
    /// hidden until the fetcher returns non-empty).
    virtual bool start() = 0;

    /// @brief Tear down the refresh thread + close the
    /// window. Called from the orchestrator's destructor;
    /// idempotent.
    virtual void stop() = 0;
};

/// @brief Construct the platform-default overlay (X11 on
/// Linux, Win32 on Windows). The orchestrator owns one
/// instance and wires its progress fetcher to a snapshot of
/// the active sender + receiver state.
std::unique_ptr<ITransferOverlay> make_transfer_overlay();

}  // namespace unio_ui::platform
