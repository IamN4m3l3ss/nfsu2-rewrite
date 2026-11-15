// bMemory statistics and pool wrappers (EA core library, zBWare.cpp), written from the PC machine code.
//   bMemoryPool::GetAmountFree        0x0043E860  thiscall: sum of free block sizes (under the pool lock)
//   bMemoryPool::GetLargestFreeBlock  0x0043E8A0  thiscall: largest free block (signed compare, from 0)
//   bCountFreeMemory(pool)            0x0043E8E0  pool -> GetAmountFree; else external allocator's +14 (ctx)
//   bLargestMalloc(pool)              0x0043E910  pool 0 -> 0x6300000; pool -> largest - 0xDC (>= 0); else ext +18
//   bAddToMemoryPool(pool, mem, size) 0x00440090  total += size, then AddFreeMemory (0x43FA50)
//   bVerifyPoolIntegrity(pool)        0x004400B0  pool -> VerifyPoolIntegrity(1) (0x43FEF0)
//   bMemoryPool::VerifyPoolIntegrity  0x0043FEF0  thiscall(check_fill): every free block / allocation and its
//       neighbours must pass IsBadReadPtr||IsBadWritePtr (both bad = broken), free blocks carry 0x44443333,
//       allocations 0x22 at +9; with check_fill and the pool's debug fill on, free bytes after the 0x10 header must
//       be 0xEE (else flag 0x82853C = 1). Every failure is an int3 (a hard stop in the shipped game); a set flag
//       ends the free-list walk early.
// Pool layout: see pool.cpp. External allocator table 0x82802C[8]: {ctx, base, size, alloc, free, count_free (+14),
// largest (+18)} - the last two are what these functions call (tail jump with the first argument replaced by ctx).
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"
#include "test_heap.h"

