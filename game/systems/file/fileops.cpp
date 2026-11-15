// EA RealFile internals (NA v1.2): FILEOPERATION allocation/construction/ids and the intrusive single lists they live
// in. EA names from the GameCube build (RE kit); PC ABI from the disassembly.
//
//   FILESYSINFO (global, *0x8AD610): +00 ListSingle FreeOps {head, tail, count}  +0C FreeOps mutex wrapper
//                                    +50 NextOp  +54 NextOp mutex  +98 FILEDEVICE* DeviceMemory (0x84 each)
//   FILEOPERATION (0x40): +00 vtable  +08 list link (-> next op's +08)  +0C id  +10 u8 cancelled/running
//                         +11 s8 status  +12 u8 priority  +14 filehandle (-1)  +18 userdata  +1C callback
//                         +20..+30 amount/data/value (0)
//   id = NextOp << 8 | device index; NextOp wraps 0x10000 -> 1
//   ListSingle<FILEOPERATION> {head, tail, count}, links at node+8 pointing at the next node's link field;
//   ListSingle (plain) {head, tail, count}, links at node+0.
#include <cstring>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

struct List {
    uint8_t* head;
    uint8_t* tail;
    int32_t count;
};
inline uint8_t*& link8(uint8_t* node) { return *reinterpret_cast<uint8_t**>(node + 8); }
inline uint8_t* next8(uint8_t* node) { return link8(node) ? link8(node) - 8 : nullptr; }
inline uint8_t*& link0(uint8_t* node) { return *reinterpret_cast<uint8_t**>(node); }
inline uint8_t* fsinfo() { return game_var<uint8_t*>(0x008AD610); }
inline void lock(void* w) { game_fn<void(__thiscall*)(void*)>(0x006F55DD)(w); }
inline void unlock(void* w) { game_fn<void(__thiscall*)(void*)>(0x006F55E8)(w); }

// ---------------------------------------------------------------- ListSingle<FILEOPERATION> (links at +8)
using L1_t = void(__thiscall*)(List*, uint8_t*);
L1_t o_OpListPushBack = nullptr;
void __cdecl OpListPushBack(List* self, uint8_t* op) {  // 0x006F7F14
    uint8_t* tail_link = self->tail ? self->tail + 8 : nullptr;
    link8(op) = nullptr;
    ++self->count;
    self->tail = op;
    if (!tail_link)
        self->head = op;
    else
        *reinterpret_cast<uint8_t**>(tail_link) = op + 8;
}

L1_t o_OpListPushFront = nullptr;
void __cdecl OpListPushFront(List* self, uint8_t* op) {  // 0x006F7EEC
    link8(op) = self->head ? self->head + 8 : nullptr;
    ++self->count;
    self->head = op;
    if (!link8(op)) self->tail = op;
}

using Pop_t = uint8_t*(__thiscall*)(List*);
Pop_t o_OpListPopFirst = nullptr;
uint8_t* __cdecl OpListPopFirst(List* self) {  // 0x006F7F44
    uint8_t* op = self->head;
    if (!op) return nullptr;
    if (op == self->tail) {
        self->tail = nullptr;
        self->head = nullptr;
    } else {
        self->head = next8(op);
    }
    --self->count;
    link8(op) = nullptr;
    return op;
}

using L2_t = void(__thiscall*)(List*, uint8_t*, uint8_t*);
L2_t o_OpListInsertAfter = nullptr;
void __cdecl OpListInsertAfter(List* self, uint8_t* prev, uint8_t* op) {  // 0x006F7F70 (prev null: push front)
    if (!prev) {
        game_fn<L1_t>(0x006F7EEC)(self, op);
        return;
    }
    if (!link8(prev)) self->tail = op;
    link8(op) = link8(prev);
    link8(prev) = op + 8;
    ++self->count;
}

