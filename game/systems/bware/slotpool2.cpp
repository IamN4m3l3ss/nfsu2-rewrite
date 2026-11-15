// bWare slot pool, part 2: expansion cleanup, the SlotPoolManager (global at 0x828530) and the bPNode pool.
// Adapted from nfsu2-gcpc (CC0, github.com/octavioCuatrochio/nfsu2-gcpc, port/game/bWare/bSlotPool.cpp); PC ABI
// re-checked against SPEED2.EXE (bNewSlotPool inlines the manager's NewSlotPool, bPNode::Free inlines Free +
// bPListClose) and proven with our own diff tests.
//
//   SlotPoolManager {int enabled; SlotPool* head; SlotPool* tail}: pools are linked through SlotPool +00/+04 with the
//   manager's +04 as the list sentinel; only linked/unlinked when enabled.
//   bPNode pool: SlotPool* at 0x8284F0 ("bPNode SlotPool", 12-byte nodes), "want to close" flag at 0x8284F4.
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
};
struct Manager {
    int32_t enabled;
    SlotPool* head;
    SlotPool* tail;
    SlotPool* sentinel() { return reinterpret_cast<SlotPool*>(&head); }
};
constexpr uintptr_t kManager = 0x00828530, kPNodePool = 0x008284F0, kPNodeWantClose = 0x008284F4;

using SlotNum_t = int32_t(__thiscall*)(SlotPool*, const void*);
using GetAlloc_t = void*(__thiscall*)(SlotPool*, int32_t);
using New_t = SlotPool*(__cdecl*)(uint32_t, int32_t, const char*, uint32_t);
using Malloc_t = void*(__thiscall*)(SlotPool*);
inline int32_t slot_number(SlotPool* p, const void* f) { return game_fn<SlotNum_t>(0x0043CD80)(p, f); }
inline void bware_free(void* p) { game_fn<void(__cdecl*)(void*)>(0x00440540)(p); }

// ---------------------------------------------------------------- SlotPool::CleanupExpandedSlotPools 0x00440650
// recount each expansion pool's allocations (slots minus its free-list entries), then unlink and free every
// expansion pool with none allocated (its entries removed from the free list, total slots reduced)
using M_t = void(__thiscall*)(SlotPool*);
M_t o_CleanupExpanded = nullptr;
void __cdecl CleanupExpanded(SlotPool* self) {
    if (!self->next) return;
    for (SlotPool* p = self->next; p; p = p->next) p->allocated = p->num_slots;
    for (void* f = self->free_list; f; f = *static_cast<void**>(f)) {
        for (SlotPool* p = self->next; p; p = p->next) {
            const int32_t i = slot_number(p, f);
            if (i >= 0 && i < p->num_slots) {
                --p->allocated;
                break;
            }
        }
    }
    SlotPool* prev = self;
    for (SlotPool* p = self->next; p;) {
        SlotPool* nx = p->next;
        if (p->allocated == 0) {
            void** link = &self->free_list;
            for (void* f = *link; f;) {
                void* fn = *static_cast<void**>(f);
                const int32_t i = slot_number(p, f);
                if (i >= 0 && i < p->num_slots)
                    *link = fn;
                else
                    link = static_cast<void**>(f);
                f = fn;
            }
            self->total_slots -= p->num_slots;
            prev->next = p->next;
            if (p) bware_free(p);  // SlotPool::DeleteSlotPool (inlined)
        } else {
            prev = p;
        }
        p = nx;
    }
}

// ---------------------------------------------------------------- SlotPoolManager
using MgrM_t = Manager*(__thiscall*)(Manager*);
MgrM_t o_ManagerCtor = nullptr;
Manager* __cdecl ManagerCtor(Manager* self) {  // 0x004407B0: enabled, empty list
    self->head = self->tail = self->sentinel();
    self->enabled = 1;
    return self;
}
using MgrV_t = void(__thiscall*)(Manager*);
MgrV_t o_ManagerDtor = nullptr;
void __cdecl ManagerDtor(Manager* self) {  // 0x004407D0: the list is emptied BEFORE the delete loop, so nothing is freed
    self->enabled = 0;
    self->head = self->tail = self->sentinel();
}
MgrV_t o_ManagerCleanup = nullptr;
void __cdecl ManagerCleanup(Manager* self) {  // 0x00440800
    for (SlotPool* p = self->head; p != self->sentinel(); p = p->list_next) game_fn<M_t>(0x00440650)(p);
}
using MgrDel_t = void(__thiscall*)(Manager*, SlotPool*);
MgrDel_t o_ManagerDeleteSlotPool = nullptr;
void __cdecl ManagerDeleteSlotPool(Manager* self, SlotPool* pool) {  // 0x00440DA0: unlink, leak check, free the chain
    if (!pool) return;
    if (self->enabled != 0) {
        SlotPool* nx = pool->list_next;
        SlotPool* pv = pool->list_prev;
        pv->list_next = nx;
        nx->list_prev = pv;
    }
    if ((pool->flags & 8) && pool->allocated > 0) game_fn<GetAlloc_t>(0x00440C30)(pool, 0);  // leak report; unused
    while (pool) {
        SlotPool* nx = pool->next;
        bware_free(pool);
        pool = nx;
    }
}

