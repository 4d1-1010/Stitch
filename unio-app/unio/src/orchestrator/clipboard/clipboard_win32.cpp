/// @file clipboard_win32.cpp
/// @brief Win32 multi-format clipboard implementation:
/// CF_UNICODETEXT (plain text), CF_HTML (HTML, registered),
/// CF_PNG (image, registered). UTF-16 ↔ UTF-8 at the text
/// boundary; CF_HTML's Microsoft-specific header is parsed on
/// read and synthesised on write so the wire-format HTML
/// stays a plain document fragment.
///
/// OpenClipboard can fail when another process holds the
/// clipboard mid-update; we retry briefly the same way the
/// Python tree's WindowsBackend does.

#include "orchestrator/clipboard_backend.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace unio_ui::orchestrator {

namespace {

std::wstring utf8_to_utf16(const std::string& s) {
    if (s.empty()) return {};
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
        nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                           static_cast<int>(s.size()),
                           out.data(), needed);
    return out;
}

std::string utf16_to_utf8(const wchar_t* s, int len) {
    if (s == nullptr || len <= 0) return {};
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, s, len,
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, len,
                           out.data(), needed,
                           nullptr, nullptr);
    return out;
}

namespace fs = std::filesystem;

/// @brief Walk @p root recursively, appending every regular
/// file inside it to @p files with relative paths anchored at
/// @p relative_root. Skips reparse points (symlinks /
/// junctions) and anything we can't stat — copy-paste maps
/// to regular file trees only.
void enumerate_path(const fs::path& root,
                     const std::string& relative_root,
                     std::vector<LocalFile>& files) {
    std::error_code ec;
    auto status = fs::symlink_status(root, ec);
    if (ec) return;
    if (fs::is_regular_file(status)) {
        LocalFile f;
        f.absolute_path = root.string();
        f.relative_path = relative_root;
        f.size = static_cast<std::uint64_t>(
            fs::file_size(root, ec));
        if (!ec) files.push_back(std::move(f));
        return;
    }
    if (!fs::is_directory(status)) return;

    fs::recursive_directory_iterator it(
        root,
        fs::directory_options::skip_permission_denied,
        ec);
    if (ec) return;
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::file_status entry_status =
            it->symlink_status(ec);
        if (ec) continue;
        if (!fs::is_regular_file(entry_status)) continue;
        const fs::path rel =
            fs::relative(it->path(), root, ec);
        if (ec) continue;
        std::string rel_str = rel.generic_string();
        std::string full_rel = relative_root;
        if (!full_rel.empty() && !rel_str.empty()) {
            full_rel.push_back('/');
        }
        full_rel.append(rel_str);

        LocalFile f;
        f.absolute_path = it->path().string();
        f.relative_path = std::move(full_rel);
        std::error_code size_ec;
        f.size = static_cast<std::uint64_t>(
            fs::file_size(it->path(), size_ec));
        if (size_ec) continue;
        files.push_back(std::move(f));
    }
}

/// @brief Build a @ref ClipboardFiles from a list of absolute
/// paths the user selected. Matches the X11 backend's
/// enumerate_selection: folders recursed, result sorted by
/// absolute_path so structurally equal selections compare
/// equal across consecutive polls.
ClipboardFiles enumerate_selection(
    const std::vector<std::string>& selection_paths) {
    ClipboardFiles out;
    out.selection_roots.reserve(selection_paths.size());
    for (const auto& abs : selection_paths) {
        const fs::path p(abs);
        std::string root_name = p.filename().string();
        if (root_name.empty()) {
            root_name = p.parent_path().filename().string();
        }
        if (root_name.empty()) continue;
        out.selection_roots.push_back(root_name);
        enumerate_path(p, root_name, out.files);
    }
    std::sort(out.files.begin(), out.files.end(),
              [](const LocalFile& a, const LocalFile& b) {
                  return a.absolute_path < b.absolute_path;
              });
    return out;
}

