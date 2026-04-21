#pragma once

#include "encoder.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace unio {

// One outbound QUIC connection carrying a single stream of H.264
// Annex-B packets from source → sink. The caller (the send thread
// inside stream_manager) hands us EncodedPackets; we frame them on
// the wire as [u32_le length][bytes] and ship them.
//
// msquic is fully async under the hood — Connect / Send hand the
// I/O off to the library's worker threads and return immediately.
// To keep the stream_manager code straightforward, this wrapper
// exposes a synchronous-looking interface: Connect blocks until
// the handshake completes (or fails), and Send blocks only when
// the library pushes back.
class QuicOutbound {
public:
    struct Config {
        std::string peer_host;     // IPv4/IPv6 literal or DNS name
        std::uint16_t peer_port = 0;
        std::string stream_id;     // echoed in logging + telemetry
        std::uint32_t handshake_timeout_ms = 3000;
    };

    QuicOutbound();
    ~QuicOutbound();

    // No copy / no move — the impl holds an msquic connection
    // handle that's registered with library worker threads.
    QuicOutbound(const QuicOutbound&) = delete;
    QuicOutbound& operator=(const QuicOutbound&) = delete;

    // Blocks until connected or handshake_timeout_ms elapses.
    // Returns an error string on failure; nullopt on success.
    std::optional<std::string> Connect(const Config& cfg);

    // Public so the C-ABI msquic callbacks (which live in the .cpp
    // as free functions with extern-"C"-style signatures) can
    // reach into the state. Don't poke it from outside the .cpp.
    struct Impl;

    // Write one encoded packet (just the Annex-B bytes). A
    // 4-byte little-endian length prefix is prepended on the wire
    // so the receiver can reconstruct packet boundaries. Returns
    // false if the underlying stream is dead.
    bool SendPacket(const std::uint8_t* bytes, std::size_t len);

    // Graceful close: FIN the stream, wait briefly for peer ACK,
    // then tear down the connection.
    void Close();

    // True once the handshake has completed. False while
    // connecting or after a failure.
    bool IsConnected() const;

private:
    std::unique_ptr<Impl> impl_;
};

// Inbound: listen on a UDP port, accept one connection + one
// stream, decode length-prefixed packets and pass each to the
// callback. Lifetime of the packet-bytes pointer ends when the
// callback returns, so the callback must copy or consume inline.
class QuicInbound {
public:
    using PacketCallback = std::function<void(
        const std::uint8_t* bytes, std::size_t len)>;

    struct Config {
        std::uint16_t listen_port = 0;
        std::string stream_id;
    };

    QuicInbound();
    ~QuicInbound();

    QuicInbound(const QuicInbound&) = delete;
    QuicInbound& operator=(const QuicInbound&) = delete;

    // Starts the listener and returns immediately. The callback
    // fires on msquic worker threads — do minimal work inside it,
    // push to a ring if the consumer is on another thread.
    std::optional<std::string> Start(const Config& cfg,
                                     PacketCallback cb);
    void Stop();

    // Counters exposed via helper_status.
    std::uint64_t PacketsReceived() const;
    std::uint64_t BytesReceived() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace unio
