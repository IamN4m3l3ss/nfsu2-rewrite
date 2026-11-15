// bWare: bCalculateCrc32 and the doubly linked bList (EA core library, zBWare.cpp).
// Adapted from nfsu2-gcpc (CC0, github.com/octavioCuatrochio/nfsu2-gcpc, port/game/bWare); PC ABI re-checked against
// SPEED2.EXE (ret sizes, register contract) and proven here with our own diff tests.
//
//   bList: sentinel {next, prev}; nodes start with the same links; an empty list points at itself.
//   comparators: __cdecl int(bNode* a, bNode* b), nonzero = a goes first.
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

struct bNode {
    bNode* next;
    bNode* prev;
};
struct bList {
    bNode head;
};
using bListCompare = int32_t(__cdecl*)(bNode*, bNode*);

// ---------------------------------------------------------------- bCalculateCrc32 0x0043BCD0
// MSB-first CRC-32 (poly 0x04C11DB7) over the data, then over the bytes of the length (low byte first, until 0);
// inverted. length <= 0: ~seed. The table is computed (identical to the game's at 0x7868E8).
constexpr std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
        t[i] = c;
    }
    return t;
}
constexpr std::array<uint32_t, 256> kCrcTable = MakeCrcTable();

using Crc_t = uint32_t(__cdecl*)(const void*, int32_t, uint32_t);
Crc_t o_bCalculateCrc32 = nullptr;
uint32_t __cdecl bCalculateCrc32(const void* data, int32_t length, uint32_t seed) {
    uint32_t crc = seed;
    if (length > 0) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (int32_t i = 0; i < length; ++i) crc = (crc << 8) ^ kCrcTable[(crc >> 24) ^ p[i]];
        for (int32_t n = length; n > 0; n >>= 8) crc = (crc << 8) ^ kCrcTable[((crc >> 24) ^ static_cast<uint32_t>(n)) & 0xFF];
    }
    return ~crc;
}

// ---------------------------------------------------------------- bList::TraversebList 0x0043BE80
// 1-based position of node; not found: the count if node is null, else 0
using Trav_t = int32_t(__thiscall*)(bList*, const bNode*);
Trav_t o_TraversebList = nullptr;
int32_t __cdecl TraversebList(bList* self, const bNode* node) {
    int32_t i = 0;
    for (bNode* n = self->head.next; n != &self->head; n = n->next) {
        ++i;
        if (n == node) return i;
    }
    return node ? 0 : i;
}

// ---------------------------------------------------------------- bList::AddTail(bList*) 0x0043BEB0
// moves every node of other to the tail; other becomes empty
using AddTail_t = void(__thiscall*)(bList*, bList*);
AddTail_t o_bListAddTail = nullptr;
void __cdecl bListAddTail(bList* self, bList* other) {
    bNode* first = other->head.next;
    bNode* last = other->head.prev;
    if (first == &other->head) return;
    bNode* tail = self->head.prev;
    last->next = &self->head;
    first->prev = tail;
    tail->next = first;
    self->head.prev = last;
    other->head.next = other->head.prev = &other->head;
}

// ---------------------------------------------------------------- bList::AddSorted 0x0043BEE0
// before the first node with cmp(node, it) == 0 (checked in the PC code: test eax / je insert), else at the tail
using AddSorted_t = bNode*(__thiscall*)(bList*, bListCompare, bNode*);
AddSorted_t o_AddSorted = nullptr;
bNode* __cdecl AddSorted(bList* self, bListCompare cmp, bNode* node) {
    bNode* at = self->head.next;
    while (at != &self->head && cmp(node, at) != 0) at = at->next;
    bNode* prev = at->prev;
    prev->next = node;
    at->prev = node;
    node->prev = prev;
    node->next = at;
    return node;
}