namespace {

struct FreeBlock {
    FreeBlock* next;
    FreeBlock* prev;
    int32_t size;
    uint32_t magic;
};
struct AllocNode {
    AllocNode* next;
    AllocNode* prev;
    uint8_t pool, magic;  // magic 0x22 at +9
};
struct PoolHead {  // the part of bMemoryPool (0x58) these functions use
    const char* name;
    FreeBlock* free_next;  // +04 free list sentinel {next, prev}
    FreeBlock* free_prev;
    AllocNode* alloc_next;  // +0C live allocations {next, prev}
    AllocNode* alloc_prev;
    uint8_t pad14[0x24 - 0x14];
    int32_t total;  // +24
    uint8_t pad28[0x38 - 0x28];
    uint8_t debug_fill;  // +38
};
struct ExtAllocator {
    void* ctx;
    int32_t base, size;
    void* alloc;
    void* free;
    int32_t(__cdecl* count_free)(void* ctx);  // +14
    int32_t(__cdecl* largest)(void* ctx);     // +18
};

constexpr uintptr_t kPoolTable = 0x00828154, kExtTable = 0x0082802C;
inline PoolHead*& pool_slot(uint32_t i) { return game_var<PoolHead*>(kPoolTable + i * 4); }
inline ExtAllocator*& ext_slot(uint32_t i) { return game_var<ExtAllocator*>(kExtTable + i * 4); }
using CsFn = void(__thiscall*)(void*);
inline void cs_enter(PoolHead* p) { game_fn<CsFn>(0x006F55DD)(reinterpret_cast<uint8_t*>(p) + 0x3C); }
inline void cs_leave(PoolHead* p) { game_fn<CsFn>(0x006F55E8)(reinterpret_cast<uint8_t*>(p) + 0x3C); }
FreeBlock* sentinel(PoolHead* p) { return reinterpret_cast<FreeBlock*>(&p->free_next); }

using Stat_t = int32_t(__thiscall*)(PoolHead*);
Stat_t o_PoolGetAmountFree = nullptr;
int32_t __cdecl PoolGetAmountFree(PoolHead* self) {
    cs_enter(self);
    int32_t sum = 0;
    for (FreeBlock* b = self->free_next; b != sentinel(self); b = b->next) sum += b->size;
    cs_leave(self);
    return sum;
}
Stat_t o_PoolGetLargestFree = nullptr;
int32_t __cdecl PoolGetLargestFree(PoolHead* self) {
    cs_enter(self);
    int32_t best = 0;
    for (FreeBlock* b = self->free_next; b != sentinel(self); b = b->next)
        if (b->size > best) best = b->size;
    cs_leave(self);
    return best;
}

using PoolFn_t = int32_t(__cdecl*)(uint32_t);
PoolFn_t o_bCountFreeMemory = nullptr;
int32_t __cdecl bCountFreeMemory(uint32_t pool) {
    if (PoolHead* p = pool_slot(pool)) return PoolGetAmountFree(p);
    ExtAllocator* e = ext_slot(pool);
    return e ? e->count_free(e->ctx) : 0;
}
PoolFn_t o_bLargestMalloc = nullptr;
int32_t __cdecl bLargestMalloc(uint32_t pool) {
    if (PoolHead* p = pool_slot(pool)) {
        if (pool == 0) return 0x6300000;
        const int32_t n = PoolGetLargestFree(p) - 0xDC;
        return n < 0 ? 0 : n;
    }
    ExtAllocator* e = ext_slot(pool);
    return e ? e->largest(e->ctx) : 0;
}

using Add_t = void(__cdecl*)(uint32_t, uint8_t*, int32_t);
Add_t o_bAddToMemoryPool = nullptr;
void __cdecl bAddToMemoryPool(uint32_t pool, uint8_t* mem, int32_t size) {
    PoolHead* p = pool_slot(pool);
    p->total += size;
    game_fn<void(__thiscall*)(PoolHead*, uint8_t*, int32_t)>(0x0043FA50)(p, mem, size);  // AddFreeMemory
}
// ---------------------------------------------------------------- bMemoryPool::VerifyPoolIntegrity 0x0043FEF0
volatile uint8_t* const kCorruptFlag = reinterpret_cast<volatile uint8_t*>(0x0082853C);
// fails only when IsBadReadPtr AND IsBadWritePtr say so (the game's imports, called through its IAT)
bool bad_ptr(const void* q) {
    using Is_t = BOOL(__stdcall*)(const void*, UINT_PTR);
    if (!game_var<Is_t>(0x00783110)(q, 1)) return false;   // IsBadReadPtr
    return game_var<Is_t>(0x0078310C)(q, 1) != 0;           // IsBadWritePtr
}
using VerifyThis_t = void(__thiscall*)(PoolHead*, int32_t);
VerifyThis_t o_PoolVerify = nullptr;
void __cdecl PoolVerify(PoolHead* p, int32_t check_fill) {
    cs_enter(p);
    AllocNode* const asent = reinterpret_cast<AllocNode*>(&p->alloc_next);
    for (FreeBlock* b = p->free_next; b != sentinel(p); b = b->next) {
        if (bad_ptr(b) || bad_ptr(b->next) || bad_ptr(b->prev) || b->magic != 0x44443333u) goto free_failed;
        if (*kCorruptFlag) break;
        if (static_cast<uint8_t>(check_fill) && p->debug_fill) {
            const uint8_t* const end = reinterpret_cast<const uint8_t*>(b) + b->size;
            for (const uint8_t* c = reinterpret_cast<const uint8_t*>(b) + 0x10; c != end; ++c)
                if (*c != 0xEE) {
                    *kCorruptFlag = 1;
                    goto free_failed;
                }
        }
    }
    for (AllocNode* a = p->alloc_next; a != asent; a = a->next)
        if (bad_ptr(a) || bad_ptr(a->next) || bad_ptr(a->prev) || a->magic != 0x22) {
            __debugbreak();
            cs_leave(p);
            return;
        }
    cs_leave(p);
    return;
free_failed:
    __debugbreak();  // the original continues here only under a debugger: sort the allocations, walk them, unlock
    game_fn<void(__thiscall*)(void*, uintptr_t)>(0x0043E650)(&p->alloc_next, 0x0043CAE0);  // bList::Sort
    for (AllocNode* a = p->alloc_next; a != asent; a = a->next) {
    }
    cs_leave(p);
}

using Verify_t = void(__cdecl*)(uint32_t);
Verify_t o_bVerifyPoolIntegrity = nullptr;
void __cdecl bVerifyPoolIntegrity(uint32_t pool) {
    if (PoolHead* p = pool_slot(pool)) PoolVerify(p, 1);
}

}  // namespace

NFSU2_REWRITE_THIS(0x0043E860, PoolGetAmountFree, 0, "int bMemoryPool::GetAmountFree()", o_PoolGetAmountFree);
NFSU2_REWRITE_THIS(0x0043E8A0, PoolGetLargestFree, 0, "int bMemoryPool::GetLargestFreeBlock()", o_PoolGetLargestFree);
NFSU2_REWRITE_N(0x0043E8E0, bCountFreeMemory, 1, "int __cdecl bCountFreeMemory(int pool)", o_bCountFreeMemory);
NFSU2_REWRITE_N(0x0043E910, bLargestMalloc, 1, "int __cdecl bLargestMalloc(int pool)", o_bLargestMalloc);
NFSU2_REWRITE_N(0x00440090, bAddToMemoryPool, 3, "void __cdecl bAddToMemoryPool(int pool, void* mem, int size)", o_bAddToMemoryPool);
NFSU2_REWRITE_THIS(0x0043FEF0, PoolVerify, 1, "void bMemoryPool::VerifyPoolIntegrity(bool check_fill)", o_PoolVerify);
NFSU2_REWRITE_N(0x004400B0, bVerifyPoolIntegrity, 1, "void __cdecl bVerifyPoolIntegrity(int pool)", o_bVerifyPoolIntegrity);

