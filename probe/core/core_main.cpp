// nfsu2_core.dll entry points. Loaded (and hot-reloaded) by the loader, nfsu2_probe.asi; see probe/common/api.h.
#include "probe.h"

#include <cstdio>
#include <cstdlib>

#include <tlhelp32.h>
#include <stdexcept>

#include "MinHook.h"

namespace probe {

// Every loaded module's address range: crash addresses in a DLL (ours, a mod, Wine) can be attributed afterwards.
json modules(bool log) {
    json arr = json::array();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return arr;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        char name[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, name, sizeof name, nullptr, nullptr);
        const auto base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
        if (log) logf("module %08x-%08x %s", unsigned(base), unsigned(base + me.modBaseSize), name);
        arr.push_back({{"name", name}, {"base", base}, {"size", me.modBaseSize}});
    }
    CloseHandle(snap);
    return arr;
}

namespace {

const LoaderApi* g_api = nullptr;
ExeInfo g_exe;
std::string g_state_out;
bool g_hooks = false;
const char* kBuild = __DATE__ " " __TIME__;

bool core_init(const LoaderApi* a, const char* state_json) {
    if (!a || a->version != kApiVersion) return false;
    g_api = a;
    g_exe.base = a->exe_base;
    g_exe.image_size = a->exe_image_size;
    g_exe.sha256 = a->exe_sha256;
    g_exe.hash_ok = a->exe_hash_ok;
    g_exe.init_thread = a->init_thread;
    const bool reload = state_json && *state_json;
    logf("core %s (build %s) init, %s", kVersion, kBuild, reload ? "reload" : "first load");

    uint16_t port = kDefaultPort;
    if (const char* env = std::getenv("NFSU2_PROBE_PORT")) port = static_cast<uint16_t>(std::atoi(env));
    if (g_exe.hash_ok) {
        MH_STATUS s = MH_Initialize();
        if (s != MH_OK) {
            logf("ERROR: MH_Initialize: %s", MH_StatusToString(s));
        } else {
            g_hooks = true;
            input_install_hooks();  // DirectInput8Create: only matters on first load (game start)
            json st = json::object();
            if (reload) {
                try {
                    st = json::parse(state_json);
                    input_restore_state(st);
                } catch (const std::exception& e) {
                    logf("ERROR: restoring state: %s", e.what());
                }
            } else {
                no_traffic_apply_env();
            }
            rewrite_init(st);
            modules(true);
        }
    }
    server_start(port);
    return true;
}

void core_on_frame(uint64_t frame) {
    if (g_hooks) rewrite_on_frame(frame);
    input_frame_tick(frame);
    run_tasks();
    overlay_frame();
}

const char* core_shutdown() {
    logf("core shutdown");
    overlay_shutdown();
    server_stop();
    tasks_close();
    json state = json::object();
    if (g_hooks) {
        state["input"] = input_save_state();
        state["rewrite"] = rewrite_save_state();
        rewrite_shutdown();
        input_shutdown();
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_hooks = false;
    }
    g_state_out = state.dump();
    g_api = nullptr;
    return g_state_out.c_str();
}

const CoreApi g_core_api{kApiVersion, kBuild, &core_init, &core_on_frame, &core_shutdown};

}  // namespace

const LoaderApi& api() { return *g_api; }
bool has_api() { return g_api != nullptr; }
const char* core_build() { return kBuild; }
const ExeInfo& exe() { return g_exe; }

std::string hex_addr(uintptr_t a) {
    char b[16];
    snprintf(b, sizeof b, "0x%08x", static_cast<unsigned>(a));
    return b;
}

std::string to_hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = d[p[i] >> 4];
        s[i * 2 + 1] = d[p[i] & 15];
    }
    return s;
}

std::vector<uint8_t> from_hex(const std::string& in) {
    std::string s;
    for (char c : in)
        if (!isspace(static_cast<unsigned char>(c))) s += c;
    if (s.size() % 2) throw std::runtime_error("odd number of hex digits");
    std::vector<uint8_t> out(s.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(std::stoul(s.substr(i * 2, 2), nullptr, 16));
    return out;
}

std::string base64(const uint8_t* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i < n) {
        uint32_t v = p[i] << 16;
        if (i + 1 < n) v |= p[i + 1] << 8;
        o += t[v >> 18]; o += t[(v >> 12) & 63];
        o += (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        o += '=';
    }
    return o;
}

}  // namespace probe

extern "C" __declspec(dllexport) const probe::CoreApi* nfsu2_core_api() { return &probe::g_core_api; }