/// @brief Standard base64 encode without line breaks. Output
/// is suitable for embedding in a data URL.
std::string base64_encode(const std::uint8_t* data, std::size_t len) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? kAlphabet[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? kAlphabet[v & 0x3F]        : '=');
    }
    return out;
}

/// @brief Replace every @c src="..." (or @c src='...') value
/// inside @c <img ...> tags with @p data_url. LibreOffice and
/// browsers both put @c <img src="file:///tmp/..."> references
/// to local files in their HTML clipboard payload — receivers
/// on a different host can't resolve the path, and Word
/// renders the result as a broken-image placeholder. Embedding
/// our captured PNG as a data URL makes the HTML self-
/// contained so the image renders inline regardless of the
/// receiver's filesystem. Single-image clipboards are the
/// realistic case; multi-image clipboards reuse the same data
/// URL for every <img> (we only carry one PNG on the wire).
std::string embed_image_in_html(const std::string& html,
                                  const std::string& data_url) {
    if (data_url.empty()) return html;
    std::string out = html;
    std::size_t pos = 0;
    while ((pos = out.find("<img", pos)) != std::string::npos) {
        const std::size_t tag_end = out.find('>', pos);
        if (tag_end == std::string::npos) break;
        const std::size_t src_pos = out.find("src=", pos);
        if (src_pos != std::string::npos && src_pos < tag_end) {
            const std::size_t quote_pos = src_pos + 4;
            const char qc = out[quote_pos];
            if (qc == '"' || qc == '\'') {
                const std::size_t value_start = quote_pos + 1;
                const std::size_t value_end =
                    out.find(qc, value_start);
                if (value_end != std::string::npos
                    && value_end <= tag_end) {
                    out.replace(value_start,
                                value_end - value_start,
                                data_url);
                    pos = value_start + data_url.size() + 1;
                    continue;
                }
            }
        }
        pos = tag_end + 1;
    }
    return out;
}

/// @brief Build a CF_HTML payload around @p fragment. The
/// Microsoft "HTML Format" requires a specific header with
/// byte-offset markers into the same blob; receivers extract
/// the fragment using StartFragment / EndFragment.
std::string wrap_html_for_cf_html(const std::string& fragment) {
    constexpr const char* kHtmlPre =
        "<html><body><!--StartFragment-->";
    constexpr const char* kHtmlPost =
        "<!--EndFragment--></body></html>";

    // The header's offsets are 10-digit decimal byte offsets
    // counted from the very start of the blob (including the
    // header itself). Build a placeholder header to discover
    // its length, then patch in the real offsets.
    std::string header =
        "Version:0.9\r\n"
        "StartHTML:0000000000\r\n"
        "EndHTML:0000000000\r\n"
        "StartFragment:0000000000\r\n"
        "EndFragment:0000000000\r\n";

    const std::size_t header_len    = header.size();
    const std::size_t start_html    = header_len;
    const std::size_t pre_len       = std::strlen(kHtmlPre);
    const std::size_t post_len      = std::strlen(kHtmlPost);
    const std::size_t start_frag    = start_html + pre_len;
    const std::size_t end_frag      = start_frag + fragment.size();
    const std::size_t end_html      = end_frag + post_len;

    auto patch = [&](const char* key, std::size_t value) {
        char buf[11];
        std::snprintf(buf, sizeof(buf), "%010zu", value);
        const auto key_pos = header.find(key);
        // The 10-zero placeholder lives right after the key's
        // ':' separator.
        const auto zero_pos = header.find("0000000000", key_pos);
        header.replace(zero_pos, 10, buf, 10);
    };
    patch("StartHTML",     start_html);
    patch("EndHTML",       end_html);
    patch("StartFragment", start_frag);
    patch("EndFragment",   end_frag);

    return header + kHtmlPre + fragment + kHtmlPost;
}

