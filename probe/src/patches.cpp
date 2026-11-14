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
#include <cstring>
#include <stdexcept>

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

bool keep_active_enabled() { return g_keep_active.load(std::memory_order_relaxed); }

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

// no_traffic: 0x0042F910 is a __thiscall(1 arg) per-frame traffic update (only caller 0x00609ED7). Patching its
// first bytes to `ret 4` stops traffic, the same patch ZMenu's "No Traffic" uses (found in ZMenuU2.asi at
// 0x10057DA0). Used to make long drives repeatable (traffic collisions amplify any drift).
namespace {
constexpr uintptr_t kTrafficUpdate = 0x0042F910;
const uint8_t kTrafficOrig[4] = {0x55, 0x8B, 0xEC, 0x83};  // push ebp; mov ebp, esp; sub esp, ..
const uint8_t kTrafficOff[4] = {0xC2, 0x04, 0x00, 0x83};   // ret 4
}  // namespace

json no_traffic(const json& p) {
    uint8_t cur[4];
    if (!mem_read(kTrafficUpdate, cur, 4)) throw std::runtime_error("cannot read traffic update");
    const bool on = memcmp(cur, kTrafficOff, 4) == 0;
    if (!on && memcmp(cur, kTrafficOrig, 4) != 0)
        throw std::runtime_error("unexpected bytes at traffic update (another mod patched it?): " + to_hex(cur, 4));
    if (p.contains("enabled") && p["enabled"].get<bool>() != on) {
        const bool want = p["enabled"].get<bool>();
        if (!mem_write(kTrafficUpdate, want ? kTrafficOff : kTrafficOrig, 4))
            throw std::runtime_error("write failed");
        logf("no_traffic=%d", want);
        return {{"enabled", want}};
    }
    return {{"enabled", on}};
}

void no_traffic_apply_env() {
    if (const char* env = std::getenv("NFSU2_NO_TRAFFIC"); env && std::atoi(env)) {
        try {
            no_traffic({{"enabled", true}});
        } catch (const std::exception& e) {
            logf("ERROR: no_traffic: %s", e.what());
        }
    }
}

}  // namespace probe
