/// @file cursor_handoff.cpp
/// @brief Cursor-handoff method bodies of @ref FacadeOrchestrator.
/// Owns the wiring between the platform input backend, the
/// cursor router state machine, the polled-motion forwarder,
/// and the per-peer Handoff / MouseRel transport sends.
///
/// Class declaration lives in @c facade.hpp; the rest of
/// FacadeOrchestrator's bodies are split between
/// @c orchestrator.cpp (composition + dispatcher + worker) and
/// @c clipboard/coordinator.cpp (clipboard + file-transfer).

#include "orchestrator/facade.hpp"

#include <cstdio>
#include <utility>

namespace xorio::orchestrator::detail {

bool FacadeOrchestrator::is_blocked_os_hotkey(std::uint32_t sc,
                                                bool ctrl,
                                                bool alt,
                                                bool win) {
    // Win modifier engaged covers WIN+L, WIN+D, WIN+R, WIN+TAB,
    // WIN+E, WIN+arrow, WIN+P, WIN+number, etc. — sweeping but
    // intentional: every Win+X is OS-level on Windows and most
    // Linux DEs.
    if (win) return true;
    // Win key tapped alone pops the Start menu / activities
    // overview. Block its keyup too so a forwarded Win-down
    // earlier (already blocked) doesn't leave an orphan up.
    if (sc == kHidLeftWin || sc == kHidRightWin) return true;
    // Specific Alt-modified OS shortcuts.
    if (alt) {
        switch (sc) {
            case kHidTab:    // Alt+Tab — switcher
            case kHidEscape: // Alt+Esc — cycle
            case kHidSpace:  // Alt+Space — window menu
            case kHidF4:     // Alt+F4 — close window
                return true;
        }
    }
    // Ctrl+Alt+Del. Usually intercepted by the secure-attention
    // sequence before reaching our hook, but block defensively.
    if (ctrl && alt && sc == kHidDelete) return true;
    // Ctrl+Shift+anything stays through — that's the cross
    // modifier the user explicitly asked not to block.
    return false;
}

void FacadeOrchestrator::wire_cursor_poller() {
    if (!input_backend_) return;
    cursor_router_ = std::make_unique<CursorRouter>(
        local_machine_id_,
        [this](const std::string& target,
                std::int32_t entry_x, std::int32_t entry_y) {
            std::fprintf(stderr,
                         "router: handoff → %s @ (%d, %d)\n",
                         target.c_str(), entry_x, entry_y);
            send_handoff(target, entry_x, entry_y);
        },
        [this](std::int32_t x, std::int32_t y) {
            if (input_backend_) {
                last_injected_x_.store(x, std::memory_order_release);
                last_injected_y_.store(y, std::memory_order_release);
                input_backend_->inject_mouse_move(x, y);
                // Re-sync the polled-cursor reference used by the
                // raw on_motion forwarder. After we warp the OS
                // cursor (handoff entry, pin warp, tracked re-warp),
                // polled jumps to (x, y); without this re-sync the
                // next raw event would compute a huge bogus delta
                // (current_polled - stale last_polled) and forward
                // it to the active peer, racing their cursor across
                // the screen.
                if (motion_forwarder_) {
                    motion_forwarder_->invalidate_polled_reference();
                }
            }
        },
        [this](const std::string& target,
                std::int32_t dx, std::int32_t dy) {
            send_mouse_rel(target, dx, dy);
        },
        [this](std::int32_t dx, std::int32_t dy) {
            // Remote peer is forwarding their mouse motion and
            // we're the locally active source (cursor home here).
            // Apply the delta as a plain relative cursor move; the
            // active poll path sees the new polled position next
            // tick and runs edge detection from there, just as if
            // our own user had moved the mouse.
            if (!input_backend_) return;
            std::int32_t cx = 0, cy = 0;
            if (!input_backend_->get_cursor_pos(cx, cy)) return;
            input_backend_->inject_mouse_move(cx + dx, cy + dy);
        });
    // Default to active — any peer's first edge crossing claims
    // the cursor; receivers go dormant on Handoff.

    // Button transitions are forwarded from the raw-capture layer
    // (cbs.on_button in wire_raw_input_capture) — that path sees
    // button events at the device level before the local grab
    // (Win32 LL hook / X11 XGrabButton) suppresses them from
    // reaching apps. The poller's get_button_mask() reads OS-
    // pollable state which misses events the grab swallows, so
    // wiring it here would silently drop clicks while dormant.
    cursor_poller_ = std::make_unique<input::CursorPoller>(
        *input_backend_,
        [this](std::int32_t x, std::int32_t y) {
            if (cursor_router_) cursor_router_->on_local_cursor_move(x, y);
        });
}

void FacadeOrchestrator::update_cursor_poller_state() {
    if (!cursor_poller_) return;
    bool any_peer = false;
    {
        std::lock_guard lk(connected_peers_m_);
        any_peer = !connected_peers_.empty();
    }
    if (any_peer) cursor_poller_->start();
    else          cursor_poller_->stop();
}

void FacadeOrchestrator::send_handoff(const std::string& target,
                                       std::int32_t entry_x,
                                       std::int32_t entry_y) {
    if (!control_channel_ || target.empty()) return;
    // Pre-auth: don't push another peer into dormant — they may
    // still be on the access screen and the grab + cursor-hide
    // would lock their UI out.
    if (!access_authorized_.load(std::memory_order_acquire)) return;
    control::HandoffMessage m;
    m.entry_x = entry_x;
    m.entry_y = entry_y;
    const auto body = control::encode_handoff(m);
    control_channel_->send(target, control::MessageType::Handoff,
                            body.data(), body.size());
    // Cursor visibility follows the router's active flag: checked
    // peers go dormant + hide cursor; unchecked peers stay active
    // (cursor visible, locally controlled) even after firing a
    // return handoff.
    sync_cursor_visibility_locked();
}

void FacadeOrchestrator::sync_cursor_visibility_locked() {
    if (!input_backend_ || !cursor_router_) return;
    const bool active = cursor_router_->is_local_active();
    input_backend_->set_cursor_visible(active);
    // Grab local pointer + keyboard while we're dormant (cursor
    // lives on another peer): without it, the user clicking or
    // typing on this PC fires both locally (because the OS cursor
    // is here, just hidden) and remotely (forwarded over the
    // wire), so the click executes twice. Only meaningful for
    // input-member peers — non-members never go dormant in the
    // first place (they're either local-active or active+tracked
    // receiving a visit).
    const bool dormant = !active;
    const bool grab_ptr = dormant
                        && cursor_router_->is_cursor_member();
    const bool grab_kbd = dormant
                        && cursor_router_->is_keyboard_member();
    input_backend_->set_input_grabbed(grab_ptr, grab_kbd);
}

void FacadeOrchestrator::send_mouse_rel(const std::string& target,
                                         std::int32_t dx,
                                         std::int32_t dy) {
    if (!control_channel_ || target.empty()) return;
    if (!access_authorized_.load(std::memory_order_acquire)) return;
    std::fprintf(stderr, "fwd: → %s rel=(%d, %d)\n",
                 target.c_str(), dx, dy);
    control::MouseRelMessage m;
    m.dx = dx;
    m.dy = dy;
    const auto body = control::encode_mouse_rel(m);
    control_channel_->send(target, control::MessageType::MouseRel,
                            body.data(), body.size());
}

void FacadeOrchestrator::wire_raw_input_capture() {
    if (!input_backend_) return;
    input::IInputBackend::RawInputCallbacks cbs;
    // Build the polled-motion forwarder lazily here — it needs
    // both input_backend_ and cursor_router_, which are
    // constructed earlier but not from the same init list.
    // Owning it via unique_ptr keeps the orchestrator
    // declaration order independent of the forwarder's lifetime.
    if (input_backend_ && cursor_router_) {
        motion_forwarder_ =
            std::make_unique<input::PolledMotionForwarder>(
                *input_backend_, *cursor_router_,
                [this](const std::string& target,
                        std::int32_t dx, std::int32_t dy) {
                    send_mouse_rel(target, dx, dy);
                });
    }
    cbs.on_motion = [this](std::int32_t dx, std::int32_t dy) {
        // Two motion paths:
        //   * Backend-grabbed (Linux EVIOCGRAB): kernel input is
        //     exclusively ours; the OS cursor is frozen. The
        //     polled-cursor forwarder would see zero deltas, so
        //     we forward the raw delta directly.
        //   * Backend not grabbed (Windows LL hook lets motion
        //     through; Linux while active): polled-cursor
        //     forwarder handles edge pin-warp + tracked sync.
        if (input_backend_ && input_backend_->is_input_grabbed()) {
            if (!cursor_router_ || !control_channel_) return;
            cursor_router_->note_local_hardware_motion();
            const auto target = cursor_router_->forward_target();
            if (target.empty()) return;
            send_mouse_rel(target, dx, dy);
            return;
        }
        if (motion_forwarder_) {
            motion_forwarder_->on_hardware_motion(dx, dy);
        }
    };
    cbs.on_button = [this](input::MouseButton btn, bool pressed) {
        // Button forwarding lives on the raw-capture path because
        // the dormant peer's local-input grab (Win32 LL hook /
        // X11 XGrabButton) suppresses button events from reaching
        // the OS-level pollers — RawInput / XInput2 see the
        // device-level event before any of that suppression takes
        // effect.
        if (!cursor_router_ || !control_channel_) return;
        const auto target = cursor_router_->forward_target();
        if (target.empty()) return;
        control::MouseButtonMessage m;
        m.button  = static_cast<std::uint8_t>(btn);
        m.pressed = pressed;
        const auto body = control::encode_mouse_button(m);
        control_channel_->send(target,
                                control::MessageType::MouseButton,
                                body.data(), body.size());
    };
    cbs.on_scroll = [this](std::int32_t dx, std::int32_t dy) {
        if (!cursor_router_ || !control_channel_) return;
        const auto target = cursor_router_->forward_target();
        if (target.empty()) return;
        control::MouseScrollMessage m;
        m.dx = dx;
        m.dy = dy;
        const auto body = control::encode_mouse_scroll(m);
        control_channel_->send(target,
                                control::MessageType::MouseScroll,
                                body.data(), body.size());
    };
    cbs.on_key = [this](std::uint32_t scancode, bool pressed) {
        // Track the user's modifier state from the local
        // keyboard regardless of active/dormant — when we're
        // active and the user is here, this is the only place we
        // see their Ctrl/Shift/Alt/Win toggles. (When we're
        // dormant the local raw events are also forwarded to
        // the active peer, which runs its own
        // track_modifier_key on receive.)
        track_modifier_key(scancode, pressed);
        if (!cursor_router_ || !control_channel_) return;
        // Workspace's "Block OS hotkeys from forwarding" gate.
        // Drop matching combos before the keyboard-checkbox /
        // forward path so the active peer never sees a Win+L /
        // Alt+Tab / Alt+F4 / Ctrl+Alt+Del the user typed
        // thinking it'd act on the screen they're looking at.
        // Only the source side gates: the workspace toggle is
        // LWW-propagated so both ends agree, and source-blocking
        // means nothing crosses the wire — receivers don't need
        // to re-check.
        if (block_hotkeys_) {
            const bool ctrl = ctrl_held_count_ > 0;
            const bool alt  = alt_held_count_  > 0;
            const bool win  = win_held_count_  > 0;
            if (is_blocked_os_hotkey(scancode, ctrl, alt, win)) {
                std::fprintf(stderr,
                             "raw: dropped OS hotkey sc=0x%x %s "
                             "(ctrl=%d alt=%d win=%d)\n",
                             scancode, pressed ? "down" : "up",
                             ctrl ? 1 : 0, alt ? 1 : 0,
                             win ? 1 : 0);
                return;
            }
        }
        // Keyboard checkbox gate: even if the cursor lives on
        // another peer, only forward our local keystrokes when
        // this PC is in the workspace's keyboard set.
        const bool forwardable =
            cursor_router_->keyboard_forwardable();
        const auto target = cursor_router_->forward_target();
        std::fprintf(stderr,
                     "raw: key sc=0x%x %s "
                     "(forwardable=%d target=%s)\n",
                     scancode, pressed ? "down" : "up",
                     forwardable ? 1 : 0,
                     target.empty() ? "-" : target.c_str());
        if (!forwardable || target.empty()) return;
        control::KeyEventMessage m;
        m.scancode = scancode;
        m.pressed  = pressed;
        const auto body = control::encode_key_event(m);
        control_channel_->send(target,
                                control::MessageType::KeyEvent,
                                body.data(), body.size());
    };
    input_backend_->start_raw_capture(std::move(cbs));
}

void FacadeOrchestrator::refresh_cursor_router_state() {
    if (!cursor_router_) return;

    // Active workspace = the first workspace whose member set
    // contains us. Membership (not input_members) is the canvas
    // filter so a peer with the Input checkbox off can still
    // *receive* the cursor — it just won't INITIATE handoffs from
    // its own mouse, gated separately below.
    std::unordered_set<std::string> ws_members;
    std::unordered_set<std::string> ws_input;
    std::vector<DisplayLayoutEntry> layout_entries;
    std::int32_t                    edge_margin      = 4;
    bool                            require_modifier = false;
    bool                            block_hotkeys    = false;
    if (workspaces_) {
        const auto wss = workspaces_->list();
        for (const auto& ws : wss) {
            if (ws.members.count(local_machine_id_) == 0) continue;
            ws_members       = ws.members;
            ws_input         = ws.input_members;
            layout_entries   = ws.layout;
            edge_margin      = ws.cursor_edge_margin;
            require_modifier = ws.cursor_require_modifier;
            block_hotkeys    = ws.cursor_block_hotkeys;
            break;
        }
    }
    cursor_router_->set_edge_margin(edge_margin);
    cursor_router_->set_require_modifier(require_modifier);
    block_hotkeys_ = block_hotkeys;
    std::fprintf(stderr,
                 "router: edge_margin=%d require_mod=%d "
                 "block_hotkeys=%d\n",
                 static_cast<int>(edge_margin),
                 require_modifier ? 1 : 0,
                 block_hotkeys ? 1 : 0);
    // One Input flag per peer drives both the cursor side
    // (initiates handoffs) and the keyboard side (forwards typing
    // while dormant). The cursor_router still takes both as
    // separate args so the unit-tested two-flag contract doesn't
    // change; we just pass the same value for both.
    const bool is_input_member =
        ws_input.count(local_machine_id_) > 0;
    cursor_router_->set_local_member_flags(is_input_member,
                                            is_input_member);
    std::fprintf(stderr,
                 "router: local input member=%d "
                 "(workspace members=%zu)\n",
                 is_input_member ? 1 : 0,
                 ws_members.size());
    // The router may have just transitioned from dormant back to
    // active (cursor membership revoked while forwarding) —
    // surface the local cursor again.
    sync_cursor_visibility_locked();

    // Monitors: every peer's caps from the mesh CRDT, with the
    // workspace's per-monitor layout entries overriding the
    // local-probe coords. New / un-arranged monitors fall back to
    // whatever the local probe reported.
    std::vector<RouterMonitor> monitors;
    if (mesh_) {
        for (const auto& [_, caps] : mesh_->all_caps()) {
            // Every workspace member contributes its monitors to
            // the routing layout, including peers with Input
            // unchecked: a checked peer is allowed to push the
            // global cursor onto an unchecked peer (and continue
            // driving it there from the checked peer's mouse +
            // keyboard). What "Input unchecked" forbids is local-
            // mouse-initiated handoffs from the unchecked peer
            // outward — that gate lives inside the cursor router
            // on the local peer's @c is_cursor_member_ flag.
            if (!ws_members.empty()
                && ws_members.count(caps.machine_id) == 0) {
                continue;
            }
            for (const auto& d : caps.displays) {
                RouterMonitor rm;
                rm.machine_id = d.machine_id;
                // Display.global_x/y in our model is actually the
                // announcing peer's OS-local coord — the local
                // probe doesn't yet know about mesh-global space.
                // Treat it as local; let the layout entry override
                // populate the real global fields if the user has
                // arranged this monitor.
                rm.local_x    = d.global_x;
                rm.local_y    = d.global_y;
                rm.global_x   = d.global_x;
                rm.global_y   = d.global_y;
                rm.width      = d.width;
                rm.height     = d.height;
                for (const auto& e : layout_entries) {
                    if (e.machine_id == d.machine_id
                        && e.monitor_id == d.monitor_id) {
                        rm.global_x = e.global_x;
                        rm.global_y = e.global_y;
                        break;
                    }
                }
                monitors.push_back(rm);
            }
        }
    }
    cursor_router_->set_monitors(std::move(monitors));
}

}  // namespace xorio::orchestrator::detail