/// @brief Extract the HTML fragment from a CF_HTML blob. If
/// the header isn't well-formed we return the whole blob as a
/// best-effort fallback — most apps tolerate that.
std::string extract_html_fragment(const std::string& cf_html) {
    auto find_offset = [&](const char* key) -> std::optional<std::size_t> {
        const std::string needle = std::string(key) + ":";
        const auto pos = cf_html.find(needle);
        if (pos == std::string::npos) return std::nullopt;
        const auto value_start = pos + needle.size();
        const auto eol = cf_html.find_first_of("\r\n", value_start);
        if (eol == std::string::npos) return std::nullopt;
        const std::string num =
            cf_html.substr(value_start, eol - value_start);
        try {
            return std::stoul(num);
        } catch (...) {
            return std::nullopt;
        }
    };
    auto sf = find_offset("StartFragment");
    auto ef = find_offset("EndFragment");
    if (!sf || !ef || *ef > cf_html.size() || *sf > *ef) {
        return cf_html;
    }
    return cf_html.substr(*sf, *ef - *sf);
}

class Win32ClipboardBackend final : public IClipboardBackend {
public:
    bool open() override {
        // Register the non-built-in formats once. IDs are
        // stable for the process lifetime; if any registration
        // returns 0 (out of clipboard format slots — shouldn't
        // happen) we fall back to whatever subset we got.
        cf_html_ = ::RegisterClipboardFormatW(L"HTML Format");
        cf_png_  = ::RegisterClipboardFormatW(L"PNG");
        cf_jpeg_ = ::RegisterClipboardFormatW(L"JFIF");
        cf_bmp_  = ::RegisterClipboardFormatW(L"image/bmp");
        return true;
    }
    void close() override {}