// Remove(op, unused): finds the predecessor itself; returns whether op was in the list (callers test AL only)
using Rm_t = int32_t(__thiscall*)(List*, uint8_t*, int32_t);
Rm_t o_OpListRemove = nullptr;
int32_t __cdecl OpListRemove(List* self, uint8_t* op, int32_t) {  // 0x006F7FA4
    uint8_t* const head = self->head;
    if (op == head) {
        --self->count;
        if (op == self->tail) {
            self->head = nullptr;
            self->tail = nullptr;
        } else {
            self->head = next8(op);
        }
        link8(op) = nullptr;
        return 1;
    }
    if (!head) return 0;
    uint8_t* const target = op ? op + 8 : nullptr;
    uint8_t* p = head;
    if (link8(p)) {
        for (;;) {
            uint8_t* l = link8(p);
            if (l == target) break;
            p = l ? l - 8 : nullptr;
            if (!link8(p)) break;
        }
    }
    uint8_t* l = link8(p);
    if (!l || l != target) return 0;
    --self->count;
    link8(p) = link8(op);
    if (op == self->tail) self->tail = p;
    link8(op) = nullptr;
    return 1;
}

// ---------------------------------------------------------------- ListSingle (links at +0)
L1_t o_ListPushFront = nullptr;
void __cdecl ListPushFront(List* self, uint8_t* node) {  // 0x006F8041
    link0(node) = self->head;
    ++self->count;
    self->head = node;
    if (!link0(node)) self->tail = node;
}

L1_t o_ListPushBack = nullptr;
void __cdecl ListPushBack(List* self, uint8_t* node) {  // 0x006F805C
    uint8_t* old_tail = self->tail;
    link0(node) = nullptr;
    ++self->count;
    self->tail = node;
    if (!old_tail)
        self->head = node;
    else
        link0(old_tail) = node;
}

using M_t = void(__thiscall*)(List*);
M_t o_ListClear = nullptr;
void __cdecl ListClear(List* self) {  // 0x006F6B35 (unlinks every node)
    for (uint8_t* n = self->head; n;) {
        uint8_t* next = link0(n);
        if (!next) break;
        link0(n) = nullptr;
        n = next;
    }
    self->tail = nullptr;
    self->head = nullptr;
    self->count = 0;
}

Pop_t o_ListPopFirst = nullptr;
uint8_t* __cdecl ListPopFirst(List* self) {  // 0x006F6B56
    uint8_t* n = self->head;
    if (!n) return nullptr;
    if (n == self->tail) {
        self->tail = nullptr;
        self->head = nullptr;
    } else {
        self->head = link0(n);
    }
    --self->count;
    link0(n) = nullptr;
    return n;
}

// ---------------------------------------------------------------- FILEOPERATION::operator new 0x006F70AC
// (size argument ignored: every operation is a 0x40 slot from the FreeOps list)
using New_t = uint8_t*(__cdecl*)(uint32_t);
New_t o_FileOpNew = nullptr;
uint8_t* __cdecl FileOpNew(uint32_t) {
    lock(fsinfo() + 0x0C);
    uint8_t* fs = fsinfo();
    uint8_t* op = game_fn<Pop_t>(0x006F7F44)(reinterpret_cast<List*>(fs));
    unlock(fs + 0x0C);
    return op;
}

// ---------------------------------------------------------------- FILEOPERATION::iGetNextOpId 0x006F6C66
using NextId_t = int32_t(__thiscall*)(uint8_t*, uint8_t*);
NextId_t o_iGetNextOpId = nullptr;
int32_t __cdecl iGetNextOpId(uint8_t*, uint8_t* device) {
    lock(fsinfo() + 0x54);
    uint8_t* fs = fsinfo();
    const int32_t n = *reinterpret_cast<int32_t*>(fs + 0x50);
    *reinterpret_cast<int32_t*>(fs + 0x50) = n + 1;
    if (n + 1 == 0x10000) *reinterpret_cast<int32_t*>(fs + 0x50) = 1;
    unlock(fs + 0x54);
    fs = fsinfo();
    const int32_t index = static_cast<int32_t>(device - *reinterpret_cast<uint8_t**>(fs + 0x98)) / 0x84;
    return index | (n << 8);
}

