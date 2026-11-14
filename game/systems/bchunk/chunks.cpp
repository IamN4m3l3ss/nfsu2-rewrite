// EA bChunk loading (NA v1.2): loader registry lookup and the per-bundle chunk dispatch.
//   bChunk {u32 id; u32 size; u8 data[size];}  next = (u8*)c + 8 + size
//   bChunkLoader {bChunkLoader* next; u32 id; int (*load)(bChunk*); int (*unload)(bChunk*);}
//   registry: 64 buckets at 0x828050, bucket = (id + (id>>12) + (id>>6)) & 63 (ctor 0x43C0B0, 24 registrations)
//   legacy handlers: 36 function pointers at 0x7FA858..0x7FA8E8, tried in order when no loader is registered
#include <cstring>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

struct bChunk {
    uint32_t id, size;
};
struct bChunkLoader {
    bChunkLoader* next;
    uint32_t id;
    int(__cdecl* load)(bChunk*);
    int(__cdecl* unload)(bChunk*);
};
constexpr uintptr_t kLoaderTable = 0x00828050;
constexpr uintptr_t kLegacyBegin = 0x007FA858, kLegacyEnd = 0x007FA8E8;

// ---------------------------------------------------------------- FindChunkLoader 0x0043C100 (2 callers)
using Find_t = bChunkLoader*(__cdecl*)(uint32_t);
Find_t o_FindChunkLoader = nullptr;
bChunkLoader* __cdecl FindChunkLoader(uint32_t id) {
    const uint32_t bucket = ((id >> 6) + (id + (id >> 12))) & 0x3F;
    for (bChunkLoader* l = game_var<bChunkLoader*>(kLoaderTable + bucket * 4); l; l = l->next)
        if (l->id == id) return l;
    return nullptr;
}

// ---------------------------------------------------------------- LoadChunk 0x00570D70 (1 caller)
using LoadChunk_t = int(__cdecl*)(bChunk*);
LoadChunk_t o_LoadChunk = nullptr;
int __cdecl LoadChunk(bChunk* c) {
    if (c->id == 0) return 1;
    if (bChunkLoader* l = game_fn<Find_t>(0x0043C100)(c->id)) return l->load(c);
    for (uintptr_t p = kLegacyBegin; p < kLegacyEnd; p += 4)
        if (game_var<LoadChunk_t>(p)(c)) return 1;
    return 0;
}

// ---------------------------------------------------------------- LoadChunks 0x00577AB0 (3 callers)
using Void_t = void(__cdecl*)();
using LoadChunks_t = int(__cdecl*)(bChunk*, int);
LoadChunks_t o_LoadChunks = nullptr;
int __cdecl LoadChunks(bChunk* start, int size) {
    uint8_t* p = reinterpret_cast<uint8_t*>(start);
    uint8_t* const end = p + size;
    while (p < end) {
        game_fn<Void_t>(0x0043BDF0)();  // tick (QPC), result unused
        if (!game_fn<LoadChunk_t>(0x00570D70)(reinterpret_cast<bChunk*>(p)))
            game_fn<Void_t>(0x0043BD50)();  // int3 left in the release build: a chunk nobody loads
        p += reinterpret_cast<bChunk*>(p)->size + 8;
    }
    if (game_var<uint8_t>(0x00864F74)) return 0;  // original: plain ret (EAX = flag byte; callers ignore EAX)
    game_fn<Void_t>(0x0043BDF0)();
    if (game_var<uint32_t>(0x0082DAB4)) game_fn<Void_t>(0x004908F0)();
    return game_fn<LoadChunks_t>(0x004887C0)(start, size);  // original tail-jumps with the same arguments
}