namespace {

using game::testing::TestHeap;
using game::testing::slot7_free;
int32_t __cdecl fake_count(void* ctx) { return static_cast<int32_t>(reinterpret_cast<uintptr_t>(ctx) * 3 + 1); }
int32_t __cdecl fake_largest(void* ctx) { return static_cast<int32_t>(reinterpret_cast<uintptr_t>(ctx) ^ 0x5A5A); }

// the heap's free list and counters as offsets from its arena (two heaps built alike compare equal)
std::vector<int64_t> snapshot(TestHeap& h, uint8_t* extra) {
    std::vector<int64_t> v;
    PoolHead* p = reinterpret_cast<PoolHead*>(h.pool);
    for (uint32_t o = 0x1C; o < 0x38; o += 4) v.push_back(*reinterpret_cast<int32_t*>(h.pool + o));
    auto rel = [&](const void* q) -> int64_t {
        const uintptr_t a = reinterpret_cast<uintptr_t>(q), b = reinterpret_cast<uintptr_t>(h.base),
                        e = reinterpret_cast<uintptr_t>(extra);
        if (a >= e && a < e + 4096) return 0x40000000 + static_cast<int64_t>(a - e);
        return static_cast<int64_t>(a) - static_cast<int64_t>(b);
    };
    int n = 0;
    for (FreeBlock* b = p->free_next; b != sentinel(p) && n < 10000; b = b->next, ++n) {
        v.push_back(rel(b));
        v.push_back(b->size);
        v.push_back(b->magic);
    }
    return v;
}

}  // namespace

