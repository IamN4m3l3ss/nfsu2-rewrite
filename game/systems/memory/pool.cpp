// EA bMemory (NA v1.2): fixed table of 8 pools + optional external allocators, first/best-fit free lists.
//
//   bMemoryPool (0x58 bytes, static array at 0x828218, table of pointers at 0x828154):
//     +00 name   +04 free list {next,prev} (sorted by address)   +0C live allocations {next,prev}
//     +14 OS blocks {next,prev} (grown from the CRT heap)        +1C allocs  +20 total allocs  +24 total bytes
//     +28 used   +2C peak used   +30 free = total-used   +34 total-peak   +38 debug fill (0xAA alloc / 0xEE free)
//     +3C critical-section wrapper (CRITICAL_SECTION at +40; thiscall wrappers 0x6F55C7 init / DD enter /
//         E8 leave / D2 delete, each on ECX+4)
//   free block   {next, prev, size, 0x44443333}                   OS block {next, prev, mem, size} (operator new)
//   allocation:  [pad][name space, unused][header 0x14][user data]
//     header {next, prev, u8 pool, u8 0x22, u16 pad+name space, u32 block size, u32 requested size}
//   external allocators at 0x82802C[8]: {ctx, base, size, alloc(ctx,size,name,line,flags), free(ctx,ptr)}
//   flags: &7 pool, &8 take from the bottom of a block (else top), &0x10 best fit (else first fit),
//          (flags>>6)&0x1FFC alignment (0 -> 16), (flags>>17)&0x1FFC alignment offset
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"
#include "test_heap.h"

