// bWare slot pool: fixed-size block allocator, chained when expanded (EA core library, zBWare.cpp "SlotPool").
// Adapted from nfsu2-gcpc (CC0, github.com/octavioCuatrochio/nfsu2-gcpc, port/game/bWare/bSlotPool.cpp); PC ABI
// re-checked against SPEED2.EXE and proven with our own diff tests.
//
//   SlotPool +00 manager list next  +04 prev  +08 next (expansion) pool  +0C name  +10 free list (through the first
//            word of each free slot)  +14 flags (1 expand when empty, 2 zero on Malloc; new pools: 0xB)
//            +18 allocated  +1C most allocated  +20 bWareMalloc flags  +24 slots in this pool  +28 slot size
//            +2C total slots over the chain  +30 slot data
#include <cstring>
#include <string>
#include <vector>

#include "../memory/test_heap.h"
#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

struct SlotPool {
    SlotPool* list_next;
    SlotPool* list_prev;
    SlotPool* next;
    const char* name;
    void* free_list;
    uint32_t flags;
    int32_t allocated, most_allocated;
    uint32_t mem_flags;
    int32_t num_slots;
    uint32_t slot_size;
    int32_t total_slots;
    uint8_t* data() { return reinterpret_cast<uint8_t*>(this) + 0x30; }
};
static_assert(sizeof(SlotPool) == 0x30, "SlotPool header");

inline void* bware_malloc(uint32_t size, uint32_t flags) {
    return game_fn<void*(__cdecl*)(uint32_t, const char*, int32_t, uint32_t)>(0x00440390)(size, nullptr, 0, flags);
}
inline void bware_free(void* p) { game_fn<void(__cdecl*)(void*)>(0x00440540)(p); }

// ---------------------------------------------------------------- counters (cdecl)
using Count_t = int32_t(__cdecl*)(const SlotPool*);
Count_t o_bIsSlotPoolFull = nullptr, o_bCountFreeSlots = nullptr, o_bCountTotalSlots = nullptr;
int32_t __cdecl bIsSlotPoolFull(const SlotPool* p) { return p->allocated == p->total_slots; }      // 0x0043CD10
int32_t __cdecl bCountFreeSlots(const SlotPool* p) { return p->total_slots - p->allocated; }        // 0x0043CD20
int32_t __cdecl bCountTotalSlots(const SlotPool* p) { return p->total_slots; }                      // 0x0043CD30

// ---------------------------------------------------------------- SlotPool::FlushSlotPool 0x0043CD40 (this may be null)
using Flush_t = void(__thiscall*)(SlotPool*);
Flush_t o_FlushSlotPool = nullptr;
void __cdecl FlushSlotPool(SlotPool* pool) {  // rebuild every chained pool's free list in slot order
    for (SlotPool* p = pool; p; p = p->next) {
        const int32_t n = p->num_slots;
        const uint32_t size = p->slot_size;
        p->free_list = nullptr;
        if (n == 0) continue;
        uint8_t* slot = p->data();
        p->free_list = slot;
        for (int32_t i = n - 1; i > 0; --i) {
            uint8_t* nx = slot + size;
            *reinterpret_cast<void**>(slot) = nx;
            slot = nx;
        }
        *reinterpret_cast<void**>(slot) = nullptr;
    }
}

// ---------------------------------------------------------------- SlotPool::GetSlotNumber 0x0043CD80 (this may be null)
// index over the chain, -1 if in no pool; uses the FIRST pool's slot size for every pool, unsigned division
using SlotNum_t = int32_t(__thiscall*)(SlotPool*, const void*);
SlotNum_t o_GetSlotNumber = nullptr;
int32_t __cdecl GetSlotNumber(SlotPool* pool, const void* slot) {
    if (!pool) return -1;
    const uint32_t size = pool->slot_size;
    int32_t base = 0;
    for (SlotPool* p = pool; p; p = p->next) {
        const int32_t idx = static_cast<int32_t>(
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(slot) - reinterpret_cast<uintptr_t>(p) - 0x30) / size);
        if (idx >= 0 && idx < p->num_slots) return idx + base;
        base += p->num_slots;
    }
    return -1;
}

// ---------------------------------------------------------------- FastMalloc / Free / bOMalloc / bFree
using Fast_t = void*(__thiscall*)(SlotPool*);
Fast_t o_FastMalloc = nullptr;
void* __cdecl FastMalloc(SlotPool* self) {  // 0x0043CDD0: pop the free list (null if empty)
    const int32_t n = self->allocated + 1;
    void* slot = self->free_list;
    if (slot) {
        void* nx = *static_cast<void**>(slot);
        if (n > self->most_allocated) self->most_allocated = n;
        self->free_list = nx;
        self->allocated = n;
    }
    return slot;
}