// ---------------------------------------------------------------- UnloadChunks 0x00577B10 (5 callers)
// Unloads in exact reverse order: collect the chunks of [start, end) into a 64-entry ring, unload the last
// min(n, 64) of them backwards, then the earliest one unloaded becomes the new end. Each non-zero id is also
// formatted "BCHUNK_0x%08X" into a 4-slot ring of 20-byte debug name buffers (0x8665C0, index 0x866610).
constexpr uintptr_t kUnloadLegacyBegin = 0x007FA8E8, kUnloadLegacyEnd = 0x007FA978;
constexpr uintptr_t kNameRing = 0x008665C0, kNameRingIndex = 0x00866610;
LoadChunks_t o_UnloadChunks = nullptr;
int __cdecl UnloadChunks(bChunk* start, int size) {
    uint8_t* end = reinterpret_cast<uint8_t*>(start) + size;
    while (reinterpret_cast<uint8_t*>(start) < end) {
        bChunk* ring[64];
        int count = 0;
        for (uint8_t* p = reinterpret_cast<uint8_t*>(start); p < end; p += reinterpret_cast<bChunk*>(p)->size + 8)
            ring[count++ & 63] = reinterpret_cast<bChunk*>(p);
        bChunk* c = nullptr;
        for (int i = count - 1, n = count > 64 ? 64 : count; n > 0; --i, --n) {
            c = ring[i & 63];
            if (c->id != game_var<uint32_t>(0x007FAA0C)) {  // the one-entry "don't name" table holds id 0
                const uint32_t slot = (game_var<uint32_t>(kNameRingIndex) + 1) & 3;
                game_var<uint32_t>(kNameRingIndex) = slot;
                game_fn<int(__cdecl*)(char*, const char*, ...)>(0x004400D0)(
                    reinterpret_cast<char*>(kNameRing + slot * 20), reinterpret_cast<const char*>(0x007A03A8), c->id);
            }
            game_fn<Void_t>(0x0043BDF0)();
            if (c->id == 0) continue;
            int ok = 0;
            if (bChunkLoader* l = game_fn<Find_t>(0x0043C100)(c->id)) {
                ok = l->unload(c);
            } else {
                for (uintptr_t p = kUnloadLegacyBegin; p < kUnloadLegacyEnd && !ok; p += 4)
                    ok = game_var<LoadChunk_t>(p)(c);
            }
            if (!ok) game_fn<Void_t>(0x0043BD50)();  // bBreak
        }
        end = reinterpret_cast<uint8_t*>(c);
    }
    if (game_var<uint8_t>(0x00864F74)) return 0;
    game_fn<Void_t>(0x0043BDF0)();
    if (game_var<uint32_t>(0x0082DAB4)) game_fn<Void_t>(0x004908F0)();  // (0x4022C0 before it is an empty stub)
    return game_fn<LoadChunks_t>(0x004887C0)(start, size);
}

}  // namespace

NFSU2_REWRITE_N(0x00577B10, UnloadChunks, 2, "void __cdecl(bChunk* start, int size) -> reverse order, tail-calls 0x4887C0", o_UnloadChunks);
NFSU2_REWRITE_N(0x0043C100, FindChunkLoader, 1, "bChunkLoader* __cdecl(uint32_t id)", o_FindChunkLoader);
NFSU2_REWRITE_N(0x00570D70, LoadChunk, 1, "int __cdecl(bChunk*) -> 1 if handled", o_LoadChunk);
NFSU2_REWRITE_N(0x00577AB0, LoadChunks, 2, "void __cdecl(bChunk* start, int size) -> tail-calls 0x4887C0", o_LoadChunks);

// every registered id (walk the live table) + random ids
NFSU2_DIFF_TEST(FindChunkLoader) {
    std::vector<uint32_t> ids;
    for (uint32_t b = 0; b < 64; ++b)
        for (bChunkLoader* l = game_var<bChunkLoader*>(kLoaderTable + b * 4); l; l = l->next) ids.push_back(l->id);
    uint32_t i = 0;
    return game::run_diff(ctx, "FindChunkLoader", o_FindChunkLoader, [&ids, &i](game::Rng& r) {
        const uint32_t id = (i < ids.size()) ? ids[i] : (r.below(2) ? r.next() : ids[r.below(static_cast<uint32_t>(ids.size()))] ^ (1u << r.below(32)));
        ++i;
        bChunkLoader* a = o_FindChunkLoader(id);
        bChunkLoader* b = FindChunkLoader(id);
        return a == b ? game::Ok() : game::MismatchText("id=" + std::to_string(id));
    });
}

