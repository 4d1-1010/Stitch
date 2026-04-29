/// @file clipboard_win32.cpp
/// @brief Win32 clipboard implementation. Uses OpenClipboard /
/// GetClipboardData / SetClipboardData with CF_UNICODETEXT,
/// converting between UTF-16 (Windows native) and UTF-8 (the
/// wire format) at the boundary.
///
/// OpenClipboard can fail when another process holds the
/// clipboard mid-update; we retry briefly the same way the
/// Python tree's WindowsBackend does.

#include "orchestrator/clipboard_backend.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
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

class Win32ClipboardBackend final : public IClipboardBackend {
public:
    bool open() override  { return true; }
    void close() override {}

    std::string get_text() override {
        std::lock_guard lk(m_);
        if (!open_clipboard_with_retry()) return {};
        std::string out;
        if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT); h != nullptr) {
            if (auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
                const int len = static_cast<int>(::wcslen(p));
                out = utf16_to_utf8(p, len);
                ::GlobalUnlock(h);
            }
        }
        ::CloseClipboard();
        return out;
    }

    void set_text(const std::string& text) override {
        std::lock_guard lk(m_);
        if (!open_clipboard_with_retry()) return;
        ::EmptyClipboard();
        const std::wstring w = utf8_to_utf16(text);
        const std::size_t bytes =
            (w.size() + 1) * sizeof(wchar_t);  // include trailing NUL
        HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (handle != nullptr) {
            if (auto* dst =
                    static_cast<wchar_t*>(::GlobalLock(handle))) {
                std::memcpy(dst, w.c_str(), bytes);
                ::GlobalUnlock(handle);
                if (::SetClipboardData(CF_UNICODETEXT, handle) == nullptr) {
                    // Set failed — we still own the alloc.
                    ::GlobalFree(handle);
                }
                // On success the system owns @c handle; don't free.
            } else {
                ::GlobalFree(handle);
            }
        }
        ::CloseClipboard();
    }

private:
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
};

}  // namespace

std::unique_ptr<IClipboardBackend> make_default_clipboard_backend() {
    return std::make_unique<Win32ClipboardBackend>();
}

}  // namespace unio_ui::orchestrator
