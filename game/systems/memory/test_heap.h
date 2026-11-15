// Test helper: a private bMemoryPool on its own arena, installed in pool slot 7 (unused by the game) while a test
// runs, so bMalloc(flags & 7 == 7) / bFree in the code under test land in memory the test controls. Two heaps built
// with the same size and alignment give identical layouts for original vs rewrite runs (pointers compared as
// offsets). Render thread only.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "diff.h"
#include "rewrite.h"

namespace game::testing {

constexpr uintptr_t kPoolTable = 0x00828154, kExtAllocTable = 0x0082802C;
inline bool g_slot7_ours = false;  // a test installed its heap in slot 7 (shared by every test using slot 7)

inline void slot7_set(void* pool) {
    game_var<void*>(kPoolTable + 7 * 4) = pool;
    g_slot7_ours = pool != nullptr;
}
// true if slot 7 is free (reclaims it if a crashed test of ours left it set)
inline bool slot7_free() {
    if (game_var<void*>(kPoolTable + 7 * 4) && g_slot7_ours) slot7_set(nullptr);
    return game_var<void*>(kPoolTable + 7 * 4) == nullptr && game_var<void*>(kExtAllocTable + 7 * 4) == nullptr;
}

struct TestHeap {
    alignas(16) uint8_t pool[0x58]{};
    std::vector<uint8_t> buf;
    uint8_t* base;
    uint32_t size;
    TestHeap(uint32_t n, uint32_t mis = 0) : buf(n + 8192), size(n) {
        base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(buf.data()) + 4095) & ~uintptr_t(4095)) + mis;
        for (uint32_t i = 0; i < n; ++i) base[i] = static_cast<uint8_t>(i * 7 + 3);
        game_fn<void(__thiscall*)(void*, uint8_t*, int32_t, const char*)>(0x004402C0)(pool, base, static_cast<int32_t>(n), "test");
    }
    ~TestHeap() {
        if (game_var<void*>(kPoolTable + 7 * 4) == pool) slot7_set(nullptr);
        delete undo;  // runs the undo (harmless: slot already cleared) and pops it
        game_fn<void(__thiscall*)(void*)>(0x0043F9A0)(pool);  // Destroy (frees OS blocks, deletes the CS)
    }
    // installing registers an undo (clear slot 7) so a crashing test can't leave a dead pool installed - a crash
    // once did, and after the next reload nothing knew slot 7 was a test leftover
    void install() {
        slot7_set(pool);
        if (!undo) undo = new game::ScopedUndo([] { if (g_slot7_ours) slot7_set(nullptr); });
    }
    game::ScopedUndo* undo = nullptr;
    void uninstall() { slot7_set(nullptr); }
    int32_t num_allocs() const { return *reinterpret_cast<const int32_t*>(pool + 0x1C); }
    int32_t used() const { return *reinterpret_cast<const int32_t*>(pool + 0x28); }
    // pointer -> offset in the arena (+1, so 0 = null); outside the arena: the raw value with the top bit set
    uint64_t off(const void* p) const {
        const uintptr_t a = reinterpret_cast<uintptr_t>(p);
        if (!p) return 0;
        if (a >= reinterpret_cast<uintptr_t>(base) && a <= reinterpret_cast<uintptr_t>(base) + size)
            return a - reinterpret_cast<uintptr_t>(base) + 1;
        return (1ull << 63) | a;
    }
};

}  // namespace game::testing
