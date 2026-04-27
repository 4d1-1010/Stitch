/// @file main.cpp
/// @brief Executable entry point; dispatches to the OS-specific app loop.

#include "platform/app.hpp"

#if defined(_WIN32)

#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstdlib>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // GUI subsystem apps have detached std handles — fprintf to
    // stderr lands in /dev/null. We bind stderr to
    // %TEMP%\unio-ui.log so the orchestrator/router debug lines
    // are recoverable for cross-host bring-up.
    //
    // We can't just call freopen_s: the CRT opens with no
    // sharing, so an SSH `type` / PowerShell tail can't read the
    // log while the app is running and we'd have to kill the
    // process for every diagnosis cycle. Instead we open the
    // file ourselves via CreateFile with FILE_SHARE_READ, then
    // _dup2 the resulting fd over stderr's. freopen_s on "NUL"
    // first guarantees stderr has a valid CRT fd to dup over —
    // in a GUI app it starts as -2 (no associated stream).
    if (const char* tmp = std::getenv("TEMP")) {
        const std::string path = std::string(tmp) + "\\unio-ui.log";
        HANDLE h = ::CreateFileA(
            path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const int fd = ::_open_osfhandle(
                reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_TEXT);
            if (fd >= 0) {
                int err_fd = ::_fileno(stderr);
                if (err_fd < 0) {
                    std::FILE* dummy = nullptr;
                    ::freopen_s(&dummy, "NUL", "w", stderr);
                    err_fd = ::_fileno(stderr);
                }
                if (err_fd >= 0) {
                    ::_dup2(fd, err_fd);
                }
                ::_close(fd);
                // Unbuffered so a crash mid-startup still leaves
                // the last log lines on disk for diagnosis.
                std::setvbuf(stderr, nullptr, _IONBF, 0);
            } else {
                ::CloseHandle(h);
            }
        }
    }
    return unio_ui::platform::run({});
}

#else

int main(int, char**) {
    return unio_ui::platform::run({});
}

#endif