using Free_t = void(__thiscall*)(SlotPool*, void*);
Free_t o_SlotPoolFree = nullptr;
void __cdecl SlotPoolFree(SlotPool* self, void* slot) {  // 0x0043CDF0: push onto the free list
    *static_cast<void**>(slot) = self->free_list;
    --self->allocated;
    self->free_list = slot;
}

using OMalloc_t = void*(__cdecl*)(SlotPool*);
OMalloc_t o_bOMalloc = nullptr;
void* __cdecl bOMalloc(SlotPool* pool) { return FastMalloc(pool); }  // 0x0043F8F0 (FastMalloc inlined)

using BFree_t = void(__cdecl*)(SlotPool*, void*);
BFree_t o_bFreeSlot = nullptr;
void __cdecl bFreeSlot(SlotPool* pool, void* slot) { SlotPoolFree(pool, slot); }  // 0x0043F920 (Free inlined)

// ---------------------------------------------------------------- SlotPool::NewSlotPool 0x00440B40 (cdecl)
using New_t = SlotPool*(__cdecl*)(uint32_t, int32_t, const char*, uint32_t);
New_t o_NewSlotPool = nullptr;
SlotPool* __cdecl NewSlotPool(uint32_t slot_size, int32_t num_slots, const char* name, uint32_t mem_flags) {
    auto* p = static_cast<SlotPool*>(bware_malloc(slot_size * static_cast<uint32_t>(num_slots) + 0x30, mem_flags));
    if (!p) return nullptr;
    p->name = name;
    p->num_slots = num_slots;
    p->slot_size = slot_size;
    p->flags = 0xB;
    p->free_list = nullptr;
    p->next = nullptr;
    p->mem_flags = mem_flags;
    p->total_slots = num_slots;
    p->allocated = 0;
    p->most_allocated = 0;
    game_fn<Flush_t>(0x0043CD40)(p);
    return p;
}

// ---------------------------------------------------------------- SlotPool::ExpandSlotPool 0x00440BB0
// append a pool of n slots to the chain; its last slot links to our old free list; no allocation-failure check
using Expand_t = void(__thiscall*)(SlotPool*, int32_t);
Expand_t o_ExpandSlotPool = nullptr;
void __cdecl ExpandSlotPool(SlotPool* self, int32_t n) {
    SlotPool* np = game_fn<New_t>(0x00440B40)(self->slot_size, n, self->name, self->mem_flags);
    np->flags = self->flags;
    SlotPool* last = self;
    while (last->next) last = last->next;
    last->next = np;
    const int32_t word = static_cast<int32_t>((n - 1) * static_cast<int32_t>(self->slot_size)) / 4;  // signed
    *reinterpret_cast<void**>(np->data() + word * 4) = self->free_list;
    self->free_list = np->free_list;
    np->free_list = nullptr;
    self->total_slots += n;
}

// ---------------------------------------------------------------- SlotPool::Malloc 0x00440D40, bMalloc(pool) 0x00440E50
using Malloc_t = void*(__thiscall*)(SlotPool*);
Malloc_t o_SlotPoolMalloc = nullptr;
void* __cdecl SlotPoolMalloc(SlotPool* self) {  // expand by num_slots/4 + 1 when empty and flag 1; flag 2 zeroes it
    if (!self->free_list && (self->flags & 1)) game_fn<Expand_t>(0x00440BB0)(self, self->num_slots / 4 + 1);
    void* slot = game_fn<Fast_t>(0x0043CDD0)(self);
    if (slot && (self->flags & 2)) {
        auto* w = static_cast<uint32_t*>(slot);
        for (int32_t i = static_cast<int32_t>(self->slot_size) >> 2; i > 0; --i) *w++ = 0;
    }
    return slot;
}
OMalloc_t o_bMallocSlot = nullptr;
void* __cdecl bMallocSlot(SlotPool* pool) { return game_fn<Malloc_t>(0x00440D40)(pool); }

