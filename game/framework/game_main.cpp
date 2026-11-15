#include <windows.h>
#include <cstdio>
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

extern "C" volatile long nfsu2_inflight = 0;

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

RegOutThis call_regs_this(const void* fn, const void* self, const uint32_t* args, int nargs) {
    uint32_t ra = 0, rd = 0, before = 0, after = 0;
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
        lea eax, [esp + edi*4]
        mov before, eax
        mov ecx, self
        mov eax, 0xA0A0A0A0
        mov edx, 0xD0D0D0D0
        call ebx
        mov ra, eax
        mov rd, edx
        mov after, esp
        mov esp, before
    }
    return {ra, rd, after - (before - static_cast<uint32_t>(nargs) * 4)};
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

// A crash inside a diff test must not take the probe core down (its frame handler disables the core): run each
// test under SEH and report the exception as the test's error. No /EHa, so C++ objects in the crashed frames are
// not destroyed (leaks are acceptable for a failed test). The __try lives in a function without C++ objects.
struct SehJob {
    DiffTestFn fn;
    const DiffCtx* ctx;
    DiffResult* out;
};
void seh_thunk(SehJob* j) { *j->out = j->fn(*j->ctx); }
char g_crash_stack[400];
int seh_filter(EXCEPTION_POINTERS* ep, uint32_t* code, uintptr_t* where) {
    *code = ep->ExceptionRecord->ExceptionCode;
    *where = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    // probable return addresses on the stack: dwords pointing into SPEED2.EXE code or into this DLL
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&seh_filter), &self);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(self), hi = lo + 0x200000;
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(ep->ContextRecord->Esp);
    int len = snprintf(g_crash_stack, sizeof g_crash_stack, " esp=%08lX stack:", ep->ContextRecord->Esp);
    int found = 0;
    for (int i = 0; i < 256 && found < 14 && len < static_cast<int>(sizeof g_crash_stack) - 24; ++i) {
        const uint32_t v = sp[i];
        if (v >= 0x401000 && v < 0x784000)
            len += snprintf(g_crash_stack + len, sizeof g_crash_stack - len, " %08X", v), ++found;
        else if (v >= lo && v < hi)
            len += snprintf(g_crash_stack + len, sizeof g_crash_stack - len, " dll+%X", static_cast<unsigned>(v - lo)), ++found;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
bool seh_run(SehJob* j, uint32_t* code, uintptr_t* where) {
    __try {
        seh_thunk(j);
        return true;
    } __except (seh_filter(GetExceptionInformation(), code, where)) {
        return false;
    }
}
json run_one(const DiffTestReg& t, const DiffCtx& ctx) {
    DiffResult r;
    SehJob job{t.fn, &ctx, &r};
    uint32_t code = 0;
    uintptr_t where = 0;
    const long inflight_before = nfsu2_inflight;
    if (seh_run(&job, &code, &where)) return result_json(r);
    const int undone = game::run_pending_undos();  // fixtures the crashed test left spliced into game structures
    // a crash inside one of our entry stubs skips its "lock dec": give the leaked count back, or the next hot reload
    // waits forever for 0 and keeps the old DLL (its hooks then block the new one). Other threads' calls in flight
    // at the two samples may skew this by a little; they balance out when they return.
    const long leaked = nfsu2_inflight - inflight_before;
    if (leaked > 0) InterlockedExchangeAdd(&nfsu2_inflight, -leaked);
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&run_one), &self);
    char buf[700];
    snprintf(buf, sizeof buf, "exception %08X at %08X (nfsu2_game.dll+0x%X: llvm-symbolizer --relative-address), %d fixture(s) undone; step '%s';%s",
             code, static_cast<unsigned>(where), static_cast<unsigned>(where - reinterpret_cast<uintptr_t>(self)), undone,
             game::g_test_step, g_crash_stack);
    game::g_test_step = "";
    return json{{"name", t.name}, {"iterations", 0}, {"mismatches", 1}, {"examples", json::array({std::string(buf)})},
                {"error", std::string(buf)}};
}

const char* diff(const char* name, uint32_t iterations, uint32_t seed) {
    const DiffCtx ctx{iterations, seed};
    json out;
    try {
        if (std::string(name) == "*") {
            out = json::array();
            for (auto& t : diff_registry()) out.push_back(run_one(t, ctx));
        } else {
            out = json{{"error", std::string("no diff test ") + name}};
            for (auto& t : diff_registry())
                if (std::string(t.name) == name) out = run_one(t, ctx);
        }
    } catch (const std::exception& e) {
        out = json{{"error", e.what()}};
    }
    g_out = out.dump(-1, ' ', false, json::error_handler_t::replace);
    return g_out.c_str();
}

}  // namespace
}  // namespace game

namespace game { void gcpc_register(); }  // game/systems/frontend/gcpc_hooks.cpp

extern "C" __declspec(dllexport) const probe::GameApi* nfsu2_game_api() {
    game::gcpc_register();  // nfsu2-gcpc's verified menu/camera rewrites (built at run time from their table)
    auto& r = game::registry();
    game::g_api = {probe::kGameApiVersion, __DATE__ " " __TIME__, static_cast<uint32_t>(r.size()), r.data(),
                   &game::diff_list, &game::diff, &nfsu2_inflight};
    return &game::g_api;
}

// Harness self-check: a test that crashes must come back as a failed test (exception + address), not disable the
// probe core. _BROKEN = expected to fail.
NFSU2_DIFF_TEST(crash_BROKEN) {
    game::ScopedUndo undo([] {});  // must be reported as "1 fixture(s) undone"
    volatile int* volatile p = nullptr;
    *p = 1;
    (void)ctx;
    return game::DiffResult{};  // not reached
}