// ---------------------------------------------------------------- bNewSlotPool 0x00440DF0, bDeleteSlotPool 0x00440E40
using BNew_t = SlotPool*(__cdecl*)(int32_t, int32_t, const char*, uint32_t);
BNew_t o_bNewSlotPool = nullptr;
SlotPool* __cdecl bNewSlotPool(int32_t slot_size, int32_t num_slots, const char* name, uint32_t mem_flags) {
    SlotPool* p = game_fn<New_t>(0x00440B40)(static_cast<uint32_t>(slot_size + 3) & ~3u, num_slots, name, mem_flags);
    if (!p) return nullptr;
    Manager* m = reinterpret_cast<Manager*>(kManager);
    if (m->enabled == 1) {  // AddTail
        SlotPool* tail = m->tail;
        tail->list_next = p;
        m->tail = p;
        p->list_prev = tail;
        p->list_next = m->sentinel();
    }
    return p;
}
using BDel_t = void(__cdecl*)(SlotPool*);
BDel_t o_bDeleteSlotPool = nullptr;
void __cdecl bDeleteSlotPool(SlotPool* pool) { game_fn<MgrDel_t>(0x00440DA0)(reinterpret_cast<Manager*>(kManager), pool); }

// ---------------------------------------------------------------- bPList / bPNode
using Init_t = void(__cdecl*)(int32_t);
Init_t o_bPListInit = nullptr;
void __cdecl bPListInit(int32_t num_slots) {  // 0x00440E60: create the node pool once
    if (game_var<SlotPool*>(kPNodePool)) return;
    game_var<SlotPool*>(kPNodePool) = game_fn<BNew_t>(0x00440DF0)(0xC, num_slots, reinterpret_cast<const char*>(0x00786F40), 0);
    game_var<int32_t>(kPNodeWantClose) = 0;
}
using PMalloc_t = void*(__cdecl*)();
PMalloc_t o_bPNodeMalloc = nullptr;
void* __cdecl bPNodeMalloc() {  // 0x00440EC0
    if (!game_var<SlotPool*>(kPNodePool)) game_fn<Init_t>(0x00440E60)(0x100);
    return game_fn<Malloc_t>(0x00440D40)(game_var<SlotPool*>(kPNodePool));
}
using PFree_t = void(__cdecl*)(void*);
PFree_t o_bPNodeFree = nullptr;
void __cdecl bPNodeFree(void* node) {  // 0x00440EF0: Free inlined, then bPListClose (inlined) if wanted
    SlotPool* p = game_var<SlotPool*>(kPNodePool);
    *static_cast<void**>(node) = p->free_list;
    --p->allocated;
    p->free_list = node;
    if (!game_var<int32_t>(kPNodeWantClose)) return;
    p = game_var<SlotPool*>(kPNodePool);
    if (!p) return;
    if (p->allocated == 0) {
        game_fn<MgrDel_t>(0x00440DA0)(reinterpret_cast<Manager*>(kManager), p);
        game_var<SlotPool*>(kPNodePool) = nullptr;
    } else {
        game_var<int32_t>(kPNodeWantClose) = 1;
    }
}

}  // namespace

