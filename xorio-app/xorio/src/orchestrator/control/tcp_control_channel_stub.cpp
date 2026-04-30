/// @file tcp_control_channel_stub.cpp
/// @brief Windows-only no-op @ref IControlChannel implementation.
///
/// Phase A delivers cursor / clipboard sharing on Linux X11 only.
/// Diana (the Windows test box) still needs to link, so this TU
/// supplies a stub whose every method is a polite no-op:
///   * start() returns false (no listener opened).
///   * connect_to() / disconnect() / send() are silently dropped.
///   * Callbacks set via set_callbacks() are never invoked.
/// The orchestrator already tolerates a non-functional channel
/// (it gates connect_to on the channel being live); on Windows
/// peers simply don't exchange control traffic.
///
/// A full Win32 sibling will replace this file in a follow-up:
/// `tcp_control_channel_win32.cpp` with WSAStartup + Winsock
/// sockets + the same accept/reader thread shape as the POSIX
/// impl.

#include "orchestrator/control/control_channel.hpp"

#include <string>
#include <utility>

namespace xorio_ui::orchestrator::control {

namespace {

class StubControlChannel final : public IControlChannel {
public:
    explicit StubControlChannel(std::string machine_id)
        : machine_id_(std::move(machine_id)) {}

    bool          start(std::uint16_t) override { return false; }
    void          stop() override {}
    std::uint16_t listen_port() const override { return 0; }
    void          connect_to(const std::string&,
                              const std::string&,
                              std::uint16_t) override {}
    void          disconnect(const std::string&) override {}
    bool          send(const std::string&,
                       MessageType,
                       const std::uint8_t*,
                       std::size_t) override { return false; }
    void          set_callbacks(OnFrameFn, OnPeerEventFn, OnPeerEventFn) override {}

private:
    std::string machine_id_;
};

}  // namespace

std::unique_ptr<IControlChannel>
make_tcp_control_channel(const std::string& machine_id) {
    return std::make_unique<StubControlChannel>(machine_id);
}

}  // namespace xorio_ui::orchestrator::control
