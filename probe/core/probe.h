// nfsu2_core: declarations shared by the hot-reloadable core's sources
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api.h"

namespace probe {

using json = nlohmann::json;
constexpr const char* kVersion = "0.2.1";
constexpr uint16_t kDefaultPort = 47800;  // override with env NFSU2_PROBE_PORT

// ---- core_main.cpp ----
const LoaderApi& api();
bool has_api();
const char* core_build();

// ---- log.cpp (forwards to the loader) ----
void logf(const char* fmt, ...);
// last `n` lines with sequence number > since
json log_tail(size_t n, uint64_t since);

// ---- mem.cpp ----
bool mem_read(uintptr_t addr, void* out, size_t size);
bool mem_write(uintptr_t addr, const void* data, size_t size);
// pattern: hex bytes separated by spaces, "??" or "?" = wildcard
std::vector<uintptr_t> aob_scan(const std::string& pattern, uintptr_t start, uintptr_t end, size_t max_hits);

// ---- d3d.cpp (task queue + screenshots; the loader owns the D3D hooks) ----
void run_tasks();    // render thread, from on_frame
void tasks_close();  // fail queued tasks (shutdown)
json d3d_info();
// Run fn on the render thread at the next Present (before the original Present). Blocks up to timeout_ms.
// Throws std::runtime_error on timeout.
json run_on_main(std::function<json()> fn, unsigned timeout_ms);
json screenshot(int scale);  // must be called on the render thread
uint64_t current_frame();

// ---- patches.cpp ----
json keep_active(const json& params);  // {"enabled": bool}? -> state
bool keep_active_enabled();
json no_traffic(const json& params);  // {"enabled": bool}? -> state
void no_traffic_apply_env();         // NFSU2_NO_TRAFFIC=1 at startup

// ---- input.cpp ----
bool input_install_hooks();
json input_key(const json& params);  // {"key": DIK, "action": "press"|"hold"|"release", "ms": n}
json input_release_all();
json input_state();
json input_save_state();                 // devices + key->action maps, for the next core after a reload
void input_restore_state(const json& s);  // re-patch the devices from a previous core's state
void input_shutdown();                    // restore every vtable slot this core patched

// ---- car.cpp ----
struct CarSample {
    uint64_t frame = 0;
    int game_state = 0;
    float dt = 0;
    float pos[3]{}, forward[3]{}, up[3]{}, side[3]{}, vel[3]{}, angvel[3]{};
    int gear = 0;
    float engine_rad_s = 0;
    uint32_t watch[8]{};  // raw dwords of the trace's watch addresses
    uint8_t nwatch = 0;
    uint64_t fe_hash = 0;  // menu-engine fingerprint (trace with "fe": true)
    uint32_t fe_objects = 0;
    uint8_t fe_npk = 0;  // per package: name hash + fingerprint (first 8 active packages)
    uint32_t fe_pk_name[8]{};
    uint64_t fe_pk_hash[8]{};
};
bool car_read(CarSample& s);  // false if not in game / chain invalid
// ---- festate.cpp: menu-engine (FEng) fingerprint for A/B tests of menu rewrites (main thread)
bool fe_state_hash(uint64_t& hash, uint32_t& objects, uint32_t& packages, uint32_t* pk_names = nullptr,
                   uint64_t* pk_hashes = nullptr, uint32_t pk_max = 0);
json fe_state();
bool fe_fixed_dt(bool on, float dt);  // pin RealTimeElapsed (0x865198) per frame; main thread
json fixed_dt(const json& p);
json car_sample_json(const CarSample& s);
json car_state();

// ---- input.cpp: frame-scheduled input, recording, trace ----
void input_frame_tick(uint64_t frame);  // called at Present (render thread), before queued tasks
json input_schedule(const json& params);  // {"events":[{"f":n,"key":dik|"action":id,"down":bool}]}
json input_schedule_status();
json record_start(const json& params);    // record real keyboard actions (+ trace)
json record_stop();
json trace_start(const json& params);     // {"every": n frames}
json trace_stop();

// ---- rewrite.cpp (nfsu2_game.dll hook table) ----
void rewrite_init(const json& state);  // after MH_Initialize; state from the previous core (may be empty)
void rewrite_on_frame(uint64_t frame); // render thread: hot-reload the game DLL when its file changed
json rewrite_save_state();
void rewrite_shutdown();               // render thread: remove all rewrite hooks, unload the game DLL
json rewrite_list();
json rewrite_enable(const json& params);  // {"name"|"addr", "enabled": bool}
json rewrite_reload();
json diff_test(const json& params);
struct RewriteInfo {
    std::string name, system, signature;
    uintptr_t addr;
    bool enabled, hooked;
    uint32_t calls;  // calls of our rewrite since the game DLL was (re)loaded
};
std::vector<RewriteInfo> rewrite_snapshot();                                        // render thread
bool rewrite_set(const std::string& name, bool on);                                  // render thread
std::string rewrite_diff_now(const std::string& name, uint32_t iterations, uint32_t seed);  // render thread, JSON
std::string rewrite_build();

// ---- overlay.cpp (Dear ImGui panel, F8) ----
void overlay_init();
void overlay_frame();     // render thread, at Present after queued tasks
void overlay_shutdown();
bool overlay_wants_keyboard();
bool overlay_wants_mouse();  // {"name": test|"*"|"list", "iterations", "seed"}

// ---- server.cpp ----
void server_start(uint16_t port);
void server_stop();

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
