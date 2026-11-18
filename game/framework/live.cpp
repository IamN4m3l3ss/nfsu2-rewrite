// Live check engine (see live.h). Called by the probe core's live hook on real calls, from any game thread.
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "live.h"

#include "../../probe/common/game_api.h"

namespace game::live {

std::vector<Spec>& specs() {
    static std::vector<Spec> s;
    return s;
}

namespace {
struct Stat {
    uint32_t checked = 0, mismatches = 0, skipped = 0;
    std::vector<std::string> examples;
};
std::mutex g_mu;
std::vector<Stat> g_stats;
thread_local bool g_inside = false;  // a check's own calls never start another check
std::string g_out;

bool safe_copy(void* dst, uint32_t src, size_t n) {
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER; no windows.h: its CDECL/THIS/max macros clash with leaf.h */) {
        return false;
    }
}

void check(const Spec& sp, Stat& st, const uint32_t* regs, void* tramp) {
    using namespace leaf;
    const uint32_t n = static_cast<uint32_t>(sp.arg_size.size());
    const uint32_t self = regs[7];
    std::vector<std::vector<uint8_t>> snap(1 + n);
    snap[0].resize(sp.this_size);
    if (sp.this_size && !safe_copy(snap[0].data(), self, sp.this_size)) {
        ++st.skipped;
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        snap[1 + i].resize(sp.arg_size[i]);
        if (sp.arg_size[i] && !safe_copy(snap[1 + i].data(), regs[11 + i], sp.arg_size[i])) {
            ++st.skipped;
            return;
        }
    }
    leaf::Spec gs{static_cast<int>(1 + n), 0x10, {}, {}};
    for (auto& s : snap) gs.size = std::max<uint32_t>(gs.size, static_cast<uint32_t>((s.size() + 0xF) & ~0xFu));
    Graph g[2];
    for (Graph& gr : g) {
        gr.sp = &gs;
        gr.raw.assign(gs.blocks, {});
        gr.blk.assign(gs.blocks, {});
        for (int k = 0; k < gs.blocks; ++k) {
            gr.raw[k].assign(gs.size + 2 * kGuard, 0);
            gr.blk[k] = {gr.raw[k].data() + kGuard, gs.size};
            memcpy(gr.blk[k].data(), snap[k].data(), snap[k].size());
        }
    }
    uint32_t a0[16] = {}, a1[16] = {};
    for (uint32_t i = 0; i < n && i < 16; ++i) {
        a0[i] = sp.arg_size[i] ? u(g[0].blk[1 + i].data()) : regs[11 + i];
        a1[i] = sp.arg_size[i] ? u(g[1].blk[1 + i].data()) : regs[11 + i];
    }
    const Fn& f = sp.fn;
    const void* s0 = f.conv == THIS ? (sp.this_size ? g[0].blk[0].data() : reinterpret_cast<void*>(self)) : nullptr;
    const void* s1 = f.conv == THIS ? (sp.this_size ? g[1].blk[0].data() : reinterpret_cast<void*>(self)) : nullptr;
    const RegOutAny x = call_any(tramp, s0, a0, static_cast<int>(n), f.ret == FLT);
    const RegOutAny y = call_any(f.mine, s1, a1, static_cast<int>(n), f.ret == FLT);
    std::string bad;
    if (f.ret == FLT ? !same_float(x.st0, y.st0)
                     : (x.eax != y.eax && g[0].rel(x.eax) != g[1].rel(y.eax) &&
                        !(f.ret == LOW8 && (x.eax & 0xFF) == (y.eax & 0xFF)) &&
                        !(f.ret == LOW16 && (x.eax & 0xFFFF) == (y.eax & 0xFFFF)))) {
        char t[120];
        if (f.ret == FLT) snprintf(t, sizeof t, " st0 %g/%g", x.st0, y.st0);
        else snprintf(t, sizeof t, " eax %08X/%08X", static_cast<unsigned>(x.eax), static_cast<unsigned>(y.eax));
        bad += t;
    }
    if (x.popped != y.popped) bad += " pops";
    bad += g[0].diff(g[1]);
    ++st.checked;
    if (!bad.empty()) {
        ++st.mismatches;
        if (st.examples.size() < 3) st.examples.push_back(bad);
    }
}
}  // namespace

const char* live_list() {
    std::lock_guard lk(g_mu);
    g_stats.resize(specs().size());
    nlohmann::json a = nlohmann::json::array();
    for (size_t i = 0; i < specs().size(); ++i) {
        const Stat& st = g_stats[i];
        a.push_back({{"name", specs()[i].fn.name}, {"addr", specs()[i].addr}, {"checked", st.checked},
                     {"mismatches", st.mismatches}, {"skipped", st.skipped}, {"examples", st.examples}});
    }
    g_out = a.dump();
    return g_out.c_str();
}

void live_check(uintptr_t addr, const uint32_t* regs, void* tramp) {
    if (g_inside) return;
    g_inside = true;
    {
        std::lock_guard lk(g_mu);
        g_stats.resize(specs().size());
        for (size_t i = 0; i < specs().size(); ++i)
            if (specs()[i].addr == addr) {
                check(specs()[i], g_stats[i], regs, tramp);
                break;
            }
    }
    g_inside = false;
}

void live_reset() {
    std::lock_guard lk(g_mu);
    g_stats.assign(specs().size(), {});
}

}  // namespace game::live