// ---------------------------------------------------------------- bList::MergeSort 0x0043BF40
// bottom-up merge sort of the ring (takes a when cmp(a, b) > 0 is false... see loop)
using Sort_t = void(__thiscall*)(bList*, bListCompare);
Sort_t o_MergeSort = nullptr;
void __cdecl MergeSort(bList* self, bListCompare cmp) {
    bNode* list = self->head.next;
    if (list == &self->head) return;
    list->prev = self->head.prev;  // ring without the sentinel
    self->head.prev->next = list;
    int32_t insize = 1;
    bNode* tail;
    for (;;) {
        bNode* const start = list;
        bNode* p = start;
        list = nullptr;
        tail = nullptr;
        int32_t nmerges = 0;
        while (p) {
            ++nmerges;
            bNode* q = p;
            int32_t psize = 0;
            for (int32_t i = 0; i < insize;) {
                q = q->next;
                ++psize;
                if (q == start) {
                    q = nullptr;
                    break;
                }
                if (!q) break;
                ++i;
            }
            int32_t qsize = insize;
            for (;;) {
                bNode* e;
                if (psize > 0) {
                    if (qsize != 0 && q && !(cmp(p, q) > 0)) {
                        e = q, q = q->next, --qsize;
                        if (q == start) q = nullptr;
                    } else {
                        e = p, p = p->next, --psize;
                        if (p == start) p = nullptr;
                    }
                } else {
                    if (qsize <= 0 || !q) break;
                    e = q, q = q->next, --qsize;
                    if (q == start) q = nullptr;
                }
                if (tail) {
                    tail->next = e;
                    e->prev = tail;
                } else {
                    e->prev = tail;
                    list = e;
                }
                tail = e;
            }
            p = q;
        }
        tail->next = list;
        list->prev = tail;
        if (nmerges <= 1) break;
        insize *= 2;
    }
    list->prev = &self->head;
    self->head.next = list;
    self->head.prev = tail;
    tail->next = &self->head;
}

// ---------------------------------------------------------------- bList::Sort 0x0043E650
// one pass moving each node that should precede its predecessor (cmp(prev, node) == 0) in front of it;
// MergeSort (through the game's address) if anything moved
Sort_t o_bListSort = nullptr;
void __cdecl bListSort(bList* self, bListCompare cmp) {
    int32_t moved = 0;
    bNode* a = self->head.next;
    if (a == &self->head) return;
    bNode* b = a->next;
    while (b != &self->head) {
        if (cmp(a, b) == 0) {  // b goes in front of a
            b->prev->next = b->next;
            b->next->prev = b->prev;
            bNode* p = a->prev;
            p->next = b;
            a->prev = b;
            b->prev = p;
            b->next = a;
            b = a->next;
            ++moved;
        } else {
            a = b;
            b = b->next;
        }
        if (a == &self->head) break;
    }
    if (moved) game_fn<Sort_t>(0x0043BF40)(self, cmp);
}

}  // namespace

NFSU2_REWRITE_N(0x0043BCD0, bCalculateCrc32, 3, "uint32 __cdecl(const void* data, int length, uint32 seed)", o_bCalculateCrc32);
NFSU2_REWRITE_THIS(0x0043BE80, TraversebList, 1, "int bList::TraversebList(bNode*)", o_TraversebList);
NFSU2_REWRITE_THIS(0x0043BEB0, bListAddTail, 1, "void bList::AddTail(bList* other)", o_bListAddTail);
NFSU2_REWRITE_THIS(0x0043BEE0, AddSorted, 2, "bNode* bList::AddSorted(cmp, bNode*)", o_AddSorted);
NFSU2_REWRITE_THIS(0x0043BF40, MergeSort, 1, "void bList::MergeSort(cmp)", o_MergeSort);
NFSU2_REWRITE_THIS(0x0043E650, bListSort, 1, "void bList::Sort(cmp)", o_bListSort);

// ============================================================================================ diff tests
namespace {

// nodes carry a key after the links; comparators are ours (cdecl, called by the original too)
struct KNode {
    bNode n;
    int32_t key;
};
int32_t __cdecl cmp_less(bNode* a, bNode* b) { return reinterpret_cast<KNode*>(a)->key < reinterpret_cast<KNode*>(b)->key; }
int32_t __cdecl cmp_lesseq(bNode* a, bNode* b) { return reinterpret_cast<KNode*>(a)->key <= reinterpret_cast<KNode*>(b)->key; }
int32_t __cdecl cmp_signed(bNode* a, bNode* b) {  // -1/0/1, exercises "> 0" vs "!= 0"
    const int32_t x = reinterpret_cast<KNode*>(a)->key, y = reinterpret_cast<KNode*>(b)->key;
    return x < y ? 1 : (x == y ? 0 : -1);
}
const bListCompare kCmps[] = {&cmp_less, &cmp_lesseq, &cmp_signed};

struct Lists {
    std::vector<KNode> pool;
    bList a{}, b{};
    Lists(uint32_t seed, int n) : pool(n) {
        game::Rng r(seed);
        a.head.next = a.head.prev = &a.head;
        b.head.next = b.head.prev = &b.head;
        for (int i = 0; i < n; ++i) {
            pool[i].key = static_cast<int32_t>(r.below(8));
            pool[i].n.next = pool[i].n.prev = nullptr;
            if (r.below(4) == 0) continue;  // some nodes stay out of both lists
            bList& l = r.below(3) ? a : b;
            bNode* t = l.head.prev;
            t->next = &pool[i].n;
            pool[i].n.prev = t;
            pool[i].n.next = &l.head;
            l.head.prev = &pool[i].n;
        }
    }
    uint64_t idx(const bNode* n) const {
        if (n == &a.head) return 0xA;
        if (n == &b.head) return 0xB;
        for (size_t i = 0; i < pool.size(); ++i)
            if (n == &pool[i].n) return 0x100 + i;
        return n ? 0xFFFF : 0;
    }
    std::vector<uint64_t> shape() const {
        std::vector<uint64_t> v;
        for (const bList* l : {&a, &b}) {
            int g = 0;
            for (const bNode* n = l->head.next; n != &l->head && g < 200; n = n->next, ++g) v.push_back(idx(n)), v.push_back(idx(n->prev));
            v.push_back(idx(l->head.prev));
            v.push_back(0xEEEE);
        }
        for (const auto& k : pool) v.push_back(idx(k.n.next)), v.push_back(idx(k.n.prev));
        return v;
    }
};

}  // namespace

