// In-game panel (Dear ImGui 1.93.0, D3D9 + Win32 backends). F8 shows/hides it.
//
// Lives in the core, so it hot-reloads with it. The loader forwards window messages (MsgHook) and device resets
// (ResetHook); both are cleared on shutdown before the core is unloaded. Everything here runs on the render thread
// (Present / the game's WndProc, which are the same thread), so it calls the rewrite manager directly.
#include "probe.h"

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace probe {
namespace {

bool g_ready = false;    // ImGui context + backends initialised
bool g_visible = false;  // panel shown (F8)
ULONGLONG g_hint_until = 0;
HWND g_hwnd = nullptr;
char g_filter[64] = "";
struct TestResult {
    bool pass;
    std::string text, detail;
};
std::map<std::string, TestResult> g_results;
std::string g_last_all;
// fps over the last second
ULONGLONG g_fps_t0 = 0;
uint64_t g_fps_f0 = 0;
float g_fps = 0;

bool is_mouse_msg(UINT m) { return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST); }
bool is_key_msg(UINT m) { return (m >= WM_KEYFIRST && m <= WM_KEYLAST); }

int msg_hook(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
    if (!g_ready) return 0;
    if (msg == WM_KEYDOWN && wp == VK_F8 && !(lp & (1 << 30))) {  // F8, not auto-repeat
        g_visible = !g_visible;
        ImGui::GetIO().MouseDrawCursor = g_visible;
        return 1;
    }
    if (msg == WM_KEYUP && wp == VK_F8) return 1;
    if (!g_visible) return 0;
    ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), msg, wp, lp);
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse && is_mouse_msg(msg)) return 1;
    if (io.WantCaptureKeyboard && (is_key_msg(msg) || msg == WM_CHAR)) return 1;
    return 0;
}

void reset_hook(int phase) {
    if (!g_ready) return;
    if (phase == 0)
        ImGui_ImplDX9_InvalidateDeviceObjects();
    else
        ImGui_ImplDX9_CreateDeviceObjects();
}

bool try_init() {
    IDirect3DDevice9* dev = api().device();
    g_hwnd = static_cast<HWND>(api().window());
    if (!dev || !g_hwnd) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini in the game folder
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    const float scale = std::max(1.0f, static_cast<float>(rc.bottom - rc.top) / 1080.0f) * 1.15f;
    st.ScaleAllSizes(scale);
    st.FontScaleMain = scale;
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    st.Colors[ImGuiCol_WindowBg].w = 0.93f;
    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX9_Init(dev)) {
        logf("overlay: backend init failed");
        ImGui::DestroyContext();
        return false;
    }
    api().set_msg_hook(&msg_hook);
    api().set_reset_hook(&reset_hook);
    g_ready = true;
    g_hint_until = GetTickCount64() + 10000;
    logf("overlay: ready (ImGui %s, scale %.2f) - F8 toggles the panel", ImGui::GetVersion(), scale);
    return true;
}

ImVec4 kGreen{0.30f, 0.85f, 0.55f, 1}, kRed{0.95f, 0.35f, 0.35f, 1}, kGrey{0.6f, 0.6f, 0.65f, 1},
    kYellow{0.98f, 0.78f, 0.25f, 1};

void run_test(const std::string& name) {
    try {
        json r = json::parse(rewrite_diff_now(name, 5000, 12345));
        if (r.contains("error") && !r.contains("mismatches")) {
            g_results[name] = {false, "no test", r["error"].get<std::string>()};
            return;
        }
        const uint32_t mm = r.value("mismatches", 0u), it = r.value("iterations", 0u);
        const bool broken = name.size() > 7 && name.substr(name.size() - 7) == "_BROKEN";
        const bool pass = broken ? mm > 0 : (mm == 0 && !r.contains("error"));
        std::string detail;
        if (r.contains("examples") && !r["examples"].empty()) detail = r["examples"][0].get<std::string>();
        g_results[name] = {pass, (pass ? "PASS " : "FAIL ") + std::to_string(mm) + "/" + std::to_string(it), detail};
    } catch (const std::exception& e) {
        g_results[name] = {false, "error", e.what()};
    }
}

