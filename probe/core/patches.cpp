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

// Traffic density for test drives (probe layer, never inside a rewrite). The game's own setting: global 0x89E7A8
// (eTrafficDensity, 0 = none .. 3; quick race sets it from its "Traffic Level" option). Free roam hardcodes 3:
// `mov eax, 3` in RaceStarter::StartCareerFreeRoam (0x540565) and ::StartCareerFreeRoamFromInGame (0x5407D0), then
// RaceStarter::AddTrafficCars creates no cars at 0 and TrafficTeleporter::DoTimestep spawns none. We patch those two
// immediates (and the live global) to the wanted density; the next free roam load uses it.
// (Replaces the 2026-11-14 experiment that turned TrafficTeleporter::DoTimestep into `ret 4`, ZMenu's patch: it
// skipped that whole update, so existing cars and roaming racers still drove.)
namespace {
constexpr uintptr_t kDensity = 0x0089E7A8;
constexpr uintptr_t kImm[2] = {0x00540566, 0x005407D1};  // imm32 of `mov eax, 3` (B8 03 00 00 00)
}  // namespace

json no_traffic(const json& p) {
    uint32_t cur[2];
    for (int k = 0; k < 2; ++k) {
        uint8_t op;
        if (!mem_read(kImm[k] - 1, &op, 1) || op != 0xB8 || !mem_read(kImm[k], &cur[k], 4) || cur[k] > 3)
            throw std::runtime_error("unexpected code at the free-roam traffic density (another mod?)");
    }
    int want = -1;
    if (p.contains("density")) want = p["density"].get<int>();
    else if (p.contains("enabled")) want = p["enabled"].get<bool>() ? 0 : 3;
    if (want >= 0) {
        if (want > 3) throw std::runtime_error("density 0..3");
        const uint32_t v = static_cast<uint32_t>(want);
        for (uintptr_t a : kImm)
            if (!mem_write(a, &v, 4)) throw std::runtime_error("write failed");
        mem_write(kDensity, &v, 4);  // stops new spawns now; cars already on the road stay until the next load
        logf("traffic density %d (free roam)", want);
        cur[0] = v;
    }
    uint32_t live = 0;
    mem_read(kDensity, &live, 4);
    return {{"free_roam_density", cur[0]}, {"live_density", live}, {"enabled", cur[0] == 0}};
}

void no_traffic_apply_env() {
    const char* d = std::getenv("NFSU2_TRAFFIC");
    const char* n = std::getenv("NFSU2_NO_TRAFFIC");
    try {
        if (d && *d) no_traffic({{"density", std::atoi(d)}});
        else if (n && std::atoi(n)) no_traffic({{"density", 0}});
    } catch (const std::exception& e) {
        logf("ERROR: traffic: %s", e.what());
    }
}

}  // namespace probe