// ---------------------------------------------------------------- SlotPool::GetAllocatedSlot 0x00440C30
// the n-th allocated slot in slot order (null if fewer); builds a byte map with bWareMalloc (main pool) first
using GetAlloc_t = void*(__thiscall*)(SlotPool*, int32_t);
GetAlloc_t o_GetAllocatedSlot = nullptr;
void* __cdecl GetAllocatedSlot(SlotPool* self, int32_t n) {
    auto* map = static_cast<int8_t*>(bware_malloc(static_cast<uint32_t>(self->total_slots), 0));
    std::memset(map, 1, static_cast<uint32_t>(self->total_slots));
    for (void* f = self->free_list; f; f = *static_cast<void**>(f)) map[game_fn<SlotNum_t>(0x0043CD80)(self, f)] = 0;
    void* result = nullptr;
    const int32_t total = self->total_slots;
    for (int32_t i = 0; i < total; ++i) {
        if (!map[i]) continue;
        if (n == 0) {  // GetSlot(i) inlined: first pool's slot size
            int32_t index = i;
            for (SlotPool* p = self; p; p = p->next) {
                if (index >= 0 && index < p->num_slots) {
                    result = p->data() + static_cast<uint32_t>(index) * self->slot_size;
                    break;
                }
                index -= p->num_slots;
            }
            break;
        }
        --n;
    }
    bware_free(map);
    return result;
}

}  // namespace

NFSU2_REWRITE_N(0x0043CD10, bIsSlotPoolFull, 1, "int __cdecl(SlotPool*)", o_bIsSlotPoolFull);
NFSU2_REWRITE_N(0x0043CD20, bCountFreeSlots, 1, "int __cdecl(SlotPool*)", o_bCountFreeSlots);
NFSU2_REWRITE_N(0x0043CD30, bCountTotalSlots, 1, "int __cdecl(SlotPool*)", o_bCountTotalSlots);
NFSU2_REWRITE_THIS(0x0043CD40, FlushSlotPool, 0, "void SlotPool::FlushSlotPool() (this may be null)", o_FlushSlotPool);
NFSU2_REWRITE_THIS(0x0043CD80, GetSlotNumber, 1, "int SlotPool::GetSlotNumber(void* slot) (this may be null)", o_GetSlotNumber);
NFSU2_REWRITE_THIS(0x0043CDD0, FastMalloc, 0, "void* SlotPool::FastMalloc()", o_FastMalloc);
NFSU2_REWRITE_THIS(0x0043CDF0, SlotPoolFree, 1, "void SlotPool::Free(void* slot)", o_SlotPoolFree);
NFSU2_REWRITE_N(0x0043F8F0, bOMalloc, 1, "void* __cdecl(SlotPool*) FastMalloc", o_bOMalloc);
NFSU2_REWRITE_N(0x0043F920, bFreeSlot, 2, "void __cdecl bFree(SlotPool*, void* slot)", o_bFreeSlot);
NFSU2_REWRITE_N(0x00440B40, NewSlotPool, 4, "SlotPool* __cdecl SlotPool::NewSlotPool(u32 size, int n, const char* name, u32 memflags)", o_NewSlotPool);
NFSU2_REWRITE_THIS(0x00440BB0, ExpandSlotPool, 1, "void SlotPool::ExpandSlotPool(int n)", o_ExpandSlotPool);
NFSU2_REWRITE_THIS(0x00440D40, SlotPoolMalloc, 0, "void* SlotPool::Malloc()", o_SlotPoolMalloc);
NFSU2_REWRITE_N(0x00440E50, bMallocSlot, 1, "void* __cdecl bMalloc(SlotPool*)", o_bMallocSlot);
NFSU2_REWRITE_THIS(0x00440C30, GetAllocatedSlot, 1, "void* SlotPool::GetAllocatedSlot(int n)", o_GetAllocatedSlot);

