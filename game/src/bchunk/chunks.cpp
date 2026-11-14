// EA bChunk loading (NA v1.2): loader registry lookup and the per-bundle chunk dispatch.
//   bChunk {u32 id; u32 size; u8 data[size];}  next = (u8*)c + 8 + size
//   bChunkLoader {bChunkLoader* next; u32 id; int (*load)(bChunk*); int (*unload)(bChunk*);}
//   registry: 64 buckets at 0x828050, bucket = (id + (id>>12) + (id>>6)) & 63 (ctor 0x43C0B0, 24 registrations)
//   legacy handlers: 36 function pointers at 0x7FA858..0x7FA8E8, tried in order when no loader is registered
#include <string>

#include "../diff.h"
#include "../regs.h"
#include "../rewrite.h"

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

}  // namespace

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