NFSU2_DIFF_TEST(bCalculateCrc32) {
    return game::run_diff(ctx, "bCalculateCrc32", o_bCalculateCrc32, [](game::Rng& r) {
        std::vector<uint8_t> d(r.below(4) == 0 ? r.below(70000) : r.below(300));
        for (auto& b : d) b = static_cast<uint8_t>(r.next());
        const int32_t len = r.below(16) == 0 ? -static_cast<int32_t>(r.below(5)) : static_cast<int32_t>(d.size());
        const uint32_t seed = r.below(2) ? 0 : r.next();
        const uint32_t a = o_bCalculateCrc32(d.data(), len, seed), b = bCalculateCrc32(d.data(), len, seed);
        return a == b ? game::Ok() : game::MismatchText("len " + std::to_string(len));
    });
}

NFSU2_DIFF_TEST(bList) {
    return game::run_diff(ctx, "bList", o_bListSort, [](game::Rng& r) {
        const uint32_t seed = r.next(), op = r.below(6);
        const int n = static_cast<int>(r.below(r.below(4) == 0 ? 60 : 10));
        const bListCompare cmp = kCmps[r.below(3)];
        const uint32_t pick = r.next();
        Lists x(seed, n), y(seed, n);
        int64_t rx = 0, ry = 0;
        auto node_out = [](Lists& l, uint32_t p) -> bNode* {  // a node not in any list (for AddSorted)
            for (size_t i = 0; i < l.pool.size(); ++i) {
                KNode& k = l.pool[(p + i) % l.pool.size()];
                if (!k.n.next) return &k.n;
            }
            return nullptr;
        };
        auto any_node = [](Lists& l, uint32_t p) -> bNode* {
            if (l.pool.empty() || p % 5 == 0) return p % 10 == 0 ? nullptr : reinterpret_cast<bNode*>(&l.b.head);
            return &l.pool[p % l.pool.size()].n;
        };
        switch (op) {
            case 0: rx = o_TraversebList(&x.a, any_node(x, pick)); ry = TraversebList(&y.a, any_node(y, pick)); break;
            case 1: o_bListAddTail(&x.a, &x.b); bListAddTail(&y.a, &y.b); break;
            case 2: {
                bNode* nx = node_out(x, pick);
                bNode* ny = node_out(y, pick);
                if (nx) rx = static_cast<int64_t>(x.idx(o_AddSorted(&x.a, cmp, nx))), ry = static_cast<int64_t>(y.idx(AddSorted(&y.a, cmp, ny)));
                break;
            }
            case 3: o_MergeSort(&x.a, cmp); MergeSort(&y.a, cmp); break;
            default: o_bListSort(&x.a, cmp); bListSort(&y.a, cmp); break;
        }
        if (rx != ry) return game::MismatchText("op " + std::to_string(op) + " return");
        return x.shape() == y.shape() ? game::Ok() : game::MismatchText("op " + std::to_string(op) + " n=" + std::to_string(n));
    });
}

NFSU2_DIFF_TEST(regs_blist) {
    using game::arg;
    return game::run_diff(ctx, "regs_blist", o_TraversebList, [](game::Rng& r) {
        Lists x(r.next(), 8);
        uint8_t data[64];
        for (auto& b : data) b = static_cast<uint8_t>(r.next());
        for (auto c : {game::check_regs("bCalculateCrc32", (void*)o_bCalculateCrc32, NFSU2_ENTRY(bCalculateCrc32), {arg(data), 64, r.next()}, false),
                       game::check_regs_this("TraversebList", (void*)o_TraversebList, NFSU2_ENTRY(TraversebList), &x.a, &x.a,
                                             {arg(x.pool.empty() ? nullptr : &x.pool[0].n)}, true)})
            if (!c.ok) return c;
        return game::Ok();
    });
}
