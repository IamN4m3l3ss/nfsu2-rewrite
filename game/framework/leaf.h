// Table-driven diff test for small leaf functions (no calls, or calls whose effects are on the graph only).
// Each case builds two identical random object graphs: blocks of random floats/ints/zeros, linked by pointer fields
// given per batch (Link), with integer fields kept in range (IntField). Original and rewrite run on their own graph
// with the same arguments; compared: EAX (pointers as block+offset), ECX/EDX where the original preserves them,
// stack pops, st0 for float returns, and every block (link slots skipped, copied pointers compared by place).
#pragma once
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace game::leaf {

template <class T> inline T& at(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }
inline uint8_t* ptr(void* p, uint32_t off) { return at<uint8_t*>(p, off); }
inline float gf(uintptr_t a) { return *reinterpret_cast<const float*>(a); }
inline uint32_t gu(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }
inline uint32_t u(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }

struct Link {
    int from;
    uint32_t off;
    int to;
    bool nullable = false;  // null in 1 of 4 cases (code that tests the pointer)
};
struct IntField {
    int blk;
    uint32_t off;
    int size;        // 1, 2, 4
    uint32_t range;  // value = below(range)
};
struct Spec {
    int blocks;          // block 0 = this (or the first pointer argument)
    uint32_t size;       // bytes per block
    std::vector<Link> links;
    std::vector<IntField> ints;
};

enum Conv { THIS, CDECL, STD };
enum Ret { INT, PTR, FLT, NONE };  // NONE: EAX still compared (the caller's EAX passes through or is clobbered alike)
struct Arg {
    char kind = 0;  // 'i' below(v), 'p' pointer to block v, 'f' random float bits, 'r' random dword
    uint32_t v = 0;
};
struct Fn {
    const char* name;
    void** orig;
    const void* mine;
    Conv conv;
    Ret ret;
    std::vector<Arg> args;
    Fn(const char* n, void** o, const void* m, Conv c, Ret r, Arg a0 = {}, Arg a1 = {}, Arg a2 = {}, Arg a3 = {},
       Arg a4 = {}, Arg a5 = {}, Arg a6 = {}, Arg a7 = {})
        : name(n), orig(o), mine(m), conv(c), ret(r) {
        for (const Arg& a : {a0, a1, a2, a3, a4, a5, a6, a7})
            if (a.kind) args.push_back(a);
    }
};

struct Graph {
    const Spec* sp;
    std::vector<std::vector<uint8_t>> blk;
    void fill(const Spec& s, Rng& r, uint32_t nullmask) {
        sp = &s;
        blk.assign(s.blocks, {});
        for (auto& b : blk) {
            b.resize(s.size);
            for (size_t i = 0; i < b.size(); i += 4) {
                const uint32_t k = r.below(8);
                const uint32_t x = k < 2 ? 0u
                                 : k < 6 ? 0x3F800000u + r.below(0x01000000) - (r.below(4) << 30)  // floats around +-1..4
                                 : k < 7 ? r.below(16)
                                         : r.next();
                std::memcpy(b.data() + i, &x, 4);
            }
        }
        for (const IntField& f : s.ints) {
            const uint32_t v = r.below(f.range);
            std::memcpy(blk[f.blk].data() + f.off, &v, f.size);
        }
        for (size_t i = 0; i < s.links.size(); ++i) {
            const Link& l = s.links[i];
            uint8_t* p = (l.nullable && (nullmask >> (i % 32) & 1)) ? nullptr : blk[l.to].data();
            std::memcpy(blk[l.from].data() + l.off, &p, 4);
        }
    }
    int64_t rel(uint32_t v) const {
        for (size_t k = 0; k < blk.size(); ++k) {
            const uintptr_t b = reinterpret_cast<uintptr_t>(blk[k].data());
            if (v >= b && v < b + blk[k].size() + 0x100) return (static_cast<int64_t>(k + 1) << 32) | (v - b);
        }
        return v;
    }
    std::string diff(const Graph& o) const {
        for (size_t k = 0; k < blk.size(); ++k) {
            std::vector<uint8_t> a = blk[k], b = o.blk[k];
            for (const Link& l : sp->links)
                if (l.from == static_cast<int>(k)) std::memset(a.data() + l.off, 0, 4), std::memset(b.data() + l.off, 0, 4);
            for (size_t i = 0; i + 4 <= a.size(); i += 4) {
                if (!std::memcmp(a.data() + i, b.data() + i, 4)) continue;
                float x, y;
                std::memcpy(&x, a.data() + i, 4);
                std::memcpy(&y, b.data() + i, 4);
                if (x != x && y != y) continue;  // NaN payloads may differ (CLAUDE.md NaN contract)
                uint32_t ux, uy;
                std::memcpy(&ux, &x, 4);
                std::memcpy(&uy, &y, 4);
                if (rel(ux) == o.rel(uy) && rel(ux) > 0xFFFFFFFFll) continue;  // a copied pointer, same place
                char t[64];
                snprintf(t, sizeof t, " mem[%d+0x%X] %08X/%08X", static_cast<int>(k), static_cast<unsigned>(i), ux, uy);
                return t;
            }
        }
        return {};
    }
};

inline DiffResult run(const DiffCtx& ctx, const char* test, const Spec& spec, const std::vector<Fn>& fns) {
    const void* anchor = nullptr;
    for (const Fn& f : fns)
        if (*f.orig) anchor = *f.orig;
    return run_diff(ctx, test, anchor, [&](Rng& r) -> Case {
        const Fn& f = fns[r.below(static_cast<uint32_t>(fns.size()))];
        if (!*f.orig) return Ok();  // not hooked
        const uint32_t nullmask = r.below(4) == 0 ? r.next() : 0;
        Graph a, b;
        Rng r2 = r;
        a.fill(spec, r, nullmask);
        b.fill(spec, r2, nullmask);
        uint32_t aa[8] = {}, ab[8] = {};
        const int n = static_cast<int>(f.args.size());
        for (int i = 0; i < n; ++i) {
            const Arg& g = f.args[i];
            switch (g.kind) {
                case 'p': aa[i] = u(a.blk[g.v].data()), ab[i] = u(b.blk[g.v].data()); break;
                case 'i': aa[i] = ab[i] = r.below(g.v); break;
                case 'f': aa[i] = ab[i] = 0x3F800000u + r.below(0x01000000) - (r.below(4) << 30); break;
                default: aa[i] = ab[i] = r.next(); break;
            }
        }
        const void* sa = f.conv == THIS ? a.blk[0].data() : nullptr;
        const void* sb = f.conv == THIS ? b.blk[0].data() : nullptr;
        const RegOutAny x = call_any(*f.orig, sa, aa, n, f.ret == FLT);
        const RegOutAny y = call_any(f.mine, sb, ab, n, f.ret == FLT);
        std::string bad;
        if (f.ret == FLT) {
            if (!same_float(x.st0, y.st0)) bad += " st0";
        } else if (a.rel(x.eax) != b.rel(y.eax)) {
            bad += " eax";
        }
        if (x.popped != y.popped) bad += " pops";
        if (x.edx == kSentEDX && y.edx != kSentEDX) bad += " edx";
        if (f.conv != THIS && x.ecx == kSentECX && y.ecx != kSentECX) bad += " ecx";
        bad += a.diff(b);
        return bad.empty() ? Ok() : MismatchText(std::string(f.name) + ":" + bad);
    });
}

}  // namespace game::leaf

// Register a rewrite and get its table entry: LEAF_THIS(addr, fn, nargs, sig) after the function definition;
// in the test: {"fn", (void**)&o_fn, NFSU2_ENTRY(fn), THIS, INT, {{'i', 4}}}
#define LEAF_THIS(addr, fn, nargs, sig) \
    namespace { void* o_##fn = nullptr; } \
    NFSU2_REWRITE_THIS(addr, fn, nargs, sig, o_##fn)
#define LEAF_CDECL(addr, fn, nargs, sig) \
    namespace { void* o_##fn = nullptr; } \
    NFSU2_REWRITE_N(addr, fn, nargs, sig, o_##fn)
#define LEAF_STD(addr, fn, nargs, sig) \
    namespace { void* o_##fn = nullptr; } \
    NFSU2_REWRITE_STD(addr, fn, nargs, sig, o_##fn)
#define LEAF_FN(fn, conv, ...) ::game::leaf::Fn(#fn, &o_##fn, NFSU2_ENTRY(fn), ::game::leaf::conv, ::game::leaf::__VA_ARGS__)
