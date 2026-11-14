// Test helper: a fake bChunkLoader spliced into the game's loader registry (0x828050) for the lifetime of the
// object. Its load and unload callbacks record every chunk they are given (address + 'L'/'U'), so tests can compare
// the exact call sequence of an original function and our rewrite. Registers a leaf id and a container id.
// Render thread only (the registry is read by the game's loading code).
#pragma once
#include <cstdint>
#include <vector>

#include "diff.h"

namespace game::testing {

constexpr uint32_t kFakeLeafId = 0x7EAD0001;
constexpr uint32_t kFakeContainerId = 0xFEAD0002;  // high bit: container

struct FakeChunkLoader {
    struct Node {
        Node* next;
        uint32_t id;
        int(__cdecl* load)(void*);
        int(__cdecl* unload)(void*);
    };
    struct Call {
        uintptr_t chunk;
        char kind;  // 'L' load, 'U' unload
        bool operator==(const Call&) const = default;
    };
    static inline std::vector<Call> calls;
    static int __cdecl on_load(void* c) {
        calls.push_back({reinterpret_cast<uintptr_t>(c), 'L'});
        return 1;
    }
    static int __cdecl on_unload(void* c) {
        calls.push_back({reinterpret_cast<uintptr_t>(c), 'U'});
        return 1;
    }
    Node nodes[2]{{nullptr, kFakeLeafId, &on_load, &on_unload}, {nullptr, kFakeContainerId, &on_load, &on_unload}};
    Node** heads[2];
    Node* old_heads[2];
    ScopedUndo undo;
    static Node** bucket(uint32_t id) { return reinterpret_cast<Node**>(0x00828050 + (((id >> 6) + (id + (id >> 12))) & 0x3F) * 4); }
    FakeChunkLoader()
        : heads{bucket(kFakeLeafId), bucket(kFakeContainerId)},
          old_heads{*heads[0], *heads[1]},
          undo([h0 = heads[0], h1 = heads[1], o0 = old_heads[0], o1 = old_heads[1]] {
              *h1 = o1;  // (restore in reverse: both ids may share a bucket)
              *h0 = o0;
          }) {
        for (int i = 0; i < 2; ++i) {
            nodes[i].next = *heads[i];
            *heads[i] = &nodes[i];
        }
        calls.clear();
    }
};

}  // namespace game::testing