namespace {

struct Node {
    Node* next;
    Node* prev;
};
struct FreeBlock {
    FreeBlock* next;
    FreeBlock* prev;
    uint32_t size, magic;
};
struct OsBlock {
    OsBlock* next;
    OsBlock* prev;
    uint8_t* mem;
    int32_t size;
};
struct AllocHeader {
    AllocHeader* next;
    AllocHeader* prev;
    uint8_t pool, magic;
    uint16_t pad;
    uint32_t size, requested;
};
struct Pool {
    const char* name;
    Node free_list, allocs, os;
    int32_t num_allocs, total_allocs, total, used, peak, free_bytes, lowest_free;
    uint8_t debug_fill, flag39;
    uint8_t pad3a[2];
    uint8_t cs[0x18 + 4];
};
static_assert(sizeof(Pool) == 0x58, "bMemoryPool size");
struct ExtAllocator {
    void* ctx;
    int32_t base, size;
    void*(__cdecl* alloc)(void* ctx, int size, const char* name, int line, int flags);
    void(__cdecl* free)(void* ctx, void* p);
};

constexpr uintptr_t kPoolTable = 0x00828154, kPoolArray = 0x00828218, kExtTable = 0x0082802C;
constexpr uintptr_t kMemInit = 0x00828510, kAllocCount = 0x00828504, kValidateEvery = 0x008284FC;
constexpr uintptr_t kBreakOnAlloc = 0x007EB3D4, kReleaseOsBlocks = 0x007EB3CC, kMinGrow = 0x007EB3D0;
constexpr uint32_t kFreeMagic = 0x44443333;

using CsFn = void(__thiscall*)(void*);
inline void cs_enter(Pool* p) { game_fn<CsFn>(0x006F55DD)(reinterpret_cast<uint8_t*>(p) + 0x3C); }
inline void cs_leave(Pool* p) { game_fn<CsFn>(0x006F55E8)(reinterpret_cast<uint8_t*>(p) + 0x3C); }
inline void* crt_new(uint32_t n) { return game_fn<void*(__cdecl*)(uint32_t)>(0x0075C6C8)(n); }
inline void crt_free(void* p) { game_fn<void(__cdecl*)(void*)>(0x0075C6DA)(p); }
inline Pool*& pool_slot(uint32_t i) { return game_var<Pool*>(kPoolTable + i * 4); }
using game::testing::g_slot7_ours;  // shared with every test that uses slot 7 (test_heap.h)
inline void slot7_set(Pool* p) { game::testing::slot7_set(p); }
inline bool slot7_free() { return game::testing::slot7_free(); }
inline void bbreak() { game_fn<void(__cdecl*)()>(0x0043BD50)(); }

inline void fill(uint8_t* d, uint32_t value, uint32_t n) {  // rep stosd + rep stosb
    __asm {
        mov edi, d
        mov eax, value
        mov ecx, n
        mov edx, ecx
        shr ecx, 2
        rep stosd
        mov ecx, edx
        and ecx, 3
        rep stosb
    }
}
template <class T> inline void unlink(T* n) {
    n->prev->next = n->next;
    n->next->prev = n->prev;
}

using AddFree_t = void(__thiscall*)(Pool*, uint8_t*, uint32_t);
using Allocate_t = uint8_t*(__thiscall*)(Pool*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t*);
using Init_t = void(__thiscall*)(Pool*, uint8_t*, int32_t, const char*);
using Destroy_t = void(__thiscall*)(Pool*);
inline void add_free(Pool* p, uint8_t* m, uint32_t n) { game_fn<AddFree_t>(0x0043FA50)(p, m, n); }

// ---------------------------------------------------------------- bMemoryPool::AddFreeMemory 0x0043FA50
AddFree_t o_AddFree = nullptr;
void __cdecl PoolAddFree(Pool* self, uint8_t* mem, uint32_t size) {
    if (size == 0) return;
    cs_enter(self);
    FreeBlock* const head = reinterpret_cast<FreeBlock*>(&self->free_list);
    FreeBlock* b = reinterpret_cast<FreeBlock*>(mem);
    b->size = size;
    b->magic = kFreeMagic;
    FreeBlock* n = head->next;
    while (n != head && !(reinterpret_cast<uintptr_t>(n) > reinterpret_cast<uintptr_t>(b))) n = n->next;
    FreeBlock* p = n->prev;
    p->next = b;
    n->prev = b;
    b->prev = p;
    b->next = n;
    uint8_t* fill_from = mem + 0x10;
    uint8_t* fill_to = mem + b->size;
    FreeBlock* merged = b;
    if (fill_to == reinterpret_cast<uint8_t*>(n)) {  // merge with the next block
        b->size += n->size;
        fill_to = reinterpret_cast<uint8_t*>(n) + 0x10;
        unlink(n);
    }
    p = b->prev;
    if (reinterpret_cast<uint8_t*>(p) + p->size == mem) {  // merge into the previous block
        p->size += b->size;
        fill_from = mem;
        unlink(b);
        merged = p;
    }
    if (self->debug_fill) fill(fill_from, 0xEEEEEEEE, static_cast<uint32_t>(fill_to - fill_from));
    if (game_var<int32_t>(kReleaseOsBlocks)) {  // a whole OS block is free again: give it back
        OsBlock* const oh = reinterpret_cast<OsBlock*>(&self->os);
        for (OsBlock* o = oh->next; o != oh; o = o->next) {
            if (o->mem == reinterpret_cast<uint8_t*>(merged) && static_cast<uint32_t>(o->size) == merged->size) {
                self->total -= o->size;
                unlink(merged);
                unlink(o);
                game_fn<void(__thiscall*)(OsBlock*)>(0x0043E850)(o);  // frees o->mem
                crt_free(o);
                break;
            }
        }
    }
    cs_leave(self);
}

// ---------------------------------------------------------------- bMemoryPool::Allocate 0x0043FC80
Allocate_t o_Allocate = nullptr;
uint8_t* __cdecl PoolAllocate(Pool* self, int32_t size, int32_t align, int32_t align_off, int32_t from_bottom,
                              int32_t best_fit, int32_t* out_size) {
    FreeBlock* const head = reinterpret_cast<FreeBlock*>(&self->free_list);
    for (;;) {
        cs_enter(self);
        size = (size + 3) & ~3;
        if (size < 16) size = 16;
        FreeBlock* best = nullptr;
        int32_t best_pad = 0, best_waste = 0x7FFFFFFF;
        if (from_bottom) {
            for (FreeBlock* n = head->next; n != head; n = n->next) {
                const int32_t x = static_cast<int32_t>((static_cast<uint32_t>(align_off) + reinterpret_cast<uint32_t>(n)) &
                                                       static_cast<uint32_t>(align - 1));
                const int32_t pad = (align - x) % align;
                const int32_t waste = static_cast<int32_t>(n->size) - pad - size;
                if ((waste == 0 || waste >= 16) && waste < best_waste) {
                    best_waste = waste;
                    best = n;
                    best_pad = pad;
                    if (!best_fit) break;
                }
            }
        } else {
            for (FreeBlock* n = head->prev; n != head; n = n->prev) {
                const uint32_t sz = n->size;
                const int32_t x = static_cast<int32_t>((sz - static_cast<uint32_t>(size) + reinterpret_cast<uint32_t>(n) +
                                                        static_cast<uint32_t>(align_off)) &
                                                       static_cast<uint32_t>(align - 1));
                const int32_t pad = x % align;
                const int32_t waste = static_cast<int32_t>(sz) - pad - size;
                if ((waste == 0 || waste >= 16) && waste < best_waste) {
                    best = n;
                    best_waste = waste;
                    best_pad = pad;
                    if (!best_fit) break;
                }
            }
        }
        if (best) {
            size += best_pad;
            uint8_t* res;
            if (!from_bottom) {  // take the top of the block
                best->size -= size;
                res = reinterpret_cast<uint8_t*>(best) + best->size;
                if (best->size == 0) unlink(best);
            } else {  // take the bottom; the rest stays a free block
                const uint32_t rest = best->size - size;
                if (rest != 0) {
                    FreeBlock* nb = reinterpret_cast<FreeBlock*>(reinterpret_cast<uint8_t*>(best) + size);
                    nb->size = rest;
                    nb->magic = kFreeMagic;
                    FreeBlock* c = best->next;
                    best->next = nb;
                    c->prev = nb;
                    nb->prev = best;
                    nb->next = c;
                }
                unlink(best);
                res = reinterpret_cast<uint8_t*>(best);
            }
            if (self->debug_fill) fill(res, 0xAAAAAAAA, static_cast<uint32_t>(size));
            ++self->num_allocs;
            ++self->total_allocs;
            self->used += size;
            if (self->used > self->peak) self->peak = self->used;
            self->free_bytes = self->total - self->used;
            self->lowest_free = self->total - self->peak;
            cs_leave(self);
            *out_size = size;
            return res;
        }
        // nothing fits: grow by an OS block (at least *0x7EB3D0 bytes) and retry
        cs_leave(self);
        int32_t want = size + align + 16;
        if (want < game_var<int32_t>(kMinGrow)) want = game_var<int32_t>(kMinGrow);
        OsBlock* o = static_cast<OsBlock*>(crt_new(16));
        if (o) {
            o->size = want;
            o->mem = static_cast<uint8_t*>(crt_new(static_cast<uint32_t>(want)));
        }
        if (!o->mem) {  // (the original dereferences o here too)
            crt_free(o->mem);
            crt_free(o);
            return nullptr;
        }
        self->total += o->size;
        add_free(self, o->mem, static_cast<uint32_t>(o->size));
        OsBlock* const oh = reinterpret_cast<OsBlock*>(&self->os);
        OsBlock* t = oh->prev;
        t->next = o;
        oh->prev = o;
        o->prev = t;
        o->next = oh;
    }
}

// ---------------------------------------------------------------- bMemoryPool::Init 0x004402C0
Init_t o_PoolInit = nullptr;
void __cdecl PoolInit(Pool* self, uint8_t* mem, int32_t size, const char* name) {
    self->free_list.next = self->free_list.prev = &self->free_list;
    self->allocs.next = self->allocs.prev = &self->allocs;
    self->os.next = self->os.prev = &self->os;
    self->num_allocs = self->total_allocs = self->total = self->used = self->peak = self->free_bytes =
        self->lowest_free = 0;
    game_fn<CsFn>(0x006F55C7)(reinterpret_cast<uint8_t*>(self) + 0x3C);  // InitializeCriticalSection
    self->debug_fill = 1;
    self->flag39 = 1;
    self->name = name;
    self->total += size;
    add_free(self, mem, static_cast<uint32_t>(size));
}

// ---------------------------------------------------------------- bMemoryPool::Destroy 0x0043F9A0
Destroy_t o_PoolDestroy = nullptr;
void __cdecl PoolDestroy(Pool* self) {
    OsBlock* const oh = reinterpret_cast<OsBlock*>(&self->os);
    while (oh->next != oh) {
        OsBlock* o = oh->next;
        unlink(o);
        game_fn<void(__thiscall*)(OsBlock*)>(0x0043E850)(o);
        crt_free(o);
    }
    game_fn<CsFn>(0x006F55D2)(reinterpret_cast<uint8_t*>(self) + 0x3C);  // DeleteCriticalSection
}

}  // namespace

