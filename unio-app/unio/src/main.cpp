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
    // Single-instance gate. A named mutex on the local session
    // namespace ("Local\\…") collides only within the same login
    // session, so two different users on the same machine are
    // each entitled to their own running instance. The handle is
    // released automatically on process exit — taskkill /F (the
    // dev workflow's stop step) also clears it because the kernel
    // closes all handles when the process dies.
    if (HANDLE singleton = ::CreateMutexW(
            nullptr, FALSE, L"Local\\unio-ui-singleton");
        singleton != nullptr
        && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(singleton);
        ::MessageBoxW(nullptr,
                       L"unio-ui is already running.",
                       L"unio-ui",
                       MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    // Mutex handle leaks intentionally — closing it here would
    // release the lock immediately and let a second launch
    // succeed. Kernel reaps it on process exit.

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

#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int, char**) {
    // Single-instance gate via flock on a per-user runtime path.
    // The lock is implicitly released when the process exits —
    // a hard kill (SIGKILL / pkill -9), the dev workflow's stop
    // step included, also clears it. Per-user path so two users
    // on the same machine can each run their own instance.
    {
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        std::string path = (runtime != nullptr && *runtime != '\0')
            ? std::string(runtime) + "/unio-ui.lock"
            : "/tmp/unio-ui-" + std::to_string(::getuid()) + ".lock";
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
        if (fd >= 0) {
            if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
                std::fprintf(stderr,
                             "unio-ui: another instance is already "
                             "running (lock held on %s)\n",
                             path.c_str());
                ::close(fd);
                return 1;
            }
            // fd leaks intentionally — kernel releases the flock
            // on process exit; closing here would drop the lock.
        }
    }
    return unio_ui::platform::run({});
}

#endif
