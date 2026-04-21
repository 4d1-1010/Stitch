// Tiny JSON parser / serialiser. Handles the subset the helper's
// control-plane uses — nested objects, arrays, strings, ints,
// bools, null. No floats (command fields are all ints or
// strings), no Unicode escapes beyond \" and \\, no streaming.
//
// Reason not to pull nlohmann::json: it's 25 kLOC of header and
// every symbol in the Day-1 helper is one person's walk of the
// code. Keeping the JSON surface small until we actually need
// more keeps review fast.

#include "unio_pipe.h"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <string>

namespace unio {

namespace {

struct Parser {
    std::string_view src;
    std::size_t pos = 0;

    void SkipWs() {
        while (pos < src.size() && std::isspace(
                   static_cast<unsigned char>(src[pos]))) {
            ++pos;
        }
    }

    bool Peek(char c) {
        SkipWs();
        return pos < src.size() && src[pos] == c;
    }

    bool Consume(char c) {
        SkipWs();
        if (pos < src.size() && src[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }

    bool Match(std::string_view lit) {
        SkipWs();
        if (src.substr(pos, lit.size()) == lit) {
            pos += lit.size();
            return true;
        }
        return false;
    }

    std::optional<std::string> ParseString() {
        SkipWs();
        if (pos >= src.size() || src[pos] != '"') return std::nullopt;
        ++pos;
        std::string out;
        while (pos < src.size() && src[pos] != '"') {
            char c = src[pos++];
            if (c == '\\' && pos < src.size()) {
                char esc = src[pos++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    default: return std::nullopt;  // unsupported
                }
            } else {
                out.push_back(c);
            }
        }
        if (pos >= src.size() || src[pos] != '"') return std::nullopt;
        ++pos;
        return out;
    }

    std::optional<JsonValue> ParseValue() {
        SkipWs();
        if (pos >= src.size()) return std::nullopt;
        char c = src[pos];
        JsonValue v;
        if (c == '"') {
            auto s = ParseString();
            if (!s) return std::nullopt;
            v.kind = JsonValue::Kind::String;
            v.s = *s;
            return v;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            std::size_t start = pos;
            if (src[pos] == '-') ++pos;
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
                ++pos;
            }
            std::int64_t n = 0;
            auto [p, ec] = std::from_chars(
                src.data() + start, src.data() + pos, n);
            if (ec != std::errc()) return std::nullopt;
            v.kind = JsonValue::Kind::Int;
            v.i = n;
            return v;
        }
        if (Match("true")) { v.kind = JsonValue::Kind::Bool; v.b = true; return v; }
        if (Match("false")) { v.kind = JsonValue::Kind::Bool; v.b = false; return v; }
        if (Match("null")) { v.kind = JsonValue::Kind::Null; return v; }
        if (c == '[') {
            ++pos;
            v.kind = JsonValue::Kind::Array;
            SkipWs();
            if (Consume(']')) return v;
            while (true) {
                auto elem = ParseValue();
                if (!elem) return std::nullopt;
                v.arr.push_back(std::move(*elem));
                if (Consume(']')) return v;
                if (!Consume(',')) return std::nullopt;
            }
        }
        if (c == '{') {
            ++pos;
            v.kind = JsonValue::Kind::Object;
            SkipWs();
            if (Consume('}')) return v;
            while (true) {
                auto k = ParseString();
                if (!k) return std::nullopt;
                if (!Consume(':')) return std::nullopt;
                auto val = ParseValue();
                if (!val) return std::nullopt;
                v.obj.emplace_back(std::move(*k), std::move(*val));
                if (Consume('}')) return v;
                if (!Consume(',')) return std::nullopt;
            }
        }
        return std::nullopt;
    }
};

void AppendString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out.append(R"(\")"); break;
            case '\\': out.append(R"(\\)"); break;
            case '\n': out.append(R"(\n)"); break;
            case '\r': out.append(R"(\r)"); break;
            case '\t': out.append(R"(\t)"); break;
            default: out.push_back(c);
        }
    }
    out.push_back('"');
}

}  // namespace

const JsonValue* JsonValue::Find(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    for (const auto& [k, v] : obj) {
        if (k == key) return &v;
    }
    return nullptr;
}

std::string JsonValue::AsString() const {
    return kind == Kind::String ? s : std::string{};
}

std::optional<JsonValue> ParseJson(std::string_view text) {
    Parser p{text};
    auto v = p.ParseValue();
    if (!v) return std::nullopt;
    p.SkipWs();
    if (p.pos != text.size()) return std::nullopt;
    return v;
}

std::string SerializeJson(const JsonValue& v) {
    std::string out;
    switch (v.kind) {
        case JsonValue::Kind::Null:
            out = "null";
            break;
        case JsonValue::Kind::Bool:
            out = v.b ? "true" : "false";
            break;
        case JsonValue::Kind::Int:
            out = std::to_string(v.i);
            break;
        case JsonValue::Kind::String:
            AppendString(out, v.s);
            break;
        case JsonValue::Kind::Array:
            out.push_back('[');
            for (std::size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out.push_back(',');
                out.append(SerializeJson(v.arr[i]));
            }
            out.push_back(']');
            break;
        case JsonValue::Kind::Object:
            out.push_back('{');
            for (std::size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out.push_back(',');
                AppendString(out, v.obj[i].first);
                out.push_back(':');
                out.append(SerializeJson(v.obj[i].second));
            }
            out.push_back('}');
            break;
    }
    return out;
}

}  // namespace unio