NFSU2_REWRITE_THIS(0x0043FA50, PoolAddFree, 2, "void bMemoryPool::AddFreeMemory(u8* mem, u32 size) (sorted, merges)", o_AddFree);
NFSU2_REWRITE_THIS(0x0043FC80, PoolAllocate, 6,
                   "u8* bMemoryPool::Allocate(int size, int align, int align_off, int from_bottom, int best_fit, int* out_size)",
                   o_Allocate);
NFSU2_REWRITE_THIS(0x004402C0, PoolInit, 3, "void bMemoryPool::Init(u8* mem, int size, const char* name)", o_PoolInit);
NFSU2_REWRITE_THIS(0x0043F9A0, PoolDestroy, 0, "void bMemoryPool::Destroy() (frees OS blocks, deletes CS)", o_PoolDestroy);

// ============================================================================================ global API
namespace {

// ---------------------------------------------------------------- bMalloc 0x00440390
using Malloc_t = void*(__cdecl*)(int32_t, const char*, int32_t, int32_t);
Malloc_t o_bMalloc = nullptr;
void* __cdecl bMalloc(int32_t size, const char* name, int32_t line, int32_t flags) {
    if (name && game_fn<int(__cdecl*)(const char*, const char*)>(0x0043DCC0)(name, reinterpret_cast<const char*>(0x00786F00)) == 0) {
        name = nullptr;  // "RCMP::chunkbuff" (bStrICmp)
        line = 0;
    }
    const uint32_t idx = static_cast<uint32_t>(flags) & 7;
    if (ExtAllocator* e = game_var<ExtAllocator*>(kExtTable + idx * 4))
        return e->alloc(e->ctx, size, name, line, flags);
    game_var<const char*>(0x00828508) = name;
    game_var<int32_t>(0x0082850C) = line;
    if (!game_var<int32_t>(kMemInit)) {
        Pool* main = reinterpret_cast<Pool*>(kPoolArray);
        pool_slot(0) = main;
        game_fn<Init_t>(0x004402C0)(main, nullptr, 0, reinterpret_cast<const char*>(0x00786EF4));  // "Main Pool"
        game_var<int32_t>(kMemInit) = 1;
    }
    if (const int32_t every = game_var<int32_t>(kValidateEvery))
        if (game_var<int32_t>(kAllocCount) % every == 0 && pool_slot(idx))
            game_fn<void(__thiscall*)(Pool*, int)>(0x0043FEF0)(pool_slot(idx), 1);
    int32_t align = (flags >> 6) & 0x1FFC;
    if (align == 0) align = 16;
    const int32_t align_off = (flags >> 17) & 0x1FFC;
    int32_t namelen = 0;
    if (name && *name) namelen = static_cast<int32_t>(std::strlen(name));
    Pool* pool = pool_slot(idx);
    const int32_t hdr = (namelen + 10) & ~3;
    const int32_t from_bottom = flags & 8;
    const int32_t total_off = hdr + align_off + 0x14;
    int32_t got = 0;
    uint8_t* res = game_fn<Allocate_t>(0x0043FC80)(pool, hdr + size + 0x14, align, total_off, from_bottom, flags & 0x10, &got);
    if (!res) {
        bbreak();
        return nullptr;
    }
    int32_t pad = 0;
    if (from_bottom) pad = (align - static_cast<int32_t>((static_cast<uint32_t>(total_off) + reinterpret_cast<uint32_t>(res)) & static_cast<uint32_t>(align - 1))) % align;
    pad += hdr;
    AllocHeader* tail = reinterpret_cast<AllocHeader*>(pool->allocs.prev);
    AllocHeader* h = reinterpret_cast<AllocHeader*>(res + pad);
    tail->next = h;
    pool->allocs.prev = reinterpret_cast<Node*>(h);
    h->pad = static_cast<uint16_t>(pad);
    h->prev = tail;
    h->next = reinterpret_cast<AllocHeader*>(&pool->allocs);
    h->pool = static_cast<uint8_t>(flags & 7);
    h->magic = 0x22;
    h->size = static_cast<uint32_t>(got);
    h->requested = static_cast<uint32_t>(size);
    if (game_var<int32_t>(kAllocCount) == game_var<int32_t>(kBreakOnAlloc)) bbreak();
    ++game_var<int32_t>(kAllocCount);
    return reinterpret_cast<uint8_t*>(h) + 0x14;
}

// ---------------------------------------------------------------- bMalloc(size, flags) 0x00440820
using Malloc2_t = void*(__cdecl*)(int32_t, int32_t);
Malloc2_t o_bMalloc2 = nullptr;
void* __cdecl bMalloc2(int32_t size, int32_t flags) { return game_fn<Malloc_t>(0x00440390)(size, nullptr, 0, flags); }

// ---------------------------------------------------------------- bFree 0x00440540
using Free_t = void(__cdecl*)(void*);
Free_t o_bFree = nullptr;
void __cdecl bFree(void* ptr) {
    if (!ptr) return;
    const int32_t p = static_cast<int32_t>(reinterpret_cast<uintptr_t>(ptr));
    for (uint32_t i = 0; i < 8; ++i) {
        ExtAllocator* e = game_var<ExtAllocator*>(kExtTable + i * 4);
        if (e && p >= e->base && p < e->base + e->size) {
            e->free(e->ctx, ptr);
            return;
        }
    }
    AllocHeader* h = reinterpret_cast<AllocHeader*>(static_cast<uint8_t*>(ptr) - 0x14);
    const uint32_t idx = h->pool;
    if (const int32_t every = game_var<int32_t>(kValidateEvery))
        if (game_var<int32_t>(kAllocCount) % every == 0 && pool_slot(idx))
            game_fn<void(__thiscall*)(Pool*, int)>(0x0043FEF0)(pool_slot(idx), 1);
    if (h->magic != 0x22) {
        bbreak();
        return;
    }
    AllocHeader* prev = h->prev;
    AllocHeader* next = h->next;
    h->magic = 0;
    Pool* pool = pool_slot(idx);
    prev->next = next;
    next->prev = prev;
    uint8_t* start = reinterpret_cast<uint8_t*>(h) - h->pad;
    const int32_t size = static_cast<int32_t>(h->size);
    cs_enter(pool);
    add_free(pool, start, static_cast<uint32_t>(size));
    const int32_t used = pool->used - size;
    pool->free_bytes = pool->total - used;
    --pool->num_allocs;
    pool->used = used;
    cs_leave(pool);
    if (game_var<int32_t>(kMemInit) && pool_slot(0)->num_allocs == 0) {
        game_fn<Destroy_t>(0x0043F9A0)(pool_slot(0));
        pool_slot(0) = nullptr;
        game_var<int32_t>(kMemInit) = 0;
    }
}

// ---------------------------------------------------------------- bInitMemoryPool 0x00440330
using InitPool_t = void(__cdecl*)(int32_t, uint8_t*, int32_t, const char*);
InitPool_t o_bInitMemoryPool = nullptr;
void __cdecl bInitMemoryPool(int32_t idx, uint8_t* mem, int32_t size, const char* name) {
    Pool* p = reinterpret_cast<Pool*>(kPoolArray + idx * 0x58);
    pool_slot(static_cast<uint32_t>(idx)) = p;
    game_fn<Init_t>(0x004402C0)(p, mem, size, name);
}

// ---------------------------------------------------------------- bCloseMemoryPool 0x0043FB80
using ClosePool_t = void(__cdecl*)(int32_t);
ClosePool_t o_bCloseMemoryPool = nullptr;
void __cdecl bCloseMemoryPool(int32_t idx) {
    game_fn<Destroy_t>(0x0043F9A0)(pool_slot(static_cast<uint32_t>(idx)));
    pool_slot(static_cast<uint32_t>(idx)) = nullptr;
}

}  // namespace

