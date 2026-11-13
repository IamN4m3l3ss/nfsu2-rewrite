// Probe-side behaviour changes needed for automation (not part of the rewrite).
//
// keep_active: the game stops running when its window loses focus.
//   WndProc 0x005CCD60 (__stdcall): WM_ACTIVATEAPP stores wParam into g_AppActive (0x00870988);
//   WM_SIZE stores !IsIconic(hwnd) there; WM_ACTIVATE tracks inactive state in 0x008709E0.
//   Main loop 0x005BF212: g_AppActive ? PeekMessage (non-blocking) : GetMessage (blocks -> no frames).
//   Input poll 0x005CFDAB: devices are only polled while g_AppActive.
// With keep_active on, focus-loss messages are rewritten to "active" and g_AppActive is forced to 1.
#include "probe.h"

#include <atomic>
#include <cstdlib>

#include "MinHook.h"

namespace probe {
namespace {

constexpr uintptr_t kWndProc = 0x005CCD60;
constexpr uintptr_t kAppActive = 0x00870988;

using WndProc_t = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
WndProc_t o_WndProc = nullptr;
std::atomic<bool> g_keep_active{true};

LRESULT CALLBACK hk_WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_keep_active.load(std::memory_order_relaxed)) {
        if (msg == WM_ACTIVATEAPP && !wp) wp = TRUE;
        if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) wp = MAKEWPARAM(WA_ACTIVE, HIWORD(wp));
    }
    LRESULT r = o_WndProc(wnd, msg, wp, lp);
    if (g_keep_active.load(std::memory_order_relaxed)) *reinterpret_cast<volatile int*>(kAppActive) = 1;
    return r;
}

}  // namespace

bool patches_install() {
    if (const char* env = std::getenv("NFSU2_KEEP_ACTIVE")) g_keep_active = std::atoi(env) != 0;
    MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(kWndProc), reinterpret_cast<void*>(&hk_WndProc),
                                reinterpret_cast<void**>(&o_WndProc));
    if (s == MH_OK) s = MH_EnableHook(reinterpret_cast<void*>(kWndProc));
    if (s != MH_OK) {
        logf("ERROR: hook WndProc %s: %s", hex_addr(kWndProc).c_str(), MH_StatusToString(s));
        return false;
    }
    logf("WndProc hooked, keep_active=%d", g_keep_active.load());
    return true;
}

json keep_active(const json& p) {
    if (p.contains("enabled")) {
        g_keep_active = p["enabled"].get<bool>();
        if (g_keep_active) *reinterpret_cast<volatile int*>(kAppActive) = 1;
        logf("keep_active=%d", g_keep_active.load());
    }
    int flag = 0;
    mem_read(kAppActive, &flag, sizeof flag);
    return {{"enabled", g_keep_active.load()}, {"g_AppActive", flag}};
}

}  // namespace probe
