// Entry point for unio-pipe. Day-1 shape: print the helper caps,
// open the control socket, accept commands until someone sends
// SIGTERM or closes stdin. Zero capture / encode / transport yet.

#include "unio_pipe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <string>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace {

// A real SIGTERM/SIGINT gate. Flipped from the signal handler;
// the accept loop polls it and unwinds cleanly instead of
// getting torn down mid-accept with half a client connection.
std::atomic<bool> g_shutdown_requested{false};

void HandleSignal(int /*signo*/) {
    g_shutdown_requested.store(true, std::memory_order_relaxed);
}

#if defined(_WIN32)
// Mirror of HandleSignal for the Windows console-control path.
// Without this, a parent-opened console that fires Ctrl-C would
// skip our shutdown flag and the process would die mid-accept
// (losing whatever the client was mid-writing).
BOOL WINAPI HandleWinCtrl(DWORD /*type*/) {
    g_shutdown_requested.store(true, std::memory_order_relaxed);
    return TRUE;
}
#endif

std::string DefaultSocketPath() {
    // Match helper_bridge.py's UDS_SOCKET_PATH / WIN_PIPE_NAME
    // so both sides agree on the rendezvous point without a
    // per-host config file.
#if defined(_WIN32)
    return R"(\\.\pipe\unio-pipe)";
#else
    return "/tmp/unio-pipe.sock";
#endif
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffer stderr. MSVC's CRT block-buffers stderr when it's
    // redirected to a file (4 KB) — which means logs from a
    // helper that's terminated mid-session (taskkill, crash) get
    // dropped, and tail -f style observability over redirects
    // misses every line below the buffer watermark. stderr-as-log
    // is the helper's main diagnostic channel, so unbuffering it
    // is worth the ~nil cost of one fflush per fprintf.
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::string socket_path = DefaultSocketPath();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (std::strcmp(argv[i], "--caps") == 0) {
            // One-shot caps probe for packaging / CI. Python-side
            // shell can shell out to this when helper_bridge
            // hasn't been wired yet.
            auto caps = unio::ProbeCaps();
            unio::JsonValue root;
            root.kind = unio::JsonValue::Kind::Object;
            auto str_list = [](const auto& items) {
                unio::JsonValue v;
                v.kind = unio::JsonValue::Kind::Array;
                for (const auto& item : items) {
                    unio::JsonValue s;
                    s.kind = unio::JsonValue::Kind::String;
                    s.s = item;
                    v.arr.push_back(std::move(s));
                }
                return v;
            };
            root.obj.emplace_back("encoders", str_list(caps.encoders));
            root.obj.emplace_back("decoders", str_list(caps.decoders));
            root.obj.emplace_back("presenters",
                                  str_list(caps.presenters));
            std::fputs(unio::SerializeJson(root).c_str(), stdout);
            std::fputc('\n', stdout);
            return 0;
        }
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(HandleWinCtrl, TRUE);
#else
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGINT, HandleSignal);
    // SIGPIPE when the Python peer closes its end of the UDS —
    // handle via EPIPE from write(), not a process-killing
    // signal.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    unio::ControlSocket sock;
    if (!sock.Open(socket_path)) {
        std::fprintf(stderr,
                     "unio-pipe: failed to open control socket at %s\n",
                     socket_path.c_str());
        return 1;
    }

    // Idle main thread: the accept loop runs in the ControlSocket
    // impl's thread. Block until we get a shutdown signal.
#if defined(_WIN32)
    while (!g_shutdown_requested.load()) {
        // TODO(pr6-win): port this to an event on the shutdown
        // token. For now, sleep 100 ms — fine for Day 1.
        Sleep(100);
    }
#else
    while (!g_shutdown_requested.load()) {
        // 100 ms == latency from SIGTERM arrival to exit; fine
        // for a process that isn't on the frame path.
        usleep(100 * 1000);
    }
#endif

    sock.Close();
    return 0;
}