NFSU2_REWRITE_N(0x00440390, bMalloc, 4, "void* __cdecl(int size, const char* name, int line, int flags)", o_bMalloc);
NFSU2_REWRITE_N(0x00440820, bMalloc2, 2, "void* __cdecl(int size, int flags)", o_bMalloc2);
NFSU2_REWRITE_N(0x00440540, bFree, 1, "void __cdecl(void* p)", o_bFree);
NFSU2_REWRITE_N(0x00440330, bInitMemoryPool, 4, "void __cdecl(int idx, u8* mem, int size, const char* name)", o_bInitMemoryPool);
NFSU2_REWRITE_N(0x0043FB80, bCloseMemoryPool, 1, "void __cdecl(int idx)", o_bCloseMemoryPool);

// ============================================================================================ diff tests
// Two private pools on two arenas with the same alignment (mod 4096) run the same operations, one through the
// original code and one through ours. After every operation the pool state is compared with every pointer
// normalised to (region, offset): region 0 = the arena, 1.. = OS blocks in list order, 0x100 = the pool object.
namespace {

struct Arena {
    std::vector<uint8_t> buf;
    uint8_t* base;
    uint32_t size;
    Arena(uint32_t n, uint32_t mis) : buf(n + 8192), size(n) {
        base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(buf.data()) + 4095) & ~uintptr_t(4095)) + mis;
        for (uint32_t i = 0; i < n; ++i) base[i] = static_cast<uint8_t>(i * 7 + 3);
    }
};
struct TestPool {
    Pool pool{};
    Arena arena;
    TestPool(uint32_t n, uint32_t mis) : arena(n, mis) {}
};

