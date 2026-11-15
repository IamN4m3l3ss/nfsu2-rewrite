// Registration of rewritten functions. Each rewrite is hooked in place of the original by the probe core and can be
// switched between original and rewrite at runtime (MCP rewrite_enable/disable).
//
//   using Fn = void(__cdecl*)(float);
//   static Fn o_bStrLen;                              // the original, filled in by the core
//   static int __cdecl bStrLen(const char* s) { ... }
//   NFSU2_REWRITE_N(0x0043DBB0, bStrLen, 1, "int __cdecl(const char*)", o_bStrLen);
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
    Registrar(uintptr_t addr, const char* name, const char* sig, void* fn, void** original, const char* file = "",
              volatile uint32_t* calls = nullptr) {
        registry().push_back({addr, name, sig, fn, original, file, calls});
    }
};

}  // namespace game

#define NFSU2_REWRITE(addr, fn, sig, original)                                                          \
    static ::game::Registrar g_reg_##fn(addr, #fn, sig, reinterpret_cast<void*>(&fn),                  \
                                        reinterpret_cast<void**>(&original), __FILE__)

// read/write game globals by address
template <class T> inline T& game_var(uintptr_t addr) { return *reinterpret_cast<T*>(addr); }

// Call a game function by address. The address stays opaque (volatile), so the compiler can't see a mismatched
// function type (CLAUDE.md). Calling through the game address also goes through that function's own hook, so each
// rewrite's on/off switch stays independent of its callers.
template <class F> inline F game_fn(uintptr_t addr) {
    volatile uintptr_t a = addr;
    return reinterpret_cast<F>(a);
}
