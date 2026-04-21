#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Public surface of the helper. Intentionally narrow — anything
// past this header is internal implementation detail that the
// Python bridge never sees.

namespace unio {

// Wire format of every control-socket message: 4-byte little-
// endian length, then UTF-8 JSON. Matches helper_bridge.py's
// CONTROL_HEADER_FMT / "<I".
constexpr std::size_t kHeaderSize = 4;

// Every command the helper accepts. Values match Command enum
// in helper_bridge.py exactly; divergence between the two sides
// is the kind of bug a runtime string-switch wouldn't catch.
constexpr std::string_view kCmdStartOutbound = "start_outbound";
constexpr std::string_view kCmdStartInbound = "start_inbound";
constexpr std::string_view kCmdStop = "stop";
constexpr std::string_view kCmdRequestIdr = "request_idr";
constexpr std::string_view kCmdHelperCaps = "helper_caps";
constexpr std::string_view kCmdHelperStatus = "helper_status";

// What the helper can do on this host. Probed once at startup
// and returned verbatim in response to helper_caps. Callers on
// the Python side use this to pick which encoder/decoder to
// request per stream.
struct HelperCaps {
    std::vector<std::string> encoders;   // e.g. "nvenc", "vaapi"
    std::vector<std::string> decoders;
    std::vector<std::string> presenters; // e.g. "dxgi_flip", "egl"
};

HelperCaps ProbeCaps();

// ── Minimal JSON (objects of string/int/array/string values) ───
//
// Pulling libmirror / nlohmann::json as a dependency for Day-1
// scaffolding is overkill. The helper reads one-depth-nested
// JSON; we parse what we need and fail loud on anything else.

struct JsonValue {
    enum class Kind { Null, Bool, Int, String, Array, Object };
    Kind kind = Kind::Null;
    bool b = false;
    std::int64_t i = 0;
    std::string s;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* Find(std::string_view key) const;
    std::string AsString() const;
};

// Parses ``text``. Returns std::nullopt on any parse failure.
// Not robust against malicious input — helper only talks to its
// sibling Python process over a local UDS, which itself has
// already been auth-gated upstream.
std::optional<JsonValue> ParseJson(std::string_view text);

// Serialises a JsonValue back to UTF-8. Only supports the subset
// ParseJson produces. Pretty-printing deliberately skipped: one
// line per message keeps tcpdump / journal output scannable.
std::string SerializeJson(const JsonValue& v);

// ── Control socket ─────────────────────────────────────────────

class ControlSocket {
public:
    ControlSocket();
    ~ControlSocket();

    // Opens the UDS / named pipe and starts the accept thread.
    // Returns false on bind failure (socket busy, missing
    // directory, lacking permission) — caller should log and
    // exit; a half-started helper is worse than none.
    bool Open(const std::string& path);

    // Stops the accept thread and closes the socket. Safe to
    // call from signal handlers via a pre-opened self-pipe in
    // future; today it's only called from main's shutdown.
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace unio