// ============================================================================================ diff tests
namespace {

// A chain of 1-3 pools laid out in one buffer (pure-memory functions): run the original, snapshot, restore the
// buffer, run ours on the SAME memory, compare byte for byte (pointers are then identical too).
struct PoolBuf {
    std::vector<uint8_t> mem;
    std::vector<SlotPool*> pools;
    PoolBuf(game::Rng& r) {
        const int npools = 1 + static_cast<int>(r.below(3));
        const uint32_t size = 4 * (1 + r.below(6));
        // sizes first (later pools may use a bigger slot size: GetSlotNumber quirk), then the buffer that holds them
        // all - sizing it with the base slot size overran it and corrupted the heap (game deadlock, 2026-11-15)
        std::vector<int32_t> ns;
        std::vector<uint32_t> sizes;
        size_t total = 0;
        for (int i = 0; i < npools; ++i) {
            ns.push_back(static_cast<int32_t>(r.below(12)));
            sizes.push_back(i == 0 || r.below(4) ? size : size + 4);
            total += 0x30 + sizes.back() * ns.back() + 16;
        }
        mem.resize(total);
        for (auto& b : mem) b = static_cast<uint8_t>(r.next());
        size_t at = 0;
        int32_t all = 0;
        for (int i = 0; i < npools; ++i) {
            auto* p = reinterpret_cast<SlotPool*>(mem.data() + at);
            p->num_slots = ns[i];
            p->slot_size = sizes[i];
            p->next = nullptr;
            p->flags = r.next();
            p->allocated = static_cast<int32_t>(r.below(8));
            p->most_allocated = static_cast<int32_t>(r.below(12));
            if (i) pools.back()->next = p;
            pools.push_back(p);
            all += ns[i];
            at += 0x30 + p->slot_size * ns[i] + 16;
        }
        pools[0]->total_slots = all;
        FlushSlotPool(pools[0]);  // a valid starting free list
    }
    void* slot(game::Rng& r) {  // a slot address inside some pool, or a pointer outside them
        SlotPool* p = pools[r.below(static_cast<uint32_t>(pools.size()))];
        if (p->num_slots == 0 || r.below(6) == 0) return mem.data() + r.below(static_cast<uint32_t>(mem.size()));
        return p->data() + r.below(static_cast<uint32_t>(p->num_slots)) * p->slot_size;
    }
};

}  // namespace

NFSU2_DIFF_TEST(SlotPoolMemory) {
    return game::run_diff(ctx, "SlotPoolMemory", o_FastMalloc, [](game::Rng& r) {
        PoolBuf b(r);
        // a random op sequence; the same sequence runs once with the originals, once with ours
        struct Op { uint32_t kind; uint32_t pool; void* slot; };
        std::vector<Op> ops;
        std::vector<void*> taken;
        for (int i = 0; i < 12; ++i) ops.push_back({r.below(9), r.below(static_cast<uint32_t>(b.pools.size())), b.slot(r)});
        const std::vector<uint8_t> start = b.mem;
        auto run = [&](bool ours) {
            std::vector<int64_t> v;
            std::vector<void*> got;
            for (const Op& o : ops) {
                SlotPool* p = b.pools[o.pool];
                switch (o.kind) {
                    case 0: got.push_back(ours ? FastMalloc(p) : o_FastMalloc(p)); v.push_back(reinterpret_cast<int64_t>(got.back())); break;
                    case 1: got.push_back(ours ? bOMalloc(p) : o_bOMalloc(p)); v.push_back(reinterpret_cast<int64_t>(got.back())); break;
                    case 2: if (!got.empty() && got.back()) { ours ? SlotPoolFree(p, got.back()) : o_SlotPoolFree(p, got.back()); got.pop_back(); } break;
                    case 3: if (!got.empty() && got.back()) { ours ? bFreeSlot(p, got.back()) : o_bFreeSlot(p, got.back()); got.pop_back(); } break;
                    case 4: ours ? FlushSlotPool(b.pools[0]) : o_FlushSlotPool(b.pools[0]); break;
                    case 5: v.push_back(ours ? GetSlotNumber(b.pools[0], o.slot) : o_GetSlotNumber(b.pools[0], o.slot)); break;
                    case 6: v.push_back(ours ? bIsSlotPoolFull(b.pools[0]) : o_bIsSlotPoolFull(b.pools[0])); break;
                    case 7: v.push_back(ours ? bCountFreeSlots(b.pools[0]) : o_bCountFreeSlots(b.pools[0])); break;
                    default: v.push_back(ours ? bCountTotalSlots(b.pools[0]) : o_bCountTotalSlots(b.pools[0])); break;
                }
            }
            v.push_back(ours ? GetSlotNumber(nullptr, b.mem.data()) : o_GetSlotNumber(nullptr, b.mem.data()));
            ours ? FlushSlotPool(nullptr) : o_FlushSlotPool(nullptr);
            return v;
        };
        const auto va = run(false);
        const std::vector<uint8_t> after_a = b.mem;
        b.mem = start;
        const auto vb = run(true);
        if (va != vb) return game::MismatchText("results");
        return after_a == b.mem ? game::Ok() : game::MismatchText("pool memory");
    });
}

