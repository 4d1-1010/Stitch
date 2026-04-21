// QUIC transport for the unio-pipe helper — thin C++ wrapper
// around Microsoft msquic (MIT). One outbound stream per stream_id
// from source → sink, framed as [u32_le length][Annex-B bytes] so
// the receiver can reconstruct packet boundaries from the byte
// stream the library delivers.
//
// msquic is asynchronous end-to-end; every state change comes in
// as a callback on one of the library's worker threads. We hide
// that behind blocking-looking Connect / Start / SendPacket /
// Close / Stop methods by condvar-signalling on the milestone
// events (handshake complete, peer ready, local shutdown).
//
// TLS story: QUIC always uses TLS 1.3, which gives us transport
// encryption for free. Peer authenticity is handled one layer up
// by unio's existing pairing PIN — so the QUIC client trusts any
// server cert (NO_CERTIFICATE_VALIDATION) and the server uses a
// self-signed ephemeral cert regenerated each boot. If we ever
// tighten this we'll pin the cert fingerprint onto the pairing
// record.

#include "quic_transport.h"

#if defined(UNIO_PIPE_HAS_MSQUIC)

// msquic's public header uses C anonymous structs which trip
// -Wpedantic under C++. The rest of our TU stays pedantic; only
// this include window is relaxed.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <msquic.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(__linux__)
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#endif

namespace unio {

namespace {

// One process-wide msquic API table + registration — opened on
// first use and torn down at program exit via the destructor of
// a function-local static.
struct MsQuicBootstrap {
    const QUIC_API_TABLE* api = nullptr;
    HQUIC registration = nullptr;

    MsQuicBootstrap() {
        if (QUIC_FAILED(MsQuicOpen2(&api))) {
            std::fprintf(stderr,
                         "unio-pipe: MsQuicOpen2 failed\n");
            api = nullptr;
            return;
        }
        const QUIC_REGISTRATION_CONFIG reg{
            "unio-pipe",
            QUIC_EXECUTION_PROFILE_LOW_LATENCY};
        if (QUIC_FAILED(
                api->RegistrationOpen(&reg, &registration))) {
            std::fprintf(stderr,
                         "unio-pipe: RegistrationOpen failed\n");
            MsQuicClose(api);
            api = nullptr;
        }
    }

    ~MsQuicBootstrap() {
        if (registration) api->RegistrationClose(registration);
        if (api) MsQuicClose(api);
    }
};

MsQuicBootstrap& Boot() {
    static MsQuicBootstrap b;
    return b;
}

// Our wire ALPN. One token, mirrored on both ends. Anything that
// connects with a different ALPN is rejected by msquic during
// the TLS handshake before it ever reaches our callback.
const QUIC_BUFFER kAlpn = {
    static_cast<std::uint32_t>(sizeof("unio-pipe/1") - 1),
    reinterpret_cast<std::uint8_t*>(const_cast<char*>("unio-pipe/1"))};

// Framing: one four-byte little-endian length, then the packet
// bytes. Chosen over varint for the same reason the Python
// control-plane uses it — trivially parseable, never ambiguous.
constexpr std::size_t kLengthPrefixBytes = 4;

void WriteLenPrefix(std::uint8_t* out, std::uint32_t len) {
    out[0] = static_cast<std::uint8_t>(len & 0xFF);
    out[1] = static_cast<std::uint8_t>((len >> 8) & 0xFF);
    out[2] = static_cast<std::uint8_t>((len >> 16) & 0xFF);
    out[3] = static_cast<std::uint8_t>((len >> 24) & 0xFF);
}
std::uint32_t ReadLenPrefix(const std::uint8_t* in) {
    return static_cast<std::uint32_t>(in[0])
         | (static_cast<std::uint32_t>(in[1]) << 8)
         | (static_cast<std::uint32_t>(in[2]) << 16)
         | (static_cast<std::uint32_t>(in[3]) << 24);
}

}  // namespace

// ---------------------------------------------------------------
// QuicOutbound
// ---------------------------------------------------------------

struct QuicOutbound::Impl {
    HQUIC config = nullptr;
    HQUIC connection = nullptr;
    HQUIC stream = nullptr;

    std::atomic<bool> connected{false};
    std::atomic<bool> stream_ready{false};
    std::atomic<bool> dead{false};
    std::atomic<std::string*> last_error{nullptr};

    std::mutex cv_mu;
    std::condition_variable cv;

