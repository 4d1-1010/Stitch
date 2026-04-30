/// @file session_scheduler.cpp
/// @brief Session scheduler that flips stream state through
/// @c Negotiating → @c Starting → @c Running synchronously.

#include "orchestrator/session_scheduler.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace xorio_ui::orchestrator {

namespace {

class MockSessionScheduler final : public ISessionScheduler {
public:
    void set_callbacks(StateChangedFn on_state,
                       FailureFn on_failure) override {
        std::lock_guard lk(m_);
        on_state_   = std::move(on_state);
        on_failure_ = std::move(on_failure);
    }

    std::pair<StreamId, StartOutcome>
    start(DisplayRef /*src*/, DisplayRef /*dst*/, RoutingMode /*mode*/) override {
        const StreamId id{++next_};
        {
            std::lock_guard lk(m_);
            states_[id.value] = StreamState::Negotiating;
        }
        advance(id, StreamState::Starting);
        advance(id, StreamState::Running);
        return {id, StartOutcome::Accepted};
    }

    void stop(StreamId id) override {
        advance(id, StreamState::Stopping);
        advance(id, StreamState::Stopped);
    }

    StreamState state_of(StreamId id) const override {
        std::lock_guard lk(m_);
        auto it = states_.find(id.value);
        return it == states_.end() ? StreamState::Stopped : it->second;
    }

private:
    void advance(StreamId id, StreamState to) {
        StateChangedFn cb;
        {
            std::lock_guard lk(m_);
            states_[id.value] = to;
            cb = on_state_;
        }
        if (cb) cb(id, to);
    }

    mutable std::mutex                                 m_;
    std::atomic<std::uint64_t>                         next_{0};
    std::unordered_map<std::uint64_t, StreamState>     states_;
    StateChangedFn                                     on_state_;
    FailureFn                                          on_failure_;
};

}  // namespace

std::unique_ptr<ISessionScheduler> make_mock_session_scheduler() {
    return std::make_unique<MockSessionScheduler>();
}

}  // namespace xorio_ui::orchestrator
