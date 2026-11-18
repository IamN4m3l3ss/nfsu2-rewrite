// Live check (shadow test): while the game runs normally, every real call of a checked function also runs the
// original and our rewrite on two copies of its inputs and compares them (the probe core intercepts the call and
// hands it here; the game itself continues with the original on the real data). A rewrite opts in with a live spec
// next to its registration: how many bytes of `this` and of each pointer argument the function touches.
//   LEAF_LIVE(0x00592080, SetWheelTerrain, THIS, INT, 0x400, 0, 0);   // this: 0x400 bytes, two value args
// Only for functions that write to `this` / their argument buffers: pointers INSIDE the copies still reach the live
// game (reads see live data, the same for both runs; writes through them would touch the game).
#pragma once
#include <initializer_list>
#include <vector>

#include "leaf.h"

namespace game::live {
struct Spec {
    uintptr_t addr;
    leaf::Fn fn;
    uint32_t this_size;
    std::vector<uint32_t> arg_size;  // per stack argument: 0 = a value, else bytes behind the pointer
};
std::vector<Spec>& specs();
struct Add {
    Add(uintptr_t a, const leaf::Fn& f, uint32_t this_size, std::initializer_list<uint32_t> args) {
        specs().push_back({a, f, this_size, args});
    }
};
}  // namespace game::live

#define LEAF_LIVE(addr, fn, conv, ret, this_size, ...) \
    static ::game::live::Add g_live_##fn(addr, LEAF_FN(fn, conv, ret), this_size, {__VA_ARGS__})
