/*! @file orchestrator.hpp
 *  @brief UI-facing interface onto the orchestrator layer.
 *
 *  The UI (`unio-ui`) talks only to this interface. The real
 *  implementation lands as part of the orchestrator layer
 *  (Phase 4, ARCHITECTURE.md §4) — mDNS discovery, mesh CRDT,
 *  session scheduling, `unio-pipe` RPC. Until then, the stub at
 *  @ref make_stub() returns useful-looking demo data so the
 *  screens have something to render.
 *
 *  Kept deliberately minimal: one interface, a few plain data
 *  structs. No Qt signals, no std::function callbacks — the UI
 *  polls each frame (ImGui is immediate-mode anyway) and the
 *  orchestrator publishes snapshots. When we need push-style
 *  updates later, they layer on top without breaking the
 *  query surface.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace unio_ui::orchestrator {

/// One computer discovered on the LAN (or the local machine).
struct Peer {
    std::string machine_id;
    std::string display_name;   ///< Human name (hostname or user-set).
    std::string address;        ///< LAN IP, "192.168.x.y".
    bool paired = false;        ///< Trust-established.
    bool online = true;         ///< Heartbeat seen recently.
    bool is_local = false;      ///< `true` for the running machine.
};

/// One physical display attached to a peer.
struct Display {
    std::string machine_id;
    std::string monitor_id;
    std::int32_t global_x = 0;  ///< OS global-desktop X (px).
    std::int32_t global_y = 0;
    std::int32_t width    = 0;
    std::int32_t height   = 0;
    std::int32_t number   = 0;  ///< 1-based OS display ordinal.
};

/// Current authorisation state of the local peer.
/// Mirrors shell.py's three-way dispatch in `_activity_alone_state`.
enum class AuthState {
    GracePeriod,  ///< First N seconds after launch; discovery still running.
    SignedOut,    ///< Grace expired; no peer on LAN has activated us.
    SignedIn,     ///< This PC signed in, or auto-activated by a remote peer.
};

/*! @brief UI-facing snapshot + action surface.
 *
 *  All query methods are `const` — they return an immediate view
 *  of internal state without mutating anything. Safe to call
 *  per-frame.
 */
class IOrchestrator {
public:
    virtual ~IOrchestrator() = default;

    // ── Queries ────────────────────────────────────────────
    virtual std::string local_machine_id() const = 0;
    virtual std::string local_display_name() const = 0;
    virtual AuthState auth_state() const = 0;

    /// All peers the local machine knows about, including itself
    /// (as `is_local = true`). Safe to call every frame.
    virtual std::vector<Peer> peers() const = 0;

    /// All displays across the mesh (every paired + online peer's
    /// contribution + the local machine's own). Safe per-frame.
    virtual std::vector<Display> displays() const = 0;

    // ── Actions (no-ops on the stub) ───────────────────────
    virtual void sign_in(const std::string& username,
                         const std::string& password) {
        (void)username; (void)password;
    }
    virtual void sign_out() {}
};

/// Demo stub: returns two peers (adi-pc + diana) and four displays
/// so the Activity running-state + Layout canvas have visible data
/// until the real orchestrator lands. `auth_state()` starts in
/// `GracePeriod` and transitions to `SignedIn` after ~2 s (simulates
/// discovery + auto-activation).
std::unique_ptr<IOrchestrator> make_stub();

}  // namespace unio_ui::orchestrator