uint64_t norm(const TestPool& t, const void* q) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(q);
    const uintptr_t b = reinterpret_cast<uintptr_t>(t.arena.base);
    if (a >= b && a <= b + t.arena.size) return (a - b);  // inclusive: a size-0 block can end at the region's end
    const uintptr_t pb = reinterpret_cast<uintptr_t>(&t.pool);
    if (a >= pb && a < pb + sizeof(Pool)) return (0x100ull << 32) | (a - pb);
    const OsBlock* oh = reinterpret_cast<const OsBlock*>(&t.pool.os);
    uint64_t k = 1;
    for (const OsBlock* o = oh->next; o != oh && k < 64; o = o->next, ++k) {
        const uintptr_t m = reinterpret_cast<uintptr_t>(o->mem);
        if (a >= m && a <= m + static_cast<uint32_t>(o->size)) return (k << 32) | (a - m);
    }
    return (0xFFFFull << 32) | a;  // anything else (null, or an OS block header itself)
}

std::vector<uint64_t> snapshot(const TestPool& t) {
    std::vector<uint64_t> v;
    const Pool& p = t.pool;
    for (int32_t x : {p.num_allocs, p.total_allocs, p.total, p.used, p.peak, p.free_bytes, p.lowest_free})
        v.push_back(static_cast<uint32_t>(x));
    v.push_back(p.debug_fill);
    v.push_back(p.flag39);
    v.push_back(0xF0F0);
    const FreeBlock* fh = reinterpret_cast<const FreeBlock*>(&p.free_list);
    int guard = 0;
    for (const FreeBlock* n = fh->next; n != fh && guard < 100000; n = n->next, ++guard) {
        v.push_back(norm(t, n));
        v.push_back(n->size);
        v.push_back(n->magic);
        v.push_back(norm(t, n->prev));
    }
    v.push_back(norm(t, fh->prev));
    v.push_back(0xA110C);
    const AllocHeader* ah = reinterpret_cast<const AllocHeader*>(&p.allocs);
    guard = 0;
    for (const AllocHeader* h = ah->next; h != ah && guard < 100000; h = h->next, ++guard) {
        v.push_back(norm(t, h));
        v.push_back((uint64_t(h->pool) << 24) | (uint64_t(h->magic) << 16) | h->pad);
        v.push_back(h->size);
        v.push_back(h->requested);
        v.push_back(norm(t, h->prev));
    }
    v.push_back(norm(t, ah->prev));
    v.push_back(0x05);
    const OsBlock* oh = reinterpret_cast<const OsBlock*>(&p.os);
    for (const OsBlock* o = oh->next; o != oh; o = o->next) v.push_back(static_cast<uint32_t>(o->size));
    return v;
}

// Region bytes compared dword by dword (both regions have the same alignment mod 4): equal, or both words are
// pointers into the same normalised place (free-list / header links are absolute addresses into their own pool).
bool same_region(const TestPool& a, const TestPool& b, const uint8_t* ra, const uint8_t* rb, uint32_t n,
                 const char* what, std::string& where) {
    if (std::memcmp(ra, rb, n) == 0) return true;
    const uint32_t lead = (4 - (reinterpret_cast<uintptr_t>(ra) & 3)) & 3;
    for (uint32_t k = 0; k < n;) {
        if (k < lead || k + 4 > n) {
            if (ra[k] != rb[k]) {
                where = std::string(what) + " byte " + std::to_string(k);
                return false;
            }
            ++k;
            continue;
        }
        uint32_t wa, wb;
        std::memcpy(&wa, ra + k, 4);
        std::memcpy(&wb, rb + k, 4);
        if (wa != wb) {
            const uint64_t na = norm(a, reinterpret_cast<void*>(wa)), nb = norm(b, reinterpret_cast<void*>(wb));
            // both pointing outside every live region (e.g. a stale link to a released OS block) counts as equal
            if (na != nb && !((na >> 32) == 0xFFFF && (nb >> 32) == 0xFFFF)) {
                where = std::string(what) + " dword at " + std::to_string(k);
                return false;
            }
        }
        k += 4;
    }
    return true;
}