// ---------------------------------------------------------------- FILEOPERATION::FILEOPERATION 0x006F7416
void start_device(uint8_t* device) {  // iStartDevice 0x6F7388 takes the device in ESI
    __asm {
        mov esi, device
        mov eax, 0x006F7388
        call eax
    }
}
using Ctor_t = uint8_t*(__thiscall*)(uint8_t*, int32_t, int32_t, uint8_t*);
Ctor_t o_FileOpCtor = nullptr;
uint8_t* __cdecl FileOpCtor(uint8_t* self, int32_t priority, int32_t userdata, uint8_t* device) {
    *reinterpret_cast<uint32_t*>(self) = 0x007C2EC0;  // FILEOPERATION vtable
    if (*reinterpret_cast<int32_t*>(device + 4) == 0) start_device(device);
    *reinterpret_cast<int32_t*>(self + 0x14) = -1;  // filehandle
    self[0x12] = static_cast<uint8_t>(priority);
    self[0x10] = 0;
    self[0x11] = 0;
    *reinterpret_cast<int32_t*>(self + 0x18) = userdata;
    *reinterpret_cast<uint32_t*>(self + 0x1C) = 0x006F6C46;  // iDefaultFilesysCallbackFunc
    std::memset(self + 0x20, 0, 0x14);
    *reinterpret_cast<int32_t*>(self + 0x0C) = game_fn<NextId_t>(0x006F6C66)(self, device);
    return self;
}

}  // namespace

NFSU2_REWRITE_THIS(0x006F7F14, OpListPushBack, 1, "void ListSingle<FILEOPERATION>::AddTail(FILEOPERATION*)", o_OpListPushBack);
NFSU2_REWRITE_THIS(0x006F7EEC, OpListPushFront, 1, "void ListSingle<FILEOPERATION>::AddHead(FILEOPERATION*)", o_OpListPushFront);
NFSU2_REWRITE_THIS(0x006F7F44, OpListPopFirst, 0, "FILEOPERATION* ListSingle<FILEOPERATION>::RemoveHead()", o_OpListPopFirst);
NFSU2_REWRITE_THIS(0x006F7F70, OpListInsertAfter, 2, "void ListSingle<FILEOPERATION>::InsertAfter(prev, op)", o_OpListInsertAfter);
NFSU2_REWRITE_THIS(0x006F7FA4, OpListRemove, 2, "bool ListSingle<FILEOPERATION>::Remove(op, unused)", o_OpListRemove);
NFSU2_REWRITE_THIS(0x006F8041, ListPushFront, 1, "void ListSingle::AddHead(ListNodeS*)", o_ListPushFront);
NFSU2_REWRITE_THIS(0x006F805C, ListPushBack, 1, "void ListSingle::AddTail(ListNodeS*)", o_ListPushBack);
NFSU2_REWRITE_THIS(0x006F6B35, ListClear, 0, "void LinkedList::Clear()", o_ListClear);
NFSU2_REWRITE_THIS(0x006F6B56, ListPopFirst, 0, "ListNodeS* LinkedList::PopFirst()", o_ListPopFirst);
NFSU2_REWRITE_N(0x006F70AC, FileOpNew, 1, "FILEOPERATION* __cdecl FILEOPERATION::operator new(unsigned) (size ignored)", o_FileOpNew);
NFSU2_REWRITE_THIS(0x006F6C66, iGetNextOpId, 1, "int FILEOPERATION::iGetNextOpId(FILEDEVICE*)", o_iGetNextOpId);
NFSU2_REWRITE_THIS(0x006F7416, FileOpCtor, 3, "FILEOPERATION::FILEOPERATION(int priority, void* userdata, FILEDEVICE*)", o_FileOpCtor);

