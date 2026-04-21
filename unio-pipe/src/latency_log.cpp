#include "latency_log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace unio {

namespace {

std::mutex g_mu;
std::FILE* g_fp = nullptr;
bool g_probed = false;  // one-shot: if env var was unset the first
                         // time we looked, don't keep re-getenv-ing.

void ProbeAndOpen(std::string_view env_var) {
    if (g_probed) return;
    g_probed = true;
    std::string name(env_var);
    const char* path = std::getenv(name.c_str());
    if (!path || path[0] == '\0') return;
    g_fp = std::fopen(path, "w");
    if (!g_fp) {
        std::fprintf(stderr,
            "unio-pipe: latency CSV open failed: %s\n", path);
        return;
    }
    std::fprintf(g_fp,
        "frame_id,capture_ns,decode_done_ns,present_done_ns,"
        "width,height,capture_to_decode_us,"
        "decode_to_present_us,capture_to_present_us\n");
    std::fflush(g_fp);
    std::fprintf(stderr,
        "unio-pipe: latency CSV open: %s\n", path);
}

}  // namespace

void LogLatency(std::string_view env_var,
                std::uint64_t frame_id,
                std::uint64_t capture_ns,
                std::uint64_t decode_ns,
                std::uint64_t present_ns,
                std::uint32_t width,
                std::uint32_t height) {
    std::lock_guard<std::mutex> lk(g_mu);
    ProbeAndOpen(env_var);
    if (!g_fp) return;
    const auto c2d = capture_ns ? (decode_ns - capture_ns) / 1000 : 0;
    const auto d2p = decode_ns ? (present_ns - decode_ns) / 1000 : 0;
    const auto c2p = capture_ns ? (present_ns - capture_ns) / 1000 : 0;
    std::fprintf(g_fp, "%llu,%llu,%llu,%llu,%u,%u,%llu,%llu,%llu\n",
        static_cast<unsigned long long>(frame_id),
        static_cast<unsigned long long>(capture_ns),
        static_cast<unsigned long long>(decode_ns),
        static_cast<unsigned long long>(present_ns),
        width, height,
        static_cast<unsigned long long>(c2d),
        static_cast<unsigned long long>(d2p),
        static_cast<unsigned long long>(c2p));
    std::fflush(g_fp);
}

}  // namespace unio
