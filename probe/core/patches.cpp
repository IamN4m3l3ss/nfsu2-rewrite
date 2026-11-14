// Probe-side behaviour changes needed for automation (not part of the rewrite).
//
// keep_active lives in the loader (its WndProc hook must survive core reloads); this is the RPC front-end.
#include "probe.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace probe {

bool keep_active_enabled() { return api().keep_active_get(); }

json keep_active(const json& p) {
    if (p.contains("enabled")) {
        api().keep_active_set(p["enabled"].get<bool>());
        logf("keep_active=%d", api().keep_active_get());
    }
    return {{"enabled", api().keep_active_get()}, {"g_AppActive", api().app_active()}};
}

// no_traffic (EXPERIMENTAL, DOES NOT WORK): patches 0x0042F910 (a __thiscall(1 arg) update, only caller 0x00609ED7)
// to `ret 4`, the same patch ZMenu's "No Traffic" makes (ZMenuU2.asi 0x10057DA0). Verified 2026-11-14 with the user:
// traffic and roaming racers still drive normally with it on, so 0x42F910 is NOT the traffic system and the effect
// of skipping it is unknown. Not used by any tool by default; kept only so the finding is reproducible.
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
