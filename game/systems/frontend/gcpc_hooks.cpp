// Registers nfsu2-gcpc's verified menu-engine (FEng) and camera rewrites with our hook manager.
//
// The code itself is nfsu2-gcpc's (CC0, github.com/octavioCuatrochio/nfsu2-gcpc), vendored unchanged except for
// three marked patches in third_party/nfsu2-gcpc/ and built as C++17 with an 80-bit long double (their maths mirrors
// x87 extended precision). Their differential referee - original machine code vs these exact compiled functions,
// inside SPEED2.EXE - was built with OUR toolchain and run: 1,672,945 checks, 0 mismatches (2026-11-15).
//
// Only entries listed in gcpc_allow.inc are registered: nfsu2-gcpc rewrite status "verified" in the frontend/camera
// files, minus addresses we rewrote ourselves. Each gets a runtime thunk:
//   plain:    lock inc [calls]; jmp impl
//   preserve: (their audit found callers using ECX/EDX after the call) lock inc [calls]; push ecx; push edx;
//             copy the stack args; call impl; [add esp, n]; pop edx; pop ecx; ret [n]
// Menu code runs on the game's main thread only, so these thunks don't count in-flight calls (hot reload happens on
// that thread at Present, when none of them can be running).
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "../../../third_party/nfsu2-gcpc/hooks.h"
#include "rewrite.h"

namespace {

const uint32_t kAllow[] = {
#include "gcpc_allow.inc"
};
// nfsu2-gcpc status "hooked": in the game in their builds but never compared side by side (they need live game state,
// their referee only covers self-contained functions). Registered OFF and labelled unverified until our menu A/B test
// (draw-list comparison) covers them.
const uint32_t kUnverified[] = {
#include "gcpc_hooked.inc"
};
// Never registered: the user's required mods (WidescreenFix, HD UI support) patch code INSIDE these functions;
// replacing them undid the mods (garbled menu text, 4:3 HUD - user report 2026-11-15). Found by comparing every
// byte of each function in memory with the exe on disk.
const uint32_t kModPatched[] = {0x00505150 /* FELerpVector3 */, 0x0051D620 /* FEngGetCenter */,
                                0x0051B0B0 /* cFEngRender::MakeRenderMatrix */, 0x0051B3A0 /* cFEngRender::GenerateClipInfo */,
                                0x0049B600 /* PCResolution791F30::Act */,
    // ...and every rewrite whose original reaches mod-patched code through direct calls (transitively): the
    // rewritten C++ calls its own C++ versions directly and would skip the mods' patches (garbled pause-menu text).
    0x0049C690, 0x004B8470, 0x004CF750, 0x00504910, 0x0051C110, 0x0051D170, 0x0051D9F0, 0x0051EC10,
    0x00527170, 0x0052BDC0, 0x0052D020, 0x0052D6B0, 0x0052D800, 0x0052D9F0, 0x0052DAC0, 0x0052DCC0,
    0x0052E320, 0x00530AA0, 0x005354F0, 0x005378C0, 0x00538080, 0x005390E0, 0x00539370, 0x00539650,
    0x00540D20, 0x00540E90, 0x00541DB0, 0x0054CE60, 0x0054D190, 0x00551DE0, 0x00556020,
};

constexpr size_t kSlot = 128;
uint8_t* g_thunks = nullptr;
std::vector<void*> g_originals;              // filled by the core (trampolines)
volatile uint32_t* g_calls = nullptr;       // per-entry call counters
std::vector<std::string> g_names, g_sigs;

uint8_t* emit(uint8_t* p, std::initializer_list<uint8_t> b) {
    for (uint8_t x : b) *p++ = x;
    return p;
}
uint8_t* emit32(uint8_t* p, uint32_t v) {
    std::memcpy(p, &v, 4);
    return p + 4;
}

const void* make_thunk(size_t i, const hook::Replacement& r, volatile uint32_t* counter) {
    uint8_t* const start = g_thunks + i * kSlot;
    uint8_t* p = start;
    p = emit(p, {0xF0, 0xFF, 0x05});  // lock inc dword ptr [counter]
    p = emit32(p, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(counter)));
    if (!r.preserve) {
        p = emit(p, {0xE9});  // jmp impl
        p = emit32(p, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(r.fn) - reinterpret_cast<uintptr_t>(p + 4)));
        return start;
    }
    const uint32_t n = r.argBytes / 4u;
    p = emit(p, {0x51, 0x52});  // push ecx; push edx
    for (uint32_t k = 0; k < n; ++k) {  // push dword ptr [esp + 8 + argBytes] (last arg first; offset stays constant)
        p = emit(p, {0xFF, 0xB4, 0x24});
        p = emit32(p, 8u + r.argBytes);
    }
    p = emit(p, {0xE8});  // call impl
    p = emit32(p, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(r.fn) - reinterpret_cast<uintptr_t>(p + 4)));
    if (!r.calleePops && r.argBytes) {
        p = emit(p, {0x81, 0xC4});  // add esp, argBytes
        p = emit32(p, r.argBytes);
    }
    p = emit(p, {0x5A, 0x59});  // pop edx; pop ecx
    if (r.calleePops && r.argBytes) {
        p = emit(p, {0xC2});  // ret argBytes
        const uint16_t nb = r.argBytes;
        std::memcpy(p, &nb, 2);
        p += 2;
    } else {
        p = emit(p, {0xC3});  // ret
    }
    return start;
}

}  // namespace

namespace game {
// Called once from nfsu2_game_api() (after static initialisation), before the core reads the registry.
void gcpc_register() {
    static bool done = false;
    if (done) return;
    done = true;
    std::set<uint32_t> allow(std::begin(kAllow), std::end(kAllow));
    const std::set<uint32_t> unverified(std::begin(kUnverified), std::end(kUnverified));
    allow.insert(unverified.begin(), unverified.end());
    for (uint32_t a : kModPatched) allow.erase(a);
    std::vector<const hook::Replacement*> picked;
    for (size_t i = 0; i < hook::kReplacementCount; ++i)
        if (allow.count(hook::kReplacements[i].addr)) picked.push_back(&hook::kReplacements[i]);
    g_thunks = static_cast<uint8_t*>(VirtualAlloc(nullptr, picked.size() * kSlot + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    g_originals.assign(picked.size(), nullptr);
    g_calls = new volatile uint32_t[picked.size() + 1]();
    g_names.resize(picked.size());
    g_sigs.resize(picked.size());
    for (size_t i = 0; i < picked.size(); ++i) {
        const hook::Replacement& r = *picked[i];
        if (r.preserve && (r.argBytes / 4u) * 7u + 40u > kSlot) continue;  // (never: largest argBytes is small)
        g_names[i] = r.name;
        g_sigs[i] = std::string(unverified.count(r.addr) ? "UNVERIFIED nfsu2-gcpc rewrite" : "nfsu2-gcpc rewrite") +
                    (r.preserve ? " (ECX/EDX preserved)" : "");
        const void* thunk = make_thunk(i, r, &g_calls[i]);
        registry().push_back({r.addr, g_names[i].c_str(), g_sigs[i].c_str(), const_cast<void*>(thunk), &g_originals[i],
                              "game/systems/frontend/gcpc_hooks.cpp", &g_calls[i]});
    }
}
}  // namespace game
