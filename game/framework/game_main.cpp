// nfsu2_game.dll: our rewrite of SPEED2.EXE, one function at a time. Loaded and hot-reloaded by the probe core.
#include <nlohmann/json.hpp>
#include <string>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace game {

std::vector<probe::GameRewrite>& registry() {
    static std::vector<probe::GameRewrite> r;
    return r;
}

std::vector<DiffTestReg>& diff_registry() {
    static std::vector<DiffTestReg> r;
    return r;
}

RegOut call_regs(const void* fn, const uint32_t* args, int nargs, bool returns_float) {
    uint32_t ra = 0, rc = 0, rd = 0;
    float f = 0.0f;
    const uint8_t rf = returns_float ? 1 : 0;
    __asm {
        mov ebx, fn
        mov esi, args
        mov edi, nargs
        mov ecx, edi
    push_loop:
        test ecx, ecx
        jz do_call
        dec ecx
        push dword ptr [esi + ecx*4]
        jmp push_loop
    do_call:
        mov eax, 0xA0A0A0A0
        mov ecx, 0xC0C0C0C0
        mov edx, 0xD0D0D0D0
        call ebx
        lea esp, [esp + edi*4]
        mov ra, eax
        mov rc, ecx
        mov rd, edx
        cmp rf, 0
        je no_float
        fstp f
    no_float:
    }
    return {ra, rc, rd, f};
}

namespace {

using json = nlohmann::json;
probe::GameApi g_api{};
std::string g_out;

json result_json(const DiffResult& r) {
    json j = {{"name", r.name}, {"iterations", r.iterations}, {"mismatches", r.mismatches}, {"examples", r.examples}};
    if (!r.error.empty()) j["error"] = r.error;
    return j;
}

const char* diff_list() {
    json a = json::array();
    for (auto& t : diff_registry()) a.push_back(t.name);
    g_out = a.dump();
    return g_out.c_str();
}

const char* diff(const char* name, uint32_t iterations, uint32_t seed) {
    const DiffCtx ctx{iterations, seed};
    json out;
    try {
        if (std::string(name) == "*") {
            out = json::array();
            for (auto& t : diff_registry()) out.push_back(result_json(t.fn(ctx)));
        } else {
            out = json{{"error", std::string("no diff test ") + name}};
            for (auto& t : diff_registry())
                if (std::string(t.name) == name) out = result_json(t.fn(ctx));
        }
    } catch (const std::exception& e) {
        out = json{{"error", e.what()}};
    }
    g_out = out.dump(-1, ' ', false, json::error_handler_t::replace);
    return g_out.c_str();
}

}  // namespace
}  // namespace game

extern "C" __declspec(dllexport) const probe::GameApi* nfsu2_game_api() {
    auto& r = game::registry();
    game::g_api = {probe::kGameApiVersion, __DATE__ " " __TIME__, static_cast<uint32_t>(r.size()), r.data(),
                   &game::diff_list, &game::diff};
    return &game::g_api;
}