    ClipboardData get_clipboard() override {
        std::lock_guard lk(m_);
        if (!open_clipboard_with_retry()) return {};
        ClipboardData out;
        // Plain text via CF_UNICODETEXT (always supported).
        if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT); h != nullptr) {
            if (auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
                const int len = static_cast<int>(::wcslen(p));
                out.text = utf16_to_utf8(p, len);
                ::GlobalUnlock(h);
            }
        }
        // HTML — read, then strip Microsoft's wrapping header.
        if (cf_html_ != 0) {
            if (HANDLE h = ::GetClipboardData(cf_html_); h != nullptr) {
                if (auto* p =
                        static_cast<const char*>(::GlobalLock(h))) {
                    const SIZE_T sz = ::GlobalSize(h);
                    std::string raw;
                    if (sz > 0) {
                        // CF_HTML is stored as ASCII UTF-8.
                        // Some sources null-terminate; trim
                        // trailing zero bytes so the offset
                        // math doesn't reach past the real
                        // content.
                        SIZE_T real = sz;
                        while (real > 0 && p[real - 1] == '\0') {
                            --real;
                        }
                        raw.assign(p, real);
                    }
                    ::GlobalUnlock(h);
                    out.html = extract_html_fragment(raw);
                }
            }
        }
        // Image: try registered formats in priority order.
        // Many apps put PNG + DIB; we consume PNG as the
        // primary cross-platform format. JPEG / BMP are
        // pulled too if they're the only thing available.
        struct ImgFormat { UINT cf; const char* mime; };
        const ImgFormat kImageFormats[] = {
            { cf_png_,  "image/png"  },
            { cf_jpeg_, "image/jpeg" },
            { cf_bmp_,  "image/bmp"  },
        };
        for (const auto& fmt : kImageFormats) {
            if (fmt.cf == 0) continue;
            HANDLE h = ::GetClipboardData(fmt.cf);
            if (h == nullptr) continue;
            if (auto* p =
                    static_cast<const std::uint8_t*>(::GlobalLock(h))) {
                const SIZE_T sz = ::GlobalSize(h);
                out.image_bytes.assign(p, p + sz);
                out.image_mime.assign(fmt.mime);
                ::GlobalUnlock(h);
                break;
            }
        }
        ::CloseClipboard();
        return out;
    }

    ClipboardFiles get_clipboard_files() override {
        std::lock_guard lk(m_);
        if (!open_clipboard_with_retry()) return {};
        ClipboardFiles out;
        if (HANDLE h = ::GetClipboardData(CF_HDROP); h != nullptr) {
            auto* drop = static_cast<HDROP>(::GlobalLock(h));
            if (drop != nullptr) {
                const UINT count =
                    ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                std::vector<std::string> paths;
                paths.reserve(count);
                for (UINT i = 0; i < count; ++i) {
                    const UINT n =
                        ::DragQueryFileW(drop, i, nullptr, 0);
                    if (n == 0) continue;
                    std::wstring buf(n, L'\0');
                    ::DragQueryFileW(drop, i, buf.data(), n + 1);
                    paths.push_back(utf16_to_utf8(
                        buf.data(), static_cast<int>(n)));
                }
                ::GlobalUnlock(h);
                ::CloseClipboard();
                return enumerate_selection(paths);
            }
        }
        ::CloseClipboard();
        return out;
    }

    void set_clipboard_files(
        const std::vector<std::string>& abs_paths) override {
        std::lock_guard lk(m_);
        if (abs_paths.empty()) return;
        if (!open_clipboard_with_retry()) return;
        ::EmptyClipboard();

        // CF_HDROP body layout:
        //   DROPFILES header (fWide=TRUE)
        //   <wchar_t path>\0<wchar_t path>\0...\0\0
        // Receiver-side pastes via Explorer / Files read the
        // double-NUL-terminated wide-string list.
        std::vector<std::wstring> wide_paths;
        wide_paths.reserve(abs_paths.size());
        std::size_t total_chars = 0;
        for (const auto& p : abs_paths) {
            wide_paths.push_back(utf8_to_utf16(p));
            total_chars += wide_paths.back().size() + 1;
        }
        // +1 for the trailing extra null terminating the list.
        total_chars += 1;
        const std::size_t bytes =
            sizeof(DROPFILES) + total_chars * sizeof(wchar_t);

        HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (handle != nullptr) {
            if (auto* dst = ::GlobalLock(handle)) {
                auto* hdr = static_cast<DROPFILES*>(dst);
                hdr->pFiles = sizeof(DROPFILES);
                hdr->pt.x   = 0;
                hdr->pt.y   = 0;
                hdr->fNC    = FALSE;
                hdr->fWide  = TRUE;
                wchar_t* w = reinterpret_cast<wchar_t*>(
                    static_cast<std::uint8_t*>(dst)
                    + sizeof(DROPFILES));
                for (const auto& p : wide_paths) {
                    std::memcpy(w, p.c_str(),
                                (p.size() + 1) * sizeof(wchar_t));
                    w += p.size() + 1;
                }
                *w = L'\0';
                ::GlobalUnlock(handle);
                if (::SetClipboardData(CF_HDROP, handle) == nullptr) {
                    ::GlobalFree(handle);
                }
            } else {
                ::GlobalFree(handle);
            }
        }

        // Preferred Drop Effect = DROPEFFECT_COPY (0x5). Lets
        // the receiving file manager know this was a Copy
        // (not a Cut). Without this some apps treat the paste
        // as a move and the temp-dir source vanishes.
        const UINT cf_pref =
            ::RegisterClipboardFormatW(L"Preferred DropEffect");
        if (cf_pref != 0) {
            HGLOBAL eff = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            if (eff != nullptr) {
                if (auto* p = ::GlobalLock(eff)) {
                    *static_cast<DWORD*>(p) = DROPEFFECT_COPY;
                    ::GlobalUnlock(eff);
                    if (::SetClipboardData(cf_pref, eff)
                        == nullptr) {
                        ::GlobalFree(eff);
                    }
                } else {
                    ::GlobalFree(eff);
                }
            }
        }
        ::CloseClipboard();
    }

    void set_clipboard(const ClipboardData& data) override {
        std::lock_guard lk(m_);
        if (!open_clipboard_with_retry()) return;
        ::EmptyClipboard();
        if (!data.text.empty()) {
            put_unicode_text(data.text);
        }
        if (!data.html.empty() && cf_html_ != 0) {
            // When the source bundled rich text + image, the
            // HTML almost always references the image via a
            // local file URL (LibreOffice: file:///tmp/...).
            // Word picks CF_HTML over the raw image format and
            // renders the <img> as a broken placeholder.
            // Inline our captured image as a data URL so the
            // receiver's HTML renderer doesn't need filesystem
            // access. The data URL's MIME mirrors the bytes —
            // browsers + Word handle image/png, image/jpeg
            // and image/bmp interchangeably here.
            std::string html = data.html;
            if (!data.image_bytes.empty()
                && !data.image_mime.empty()) {
                const std::string data_url =
                    "data:" + data.image_mime + ";base64,"
                    + base64_encode(data.image_bytes.data(),
                                     data.image_bytes.size());
                html = embed_image_in_html(html, data_url);
            }
            const std::string blob = wrap_html_for_cf_html(html);
            put_blob(cf_html_, blob.data(), blob.size());
        }
        if (!data.image_bytes.empty()
            && !data.image_mime.empty()) {
            // Pick the matching registered clipboard format
            // for the MIME we received. If the format is 0
            // (registration failed) we silently skip the raw
            // image — receivers still get the inlined HTML
            // data URL above.
            UINT target_cf = 0;
            if (data.image_mime == "image/png")  target_cf = cf_png_;
            else if (data.image_mime == "image/jpeg") target_cf = cf_jpeg_;
            else if (data.image_mime == "image/bmp")  target_cf = cf_bmp_;
            if (target_cf != 0) {
                put_blob(target_cf,
                          data.image_bytes.data(),
                          data.image_bytes.size());
            }
        }
        ::CloseClipboard();
    }

