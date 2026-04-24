/// @file mock_media_connection.cpp
/// @brief Synchronous mock for per-session media connections.

#include "orchestrator/media_connection.hpp"

#include <memory>
#include <mutex>
#include <utility>

namespace unio_ui::orchestrator {

namespace {

class MockMediaConnection final : public IMediaConnection {
public:
    MockMediaConnection(StreamId id,
                        IMediaConnectionFactory::StateChangedFn cb)
        : id_(id), cb_(std::move(cb)) {
        state_ = MediaConnectionState::Open;
        if (cb_) cb_(id_, state_);
    }

    StreamId             stream_id() const override { return id_; }
    MediaConnectionState state()     const override { return state_; }

    void close() override {
        if (state_ == MediaConnectionState::Closed) return;
        state_ = MediaConnectionState::Closed;
        if (cb_) cb_(id_, state_);
    }

private:
    StreamId                                id_;
    MediaConnectionState                    state_{MediaConnectionState::Opening};
    IMediaConnectionFactory::StateChangedFn cb_;
};

class MockMediaConnectionFactory final : public IMediaConnectionFactory {
public:
    void set_callback(StateChangedFn cb) override {
        std::lock_guard lk(m_);
        cb_ = std::move(cb);
    }

    std::unique_ptr<IMediaConnection>
    open(const MediaConnectionParams& params) override {
        StateChangedFn cb;
        {
            std::lock_guard lk(m_);
            cb = cb_;
        }
        return std::make_unique<MockMediaConnection>(params.stream_id, cb);
    }

private:
    std::mutex     m_;
    StateChangedFn cb_;
};

}  // namespace

std::unique_ptr<IMediaConnectionFactory>
make_mock_media_connection_factory() {
    return std::make_unique<MockMediaConnectionFactory>();
}

}  // namespace unio_ui::orchestrator
