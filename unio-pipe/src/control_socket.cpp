// UDS control-socket server. Accepts one client at a time (the
// Python shell), reads length-prefixed JSON commands, dispatches
// to the matching handler, writes the JSON response back with
// the same framing.
//
// Single-client is a deliberate constraint: the shell process is
// the only thing that should ever drive the helper. A second
// connection is always a bug and gets rejected at accept time.

#include "unio_pipe.h"
#include "stream_manager.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace unio {

namespace {

JsonValue MakeObjectWithError(std::string_view error_msg) {
    JsonValue v;
    v.kind = JsonValue::Kind::Object;
    JsonValue e;
    e.kind = JsonValue::Kind::String;
    e.s = std::string(error_msg);
    v.obj.emplace_back("error", std::move(e));
    return v;
}

JsonValue CapsToJson(const HelperCaps& caps) {
    auto str_list = [](const std::vector<std::string>& items) {
        JsonValue a;
        a.kind = JsonValue::Kind::Array;
        for (const auto& s : items) {
            JsonValue v;
            v.kind = JsonValue::Kind::String;
            v.s = s;
            a.arr.push_back(std::move(v));
        }
        return a;
    };
    JsonValue root;
    root.kind = JsonValue::Kind::Object;
    root.obj.emplace_back("encoders", str_list(caps.encoders));
    root.obj.emplace_back("decoders", str_list(caps.decoders));
    root.obj.emplace_back("presenters", str_list(caps.presenters));
    return root;
}

JsonValue DispatchCommand(const JsonValue& req,
                          StreamManager& streams) {
    const JsonValue* cmd = req.Find("cmd");
    if (cmd == nullptr || cmd->kind != JsonValue::Kind::String) {
        return MakeObjectWithError("missing cmd field");
    }
    const std::string& name = cmd->s;

    if (name == kCmdHelperCaps) {
        return CapsToJson(ProbeCaps());
    }
    if (name == kCmdHelperStatus) {
        JsonValue root;
        root.kind = JsonValue::Kind::Object;
        JsonValue arr;
        arr.kind = JsonValue::Kind::Array;
        auto int_field = [](std::int64_t v) {
            JsonValue j;
            j.kind = JsonValue::Kind::Int;
            j.i = v;
            return j;
        };
        auto str_field = [](const std::string& v) {
            JsonValue j;
            j.kind = JsonValue::Kind::String;
            j.s = v;
            return j;
        };
        auto bool_field = [](bool v) {
            JsonValue j;
            j.kind = JsonValue::Kind::Bool;
            j.b = v;
            return j;
        };
        for (const auto& s : streams.Status()) {
            JsonValue e;
            e.kind = JsonValue::Kind::Object;
            e.obj.emplace_back("stream_id", str_field(s.stream_id));
            e.obj.emplace_back("direction", str_field(s.direction));
            e.obj.emplace_back("encoder", str_field(s.encoder));
            e.obj.emplace_back("captured",
                int_field(static_cast<std::int64_t>(
                    s.frames_captured)));
            e.obj.emplace_back("dropped_at_ring",
                int_field(static_cast<std::int64_t>(
                    s.frames_dropped_at_ring)));
            e.obj.emplace_back("encoded",
                int_field(static_cast<std::int64_t>(
                    s.frames_encoded)));
            e.obj.emplace_back("dropped_at_send",
                int_field(static_cast<std::int64_t>(
                    s.packets_dropped_at_send)));
            e.obj.emplace_back("bytes_emitted",
                int_field(static_cast<std::int64_t>(
                    s.bytes_emitted)));
            e.obj.emplace_back("packets_received",
                int_field(static_cast<std::int64_t>(
                    s.packets_received)));
            e.obj.emplace_back("bytes_received",
                int_field(static_cast<std::int64_t>(
                    s.bytes_received)));
            e.obj.emplace_back("quic_connected",
                bool_field(s.quic_connected));
            arr.arr.push_back(std::move(e));
        }
        root.obj.emplace_back("per_stream", std::move(arr));
        return root;
    }
    if (name == kCmdStartOutbound) {
        const JsonValue* sid = req.Find("stream_id");
        const JsonValue* mon = req.Find("monitor_source");
        const JsonValue* w = req.Find("width");
        const JsonValue* h = req.Find("height");
        const JsonValue* fps = req.Find("fps");
        const JsonValue* peer = req.Find("peer_addr");
        const JsonValue* peer_port_v = req.Find("peer_port");
        if (!sid || sid->kind != JsonValue::Kind::String) {
            return MakeObjectWithError("missing stream_id");
        }
        int width = (w && w->kind == JsonValue::Kind::Int)
                        ? static_cast<int>(w->i) : 1920;
        int height = (h && h->kind == JsonValue::Kind::Int)
                         ? static_cast<int>(h->i) : 1080;
        int fps_v = (fps && fps->kind == JsonValue::Kind::Int)
                        ? static_cast<int>(fps->i) : 60;
        // Accept either "peer_addr" as "host:port" or separate
        // peer_addr + peer_port fields. host:port is what the
        // existing Python helper_bridge passes, so parse both.
        std::string peer_host;
        int peer_port_i = 0;
        if (peer && peer->kind == JsonValue::Kind::String
            && !peer->s.empty()) {
            const auto& p = peer->s;
            auto colon = p.find_last_of(':');
            if (colon != std::string::npos
                && colon + 1 < p.size()) {
                peer_host = p.substr(0, colon);
                try {
                    peer_port_i = std::stoi(p.substr(colon + 1));
                } catch (...) {
                    return MakeObjectWithError(
                        "peer_addr port parse failed");
                }
            } else {
                peer_host = p;
            }
        }
        if (peer_port_v && peer_port_v->kind == JsonValue::Kind::Int) {
            peer_port_i = static_cast<int>(peer_port_v->i);
        }
        auto err = streams.StartOutbound(
            sid->s,
            mon ? mon->s : std::string_view{},
            peer_host,
            peer_port_i,
            width, height, fps_v);
        if (err) return MakeObjectWithError(*err);
        JsonValue ok;
        ok.kind = JsonValue::Kind::Object;
        JsonValue v;
        v.kind = JsonValue::Kind::Bool;
        v.b = true;
        ok.obj.emplace_back("started", std::move(v));
        return ok;
    }
    if (name == kCmdStartInbound) {
        const JsonValue* sid = req.Find("stream_id");
        const JsonValue* port = req.Find("listen_port");
        if (!sid || sid->kind != JsonValue::Kind::String) {
            return MakeObjectWithError("missing stream_id");
        }
        int port_i = (port && port->kind == JsonValue::Kind::Int)
                         ? static_cast<int>(port->i) : 5080;
        auto err = streams.StartInbound(sid->s, port_i);
        if (err) return MakeObjectWithError(*err);
        JsonValue ok;
        ok.kind = JsonValue::Kind::Object;
        JsonValue v;
        v.kind = JsonValue::Kind::Bool;
        v.b = true;
        ok.obj.emplace_back("started", std::move(v));
        JsonValue pj;
        pj.kind = JsonValue::Kind::Int;
        pj.i = port_i;
        ok.obj.emplace_back("listen_port", std::move(pj));
        return ok;
    }
    if (name == kCmdStop) {
        const JsonValue* sid = req.Find("stream_id");
        if (!sid || sid->kind != JsonValue::Kind::String) {
            return MakeObjectWithError("missing stream_id");
        }
        auto err = streams.Stop(sid->s);
        if (err) return MakeObjectWithError(*err);
        JsonValue ok;
        ok.kind = JsonValue::Kind::Object;
        JsonValue v;
        v.kind = JsonValue::Kind::Bool;
        v.b = true;
        ok.obj.emplace_back("stopped", std::move(v));
        return ok;
    }
    if (name == kCmdRequestIdr) {
        // Encoder ForceIdr hook goes here once multiple subscribers
        // land — today the encoder force-idrs on first frame only.
        return MakeObjectWithError(
            std::string("not implemented yet: ") + name);
    }
    return MakeObjectWithError("unknown cmd");
}

#if !defined(_WIN32)

bool ReadExact(int fd, void* buf, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r == 0) return false;   // EOF
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

bool WriteExact(int fd, const void* buf, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

void ServiceClient(int fd, StreamManager& streams) {
    while (true) {
        std::uint32_t len_le = 0;
        if (!ReadExact(fd, &len_le, sizeof(len_le))) break;
        // The wire format is explicitly little-endian — we ship
        // only x86_64 for now but the memcpy keeps the code
        // endian-correct when Apple Silicon / aarch64 arrives.
        std::uint32_t len;
        std::memcpy(&len, &len_le, sizeof(len));
        if (len == 0 || len > (1u << 20)) break;  // sanity cap

        std::string buf;
        buf.resize(len);
        if (!ReadExact(fd, buf.data(), len)) break;

        JsonValue reply;
        auto req = ParseJson(buf);
        if (!req) {
            reply = MakeObjectWithError("bad json");
        } else {
            reply = DispatchCommand(*req, streams);
        }
        std::string out = SerializeJson(reply);
        std::uint32_t out_len = static_cast<std::uint32_t>(out.size());
        if (!WriteExact(fd, &out_len, sizeof(out_len))) break;
        if (!WriteExact(fd, out.data(), out.size())) break;
    }
    ::close(fd);
}

#endif  // !_WIN32

}  // namespace

struct ControlSocket::Impl {
    std::mutex lifecycle;
    bool open = false;
    std::thread accept_thread;
    std::string path;
    // The stream manager lives inside the socket impl because
    // the command-dispatch path needs it on every request and
    // its lifetime is bounded by the socket's. Embedding keeps
    // the Close() path straightforward: destructor tears down
    // both in the right order.
    StreamManager streams;
#if !defined(_WIN32)
    int listen_fd = -1;
#endif
};

ControlSocket::ControlSocket() : impl_(std::make_unique<Impl>()) {}

ControlSocket::~ControlSocket() { Close(); }

bool ControlSocket::Open(const std::string& path) {
#if defined(_WIN32)
    // Windows named-pipe path — scaffold only. Will land together
    // with the Windows capture work in PR 6.
    std::fprintf(stderr,
                 "unio-pipe: Windows named-pipe control socket "
                 "not yet implemented\n");
    (void)path;
    return false;
#else
    std::lock_guard<std::mutex> lk(impl_->lifecycle);
    if (impl_->open) return true;

    // Stale socket file from a previous crash blocks bind().
    // Unlinking it is safe because this helper is single-instance
    // per machine (the Python shell enforces that before spawn).
    ::unlink(path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }
    // 0600 so another user on the same box can't poke at the
    // helper's command surface. The shell process is the only
    // caller and it runs as the same user.
    ::chmod(path.c_str(), 0600);
    if (::listen(fd, 1) < 0) {
        ::close(fd);
        ::unlink(path.c_str());
        return false;
    }

    impl_->listen_fd = fd;
    impl_->path = path;
    impl_->open = true;
    impl_->accept_thread = std::thread([this]() {
        while (true) {
            int cfd = ::accept(impl_->listen_fd, nullptr, nullptr);
            if (cfd < 0) {
                if (errno == EINTR) continue;
                break;  // socket closed by Close()
            }
            // Single-client: service synchronously, close before
            // accepting another. Frame throughput isn't an issue
            // here — this is the 10 Hz control channel.
            ServiceClient(cfd, impl_->streams);
        }
    });
    return true;
#endif
}

void ControlSocket::Close() {
    std::lock_guard<std::mutex> lk(impl_->lifecycle);
    if (!impl_->open) return;
    impl_->open = false;
#if !defined(_WIN32)
    if (impl_->listen_fd >= 0) {
        ::shutdown(impl_->listen_fd, SHUT_RDWR);
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }
    if (!impl_->path.empty()) {
        ::unlink(impl_->path.c_str());
        impl_->path.clear();
    }
#endif
}

}  // namespace unio
