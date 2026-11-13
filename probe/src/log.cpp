#include "probe.h"

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace probe {
namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
struct Line { uint64_t seq; std::string text; };
std::deque<Line> g_ring;
uint64_t g_seq = 0;
constexpr size_t kRingMax = 4000;
}  // namespace

void log_init(const std::wstring& path) {
    std::lock_guard lk(g_mu);
    g_file = _wfopen(path.c_str(), L"w");
}

void logf(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [%04lx] %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
             GetCurrentThreadId(), msg);

    std::lock_guard lk(g_mu);
    if (g_file) {
        fputs(line, g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
    g_ring.push_back({++g_seq, line});
    if (g_ring.size() > kRingMax) g_ring.pop_front();
}

json log_tail(size_t n, uint64_t since) {
    std::lock_guard lk(g_mu);
    json lines = json::array();
    size_t start = g_ring.size() > n ? g_ring.size() - n : 0;
    for (size_t i = start; i < g_ring.size(); ++i)
        if (g_ring[i].seq > since) lines.push_back({{"seq", g_ring[i].seq}, {"text", g_ring[i].text}});
    return {{"last_seq", g_seq}, {"lines", lines}};
}

}  // namespace probe
