// Camera recorder for in-game A/B tests: while on, every frame copies the camera of one view (view table 0x832DE0,
// 0x70 bytes per view, camera pointer at +0x40) - its view matrix (+0x00, 16 floats) and eye position (+0x40) -
// into a buffer. The A/B tool compares two recordings frame by frame.
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "probe.h"

namespace probe {
namespace {
constexpr uintptr_t kViews = 0x00832DE0;
constexpr size_t kMaxFrames = 20000;
bool g_on = false;
int g_view = 1;
std::vector<std::array<uint32_t, 22>> g_frames;  // 16 matrix + 3 position + frame + world time + real ticks
}  // namespace

void camrec_frame(uint64_t frame) {  // render thread, after queued tasks
    if (!g_on || g_frames.size() >= kMaxFrames) return;
    const uint32_t cam = *reinterpret_cast<const uint32_t*>(kViews + 0x70 * g_view + 0x40);
    std::array<uint32_t, 22> f{};
    if (cam) std::memcpy(f.data(), reinterpret_cast<const void*>(cam), 19 * 4);
    f[19] = static_cast<uint32_t>(frame);
    f[20] = *reinterpret_cast<const uint32_t*>(0x0086518Cu);  // world time (ticks, 4000/s)
    f[21] = *reinterpret_cast<const uint32_t*>(0x008651ACu);  // real-time ticks
    g_frames.push_back(f);
}

json cam_record(const json& p) {
    const std::string action = p.value("action", std::string("start"));
    const int view = p.value("view", 1);
    return run_on_main(
        [action, view] {
            if (action == "start") {
                g_frames.clear();
                g_view = view;
                g_on = true;
                return json{{"ok", true}, {"view", view}};
            }
            g_on = false;
            json frames = json::array();
            for (const auto& f : g_frames) {
                char hex[22 * 8 + 1];
                for (int i = 0; i < 22; ++i) snprintf(hex + i * 8, 9, "%08X", f[i]);
                frames.push_back(hex);
            }
            return json{{"ok", true}, {"count", g_frames.size()}, {"frames", frames}};
        },
        5000);
}

}  // namespace probe