private:
    /// @brief Allocate a HGLOBAL, copy @p bytes into it, and
    /// hand it to SetClipboardData. On success the system owns
    /// the handle; on failure we free it so the leak budget is
    /// zero either way.
    void put_blob(UINT format, const void* bytes, std::size_t size) {
        HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, size);
        if (handle == nullptr) return;
        if (auto* dst = ::GlobalLock(handle)) {
            std::memcpy(dst, bytes, size);
            ::GlobalUnlock(handle);
            if (::SetClipboardData(format, handle) == nullptr) {
                ::GlobalFree(handle);
            }
        } else {
            ::GlobalFree(handle);
        }
    }

    /// @brief CF_UNICODETEXT helper: UTF-8 → UTF-16 → HGLOBAL
    /// (NUL-terminated wchar_t buffer). Identical setup to
    /// @ref put_blob but with the trailing NUL the format
    /// expects.
    void put_unicode_text(const std::string& text) {
        const std::wstring w = utf8_to_utf16(text);
        const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);
        HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (handle == nullptr) return;
        if (auto* dst = static_cast<wchar_t*>(::GlobalLock(handle))) {
            std::memcpy(dst, w.c_str(), bytes);
            ::GlobalUnlock(handle);
            if (::SetClipboardData(CF_UNICODETEXT, handle) == nullptr) {
                ::GlobalFree(handle);
            }
        } else {
            ::GlobalFree(handle);
        }
    }

    bool open_clipboard_with_retry() {
        for (int i = 0; i < 10; ++i) {
            if (::OpenClipboard(nullptr)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::fprintf(stderr,
                     "clipboard_win32: OpenClipboard failed after retries\n");
        return false;
    }

    std::mutex m_;
    UINT       cf_html_ = 0;
    UINT       cf_png_  = 0;
    UINT       cf_jpeg_ = 0;
    UINT       cf_bmp_  = 0;
};

}  // namespace

std::unique_ptr<IClipboardBackend> make_default_clipboard_backend() {
    return std::make_unique<Win32ClipboardBackend>();
}

}  // namespace unio_ui::orchestrator