NFSU2_REWRITE_THIS(0x00440650, CleanupExpanded, 0, "void SlotPool::CleanupExpandedSlotPools()", o_CleanupExpanded);
NFSU2_REWRITE_THIS(0x004407B0, ManagerCtor, 0, "SlotPoolManager* SlotPoolManager::SlotPoolManager()", o_ManagerCtor);
NFSU2_REWRITE_THIS(0x004407D0, ManagerDtor, 0, "void SlotPoolManager::~SlotPoolManager()", o_ManagerDtor);
NFSU2_REWRITE_THIS(0x00440800, ManagerCleanup, 0, "void SlotPoolManager::CleanupExpandedSlotPools()", o_ManagerCleanup);
NFSU2_REWRITE_THIS(0x00440DA0, ManagerDeleteSlotPool, 1, "void SlotPoolManager::DeleteSlotPool(SlotPool*)", o_ManagerDeleteSlotPool);
NFSU2_REWRITE_N(0x00440DF0, bNewSlotPool, 4, "SlotPool* __cdecl(int size, int n, const char* name, u32 memflags)", o_bNewSlotPool);
NFSU2_REWRITE_N(0x00440E40, bDeleteSlotPool, 1, "void __cdecl(SlotPool*)", o_bDeleteSlotPool);
NFSU2_REWRITE_N(0x00440E60, bPListInit, 1, "void __cdecl(int num_slots)", o_bPListInit);
NFSU2_REWRITE_N(0x00440EC0, bPNodeMalloc, 0, "void* __cdecl bPNode::Malloc()", o_bPNodeMalloc);
NFSU2_REWRITE_N(0x00440EF0, bPNodeFree, 1, "void __cdecl bPNode::Free(void*)", o_bPNodeFree);

// ============================================================================================ diff tests
namespace {
using game::testing::TestHeap;
using New_t2 = SlotPool*(__cdecl*)(uint32_t, int32_t, const char*, uint32_t);
using Expand_t = void(__thiscall*)(SlotPool*, int32_t);

// a pool chain in heap h (memflags 7 = the installed test heap), with random allocations; built with the original
// functions so both runs start identical
SlotPool* build_chain(game::Rng& r, std::vector<void*>& all_taken) {
    std::vector<void*> taken;  // this chain's slots only (sharing one list across chains freed foreign/null slots)
    SlotPool* p = game_fn<New_t2>(0x00440B40)(4 * (1 + r.below(6)), static_cast<int32_t>(1 + r.below(6)), "t", 7);
    for (int e = 0, n = static_cast<int>(r.below(4)); e < n; ++e) game_fn<Expand_t>(0x00440BB0)(p, 1 + static_cast<int32_t>(r.below(4)));
    for (int i = 0, n = static_cast<int>(r.below(12)); i < n; ++i)
        if (void* s = game_fn<void*(__thiscall*)(SlotPool*)>(0x0043CDD0)(p)) taken.push_back(s);
    for (size_t i = 0; i < taken.size(); ++i)  // free some again, in random order
        if (r.below(2)) {
            game_fn<void(__thiscall*)(SlotPool*, void*)>(0x0043CDF0)(p, taken[i]);
            taken[i] = nullptr;
        }
    p->flags = r.below(3) ? 0xB : 0x3;
    all_taken.insert(all_taken.end(), taken.begin(), taken.end());
    return p;
}
std::vector<int64_t> chain_state(const TestHeap& h, SlotPool* p) {
    std::vector<int64_t> v;
    for (SlotPool* q = p; q; q = q->next) {
        v.push_back(static_cast<int64_t>(h.off(q)));
        for (int32_t f : {static_cast<int32_t>(q->flags), q->allocated, q->num_slots, static_cast<int32_t>(q->slot_size), q->total_slots})
            v.push_back(f);
        for (void* f = q->free_list; f; f = *static_cast<void**>(f)) v.push_back(static_cast<int64_t>(h.off(f)));
        v.push_back(-7);
    }
    v.push_back(h.num_allocs());
    v.push_back(h.used());
    return v;
}

}  // namespace