// NewSlotPool / Malloc (expanding) / ExpandSlotPool / GetAllocatedSlot / bMalloc on pools in two identical test heaps
NFSU2_DIFF_TEST(SlotPoolAlloc) {
    using game::testing::TestHeap;
    return game::run_diff(ctx, "SlotPoolAlloc", o_SlotPoolMalloc, [](game::Rng& r) -> game::Case {
        if (!game::testing::slot7_free()) return game::MismatchText("slot 7 in use");
        const uint32_t seed = r.next();
        TestHeap ha(48 * 1024), hb(48 * 1024);
        auto run = [seed](TestHeap& h, bool ours) {
            game::Rng rr(seed);
            h.install();
            std::vector<int64_t> v;
            const uint32_t size = 4 * (1 + rr.below(8));
            const int32_t n = static_cast<int32_t>(rr.below(10));
            SlotPool* p = ours ? NewSlotPool(size, n, "t", 7) : o_NewSlotPool(size, n, "t", 7);
            v.push_back(static_cast<int64_t>(h.off(p)));
            if (p) {
                p->flags = rr.below(4) ? 0xB : rr.next() & 0xF;
                std::vector<void*> got;
                for (int i = 0; i < 20; ++i) {
                    const uint32_t k = rr.below(5);
                    if (k <= 1) {
                        got.push_back(k == 0 ? (ours ? SlotPoolMalloc(p) : o_SlotPoolMalloc(p)) : (ours ? bMallocSlot(p) : o_bMallocSlot(p)));
                        v.push_back(static_cast<int64_t>(h.off(got.back())));
                        if (got.back()) {  // the slot's contents (flag 2 zeroes it) - checking only the address missed a half-zeroing bug
                            // first word: a free-list link unless zeroed -> compare it as a heap offset
                            const auto* c = static_cast<const uint8_t*>(got.back());
                            v.push_back(static_cast<int64_t>(h.off(*static_cast<void* const*>(got.back()))));
                            uint64_t sum = 0;
                            for (uint32_t k2 = 4; k2 < p->slot_size; ++k2) sum = sum * 131 + c[k2];
                            v.push_back(static_cast<int64_t>(sum));
                            std::memset(got.back(), 0xA5, p->slot_size);  // dirty it so a later reuse shows zeroing
                        }
                    } else if (k == 2 && !got.empty() && got.back()) {
                        o_SlotPoolFree(p, got.back());
                        got.pop_back();
                    } else if (k == 3) {
                        ours ? ExpandSlotPool(p, 1 + static_cast<int32_t>(rr.below(4))) : o_ExpandSlotPool(p, 1 + static_cast<int32_t>(rr.below(4)));
                    } else {
                        void* s = ours ? GetAllocatedSlot(p, static_cast<int32_t>(rr.below(6))) : o_GetAllocatedSlot(p, static_cast<int32_t>(rr.below(6)));
                        v.push_back(static_cast<int64_t>(h.off(s)));
                    }
                }
                for (SlotPool* q = p; q; q = q->next) {  // the chain, pointers as heap offsets
                    for (int32_t f : {static_cast<int32_t>(q->flags), q->allocated, q->most_allocated, static_cast<int32_t>(q->mem_flags), q->num_slots,
                                      static_cast<int32_t>(q->slot_size), q->total_slots})
                        v.push_back(f);
                    v.push_back(static_cast<int64_t>(h.off(q->free_list)));
                    for (void* f = q->free_list; f; f = *static_cast<void**>(f)) v.push_back(static_cast<int64_t>(h.off(f)));
                }
            }
            v.push_back(h.num_allocs());
            h.uninstall();
            return v;
        };
        const auto a = run(ha, false), b = run(hb, true);
        return a == b ? game::Ok() : game::MismatchText("seed " + std::to_string(seed));
    });
}

NFSU2_DIFF_TEST(regs_slotpool) {
    using game::arg;
    return game::run_diff(ctx, "regs_slotpool", o_bIsSlotPoolFull, [](game::Rng& r) {
        PoolBuf b(r);
        SlotPool* p = b.pools[0];
        for (auto c : {game::check_regs("bIsSlotPoolFull", (void*)o_bIsSlotPoolFull, NFSU2_ENTRY(bIsSlotPoolFull), {arg(p)}, false),
                       game::check_regs("bCountFreeSlots", (void*)o_bCountFreeSlots, NFSU2_ENTRY(bCountFreeSlots), {arg(p)}, false),
                       game::check_regs_this("GetSlotNumber", (void*)o_GetSlotNumber, NFSU2_ENTRY(GetSlotNumber), p, p, {arg(b.slot(r))})})
            if (!c.ok) return c;
        return game::Ok();
    });
}