// ============================================================================================ diff tests
namespace {

// Random lists over a pool of fake nodes; both runs start from an identical copy.
struct FakeLists {
    std::vector<uint8_t> mem;
    uint8_t* nodes;
    List lists[2]{};  // [0] links at +8, [1] links at +0
    static constexpr int kN = 12, kSize = 0x40;
    explicit FakeLists(uint32_t seed) : mem(kN * kSize) {
        nodes = mem.data();
        game::Rng r(seed);
        for (auto& b : mem) b = static_cast<uint8_t>(r.next());
        for (int i = 0; i < kN; ++i) link8(node(i)) = nullptr, link0(node(i)) = nullptr;
        // list 0 (+8 links) takes nodes chosen at random, list 1 (+0 links) separately -> nodes are in at most one
        for (int which = 0; which < 2; ++which) {
            List& l = lists[which];
            for (int i = which; i < kN; i += 2) {
                if (r.below(3) == 0) continue;
                uint8_t* n = node(i);
                if (which == 0) {
                    if (l.tail) link8(l.tail) = n + 8;
                    else l.head = n;
                } else {
                    if (l.tail) link0(l.tail) = n;
                    else l.head = n;
                }
                l.tail = n;
                ++l.count;
            }
        }
    }
    uint8_t* node(int i) { return nodes + i * kSize; }
    uint64_t idx(const uint8_t* n) const { return n ? 1 + (n - nodes) / kSize : 0; }
    std::vector<uint64_t> shape() const {
        std::vector<uint64_t> v;
        for (int which = 0; which < 2; ++which) {
            const List& l = lists[which];
            int guard = 0;
            for (uint8_t* n = l.head; n && guard < 64; ++guard)
                v.push_back(idx(n)), n = which == 0 ? next8(n) : link0(n);
            v.push_back(0xF000 | idx(l.tail));
            v.push_back(static_cast<uint32_t>(l.count));
        }
        for (int i = 0; i < kN; ++i)  // every node's link fields, as node indices
            v.push_back(idx(link8(const_cast<uint8_t*>(nodes + i * kSize)) ? link8(const_cast<uint8_t*>(nodes + i * kSize)) - 8 : nullptr)),
                v.push_back(idx(link0(const_cast<uint8_t*>(nodes + i * kSize))));
        return v;
    }
};
// a node index that is either in the list (op on it is valid) or free (for pushes)
int pick_free(game::Rng& r, FakeLists& f, int which) {
    for (int t = 0; t < 32; ++t) {
        const int i = static_cast<int>(r.below(FakeLists::kN));
        if ((i & 1) != which) continue;
        bool in = false;
        int guard = 0;
        for (uint8_t* n = f.lists[which].head; n && guard < 64; ++guard, n = which == 0 ? next8(n) : link0(n))
            if (n == f.node(i)) in = true;
        if (!in) return i;
    }
    return -1;
}

}  // namespace

NFSU2_DIFF_TEST(FileOpLists) {
    return game::run_diff(ctx, "FileOpLists", o_OpListRemove, [](game::Rng& r) {
        const uint32_t seed = r.next(), op = r.below(9);
        FakeLists a(seed), b(seed);
        game::Rng ra(seed ^ 0x5A5A), rb(seed ^ 0x5A5A);
        uint64_t ra_ret = 0, rb_ret = 0;
        auto run = [op](FakeLists& f, game::Rng& rr, bool ours, uint64_t& ret) {
            List* l8 = &f.lists[0];
            List* l0 = &f.lists[1];
            switch (op) {
                case 0: { const int i = pick_free(rr, f, 0); if (i >= 0) (ours ? OpListPushBack : [](List* l, uint8_t* n) { o_OpListPushBack(l, n); })(l8, f.node(i)); break; }
                case 1: { const int i = pick_free(rr, f, 0); if (i >= 0) (ours ? OpListPushFront : [](List* l, uint8_t* n) { o_OpListPushFront(l, n); })(l8, f.node(i)); break; }
                case 2: ret = f.idx(ours ? OpListPopFirst(l8) : o_OpListPopFirst(l8)); break;
                case 3: {  // insert after a member (or null = front)
                    const int i = pick_free(rr, f, 0);
                    uint8_t* prev = nullptr;
                    int k = static_cast<int>(rr.below(6));
                    for (uint8_t* n = l8->head; n && k >= 0; n = next8(n), --k) prev = n;
                    if (rr.below(4) == 0) prev = nullptr;
                    if (i >= 0) (ours ? OpListInsertAfter(l8, prev, f.node(i)) : o_OpListInsertAfter(l8, prev, f.node(i)));
                    break;
                }
                case 4: {  // remove a member or a non-member (must report 0)
                    const int i = static_cast<int>(rr.below(FakeLists::kN / 2)) * 2;
                    ret = (ours ? OpListRemove(l8, f.node(i), 0) : o_OpListRemove(l8, f.node(i), 0)) & 0xFF;
                    break;
                }
                case 5: { const int i = pick_free(rr, f, 1); if (i >= 0) (ours ? ListPushFront(l0, f.node(i)) : o_ListPushFront(l0, f.node(i))); break; }
                case 6: { const int i = pick_free(rr, f, 1); if (i >= 0) (ours ? ListPushBack(l0, f.node(i)) : o_ListPushBack(l0, f.node(i))); break; }
                case 7: ours ? ListClear(l0) : o_ListClear(l0); break;
                default: ret = f.idx(ours ? ListPopFirst(l0) : o_ListPopFirst(l0)); break;
            }
        };
        run(a, ra, false, ra_ret);
        run(b, rb, true, rb_ret);
        if (ra_ret != rb_ret) return game::MismatchText("op " + std::to_string(op) + " return");
        return a.shape() == b.shape() ? game::Ok() : game::MismatchText("op " + std::to_string(op) + " list state");
    });
}

