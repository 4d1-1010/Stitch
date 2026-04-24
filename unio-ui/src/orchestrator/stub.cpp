/// @file stub.cpp
/// @brief Transitional single-file mock that honours the expanded
/// @ref unio_ui::orchestrator::IOrchestrator façade.
///
/// The seven sub-module interfaces land in a subsequent commit;
/// once they are in place this file is replaced by an
/// @c orchestrator.cpp that composes the real modules.

#include "orchestrator/orchestrator.hpp"

#include <atomic>
#include <chrono>
#include <mutex>

namespace unio_ui::orchestrator {

namespace {

class MockOrchestrator final : public IOrchestrator {
public:
    explicit MockOrchestrator(OrchestratorCallbacks cb)
        : callbacks_(std::move(cb)),
          start_time_(std::chrono::steady_clock::now()) {}

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

    StreamState stream_state(StreamId) const override {
        return StreamState::Stopped;
    }

    StreamId start_stream(DisplayRef, DisplayRef,
                          RoutingMode) override {
        return StreamId{++next_stream_};
    }
    void stop_stream(StreamId) override {}

    void request_pair(const std::string&, const std::string&) override {}
    void accept_pairing(const std::string&) override {}
    void reject_pairing(const std::string&,
                        const std::string&) override {}
    void unpair(const std::string&) override {}

private:
    OrchestratorCallbacks                  callbacks_;
    std::chrono::steady_clock::time_point  start_time_;
    std::atomic<std::uint64_t>             next_stream_{0};
};

}  // namespace

std::unique_ptr<IOrchestrator>
make_mock(const OrchestratorCallbacks& callbacks) {
    return std::make_unique<MockOrchestrator>(callbacks);
}

}  // namespace unio_ui::orchestrator
