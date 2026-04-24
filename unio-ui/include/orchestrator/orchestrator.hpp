/// @file orchestrator.hpp
/// @brief UI-facing query + action surface onto the orchestrator layer.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace unio_ui::orchestrator {

/// @brief A computer discovered on the LAN (or the local machine).
struct Peer {
    std::string machine_id;
    std::string display_name;
    std::string address;     ///< LAN IPv4 address.
    bool paired   = false;   ///< Trust established with this peer.
    bool online   = true;    ///< Heartbeat seen recently.
    bool is_local = false;   ///< @c true for the running machine.
};

/// @brief A physical display contributed by a peer.
struct Display {
    std::string machine_id;
    std::string monitor_id;
    std::int32_t global_x = 0;   ///< OS global-desktop X coordinate.
    std::int32_t global_y = 0;
    std::int32_t width    = 0;
    std::int32_t height   = 0;
    std::int32_t number   = 0;   ///< 1-based OS display ordinal.
};

/// @brief Authorisation state of the local peer.
enum class AuthState {
    GracePeriod,  ///< Discovery still in progress after launch.
    SignedOut,    ///< No peer on the LAN has activated us.
    SignedIn,     ///< Signed in locally or activated by a remote peer.
};

/// @brief Query + action surface consumed by the UI layer.
///
/// Query methods are @c const and safe to call per frame.
class IOrchestrator {
public:
    virtual ~IOrchestrator() = default;

    /// @name Queries
    /// @{
    virtual std::string local_machine_id() const = 0;
    virtual std::string local_display_name() const = 0;
    virtual AuthState auth_state() const = 0;
    virtual std::vector<Peer> peers() const = 0;
    virtual std::vector<Display> displays() const = 0;
    /// @}

    /// @name Actions
    /// @{
    virtual void sign_in(const std::string& username,
                         const std::string& password) {
        (void)username; (void)password;
    }
    virtual void sign_out() {}
    /// @}
};

/// @brief Build an in-memory orchestrator populated with demo data.
std::unique_ptr<IOrchestrator> make_stub();

}  // namespace unio_ui::orchestrator