// LoadChunk/LoadChunks call real loaders (side effects): checked by transcription + live boot + M5 drive replay.
NFSU2_DIFF_TEST(regs_chunks) {
    using game::arg;
    return game::run_diff(ctx, "regs_chunks", o_FindChunkLoader, [](game::Rng& r) {
        bChunk zero{0, 0};  // id 0: LoadChunk returns 1 without calling anything
        for (auto c : {game::check_regs("FindChunkLoader", (void*)o_FindChunkLoader, NFSU2_ENTRY(FindChunkLoader), {r.next()}, false),
                       game::check_regs("LoadChunk(id 0)", (void*)o_LoadChunk, NFSU2_ENTRY(LoadChunk), {arg(&zero)}, false)})
            if (!c.ok) return c;
        return game::Ok();
    });
}

// LoadChunks / UnloadChunks with a fake loader: a test id is registered at the head of its bucket for the duration
// of one case; its load/unload callbacks record the order of chunks they see. Original vs ours must produce the
// same call sequence, the same debug-name ring and the same ring index. Bundles of up to 300 chunks (> 64 exercises
// the ring), some with id 0 (skipped).
namespace {
constexpr uint32_t kTestId = 0x7EAD0001;
std::vector<uintptr_t> g_seen;
int __cdecl fake_cb(bChunk* c) {
    g_seen.push_back(reinterpret_cast<uintptr_t>(c));
    return 1;
}
struct FakeLoader {
    bChunkLoader node{nullptr, kTestId, &fake_cb, &fake_cb};
    bChunkLoader** head;
    FakeLoader() {
        const uint32_t bucket = ((kTestId >> 6) + (kTestId + (kTestId >> 12))) & 0x3F;
        head = reinterpret_cast<bChunkLoader**>(kLoaderTable + bucket * 4);
        node.next = *head;
        *head = &node;
    }
    ~FakeLoader() { *head = node.next; }
};
std::vector<uint8_t> fake_bundle(game::Rng& r) {
    std::vector<uint8_t> b;
    const uint32_t n = r.below(4) == 0 ? 60 + r.below(240) : 1 + r.below(10);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t id = r.below(6) == 0 ? 0 : kTestId, len = r.below(5) * 4;
        const size_t at = b.size();
        b.resize(at + 8 + len);
        std::memcpy(&b[at], &id, 4);
        std::memcpy(&b[at + 4], &len, 4);
    }
    return b;
}
game::Case run_chunks_case(game::Rng& r, LoadChunks_t orig, LoadChunks_t mine, const char* what) {
    FakeLoader fake;
    std::vector<uint8_t> bundle = fake_bundle(r);
    std::vector<uint8_t> ring0(84), ringA(84);
    std::memcpy(ring0.data(), reinterpret_cast<void*>(kNameRing), 84);  // 4 x 20 names + the index (0x866610)
    bChunk* start = reinterpret_cast<bChunk*>(bundle.data());
    const int size = static_cast<int>(bundle.size());
    g_seen.clear();
    orig(start, size);
    std::vector<uintptr_t> a = g_seen;
    std::memcpy(ringA.data(), reinterpret_cast<void*>(kNameRing), 84);
    std::memcpy(reinterpret_cast<void*>(kNameRing), ring0.data(), 84);
    g_seen.clear();
    mine(start, size);
    const bool ring_ok = std::memcmp(ringA.data(), reinterpret_cast<void*>(kNameRing), 84) == 0;
    const std::string info = std::string(what) + " chunks=" + std::to_string(bundle.size());
    if (a != g_seen) return game::MismatchText(info + " calls " + std::to_string(a.size()) + "/" + std::to_string(g_seen.size()));
    if (!ring_ok) return game::MismatchText(info + " name ring");
    return game::Ok();
}
}  // namespace

NFSU2_DIFF_TEST(LoadChunks) {
    return game::run_diff(ctx, "LoadChunks", o_LoadChunks, [](game::Rng& r) {
        return run_chunks_case(r, o_LoadChunks, &LoadChunks, "load");
    });
}
NFSU2_DIFF_TEST(UnloadChunks) {
    return game::run_diff(ctx, "UnloadChunks", o_UnloadChunks, [](game::Rng& r) {
        return run_chunks_case(r, o_UnloadChunks, &UnloadChunks, "unload");
    });
}