    std::string stream_id;

    void SetError(std::string_view msg) {
        auto* s = new std::string(msg);
        std::string* prev = last_error.exchange(s);
        delete prev;
    }

    ~Impl() {
        // Destructor runs after Close(), nothing to do here
        // beyond releasing the error-string allocation.
        delete last_error.load();
    }
};

namespace {

QUIC_STATUS QUIC_API OutboundStreamCallback(
        HQUIC, void* ctx, QUIC_STREAM_EVENT* ev) {
    auto* impl = static_cast<QuicOutbound::Impl*>(ctx);
    switch (ev->Type) {
        case QUIC_STREAM_EVENT_START_COMPLETE:
            impl->stream_ready.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(impl->cv_mu);
                impl->cv.notify_all();
            }
            break;
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            // ClientContext holds our [len|payload] blob — free it.
            std::free(ev->SEND_COMPLETE.ClientContext);
            break;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            impl->dead.store(true, std::memory_order_release);
            break;
        default:
            break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API OutboundConnectionCallback(
        HQUIC, void* ctx, QUIC_CONNECTION_EVENT* ev) {
    auto* impl = static_cast<QuicOutbound::Impl*>(ctx);
    switch (ev->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            impl->connected.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(impl->cv_mu);
                impl->cv.notify_all();
            }
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            impl->dead.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(impl->cv_mu);
                impl->cv.notify_all();
            }
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            impl->dead.store(true, std::memory_order_release);
            break;
        default:
            break;
    }
    return QUIC_STATUS_SUCCESS;
}

}  // namespace

QuicOutbound::QuicOutbound() : impl_(std::make_unique<Impl>()) {}
QuicOutbound::~QuicOutbound() { Close(); }

std::optional<std::string> QuicOutbound::Connect(const Config& cfg) {
    auto& boot = Boot();
    if (!boot.api || !boot.registration) {
        return "msquic bootstrap failed";
    }
    impl_->stream_id = cfg.stream_id;

    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = 10000;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.PeerUnidiStreamCount = 0;
    settings.IsSet.PeerUnidiStreamCount = TRUE;
    settings.PeerBidiStreamCount = 1;
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.SendBufferingEnabled = FALSE;
    settings.IsSet.SendBufferingEnabled = TRUE;

    if (QUIC_FAILED(boot.api->ConfigurationOpen(
            boot.registration, &kAlpn, 1, &settings, sizeof(settings),
            nullptr, &impl_->config))) {
        return "ConfigurationOpen failed";
    }
    QUIC_CREDENTIAL_CONFIG cred{};
    cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
    cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT
               | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    if (QUIC_FAILED(boot.api->ConfigurationLoadCredential(
            impl_->config, &cred))) {
        return "ConfigurationLoadCredential (client) failed";
    }
    if (QUIC_FAILED(boot.api->ConnectionOpen(
            boot.registration, OutboundConnectionCallback,
            impl_.get(), &impl_->connection))) {
        return "ConnectionOpen failed";
    }
    if (QUIC_FAILED(boot.api->ConnectionStart(
            impl_->connection, impl_->config, QUIC_ADDRESS_FAMILY_UNSPEC,
            cfg.peer_host.c_str(), cfg.peer_port))) {
        return "ConnectionStart failed";
    }

    {
        std::unique_lock<std::mutex> lk(impl_->cv_mu);
        impl_->cv.wait_for(lk,
            std::chrono::milliseconds(cfg.handshake_timeout_ms),
            [&]() { return impl_->connected.load()
                        || impl_->dead.load(); });
    }
    if (!impl_->connected.load()) {
        return impl_->dead.load()
               ? "handshake failed (peer rejected / unreachable)"
               : "handshake timed out";
    }

    if (QUIC_FAILED(boot.api->StreamOpen(
            impl_->connection, QUIC_STREAM_OPEN_FLAG_NONE,
            OutboundStreamCallback, impl_.get(), &impl_->stream))) {
        return "StreamOpen failed";
    }
    if (QUIC_FAILED(boot.api->StreamStart(
            impl_->stream, QUIC_STREAM_START_FLAG_NONE))) {
        return "StreamStart failed";
    }
    return std::nullopt;
}

