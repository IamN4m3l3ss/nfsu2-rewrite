// Interface between the probe core (nfsu2_core.dll) and the rewrite (nfsu2_game.dll).
// The game DLL lists its replacements; the core hooks each `addr` with MinHook, stores the trampoline (the original
// function) into `*original`, and switches each hook on/off at runtime. Plain C types only (separate static CRTs).
#pragma once
#include <cstdint>

namespace probe {

constexpr uint32_t kGameApiVersion = 1;

struct GameRewrite {
    uintptr_t addr;         // original function in SPEED2.EXE (NA v1.2)
    const char* name;       // e.g. "UpdateFrameTime"
    const char* signature;  // e.g. "void __cdecl(float real_dt)"
    void* replacement;      // our function, same calling convention and signature
    void** original;        // filled by the core: call this to run the original
};

struct GameApi {
    uint32_t version;
    const char* build;
    uint32_t count;
    const GameRewrite* rewrites;
};

}  // namespace probe

extern "C" {
using nfsu2_game_api_fn = const probe::GameApi* (*)();
}