NFSU2_DIFF_TEST(bMemoryStats) {
    return game::run_diff(ctx, "bMemoryStats", o_bCountFreeMemory, [](game::Rng& r) -> game::Case {
        const uint32_t which = r.below(4);
        if (which == 0) {  // the live pools: read only
            for (uint32_t pool = 0; pool < 7; ++pool) {
                const int32_t a = o_bCountFreeMemory(pool), b = bCountFreeMemory(pool);
                const int32_t c = o_bLargestMalloc(pool), d = bLargestMalloc(pool);
                // other threads may allocate between the two calls: re-read the original once on a difference
                if (a != b && o_bCountFreeMemory(pool) != bCountFreeMemory(pool))
                    return game::MismatchText("bCountFreeMemory(" + std::to_string(pool) + ")");
                if (c != d && o_bLargestMalloc(pool) != bLargestMalloc(pool))
                    return game::MismatchText("bLargestMalloc(" + std::to_string(pool) + ")");
            }
            return game::Ok();
        }
        if (!slot7_free()) return game::MismatchText("slot 7 in use");
        if (which == 1) {  // slot 7 empty, then an external allocator in it
            if (o_bCountFreeMemory(7) != bCountFreeMemory(7) || o_bLargestMalloc(7) != bLargestMalloc(7))
                return game::MismatchText("empty slot");
            ExtAllocator ext{reinterpret_cast<void*>(static_cast<uintptr_t>(r.next())), 0, 0, nullptr, nullptr, fake_count, fake_largest};
            game::ScopedUndo undo([] { ext_slot(7) = nullptr; });
            ext_slot(7) = &ext;
            const bool ok = o_bCountFreeMemory(7) == bCountFreeMemory(7) && o_bLargestMalloc(7) == bLargestMalloc(7);
            ext_slot(7) = nullptr;
            return ok ? game::Ok() : game::MismatchText("external allocator");
        }
        // two identical private heaps in slot 7: fragment, grow with bAddToMemoryPool, compare everything
        const uint32_t n = 1024 + r.below(16384), mis = r.below(16) * 4;
        TestHeap ha(n, mis), hb(n, mis);
        std::vector<uint32_t> sizes, frees;
        // stay well inside the arena: a heap that runs out grows into new OS blocks at unrelated addresses
        uint32_t budget = n / 3;
        for (int i = 0, k = r.below(12); i < k; ++i) {
            const uint32_t s = 1 + r.below(400);
            if (s + 0x40 > budget) break;
            budget -= s + 0x40;
            sizes.push_back(s);
        }
        for (size_t i = 0; i < sizes.size(); ++i) frees.push_back(r.below(2));
        const int32_t add = which == 3 ? 64 + static_cast<int32_t>(r.below(2048)) : 0;
        alignas(16) static uint8_t extra_a[4096], extra_b[4096];
        auto run = [&](TestHeap& h, uint8_t* extra, bool ours) -> std::vector<int64_t> {
            h.install();
            std::vector<void*> ptrs;
            for (uint32_t s : sizes) ptrs.push_back(game_fn<void*(__cdecl*)(int32_t, int32_t)>(0x00440820)(static_cast<int32_t>(s), 7));
            for (size_t i = 0; i < ptrs.size(); ++i)
                if (frees[i] && ptrs[i]) game_fn<void(__cdecl*)(void*)>(0x00440540)(ptrs[i]);
            if (add) (ours ? bAddToMemoryPool : o_bAddToMemoryPool)(7, extra, add);
            std::vector<int64_t> v = snapshot(h, extra);
            PoolHead* p = reinterpret_cast<PoolHead*>(h.pool);
            v.push_back(ours ? PoolGetAmountFree(p) : o_PoolGetAmountFree(p));
            v.push_back(ours ? PoolGetLargestFree(p) : o_PoolGetLargestFree(p));
            v.push_back(ours ? bCountFreeMemory(7) : o_bCountFreeMemory(7));
            v.push_back(ours ? bLargestMalloc(7) : o_bLargestMalloc(7));
            (ours ? bVerifyPoolIntegrity : o_bVerifyPoolIntegrity)(7);
            {  // VerifyPoolIntegrity directly: fill check with the free bytes made 0xEE, and the "flag set" early exit
                PoolHead* ph = reinterpret_cast<PoolHead*>(h.pool);
                const uint8_t old_fill = ph->debug_fill, old_flag = *kCorruptFlag;
                game::ScopedUndo undo([=] { ph->debug_fill = old_fill; *kCorruptFlag = old_flag; });
                for (FreeBlock* b = ph->free_next; b != sentinel(ph); b = b->next)
                    if (b->size > 0x10) std::memset(reinterpret_cast<uint8_t*>(b) + 0x10, 0xEE, b->size - 0x10);
                ph->debug_fill = 1;
                if (ours) PoolVerify(ph, 1); else o_PoolVerify(ph, 1);
                *kCorruptFlag = 1;
                if (ours) PoolVerify(ph, 1); else o_PoolVerify(ph, 1);
                *kCorruptFlag = old_flag;
                if (ours) PoolVerify(ph, 0); else o_PoolVerify(ph, 0);
                v.push_back(*kCorruptFlag);
            }
            for (size_t i = 0; i < ptrs.size(); ++i)
                if (!frees[i] && ptrs[i]) game_fn<void(__cdecl*)(void*)>(0x00440540)(ptrs[i]);
            h.uninstall();
            return v;
        };
        const auto va = run(ha, extra_a, false);
        const auto vb = run(hb, extra_b, true);
        return va == vb ? game::Ok() : game::MismatchText("heap n=" + std::to_string(n) + " add=" + std::to_string(add));
    });
}

NFSU2_DIFF_TEST(regs_bMemoryStats) {
    return game::run_diff(ctx, "regs_bMemoryStats", o_bLargestMalloc, [](game::Rng& r) {
        const uint32_t pool = r.below(8);
        // callers read EAX of bCountFreeMemory / bLargestMalloc; another thread may allocate in between, so retry
        for (int t = 0; t < 3; ++t) {
            const game::Case a = game::check_regs("bCountFreeMemory", (void*)o_bCountFreeMemory, NFSU2_ENTRY(bCountFreeMemory), {pool}, false);
            const game::Case b = game::check_regs("bLargestMalloc", (void*)o_bLargestMalloc, NFSU2_ENTRY(bLargestMalloc), {pool}, false);
            if (a.ok && b.ok) return game::Ok();
            if (t == 2) return a.ok ? b : a;
        }
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(bMemoryStats_BROKEN) {  // harness self-check: largest without the 0xDC header allowance
    return game::run_diff(ctx, "bMemoryStats_BROKEN", o_bLargestMalloc, [](game::Rng& r) -> game::Case {
        const uint32_t pool = 1 + r.below(6);
        PoolHead* p = pool_slot(pool);
        if (!p) return game::Ok();
        return o_bLargestMalloc(pool) == PoolGetLargestFree(p) ? game::Ok() : game::MismatchText("no 0xDC");
    });
}