bool QuicOutbound::SendPacket(const std::uint8_t* bytes,
                               std::size_t len) {
    if (impl_->dead.load()) return false;
    if (!impl_->stream) return false;
    if (len > 0xFFFF'FF00u) return false;  // sanity cap

    // Heap-allocate the framed buffer + QUIC_BUFFER so ownership
    // can survive until the SEND_COMPLETE callback frees it.
    const std::size_t total = kLengthPrefixBytes + len;
    auto* mem = static_cast<std::uint8_t*>(
        std::malloc(sizeof(QUIC_BUFFER) + total));
    if (!mem) return false;
    auto* qb = reinterpret_cast<QUIC_BUFFER*>(mem);
    std::uint8_t* payload = mem + sizeof(QUIC_BUFFER);
    WriteLenPrefix(payload, static_cast<std::uint32_t>(len));
    std::memcpy(payload + kLengthPrefixBytes, bytes, len);
    qb->Buffer = payload;
    qb->Length = static_cast<std::uint32_t>(total);

    auto& boot = Boot();
    QUIC_STATUS s = boot.api->StreamSend(
        impl_->stream, qb, 1, QUIC_SEND_FLAG_NONE, mem);
    if (QUIC_FAILED(s)) {
        std::free(mem);
        impl_->dead.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

void QuicOutbound::Close() {
    auto& boot = Boot();
    if (!boot.api) return;
    if (impl_->stream) {
        boot.api->StreamShutdown(impl_->stream,
            QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        boot.api->StreamClose(impl_->stream);
        impl_->stream = nullptr;
    }
    if (impl_->connection) {
        boot.api->ConnectionShutdown(impl_->connection,
            QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        boot.api->ConnectionClose(impl_->connection);
        impl_->connection = nullptr;
    }
    if (impl_->config) {
        boot.api->ConfigurationClose(impl_->config);
        impl_->config = nullptr;
    }
}

bool QuicOutbound::IsConnected() const {
    return impl_->connected.load() && !impl_->dead.load();
}

// ---------------------------------------------------------------
// QuicInbound (server)
// ---------------------------------------------------------------

namespace {

#if defined(__linux__)
// Build a self-signed RSA-2048 cert in memory. msquic wants PKCS12
// blob (or files) for the server credential; we use the PEM helper
// path that takes CERT + KEY as file paths. For MVP simplicity
// we write PEM files into /tmp and hand those paths to msquic.
// The files are mode 0600 and unlink-on-close.
struct SelfSignedCertFiles {
    std::string cert_path;
    std::string key_path;
    bool ok = false;

    SelfSignedCertFiles() = default;
    SelfSignedCertFiles(const SelfSignedCertFiles&) = delete;
    SelfSignedCertFiles& operator=(const SelfSignedCertFiles&) = delete;
    // Move must null out the source so its destructor doesn't
    // unlink the files while we still own them on the target.
    // Without this, the obvious `impl_->cert = MakeSelfSignedCert();`
    // wipes the cert on the spot and msquic gets ENOENT.
    SelfSignedCertFiles(SelfSignedCertFiles&& other) noexcept
        : cert_path(std::move(other.cert_path)),
          key_path(std::move(other.key_path)),
          ok(other.ok) {
        other.cert_path.clear();
        other.key_path.clear();
        other.ok = false;
    }
    SelfSignedCertFiles& operator=(SelfSignedCertFiles&& other) noexcept {
        if (this != &other) {
            if (!cert_path.empty()) std::remove(cert_path.c_str());
            if (!key_path.empty()) std::remove(key_path.c_str());
            cert_path = std::move(other.cert_path);
            key_path = std::move(other.key_path);
            ok = other.ok;
            other.cert_path.clear();
            other.key_path.clear();
            other.ok = false;
        }
        return *this;
    }

    ~SelfSignedCertFiles() {
        if (!cert_path.empty()) std::remove(cert_path.c_str());
        if (!key_path.empty()) std::remove(key_path.c_str());
    }
};

SelfSignedCertFiles MakeSelfSignedCert() {
    SelfSignedCertFiles out;
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    if (!pkey) return out;

    X509* x = X509_new();
    if (!x) { EVP_PKEY_free(pkey); return out; }
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x),
                     60 * 60 * 24 * 365);
    X509_set_pubkey(x, pkey);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("unio-pipe"),
        -1, -1, 0);
    X509_set_issuer_name(x, name);
    X509_sign(x, pkey, EVP_sha256());

    out.cert_path = "/tmp/unio-pipe-cert-XXXXXX.pem";
    out.key_path = "/tmp/unio-pipe-key-XXXXXX.pem";
    // mkstemps would be nicer but we just open+write here;
    // collisions are negligible for the dev loop.
    char cert_tmp[] = "/tmp/unio-pipe-cert-XXXXXX";
    char key_tmp[] = "/tmp/unio-pipe-key-XXXXXX";
    int cfd = mkstemp(cert_tmp);
    int kfd = mkstemp(key_tmp);
    if (cfd < 0 || kfd < 0) {
        X509_free(x); EVP_PKEY_free(pkey);
        return out;
    }
    FILE* cf = fdopen(cfd, "w");
    FILE* kf = fdopen(kfd, "w");
    PEM_write_X509(cf, x);
    PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    std::fclose(cf);
    std::fclose(kf);
    out.cert_path = cert_tmp;
    out.key_path  = key_tmp;
    X509_free(x);
    EVP_PKEY_free(pkey);
    out.ok = true;
    return out;
}
#endif  // __linux__

}  // namespace

struct QuicInbound::Impl {
    HQUIC config = nullptr;
    HQUIC listener = nullptr;
    HQUIC connection = nullptr;
    HQUIC stream = nullptr;

    QuicInbound::PacketCallback cb;
    std::atomic<std::uint64_t> packets{0};
    std::atomic<std::uint64_t> bytes{0};

    // Receiver-side reassembly buffer. msquic delivers byte runs
    // of arbitrary size; we pull out length-prefixed packets from
    // whatever window we've accumulated and leave the remainder
    // for the next event.
    std::vector<std::uint8_t> rx;

#if defined(__linux__)
    SelfSignedCertFiles cert;
#endif

    ~Impl() {
        auto& boot = Boot();
        if (!boot.api) return;
        if (stream) {
            boot.api->StreamClose(stream);
            stream = nullptr;
        }
        if (connection) {
            boot.api->ConnectionClose(connection);
            connection = nullptr;
        }
        if (listener) {
            boot.api->ListenerClose(listener);
            listener = nullptr;
        }
        if (config) {
            boot.api->ConfigurationClose(config);
            config = nullptr;
        }
    }
};

namespace {

void DrainRx(QuicInbound::Impl* impl) {
    auto& rx = impl->rx;
    std::size_t off = 0;
    while (rx.size() - off >= kLengthPrefixBytes) {
        std::uint32_t len = ReadLenPrefix(rx.data() + off);
        if (len > 64u * 1024u * 1024u) {
            // Oversized packet — assume wire corruption and stop.
            // A follow-up could surface this via helper_status.
            rx.clear();
            return;
        }
        if (rx.size() - off < kLengthPrefixBytes + len) break;
        if (impl->cb) {
            impl->cb(rx.data() + off + kLengthPrefixBytes, len);
        }
        impl->packets.fetch_add(1, std::memory_order_relaxed);
        impl->bytes.fetch_add(len, std::memory_order_relaxed);
        off += kLengthPrefixBytes + len;
    }
    if (off > 0) {
        rx.erase(rx.begin(), rx.begin() + off);
    }
}

QUIC_STATUS QUIC_API InboundStreamCallback(
        HQUIC, void* ctx, QUIC_STREAM_EVENT* ev) {
    auto* impl = static_cast<QuicInbound::Impl*>(ctx);
    switch (ev->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            for (std::uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
                const auto& b = ev->RECEIVE.Buffers[i];
                impl->rx.insert(impl->rx.end(), b.Buffer,
                                b.Buffer + b.Length);
            }
            DrainRx(impl);
            break;
        }
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            break;
        default:
            break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API InboundConnectionCallback(
        HQUIC, void* ctx, QUIC_CONNECTION_EVENT* ev) {
    auto* impl = static_cast<QuicInbound::Impl*>(ctx);
    auto& boot = Boot();
    switch (ev->Type) {
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
            impl->stream = ev->PEER_STREAM_STARTED.Stream;
            boot.api->SetCallbackHandler(
                impl->stream,
                reinterpret_cast<void*>(InboundStreamCallback),
                impl);
            break;
        }
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            if (!ev->SHUTDOWN_COMPLETE.AppCloseInProgress) {
                boot.api->ConnectionClose(impl->connection);
                impl->connection = nullptr;
            }
            break;
        default:
            break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API InboundListenerCallback(
        HQUIC, void* ctx, QUIC_LISTENER_EVENT* ev) {
    auto* impl = static_cast<QuicInbound::Impl*>(ctx);
    auto& boot = Boot();
    if (ev->Type == QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        impl->connection = ev->NEW_CONNECTION.Connection;
        boot.api->SetCallbackHandler(
            impl->connection,
            reinterpret_cast<void*>(InboundConnectionCallback),
            impl);
        return boot.api->ConnectionSetConfiguration(
            impl->connection, impl->config);
    }
    return QUIC_STATUS_SUCCESS;
}

}  // namespace

QuicInbound::QuicInbound() : impl_(std::make_unique<Impl>()) {}
QuicInbound::~QuicInbound() { Stop(); }

std::optional<std::string> QuicInbound::Start(
        const Config& cfg, PacketCallback cb) {
    auto& boot = Boot();
    if (!boot.api || !boot.registration) {
        return "msquic bootstrap failed";
    }
    impl_->cb = std::move(cb);

    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = 10000;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.PeerBidiStreamCount = 1;
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.ServerResumptionLevel = QUIC_SERVER_NO_RESUME;
    settings.IsSet.ServerResumptionLevel = TRUE;

    if (QUIC_FAILED(boot.api->ConfigurationOpen(
            boot.registration, &kAlpn, 1, &settings, sizeof(settings),
            nullptr, &impl_->config))) {
        return "ConfigurationOpen failed";
    }

#if defined(__linux__)
    impl_->cert = MakeSelfSignedCert();
    if (!impl_->cert.ok) {
        return "self-signed cert generation failed";
    }
    QUIC_CERTIFICATE_FILE certf{};
    certf.CertificateFile = impl_->cert.cert_path.c_str();
    certf.PrivateKeyFile = impl_->cert.key_path.c_str();
    QUIC_CREDENTIAL_CONFIG cred{};
    cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred.CertificateFile = &certf;
    cred.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    QUIC_STATUS cred_status =
        boot.api->ConfigurationLoadCredential(impl_->config, &cred);
    if (QUIC_FAILED(cred_status)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "ConfigurationLoadCredential (server) failed: 0x%x",
            static_cast<unsigned>(cred_status));
        return std::string(buf);
    }
#else
    return "inbound not wired yet on this platform";
#endif

    if (QUIC_FAILED(boot.api->ListenerOpen(
            boot.registration, InboundListenerCallback,
            impl_.get(), &impl_->listener))) {
        return "ListenerOpen failed";
    }
    QUIC_ADDR addr{};
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&addr, cfg.listen_port);
    if (QUIC_FAILED(boot.api->ListenerStart(
            impl_->listener, &kAlpn, 1, &addr))) {
        return "ListenerStart failed";
    }
    return std::nullopt;
}

void QuicInbound::Stop() {
    auto& boot = Boot();
    if (!boot.api) return;
    if (impl_->listener) {
        boot.api->ListenerStop(impl_->listener);
    }
}

std::uint64_t QuicInbound::PacketsReceived() const {
    return impl_->packets.load(std::memory_order_relaxed);
}
std::uint64_t QuicInbound::BytesReceived() const {
    return impl_->bytes.load(std::memory_order_relaxed);
}

}  // namespace unio

#else  // !UNIO_PIPE_HAS_MSQUIC

namespace unio {

struct QuicOutbound::Impl {};
QuicOutbound::QuicOutbound() : impl_(std::make_unique<Impl>()) {}
QuicOutbound::~QuicOutbound() = default;
std::optional<std::string> QuicOutbound::Connect(const Config&) {
    return "built without msquic (UNIO_PIPE_HAS_MSQUIC=0)";
}
bool QuicOutbound::SendPacket(const std::uint8_t*, std::size_t) {
    return false;
}
void QuicOutbound::Close() {}
bool QuicOutbound::IsConnected() const { return false; }

struct QuicInbound::Impl {};
QuicInbound::QuicInbound() : impl_(std::make_unique<Impl>()) {}
QuicInbound::~QuicInbound() = default;
std::optional<std::string> QuicInbound::Start(const Config&,
                                              PacketCallback) {
    return "built without msquic (UNIO_PIPE_HAS_MSQUIC=0)";
}
void QuicInbound::Stop() {}
std::uint64_t QuicInbound::PacketsReceived() const { return 0; }
std::uint64_t QuicInbound::BytesReceived() const { return 0; }

}  // namespace unio

#endif  // UNIO_PIPE_HAS_MSQUIC
