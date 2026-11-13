// nfsu2_probe: shared declarations
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace probe {

using json = nlohmann::json;
constexpr const char* kVersion = "0.1.0";
constexpr uint16_t kDefaultPort = 47800;  // override with env NFSU2_PROBE_PORT

// ---- log.cpp ----
void log_init(const std::wstring& path);
void logf(const char* fmt, ...);
// last `n` lines with sequence number > since
json log_tail(size_t n, uint64_t since);

// ---- mem.cpp ----
bool mem_read(uintptr_t addr, void* out, size_t size);
bool mem_write(uintptr_t addr, const void* data, size_t size);
// pattern: hex bytes separated by spaces, "??" or "?" = wildcard
std::vector<uintptr_t> aob_scan(const std::string& pattern, uintptr_t start, uintptr_t end, size_t max_hits);

// ---- d3d.cpp ----
bool d3d_install_hooks();
json d3d_info();
// Run fn on the render thread at the next Present (before the original Present). Blocks up to timeout_ms.
// Throws std::runtime_error on timeout.
json run_on_main(std::function<json()> fn, unsigned timeout_ms);
json screenshot(int scale);  // must be called on the render thread

// ---- server.cpp ----
void server_start(uint16_t port);

// ---- main.cpp ----
struct ExeInfo {
    uintptr_t base = 0;
    uint32_t image_size = 0;
    bool hash_ok = false;
    std::string sha256;
    DWORD init_thread = 0;
};
const ExeInfo& exe();

std::string hex_addr(uintptr_t a);
std::string to_hex(const uint8_t* p, size_t n);
std::vector<uint8_t> from_hex(const std::string& s);
std::string base64(const uint8_t* p, size_t n);

}  // namespace probe
