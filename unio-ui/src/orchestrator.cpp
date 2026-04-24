/*! @file orchestrator.cpp
 *  @brief Stub impl — demo data until the real orchestrator lands.
 */

#include "orchestrator.hpp"

#include <chrono>

namespace unio_ui::orchestrator {

namespace {

class StubOrchestrator final : public IOrchestrator {
public:
    StubOrchestrator() : start_time_(std::chrono::steady_clock::now()) {}

    std::string local_machine_id() const override { return "adi-pc"; }
    std::string local_display_name() const override { return "adi-pc (Linux)"; }

    AuthState auth_state() const override {
        const auto elapsed = std::chrono::steady_clock::now() - start_time_;
        // Mirrors shell.py's discovery-grace behaviour: first ~2 s
        // we're "looking for an activated unIO", then we pretend a
        // remote peer auto-activated us.
        if (elapsed < std::chrono::seconds(2)) return AuthState::GracePeriod;
        return AuthState::SignedIn;
    }

    std::vector<Peer> peers() const override {
        return {
            {"adi-pc", "adi-pc (Linux)",  "192.168.1.100",
             /*paired=*/true, /*online=*/true, /*is_local=*/true},
            {"diana",  "Diana (Windows)", "192.168.1.18",
             /*paired=*/true, /*online=*/true, /*is_local=*/false},
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
