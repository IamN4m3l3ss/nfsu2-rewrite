// Interface between the stable loader (nfsu2_probe.asi) and the hot-reloadable core (nfsu2_core.dll).
// Plain C types only: the two DLLs each link their own static CRT, so nothing allocated in one may be freed in the
// other, and no C++ objects cross the boundary. Strings returned by a function stay valid until the next call of
// that function on the same thread.
#pragma once
#include <cstdint>

struct IDirect3DDevice9;

namespace probe {

constexpr uint32_t kApiVersion = 2;

// v2: window-message and device-reset hooks for the in-game overlay (core registers them, clears them on shutdown)
using MsgHook = int (*)(void* hwnd, uint32_t msg, uintptr_t wparam, intptr_t lparam);  // nonzero = swallow
using ResetHook = void (*)(int phase);  // 0 = before IDirect3DDevice9::Reset, 1 = after (success or not)

struct LoaderApi {
    uint32_t version;
    const char* loader_version;
    // exe identity (checked by the loader before anything else)
    uintptr_t exe_base;
    uint32_t exe_image_size;
    const char* exe_sha256;
    bool exe_hash_ok;
    uint32_t init_thread;
    // log (owned by the loader so it survives reloads); thread-safe
    void (*log)(const char* message);
    const char* (*log_tail_json)(uint32_t n, uint64_t since);  // {"last_seq":..,"lines":[{"seq","text"}]}
    // render state
    uint64_t (*frames)();
    IDirect3DDevice9* (*device)();
    void* (*window)();
    uint32_t (*create_thread)();
    uint32_t (*present_thread)();
    // keep_active (WndProc hook lives in the loader)
    bool (*keep_active_get)();
    void (*keep_active_set)(bool enabled);
    int (*app_active)();
    // reload
    void (*request_reload)();
    uint32_t (*reload_count)();  // 0 = first load
    // v2
    void (*set_msg_hook)(MsgHook hook);      // called from the game's WndProc (main thread); nullptr to clear
    void (*set_reset_hook)(ResetHook hook);  // called around IDirect3DDevice9::Reset (render thread)
};

struct CoreApi {
    uint32_t version;
    const char* build;  // core build stamp
    // Called on the render thread (or from DllMain for the first load, before the game created any device).
    // state_json: empty on first load, else what the previous core's shutdown() returned.
    bool (*init)(const LoaderApi* api, const char* state_json);
    // Render thread, at Present before the original Present.
    void (*on_frame)(uint64_t frame);
    // Render thread. Stop threads, undo every hook/patch, return state JSON for the next core.
    const char* (*shutdown)();
};

}  // namespace probe

extern "C" {
using nfsu2_core_api_fn = const probe::CoreApi* (*)();
}