// The address order of the regions (arena, OS blocks): the free list is sorted by address, so runs whose heap gave
// the OS blocks a different order relative to each other/the arena legitimately diverge from that point on.
std::vector<int> region_order(const TestPool& t) {
    std::vector<std::pair<uintptr_t, int>> v{{reinterpret_cast<uintptr_t>(t.arena.base), 0}};
    const OsBlock* oh = reinterpret_cast<const OsBlock*>(&t.pool.os);
    int k = 1;
    for (const OsBlock* o = oh->next; o != oh; o = o->next) v.push_back({reinterpret_cast<uintptr_t>(o->mem), k++});
    std::sort(v.begin(), v.end());
    std::vector<int> r;
    for (auto& e : v) r.push_back(e.second);
    return r;
}

// arena bytes + every OS block's bytes (OS blocks only with debug fill on: otherwise unwritten parts are
// uninitialised heap memory)
bool same_memory(const TestPool& a, const TestPool& b, std::string& where) {
    if (!same_region(a, b, a.arena.base, b.arena.base, a.arena.size, "arena", where)) return false;
    if (!a.pool.debug_fill) return true;
    const OsBlock* ha = reinterpret_cast<const OsBlock*>(&a.pool.os);
    const OsBlock* hb = reinterpret_cast<const OsBlock*>(&b.pool.os);
    int k = 0;
    for (const OsBlock *x = ha->next, *y = hb->next; x != ha && y != hb; x = x->next, y = y->next, ++k) {
        if (x->size != y->size) {
            where = "os block size " + std::to_string(k);
            return false;
        }
        if (!same_region(a, b, x->mem, y->mem, static_cast<uint32_t>(x->size), "os block", where)) return false;
    }
    return true;
}

int32_t rand_align(game::Rng& r) {
    static const int32_t a[] = {16, 16, 16, 32, 64, 128, 256, 4096};
    return a[r.below(8)];
}

struct PoolOps {
    void (*init)(Pool*, uint8_t*, int32_t, const char*);
    void (*add_free)(Pool*, uint8_t*, uint32_t);
    uint8_t* (*allocate)(Pool*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t*);
    void (*destroy)(Pool*);
};
void oi(Pool* p, uint8_t* m, int32_t n, const char* s) { o_PoolInit(p, m, n, s); }
void oa(Pool* p, uint8_t* m, uint32_t n) { o_AddFree(p, m, n); }
uint8_t* ol(Pool* p, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t* f) { return o_Allocate(p, a, b, c, d, e, f); }
void od(Pool* p) { o_PoolDestroy(p); }
const PoolOps kOrigOps{&oi, &oa, &ol, &od};
const PoolOps kOurOps{&PoolInit, &PoolAddFree, &PoolAllocate, &PoolDestroy};

}  // namespace

NFSU2_DIFF_TEST(bMemoryPool) {
    return game::run_diff(ctx, "bMemoryPool", o_Allocate, [](game::Rng& r) -> game::Case {
        // OS blocks (growth) come from the CRT heap at unrelated addresses that are only 8-byte aligned, so cases that
        // may grow use alignment 4/8 (identical padding in both runs); the others get an arena that never runs out.
        const bool grow = r.below(3) == 0;
        const uint32_t n = grow ? 256 + r.below(8) * 1024 + r.below(64) * 4 : 32 * 1024 + r.below(64) * 4;
        const uint32_t mis = r.below(64) * 4;
        TestPool a(n, mis), b(n, mis);
        kOrigOps.init(&a.pool, a.arena.base, static_cast<int32_t>(n), "test");
        kOurOps.init(&b.pool, b.arena.base, static_cast<int32_t>(n), "test");
        a.pool.debug_fill = b.pool.debug_fill = static_cast<uint8_t>(r.below(2));
        (void)rand_align;
        std::vector<std::pair<uint8_t*, uint8_t*>> live;  // (a, b) block starts
        std::vector<int32_t> live_size;
        std::string step = "init";
        bool diverged = false;
        game::Case result = game::Ok();
        for (int op = 0; op < 40 && result.ok; ++op) {
            if (!live.empty() && r.below(3) == 0) {
                const uint32_t i = r.below(static_cast<uint32_t>(live.size()));
                step = "free #" + std::to_string(op) + " size " + std::to_string(live_size[i]);
                kOrigOps.add_free(&a.pool, live[i].first, static_cast<uint32_t>(live_size[i]));
                kOurOps.add_free(&b.pool, live[i].second, static_cast<uint32_t>(live_size[i]));
                live.erase(live.begin() + i);
                live_size.erase(live_size.begin() + i);
            } else {
                const int32_t size = grow && r.below(10) == 0 ? static_cast<int32_t>(n + r.below(4096)) : static_cast<int32_t>(r.below(600));
                const int32_t align = grow ? (r.below(2) ? 4 : 8) : rand_align(r), off = static_cast<int32_t>(r.below(17) * 4);
                const int32_t bottom = r.below(2) ? 8 : 0, best = r.below(2) ? 0x10 : 0;
                int32_t sa = -1, sb = -1;
                step = "alloc #" + std::to_string(op) + " size " + std::to_string(size) + " align " + std::to_string(align) +
                       " off " + std::to_string(off) + (bottom ? " bottom" : " top") + (best ? " best" : " first");
                uint8_t* ra = kOrigOps.allocate(&a.pool, size, align, off, bottom, best, &sa);
                uint8_t* rb = kOurOps.allocate(&b.pool, size, align, off, bottom, best, &sb);
                if (region_order(a) != region_order(b) || (!grow && a.pool.os.next != &a.pool.os)) {
                    diverged = true;  // heap placed the new OS block differently: stop comparing here
                    break;
                }
                if (norm(a, ra) != norm(b, rb) || sa != sb) {
                    result = game::MismatchText(step + ": result differs");
                    break;
                }
                if (ra) {
                    live.push_back({ra, rb});
                    live_size.push_back(sa);
                }
            }
            if (snapshot(a) != snapshot(b)) result = game::MismatchText(step + ": pool state differs");
        }
        std::string where;
        if (result.ok && !diverged && !same_memory(a, b, where))
            result = game::MismatchText(step + ": memory differs at " + where);
        kOrigOps.destroy(&a.pool);
        kOrigOps.destroy(&b.pool);
        return result;
    });
}

