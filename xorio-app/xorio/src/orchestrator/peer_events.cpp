/// @file peer_events.cpp
/// @brief Implementation of @ref PeerEventHandler.

#include "orchestrator/peer_events.hpp"

#include <cstdint>

namespace xorio::orchestrator {

CapsRecord PeerEventHandler::synthesize_peer_caps(
    const std::string& machine_id,
    const std::string& display_name) {
    // Stable hash → small X offset relative to the local probe's
    // 0..4480 window. Range 0..3 buckets × 600 px = 0..1800 of
    // jitter; the base offset (5000) plus the largest jitter still
    // keeps every peer inside the ~7000 px-wide global desktop
    // strip.
    std::uint64_t h = 1469598103934665603ull;          // FNV-1a init
    for (unsigned char c : machine_id) {
        h ^= c;
        h *= 1099511628211ull;
    }
    const std::int32_t base_x =
        5000 + static_cast<std::int32_t>((h % 4u) * 600u);

    CapsRecord c;
    c.machine_id   = machine_id;
    c.display_name = display_name.empty() ? machine_id : display_name;
    c.displays.push_back(Display{
        machine_id, "DISPLAY1",
        base_x,        0, 1920, 1080, 1
    });
    c.displays.push_back(Display{
        machine_id, "DISPLAY2",
        base_x + 1920, 0, 2560, 1440, 2
    });
    return c;
}

void PeerEventHandler::handle_peer_observed(const DiscoveryAnnouncement& a) {
    if (a.authed && !access_authorized_.load(std::memory_order_acquire)) {
        access_authorized_.store(true, std::memory_order_release);
    }

    bool first_seen = false;
    {
        std::lock_guard lk(peers_mtx_);
        auto it = peers_.find(a.machine_id);
        first_seen = (it == peers_.end());

        Peer p;
        p.machine_id   = a.machine_id;
        p.display_name = a.display_name.empty()
                         ? a.machine_id
                         : a.display_name;
        p.address      = a.address;
        p.paired = true;
        p.online = true;
        peers_[a.machine_id] = p;
    }

    // Refresh the peer's caps every announce — peers' real displays
    // come over the wire now, and we want plug/unplug events to
    // surface in the mesh on the next 2 s tick. Mock CRDT writes
    // are idempotent; the cost is minimal.
    if (!a.displays.empty()) {
        CapsRecord c;
        c.machine_id   = a.machine_id;
        c.display_name = a.display_name.empty() ? a.machine_id
                                                 : a.display_name;
        c.displays     = a.displays;
        mesh_.put_caps(std::move(c));
    } else {
        // Wire payload didn't carry displays (older Python instance
        // or pre-R2 build) — placeholder so the Layout still has
        // something to show for this peer.
        mesh_.put_caps(synthesize_peer_caps(a.machine_id, a.display_name));
    }

    // Merge the announcer's workspace catalogue into ours. LWW
    // means an empty / outdated remote view never overwrites a
    // local-newer record; the cost when nothing's changed is a
    // sub-microsecond scan of the workspace map.
    if (!a.workspaces.empty()) {
        workspaces_.merge_remote(a.workspaces);
    }

    // Open the real TCP control channel to this peer on every
    // announce — both ports run through the channel's own
    // peers_ lookup which short-circuits when we already hold a
    // live connection. Doing it on every announce (instead of
    // only first_seen) means a stale-port view auto-recovers
    // when the peer relaunches and announces a new port: the
    // first attempt died (timeout or refused), nothing was
    // added to peers_, so the next announce's connect_to
    // re-resolves and retries against the freshly announced
    // port. Without this the original first_seen guard pinned
    // us to the old port forever.
    if (control_channel_ != nullptr
        && a.control_port != 0
        && !a.address.empty()) {
        control_channel_->connect_to(
            a.machine_id, a.address, a.control_port);
    }
    // Sibling data channel — distinct port, same address.
    // File-transfer frames travel here so a long stream of
    // chunks doesn't queue cursor / keyboard messages behind
    // the control channel's send mutex.
    if (data_channel_ != nullptr
        && a.data_port != 0
        && !a.address.empty()) {
        data_channel_->connect_to(
            a.machine_id, a.address, a.data_port);
    }

    if (first_seen) {
        // Auto-pair on first sighting matches the mock's behaviour.
        // Real pairing flow (PIN exchange, mutual confirm) is the
        // pairing sub-module's concern; this call is a no-op on
        // the mock impl.
        pairing_.accept(a.machine_id);
        control_.ensure_connection(a.machine_id, a.address, a.control_port);

        Peer p;
        {
            std::lock_guard lk(peers_mtx_);
            p = peers_[a.machine_id];
        }
        if (callbacks_.on_peer_joined) callbacks_.on_peer_joined(p);
        if (callbacks_.on_peer_capabilities_changed) {
            callbacks_.on_peer_capabilities_changed(a.machine_id);
        }
    }
}

void PeerEventHandler::handle_peer_lost(const std::string& machine_id) {
    bool was_present = false;
    {
        std::lock_guard lk(peers_mtx_);
        was_present = peers_.erase(machine_id) > 0;
    }
    if (was_present && callbacks_.on_peer_left) {
        callbacks_.on_peer_left(machine_id);
    }
}

}  // namespace xorio::orchestrator
