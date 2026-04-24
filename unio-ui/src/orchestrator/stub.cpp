/// @file stub.cpp
/// @brief In-memory @ref unio_ui::orchestrator::IOrchestrator
/// implementation populated with static demo data.

#include "orchestrator/orchestrator.hpp"

#include <chrono>

namespace unio_ui::orchestrator {

namespace {

/// @brief Orchestrator that returns fixed data and simulates a
/// short discovery grace period before flipping to SignedIn.
class StubOrchestrator final : public IOrchestrator {
public:
    StubOrchestrator() : start_time_(std::chrono::steady_clock::now()) {}

    std::string local_machine_id() const override { return "adi-pc"; }
    std::string local_display_name() const override { return "adi-pc (Linux)"; }

    AuthState auth_state() const override {
        const auto elapsed = std::chrono::steady_clock::now() - start_time_;
        if (elapsed < std::chrono::seconds(2)) return AuthState::GracePeriod;
        return AuthState::SignedIn;
    }

    std::vector<Peer> peers() const override {
        return {
            {"adi-pc", "adi-pc (Linux)",  "192.168.1.100",
             true, true, true},
            {"diana",  "Diana (Windows)", "192.168.1.18",
             true, true, false},
        };
    }

    std::vector<Display> displays() const override {
        return {
            {"adi-pc", "eDP-1",            0,    0, 1920, 1080, 1},
            {"adi-pc", "HDMI-1",           1920, 0, 2560, 1440, 2},
            {"diana",  "\\\\.\\DISPLAY1",  4480, 0, 1920, 1080, 1},
            {"diana",  "\\\\.\\DISPLAY2",  6400, 0, 1920, 1080, 2},
        };
    }

private:
    std::chrono::steady_clock::time_point start_time_;
};

}  // namespace

std::unique_ptr<IOrchestrator> make_stub() {
    return std::make_unique<StubOrchestrator>();
}

}  // namespace unio_ui::orchestrator