// bMalloc / bMalloc2 / bFree through pool slot 7 (unused by the game), with random names and flags.
NFSU2_DIFF_TEST(bMalloc) {
    return game::run_diff(ctx, "bMalloc", o_bMalloc, [](game::Rng& r) -> game::Case {
        if (!slot7_free()) return game::MismatchText("slot 7 in use");
        const bool grow = r.below(3) == 0;  // see bMemoryPool: growing cases use alignment 4/8
        const uint32_t n = grow ? 512 + r.below(16) * 1024 : 32 * 1024, mis = r.below(64) * 4;
        TestPool a(n, mis), b(n, mis);
        o_PoolInit(&a.pool, a.arena.base, static_cast<int32_t>(n), "slot7");
        o_PoolInit(&b.pool, b.arena.base, static_cast<int32_t>(n), "slot7");
        a.pool.debug_fill = b.pool.debug_fill = static_cast<uint8_t>(r.below(2));  // off: freed headers stay visible
        static const char* const kNames[] = {nullptr, "", "abc", "RCMP::chunkbuff", "rcmp::CHUNKBUFF", "CarTexturePack",
                                             "a much longer allocation name for the header"};
        struct Op {
            int kind;  // 0 bMalloc, 1 bMalloc2, 2 bFree
            int32_t size, line, flags;
            const char* name;
            uint32_t victim;
        };
        std::vector<Op> ops;
        uint32_t live = 0;
        for (int i = 0; i < 30; ++i) {
            Op o{};
            if (live && r.below(3) == 0) {
                o.kind = 2;
                o.victim = r.below(live);
                --live;
            } else {
                o.kind = r.below(4) == 0 ? 1 : 0;
                o.size = grow && r.below(10) == 0 ? static_cast<int32_t>(n) : static_cast<int32_t>(r.below(700));
                o.name = kNames[r.below(7)];
                o.line = static_cast<int32_t>(r.below(3000));
                static const int32_t al[] = {0, 0, 16, 32, 64, 128, 1024, 4096};
                const int32_t align = grow ? (r.below(2) ? 4 : 8) : al[r.below(8)];
                o.flags = 7 | (r.below(2) ? 8 : 0) | (r.below(2) ? 0x10 : 0) | (align << 6) | (static_cast<int32_t>(r.below(17) * 4) << 17);
                ++live;
            }
            ops.push_back(o);
        }
        // run all ops on one pool with the given functions; record normalised results + state after each op
        struct Run {
            std::vector<std::vector<uint64_t>> steps;
            std::vector<std::vector<int>> orders;
        };
        auto run = [&ops](TestPool& t, Malloc_t m, Malloc2_t m2, Free_t f) {
            Run out;
            std::vector<void*> ptrs;
            slot7_set(&t.pool);
            for (const Op& o : ops) {
                if (o.kind == 2) {
                    uint8_t* hp = static_cast<uint8_t*>(ptrs[o.victim]) - 0x14;
                    f(ptrs[o.victim]);
                    ptrs.erase(ptrs.begin() + o.victim);
                    out.steps.push_back(snapshot(t));
                    // the freed header's {pool, magic, pad} word (bFree clears the magic) - unless the free gave
                    // its whole OS block back to the heap: then that memory is gone
                    uint32_t w = 0xDEADDEAD;
                    if ((norm(t, hp) >> 32) != 0xFFFF) std::memcpy(&w, hp + 8, 4);
                    out.steps.back().push_back(w);
                } else {
                    void* p = o.kind == 0 ? m(o.size, o.name, o.line, o.flags) : m2(o.size, o.flags);
                    ptrs.push_back(p);
                    out.steps.push_back(snapshot(t));
                    out.steps.back().push_back(norm(t, p));
                }
                out.orders.push_back(region_order(t));
            }
            slot7_set(nullptr);
            return out;
        };
        const auto ta = run(a, o_bMalloc, o_bMalloc2, o_bFree);
        const auto tb = run(b, &bMalloc, &bMalloc2, &bFree);
        game::Case res = game::Ok();
        std::string where;
        bool diverged = false;
        for (size_t k = 0; k < ta.steps.size(); ++k) {
            // see region_order; a non-growing case that grew anyway (arena full) uses alignments >= 16 on heap memory
            if (ta.orders[k] != tb.orders[k] || (!grow && (ta.orders[k].size() > 1 || tb.orders[k].size() > 1))) {
                diverged = true;
                break;
            }
            if (ta.steps[k] != tb.steps[k]) {
                const Op& o = ops[k];
                size_t i = 0;
                const auto &x = ta.steps[k], &y = tb.steps[k];
                while (i < x.size() && i < y.size() && x[i] == y[i]) ++i;
                char buf[96];
                snprintf(buf, sizeof buf, " at %u/%u: %llx vs %llx", (unsigned)i, (unsigned)x.size(),
                         i < x.size() ? (unsigned long long)x[i] : 0ull, i < y.size() ? (unsigned long long)y[i] : 0ull);
                res = game::MismatchText("op " + std::to_string(k) + " kind " + std::to_string(o.kind) + " size " +
                                         std::to_string(o.size) + " flags " + std::to_string(o.flags) + ": state differs" + buf);
                break;
            }
        }
        if (!res.ok || diverged) {
        } else if (!same_memory(a, b, where)) {
            res = game::MismatchText("memory differs at " + where);
        }
        o_PoolDestroy(&a.pool);
        o_PoolDestroy(&b.pool);
        return res;
    });
}

