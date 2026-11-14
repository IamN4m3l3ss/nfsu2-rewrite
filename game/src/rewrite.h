// Registration of rewritten functions. Each rewrite is hooked in place of the original by the probe core and can be
// switched between original and rewrite at runtime (MCP rewrite_enable/disable).
//
//   using Fn = void(__cdecl*)(float);
//   static Fn o_UpdateFrameTime;                      // the original, filled in by the core
//   static void __cdecl UpdateFrameTime(float dt) { ... o_UpdateFrameTime(dt); ... }
//   NFSU2_REWRITE(0x0057EAD0, UpdateFrameTime, "void __cdecl(float real_dt)", o_UpdateFrameTime);
//
// Game addresses stay integers until the moment they're called (see CLAUDE.md: never cast our own functions to game
// signatures; clang may treat a provably mismatched call as UB).
#pragma once
#include <cstdint>
#include <vector>

#include "game_api.h"

namespace game {

std::vector<probe::GameRewrite>& registry();

struct Registrar {
    Registrar(uintptr_t addr, const char* name, const char* sig, void* fn, void** original) {
        registry().push_back({addr, name, sig, fn, original});
    }
};

}  // namespace game

#define NFSU2_REWRITE(addr, fn, sig, original)                                                          \
    static ::game::Registrar g_reg_##fn(addr, #fn, sig, reinterpret_cast<void*>(&fn),                  \
                                        reinterpret_cast<void**>(&original))

// read/write game globals by address
template <class T> inline T& game_var(uintptr_t addr) { return *reinterpret_cast<T*>(addr); }