// operator new / iGetNextOpId / constructor against the real FILESYSINFO, each under the lock the function itself
// takes (recursive), so no other thread sees the intermediate states; state is restored after each run.
NFSU2_DIFF_TEST(FileOpAlloc) {
    return game::run_diff(ctx, "FileOpAlloc", o_FileOpNew, [](game::Rng& r) -> game::Case {
        uint8_t* fs = fsinfo();
        List* free_ops = reinterpret_cast<List*>(fs);
        const uint32_t which = r.below(3);
        if (which == 0) {  // operator new
            lock(fs + 0x0C);
            const List saved = *free_ops;
            uint8_t* const h = saved.head;
            uint8_t* const hl = h ? link8(h) : nullptr;
            uint8_t* a = o_FileOpNew(0x40);
            const List after_a = *free_ops;
            *free_ops = saved;
            if (h) link8(h) = hl;
            uint8_t* b = FileOpNew(0x40);
            const List after_b = *free_ops;
            *free_ops = saved;
            if (h) link8(h) = hl;
            unlock(fs + 0x0C);
            if (a != b || after_a.head != after_b.head || after_a.tail != after_b.tail || after_a.count != after_b.count)
                return game::MismatchText("operator new");
            return game::Ok();
        }
        // a real, running device (inactive ones have uninitialised locks - never touch those)
        uint8_t* dev = nullptr;
        for (int t = 0; t < 64 && !dev; ++t) {
            uint8_t* d = *reinterpret_cast<uint8_t**>(fs + 0x98) + r.below(8) * 0x84;
            if (*reinterpret_cast<int32_t*>(d + 4)) dev = d;
        }
        if (!dev) return game::Ok();
        lock(fs + 0x54);
        int32_t& next = *reinterpret_cast<int32_t*>(fs + 0x50);
        const int32_t saved = next;
        if (r.below(8) == 0) next = 0xFFFF;  // exercise the wrap to 1
        const int32_t start = next;
        game::Case res = game::Ok();
        if (which == 1) {
            const int32_t a = o_iGetNextOpId(nullptr, dev);
            const int32_t na = next;
            next = start;
            const int32_t b = iGetNextOpId(nullptr, dev);
            if (a != b || na != next) res = game::MismatchText("iGetNextOpId");
        } else {
            uint8_t A[0x40], B[0x40];
            for (int i = 0; i < 0x40; ++i) A[i] = B[i] = static_cast<uint8_t>(r.next());
            const int32_t prio = static_cast<int32_t>(r.next()), user = static_cast<int32_t>(r.next());
            o_FileOpCtor(A, prio, user, dev);
            next = start;
            FileOpCtor(B, prio, user, dev);
            if (std::memcmp(A, B, sizeof A) != 0) res = game::MismatchText("FILEOPERATION ctor");
        }
        next = saved;
        unlock(fs + 0x54);
        return res;
    });
}