NFSU2_DIFF_TEST(SlotPoolCleanup) {  // SlotPool::CleanupExpandedSlotPools + SlotPoolManager methods on a fake manager
    return game::run_diff(ctx, "SlotPoolCleanup", o_CleanupExpanded, [](game::Rng& r) -> game::Case {
        if (!game::testing::slot7_free()) return game::MismatchText("slot 7 in use");
        const uint32_t seed = r.next(), op = r.below(4);
        TestHeap ha(64 * 1024), hb(64 * 1024);
        auto run = [seed, op](TestHeap& h, bool ours) {
            game::Rng rr(seed);
            h.install();
            std::vector<void*> taken;
            std::vector<int64_t> v;
            Manager m{};
            ManagerCtor(&m);  // (either implementation; the ctor is compared separately below)
            SlotPool* p = build_chain(rr, taken);
            SlotPool* p2 = build_chain(rr, taken);
            for (SlotPool* q : {p, p2}) {  // register both in the fake manager
                SlotPool* tail = m.tail;
                tail->list_next = q;
                m.tail = q;
                q->list_prev = tail;
                q->list_next = m.sentinel();
            }
            if (op == 0) {
                ours ? CleanupExpanded(p) : o_CleanupExpanded(p);
                v = chain_state(h, p);
            } else if (op == 1) {
                ours ? ManagerCleanup(&m) : o_ManagerCleanup(&m);
                v = chain_state(h, p);
                auto v2 = chain_state(h, p2);
                v.insert(v.end(), v2.begin(), v2.end());
            } else if (op == 2) {
                m.enabled = static_cast<int32_t>(rr.below(3));
                ours ? ManagerDeleteSlotPool(&m, p) : o_ManagerDeleteSlotPool(&m, p);
                v = chain_state(h, p2);
                v.push_back(static_cast<int64_t>(h.off(m.head)));
                v.push_back(static_cast<int64_t>(h.off(m.tail)));
                v.push_back(m.head == m.sentinel() ? 1 : m.head == p2 ? 2 : 3);
            } else {
                Manager x{7, reinterpret_cast<SlotPool*>(1), reinterpret_cast<SlotPool*>(2)};
                ours ? ManagerCtor(&x) : o_ManagerCtor(&x);
                v = {x.enabled, x.head == x.sentinel(), x.tail == x.sentinel()};
                ours ? ManagerDtor(&x) : o_ManagerDtor(&x);
                v.push_back(x.enabled), v.push_back(x.head == x.sentinel()), v.push_back(x.tail == x.sentinel());
            }
            h.uninstall();
            return v;
        };
        const auto a = run(ha, false), b = run(hb, true);
        return a == b ? game::Ok() : game::MismatchText("op " + std::to_string(op) + " seed " + std::to_string(seed));
    });
}

// bNewSlotPool / bDeleteSlotPool use the REAL manager: the pool is appended to the game's list and removed again
NFSU2_DIFF_TEST(bNewDeleteSlotPool) {
    return game::run_diff(ctx, "bNewDeleteSlotPool", o_bNewSlotPool, [](game::Rng& r) -> game::Case {
        if (!game::testing::slot7_free()) return game::MismatchText("slot 7 in use");
        Manager* m = reinterpret_cast<Manager*>(kManager);
        const Manager saved = *m;
        const int32_t size = static_cast<int32_t>(r.below(40)), n = static_cast<int32_t>(r.below(10));
        TestHeap ha(16 * 1024), hb(16 * 1024);
        auto run = [&](TestHeap& h, bool ours) {
            h.install();
            std::vector<int64_t> v;
            SlotPool* p = ours ? bNewSlotPool(size, n, "t", 7) : o_bNewSlotPool(size, n, "t", 7);
            v.push_back(static_cast<int64_t>(h.off(p)));
            if (p) {
                v.push_back(static_cast<int32_t>(p->slot_size));
                v.push_back(m->tail == p);
                v.push_back(p->list_next == m->sentinel());
                v.push_back(p->list_prev == saved.tail);
                ours ? bDeleteSlotPool(p) : o_bDeleteSlotPool(p);
            }
            v.push_back(m->tail == saved.tail && m->head == saved.head);  // the game's list is back as it was
            v.push_back(h.num_allocs());
            h.uninstall();
            return v;
        };
        const auto a = run(ha, false), b = run(hb, true);
        *m = saved;  // (already restored by the delete; belt and braces)
        if (saved.tail != m->sentinel()) saved.tail->list_next = m->sentinel();
        return a == b ? game::Ok() : game::MismatchText("size " + std::to_string(size) + " n " + std::to_string(n));
    });
}

// bPNode::Malloc then Free on the game's real node pool restores it exactly (Malloc pops the head, Free pushes it
// back); bPListInit with the pool present is a no-op. The want-to-close path is checked by transcription only.
NFSU2_DIFF_TEST(bPNode) {
    return game::run_diff(ctx, "bPNode", o_bPNodeMalloc, [](game::Rng& r) -> game::Case {
        SlotPool* pool = game_var<SlotPool*>(kPNodePool);
        if (!pool || game_var<int32_t>(kPNodeWantClose)) return game::Ok();
        const void* head = pool->free_list;
        const int32_t alloc = pool->allocated;
        void* a = o_bPNodeMalloc();
        o_bPNodeFree(a);
        void* b = bPNodeMalloc();
        bPNodeFree(b);
        const int32_t n = static_cast<int32_t>(r.below(512));
        o_bPListInit(n);
        bPListInit(n);
        if (a != b) return game::MismatchText("Malloc result");
        if (pool->free_list != head || pool->allocated != alloc || game_var<SlotPool*>(kPNodePool) != pool) return game::MismatchText("pool state");
        return game::Ok();
    });
}
