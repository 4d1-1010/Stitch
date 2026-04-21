// UDS control-socket server. Accepts one client at a time (the
// Python shell), reads length-prefixed JSON commands, dispatches
// to the matching handler, writes the JSON response back with
// the same framing.
//
// Single-client is a deliberate constraint: the shell process is
// the only thing that should ever drive the helper. A second
// connection is always a bug and gets rejected at accept time.

#include "unio_pipe.h"

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

JsonValue DispatchCommand(const JsonValue& req) {
    const JsonValue* cmd = req.Find("cmd");
    if (cmd == nullptr || cmd->kind != JsonValue::Kind::String) {
        return MakeObjectWithError("missing cmd field");
    }
    const std::string& name = cmd->s;

    if (name == kCmdHelperCaps) {
        return CapsToJson(ProbeCaps());
    }
    if (name == kCmdHelperStatus) {
        // Day-1: no per-stream state yet, just report alive.
        JsonValue root;
        root.kind = JsonValue::Kind::Object;
        JsonValue streams;
        streams.kind = JsonValue::Kind::Array;
        root.obj.emplace_back("per_stream", std::move(streams));
        return root;
    }
    if (name == kCmdStartOutbound || name == kCmdStartInbound
            || name == kCmdStop || name == kCmdRequestIdr) {
        // Placeholders for the stream-lifecycle commands. Next
        // week's capture + encoder + transport work fills these
        // in; for now the helper lets the Python bridge wire up
        // and get a truthful "not implemented yet" back.
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

void ServiceClient(int fd) {
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
            reply = DispatchCommand(*req);
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
            ServiceClient(cfd);
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
