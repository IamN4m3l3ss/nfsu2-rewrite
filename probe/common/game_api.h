// Interface between the probe core (nfsu2_core.dll) and the rewrite (nfsu2_game.dll).
// The game DLL lists its replacements; the core hooks each `addr` with MinHook, stores the trampoline (the original
// function) into `*original`, and switches each hook on/off at runtime. Plain C types only (separate static CRTs).
#pragma once
#include <cstdint>

namespace probe {

constexpr uint32_t kGameApiVersion = 6;  // v3: GameRewrite.file, v4: GameRewrite.calls, v5: inflight, v6: live check

struct GameRewrite {
    uintptr_t addr;         // original function in SPEED2.EXE (NA v1.2)
    const char* name;       // e.g. "bStrLen"
    const char* signature;  // e.g. "void __cdecl(float real_dt)"
    void* replacement;      // our function, same calling convention and signature
    void** original;        // filled by the core: call this to run the original
    const char* file;       // source file (game/systems/<system>/...), used to group rewrites by system
    volatile uint32_t* calls;  // incremented by the entry stub on every call of the rewrite (null: not counted)
};

struct GameApi {
    uint32_t version;
    const char* build;
    uint32_t count;
    const GameRewrite* rewrites;
    // M7 diff harness. Both return JSON owned by the game DLL, valid until the next call. Render thread.
    const char* (*diff_list)();  // ["bStrLen", ...]
    const char* (*diff)(const char* name, uint32_t iterations, uint32_t seed);
    // v5: number of calls currently inside an entry stub, all threads (wait for 0 before unloading the DLL)
    volatile long* inflight;
    // v6 live check (shadow test during normal play). live_list: JSON [{"name","addr","checked","mismatches",
    // "examples":[...]}] of every rewrite with a live spec (render thread). live_check: called by the core's hook on
    // a real call of `addr` (any thread): regs = saved registers + stack as laid out by the core's live hook
    // ([0] eflags, [1..8] edi esi ebp esp ebx edx ecx eax, [9] scratch, [10] return address, [11..] args),
    // tramp = the original function; runs original and rewrite on copies and records the result. live_reset: zero
    // the counters.
    const char* (*live_list)();
    void (*live_check)(uintptr_t addr, const uint32_t* regs, void* tramp);
    void (*live_reset)();
    // {"name","iterations","mismatches","examples":[...],"error"} (name "*" = run all -> array)
};

}  // namespace probe

extern "C" {
using nfsu2_game_api_fn = const probe::GameApi* (*)();
}