// bInitMemoryPool / bCloseMemoryPool on slot 7 (static pool object 0x828480)
NFSU2_DIFF_TEST(bMemoryPoolSlots) {
    return game::run_diff(ctx, "bMemoryPoolSlots", o_bInitMemoryPool, [](game::Rng& r) -> game::Case {
        if (!slot7_free()) return game::MismatchText("slot 7 in use");
        const uint32_t n = 64 + r.below(4096);
        Arena ar(n, r.below(64) * 4);
        Pool* const p7 = reinterpret_cast<Pool*>(kPoolArray + 7 * 0x58);
        std::vector<uint8_t> s1(0x58), s2(0x58);
        g_slot7_ours = true;
        o_bInitMemoryPool(7, ar.base, static_cast<int32_t>(n), "slot7");
        const bool slot_a = pool_slot(7) == p7;
        std::memcpy(s1.data(), p7, 0x3C);
        o_bCloseMemoryPool(7);
        const bool null_a = pool_slot(7) == nullptr;
        for (uint32_t i = 0; i < n; ++i) ar.base[i] = static_cast<uint8_t>(i * 7 + 3);
        bInitMemoryPool(7, ar.base, static_cast<int32_t>(n), "slot7");
        const bool slot_b = pool_slot(7) == p7;
        std::memcpy(s2.data(), p7, 0x3C);
        bCloseMemoryPool(7);
        const bool null_b = pool_slot(7) == nullptr;
        g_slot7_ours = false;
        if (slot_a != slot_b || null_a != null_b) return game::MismatchText("slot pointer");
        if (s1 != s2) return game::MismatchText("pool object n=" + std::to_string(n));
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(regs_memory) {
    using game::arg;
    return game::run_diff(ctx, "regs_memory", o_Allocate, [](game::Rng& r) -> game::Case {
        TestPool a(4096, 0), b(4096, 0);
        o_PoolInit(&a.pool, a.arena.base, 4096, "r");
        o_PoolInit(&b.pool, b.arena.base, 4096, "r");
        int32_t sa = 0;
        const uint32_t size = r.below(500);
        game::Case cs[] = {
            game::check_regs_this("Allocate", (void*)o_Allocate, NFSU2_ENTRY(PoolAllocate), &a.pool, &b.pool,
                                  {size, 16, 0, 8, 0, arg(&sa)}, false),
            game::check_regs_this("AddFree(0)", (void*)o_AddFree, NFSU2_ENTRY(PoolAddFree), &a.pool, &b.pool,
                                  {arg(a.arena.base), 0}, false),
            game::check_regs("bFree(null)", (void*)o_bFree, NFSU2_ENTRY(bFree), {0}, false, false),
        };
        o_PoolDestroy(&a.pool);
        o_PoolDestroy(&b.pool);
        for (auto& c : cs)
            if (!c.ok) return c;
        // bMalloc's EAX is the block pointer (checked normalised by the bMalloc test); here: ECX/EDX + stack only
        if (!slot7_free()) return game::MismatchText("slot 7 in use");
        TestPool t(8192, 0);
        o_PoolInit(&t.pool, t.arena.base, 8192, "r");
        slot7_set(&t.pool);
        game::Case m = game::check_regs("bMalloc", (void*)o_bMalloc, NFSU2_ENTRY(bMalloc), {64, 0, 0, 7}, false, false);
        game::Case m2 = game::check_regs("bMalloc2", (void*)o_bMalloc2, NFSU2_ENTRY(bMalloc2), {64, 7}, false, false);
        slot7_set(nullptr);
        o_PoolDestroy(&t.pool);
        if (!m.ok) return m;
        return m2;
    });
}
