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
            e.obj.emplace_back("frames_decoded",
                int_field(static_cast<std::int64_t>(
                    s.frames_decoded)));
            e.obj.emplace_back("decode_width",
                int_field(static_cast<std::int64_t>(
                    s.decode_width)));
            e.obj.emplace_back("decode_height",
                int_field(static_cast<std::int64_t>(
                    s.decode_height)));
            e.obj.emplace_back("decoder", str_field(s.decoder));
            e.obj.emplace_back("presenter", str_field(s.presenter));
            e.obj.emplace_back("frames_presented",
                int_field(static_cast<std::int64_t>(
                    s.frames_presented)));
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
        const JsonValue* cx = req.Find("capture_x");
        const JsonValue* cy = req.Find("capture_y");
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
        int cap_x = (cx && cx->kind == JsonValue::Kind::Int)
                        ? static_cast<int>(cx->i) : 0;
        int cap_y = (cy && cy->kind == JsonValue::Kind::Int)
                        ? static_cast<int>(cy->i) : 0;
        auto err = streams.StartOutbound(
            sid->s,
            mon ? mon->s : std::string_view{},
            peer_host,
            peer_port_i,
            width, height, fps_v, cap_x, cap_y);
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
        const JsonValue* w = req.Find("window_w");
        const JsonValue* h = req.Find("window_h");
        if (!sid || sid->kind != JsonValue::Kind::String) {
            return MakeObjectWithError("missing stream_id");
        }
        int port_i = (port && port->kind == JsonValue::Kind::Int)
                         ? static_cast<int>(port->i) : 5080;
        int w_i = (w && w->kind == JsonValue::Kind::Int)
                      ? static_cast<int>(w->i) : 0;
        int h_i = (h && h->kind == JsonValue::Kind::Int)
                      ? static_cast<int>(h->i) : 0;
        auto err = streams.StartInbound(sid->s, port_i, w_i, h_i);
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

#if defined(_WIN32)

bool ReadExact(HANDLE h, void* buf, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(buf);
    DWORD read = 0;
    while (n > 0) {
        if (!ReadFile(h, p, static_cast<DWORD>(n), &read, nullptr)) {
            return false;
        }
        if (read == 0) return false;  // client closed
        p += read;
        n -= read;
    }
    return true;
}

bool WriteExact(HANDLE h, const void* buf, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    DWORD written = 0;
    while (n > 0) {
        if (!WriteFile(h, p, static_cast<DWORD>(n),
                        &written, nullptr)) {
            return false;
        }
        p += written;
        n -= written;
    }
    return true;
}

void ServiceClient(HANDLE pipe, StreamManager& streams) {
    while (true) {
        std::uint32_t len_le = 0;
        if (!ReadExact(pipe, &len_le, sizeof(len_le))) break;
        std::uint32_t len;
        std::memcpy(&len, &len_le, sizeof(len));
        if (len == 0 || len > (1u << 20)) break;

        std::string buf;
        buf.resize(len);
        if (!ReadExact(pipe, buf.data(), len)) break;

        JsonValue reply;
        auto req = ParseJson(buf);
        if (!req) {
            reply = MakeObjectWithError("bad json");
        } else {
            reply = DispatchCommand(*req, streams);
        }
        std::string out = SerializeJson(reply);
        std::uint32_t out_len = static_cast<std::uint32_t>(out.size());
        if (!WriteExact(pipe, &out_len, sizeof(out_len))) break;
        if (!WriteExact(pipe, out.data(), out.size())) break;
    }
    // Caller disconnects + closes.
}

#else  // POSIX

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

#endif  // _WIN32 vs POSIX ServiceClient + Read/WriteExact

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
#if defined(_WIN32)
    // Named-pipe handle that Close() signals to unblock
    // ConnectNamedPipe and wake the accept thread. Single-
    // instance-per-machine is enforced by the Python shell, so
    // one listener at a time is fine.
    HANDLE listen_handle = INVALID_HANDLE_VALUE;
#else
    int listen_fd = -1;
#endif
};

ControlSocket::ControlSocket() : impl_(std::make_unique<Impl>()) {}

ControlSocket::~ControlSocket() { Close(); }

bool ControlSocket::Open(const std::string& path) {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(impl_->lifecycle);
    if (impl_->open) return true;
    impl_->path = path;
    impl_->open = true;
    impl_->accept_thread = std::thread([this]() {
        // Mirror of the POSIX accept loop. Single-client: create
        // the named pipe, wait for a connection, service it
        // synchronously, disconnect, loop back for the next one.
        // That's what the Python shell expects — it opens, sends
        // its burst of commands, and closes.
        while (impl_->open) {
            HANDLE h = CreateNamedPipeA(
                impl_->path.c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1,           // single-instance
                64 * 1024,   // out buffer
                64 * 1024,   // in buffer
                0,           // default timeout
                nullptr);    // default security (current user)
            if (h == INVALID_HANDLE_VALUE) {
                std::fprintf(stderr,
                    "unio-pipe: CreateNamedPipe failed, "
                    "GetLastError=%lu\n",
                    static_cast<unsigned long>(GetLastError()));
                break;
            }
            impl_->listen_handle = h;
            BOOL ok = ConnectNamedPipe(h, nullptr);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_PIPE_CONNECTED) {
                    // Client connected between CreateNamedPipe
                    // and ConnectNamedPipe — still valid.
                    ok = TRUE;
                } else if (err == ERROR_BROKEN_PIPE
                           || err == ERROR_OPERATION_ABORTED) {
                    // Close() on this handle; exit the loop.
                    CloseHandle(h);
                    impl_->listen_handle = INVALID_HANDLE_VALUE;
                    break;
                }
            }
            if (ok && impl_->open) {
                ServiceClient(h, impl_->streams);
            }
            DisconnectNamedPipe(h);
            impl_->listen_handle = INVALID_HANDLE_VALUE;
            CloseHandle(h);
        }
    });
    return true;
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
#if defined(_WIN32)
    // Two ways the accept thread can be blocked: inside
    // ConnectNamedPipe (waiting for a client) or inside
    // ServiceClient's ReadFile. Both unblock on a client
    // disconnect; to force-disconnect our own half we open a
    // wakeup client against the pipe, which trips
    // ConnectNamedPipe's return, and then the accept loop sees
    // impl_->open=false and exits.
    if (impl_->listen_handle != INVALID_HANDLE_VALUE) {
        HANDLE wake = CreateFileA(
            impl_->path.c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
    }
    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }
    impl_->path.clear();
#else
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