void run_all_tests() {
    int pass = 0, fail = 0;
    try {
        json all = json::parse(rewrite_diff_now("*", 5000, 12345));
        for (const json& r : all) {
            const std::string n = r.value("name", std::string("?"));
            const uint32_t mm = r.value("mismatches", 0u), it = r.value("iterations", 0u);
            const bool broken = n.size() > 7 && n.substr(n.size() - 7) == "_BROKEN";
            const bool ok = broken ? mm > 0 : (mm == 0 && !r.contains("error"));
            std::string detail;
            if (r.contains("examples") && !r["examples"].empty()) detail = r["examples"][0].get<std::string>();
            g_results[n] = {ok, (ok ? "PASS " : "FAIL ") + std::to_string(mm) + "/" + std::to_string(it), detail};
            (ok ? pass : fail)++;
        }
    } catch (const std::exception& e) {
        g_last_all = std::string("error: ") + e.what();
        return;
    }
    g_last_all = std::to_string(pass) + " passed, " + std::to_string(fail) + " failed";
}

void tab_rewrites() {
    auto list = rewrite_snapshot();
    int on = 0;
    for (auto& r : list) on += r.enabled;
    ImGui::Text("Our code is running for %d of %d rewritten functions.", on, static_cast<int>(list.size()));
    ImGui::TextColored(kGrey, "Untick a function to put the game's original code back. Changes apply instantly.");
    if (ImGui::Button("All on"))
        for (auto& r : list)
            if (!r.enabled && r.name != "UpdateFrameTime") rewrite_set(r.name, true);
    ImGui::SameLine();
    if (ImGui::Button("All off (original game)"))
        for (auto& r : list)
            if (r.enabled) rewrite_set(r.name, false);
    ImGui::SameLine();
    if (ImGui::Button("Run all tests")) run_all_tests();
    if (!g_last_all.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(g_last_all.find(" 0 failed") != std::string::npos ? kGreen : kRed, "%s", g_last_all.c_str());
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##filter", "filter by name...", g_filter, sizeof g_filter);
    ImGui::Separator();

    std::map<std::string, std::vector<RewriteInfo>> by;
    for (auto& r : list) by[r.system].push_back(r);
    static const std::map<std::string, std::string> label = {
        {"bstring", "Strings"}, {"bmath", "Maths"}, {"bchunk", "File & chunk loading"}, {"time", "Frame time"}};
    for (auto& [sys, rows] : by) {
        int son = 0;
        for (auto& r : rows) son += r.enabled;
        const std::string title = (label.count(sys) ? label.at(sys) : sys) + "  (" + std::to_string(son) + "/" +
                                  std::to_string(rows.size()) + " on)###" + sys;
        if (!ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
        if (ImGui::BeginTable(sys.c_str(), 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Function");
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Diff test");
            for (auto& r : rows) {
                if (g_filter[0] && r.name.find(g_filter) == std::string::npos) continue;
                ImGui::TableNextRow();
                ImGui::PushID(r.name.c_str());
                ImGui::TableNextColumn();
                bool en = r.enabled;
                if (ImGui::Checkbox("##on", &en)) rewrite_set(r.name, en);
                ImGui::TableNextColumn();
                ImGui::TextColored(r.enabled ? kGreen : kGrey, "%s", r.name.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.signature.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(kGrey, "0x%08X", static_cast<unsigned>(r.addr));
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("test")) run_test(r.name);
                auto it = g_results.find(r.name);
                if (it != g_results.end()) {
                    ImGui::SameLine();
                    ImGui::TextColored(it->second.pass ? kGreen : kRed, "%s", it->second.text.c_str());
                    if (!it->second.detail.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", it->second.detail.c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}

const char* state_name(int s) {
    switch (s) {
        case 3: return "frontend (menus)";
        case 4: case 5: return "loading";
        case 6: return "in game";
        default: return "?";
    }
}

void tab_car() {
    CarSample cs;
    cs.frame = current_frame();
    if (!car_read(cs)) {
        ImGui::TextColored(kGrey, "No player car right now (menus or loading).");
        return;
    }
    const float speed = std::sqrt(cs.vel[0] * cs.vel[0] + cs.vel[1] * cs.vel[1] + cs.vel[2] * cs.vel[2]);
    const float rpm = cs.engine_rad_s * 60.0f / 6.28318531f;
    ImGui::Text("Speed");
    ImGui::SameLine(ImGui::GetFontSize() * 7);
    ImGui::Text("%.1f km/h", speed * 3.6f);
    ImGui::Text("Gear");
    ImGui::SameLine(ImGui::GetFontSize() * 7);
    if (cs.gear == 0) ImGui::Text("R");
    else if (cs.gear == 1) ImGui::Text("N");
    else ImGui::Text("%d", cs.gear - 1);
    ImGui::Text("RPM");
    ImGui::SameLine(ImGui::GetFontSize() * 7);
    char buf[32];
    snprintf(buf, sizeof buf, "%.0f", rpm);
    ImGui::ProgressBar(std::clamp(rpm / 9000.0f, 0.0f, 1.0f), ImVec2(ImGui::GetFontSize() * 16, 0), buf);
    ImGui::Separator();
    ImGui::Text("Position   %10.2f %10.2f %8.2f", cs.pos[0], cs.pos[1], cs.pos[2]);
    ImGui::Text("Velocity   %10.2f %10.2f %8.2f m/s", cs.vel[0], cs.vel[1], cs.vel[2]);
    ImGui::Text("Facing     %10.3f %10.3f %8.3f", cs.forward[0], cs.forward[1], cs.forward[2]);
    ImGui::Text("Game state %d (%s)", cs.game_state, state_name(cs.game_state));
    if (ImGui::Button("Copy position to clipboard")) {
        snprintf(buf, sizeof buf, "%.2f, %.2f, %.2f", cs.pos[0], cs.pos[1], cs.pos[2]);
        ImGui::SetClipboardText(buf);
    }
}

void tab_probe() {
    const LoaderApi& a = api();
    ImGui::Text("Loader %s   core %s (build %s)   reloads %u", a.loader_version, kVersion, core_build(), a.reload_count());
    ImGui::Text("Rewrite DLL build %s", rewrite_build().c_str());
    ImGui::Text("Frame %llu   %.0f fps", static_cast<unsigned long long>(a.frames()), g_fps);
    unsigned short cw = 0;
    __asm { fnstcw cw }
    ImGui::Text("x87 control word 0x%04X (%s precision)", cw,
                ((cw >> 8) & 3) == 0 ? "24-bit" : ((cw >> 8) & 3) == 2 ? "53-bit" : "64-bit");
    bool keep = a.keep_active_get();
    if (ImGui::Checkbox("Keep running when unfocused", &keep)) a.keep_active_set(keep);
    ImGui::Separator();
    if (ImGui::Button("Reload probe core")) a.request_reload();
    ImGui::SameLine();
    ImGui::TextColored(kGrey, "(also happens automatically after a rebuild)");
}

void tab_log() {
    json t;
    try {
        t = json::parse(api().log_tail_json(300, 0));
    } catch (...) {
        return;
    }
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (const json& l : t["lines"]) {
        const std::string s = l["text"].get<std::string>();
        const bool err = s.find("ERROR") != std::string::npos || s.find("exception") != std::string::npos;
        if (err)
            ImGui::TextColored(kRed, "%s", s.c_str());
        else
            ImGui::TextUnformatted(s.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void draw() {
    if (!g_visible) {
        ImGui::SetNextWindowPos(ImVec2(20, 20));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextColored(kYellow, "NFSU2 rewrite loaded - press F8 for the panel");
        ImGui::End();
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 48, ImGui::GetFontSize() * 36), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("NFSU2 rewrite  (F8 to hide)", &g_visible)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Rewrites")) {
                tab_rewrites();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Car")) {
                tab_car();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Probe")) {
                tab_probe();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Log")) {
                tab_log();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    ImGui::GetIO().MouseDrawCursor = g_visible;
}

}  // namespace

void overlay_init() {}

void overlay_frame() {
    // fps
    const ULONGLONG now = GetTickCount64();
    if (now - g_fps_t0 >= 1000) {
        const uint64_t f = api().frames();
        if (g_fps_t0) g_fps = static_cast<float>(f - g_fps_f0) * 1000.0f / static_cast<float>(now - g_fps_t0);
        g_fps_t0 = now;
        g_fps_f0 = f;
    }
    if (!g_ready && !try_init()) return;
    if (!g_visible && now > g_hint_until) return;  // nothing to draw: skip ImGui entirely
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw();
    ImGui::EndFrame();
    ImGui::Render();
    IDirect3DDevice9* dev = api().device();
    if (SUCCEEDED(dev->BeginScene())) {
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        dev->EndScene();
    }
}

void overlay_shutdown() {
    if (!g_ready) return;
    api().set_msg_hook(nullptr);
    api().set_reset_hook(nullptr);
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_ready = false;
}

bool overlay_wants_keyboard() { return g_ready && g_visible && ImGui::GetIO().WantCaptureKeyboard; }
bool overlay_wants_mouse() { return g_ready && g_visible && ImGui::GetIO().WantCaptureMouse; }

}  // namespace probe
