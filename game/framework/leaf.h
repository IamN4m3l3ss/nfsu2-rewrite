// Table-driven diff test for small leaf functions (no calls, or calls whose effects are on the graph only).
// Each case builds two identical random object graphs: blocks of random floats/ints/zeros, linked by pointer fields
// given per batch (Link), with integer fields kept in range (IntField). Original and rewrite run on their own graph
// with the same arguments; compared: EAX (pointers as block+offset), ECX/EDX where the original preserves them,
// stack pops, st0 for float returns, and every block (link slots skipped, copied pointers compared by place).
#pragma once
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <initializer_list>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace game::leaf {

template <class T> inline T& at(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }
template <class T> inline T& at(const void* p, uint32_t off) { return at<T>(const_cast<void*>(p), off); }
template <class T> inline T& at(uint32_t p, uint32_t off) { return at<T>(reinterpret_cast<void*>(static_cast<uintptr_t>(p)), off); }
// a pointer field: converts to any pointer type (uint8_t*, uint32_t*, float*, ...), byte arithmetic with +
struct P {
    uint8_t* p;
    template <class T> operator T*() const { return reinterpret_cast<T*>(p); }
    operator uint32_t() const { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
    template <class I, std::enable_if_t<std::is_integral_v<I>, int> = 0> uint8_t* operator+(I n) const { return p + n; }
    explicit operator bool() const { return p != nullptr; }
    bool operator==(std::nullptr_t) const { return p == nullptr; }
    bool operator!=(std::nullptr_t) const { return p != nullptr; }
};
inline P ptr(void* p, uint32_t off) { return {at<uint8_t*>(p, off)}; }
inline P ptr(const P& p, uint32_t off) { return ptr(p.p, off); }
inline P ptr(const void* p, uint32_t off) { return ptr(const_cast<void*>(p), off); }
template <class T> inline T& at(const P& p, uint32_t off) { return at<T>(p.p, off); }
inline uint32_t u(const P& p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p.p)); }
inline float gf(const P& p) { return *reinterpret_cast<const float*>(p.p); }
inline uint32_t gu(const P& p) { return *reinterpret_cast<const uint32_t*>(p.p); }
// gf(p, off) / gu(p, off): a float / dword field (what the models often mean)
template <class Q> inline float gf(const Q& p, uint32_t off) { return at<float>(p, off); }
template <class Q> inline uint32_t gu(const Q& p, uint32_t off) { return at<uint32_t>(p, off); }
inline P ptr(uint32_t p, uint32_t off) { return ptr(reinterpret_cast<void*>(static_cast<uintptr_t>(p)), off); }
inline float gf(uintptr_t a) { return *reinterpret_cast<const float*>(a); }
inline uint32_t gu(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }
inline float gf(const void* a) { return *static_cast<const float*>(a); }
inline uint32_t gu(const void* a) { return *static_cast<const uint32_t*>(a); }
inline uint32_t u(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
inline uint32_t u(uint32_t v) { return v; }

struct Link {
    int from;
    uint32_t off;
    int to;
    bool nullable = false;  // null in 1 of 4 cases (code that tests the pointer)
    uint32_t add = 0;       // points this many bytes into the target block (intrusive list heads inside objects)
};
struct IntField {
    int blk;
    uint32_t off;
    int size;        // 1, 2, 4
    uint32_t range;  // value = base + below(range)
    uint32_t base = 0;
};
struct Spec {
    int blocks;          // block 0 = this (or the first pointer argument)
    uint32_t size;       // bytes per block
    std::vector<Link> links;
    std::vector<IntField> ints;
};

enum Conv { THIS, CDECL, STD };
enum Ret { INT, PTR, FLT, NONE, LOW16 };  // LOW16: only AX compared (upper EAX = a pointer's high half)  // NONE: EAX still compared (the caller's EAX passes through or is clobbered alike)
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
            const uint32_t v = f.base + r.below(f.range);
            std::memcpy(blk[f.blk].data() + f.off, &v, f.size);
        }
        for (size_t i = 0; i < s.links.size(); ++i) {
            const Link& l = s.links[i];
            uint8_t* p = (l.nullable && (nullmask >> (i % 32) & 1)) ? nullptr : blk[l.to].data() + l.add;
            std::memcpy(blk[l.from].data() + l.off, &p, 4);
        }
    }
    // a pointer as (block, offset): inside a block first, then up to 0x100 past its end (one-past-the-end
    // pointers); blocks can sit back to back, so the exact pass must come first
    int64_t rel(uint32_t v) const {
        for (uint32_t slack : {0u, 0x100u})
            for (size_t k = 0; k < blk.size(); ++k) {
                const uintptr_t b = reinterpret_cast<uintptr_t>(blk[k].data());
                if (v >= b && v < b + blk[k].size() + slack) return (static_cast<int64_t>(k + 1) << 32) | (v - b);
            }
        return v;
    }
    std::string diff(const Graph& o) const {
        for (size_t k = 0; k < blk.size(); ++k) {
            // pointer fields (links, and pointers the code wrote) compare by where they point (block + offset)
            const std::vector<uint8_t>& a = blk[k];
            const std::vector<uint8_t>& b = o.blk[k];
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

// a bad Spec must fail the test with a message, not write outside the blocks
inline std::string check_spec(const Spec& s, const std::vector<Fn>& fns) {
    char t[160];
    if (s.blocks < 1 || s.blocks > 32 || s.size < 0x10 || s.size > 0x10000) return "Spec: blocks 1..32, size 0x10..0x10000";
    for (const Link& l : s.links)
        if (l.from < 0 || l.from >= s.blocks || l.to < 0 || l.to >= s.blocks || l.off + 4 > s.size) {
            snprintf(t, sizeof t, "Spec: Link {%d, 0x%X, %d} is outside %d blocks of 0x%X bytes", l.from,
                     static_cast<unsigned>(l.off), l.to, s.blocks, static_cast<unsigned>(s.size));
            return t;
        }
    for (const IntField& f : s.ints)
        if (f.blk < 0 || f.blk >= s.blocks || (f.size != 1 && f.size != 2 && f.size != 4) || f.off + f.size > s.size || f.range == 0) {
            snprintf(t, sizeof t, "Spec: IntField {%d, 0x%X, %d, %u} is outside the blocks or has size not 1/2/4 or range 0",
                     f.blk, static_cast<unsigned>(f.off), f.size, static_cast<unsigned>(f.range));
            return t;
        }
    for (const Fn& f : fns)
        for (const Arg& a : f.args)
            if ((a.kind == 'p' && a.v >= static_cast<uint32_t>(s.blocks)) ) {
                snprintf(t, sizeof t, "Spec: argument {'%c', %u} of %s: 'p' needs an existing block, 'i' a range > 0", a.kind,
                         static_cast<unsigned>(a.v), f.name);
                return t;
            }
    return {};
}

// forgive bookkeeping slips: enough blocks for every link/argument, blocks big enough for every offset
inline Spec grown(Spec s, const std::vector<Fn>& fns) {
    int nb = s.blocks;
    uint32_t sz = s.size;
    for (const Link& l : s.links) nb = std::max({nb, l.from + 1, l.to + 1}), sz = std::max(sz, l.off + 0x100);
    for (IntField& f : s.ints) {
        if (f.size != 1 && f.size != 2 && f.size != 4) f.size = 4;
        if (f.range == 0) f.range = 0xFFFFFFFFu;
        nb = std::max(nb, f.blk + 1), sz = std::max(sz, f.off + 0x100);
    }
    for (const Fn& f : fns)
        for (const Arg& a : f.args)
            if (a.kind == 'p') nb = std::max(nb, static_cast<int>(a.v) + 1);
    s.blocks = std::min(nb, 32);
    s.size = std::min((std::max(sz, 0x100u) + 0xFF) & ~0xFFu, 0x10000u);
    return s;
}

inline DiffResult run(const DiffCtx& ctx, const char* test, const Spec& spec_in, const std::vector<Fn>& fns) {
    static std::vector<Spec> keep;  // the lambda below holds a reference
    keep.push_back(grown(spec_in, fns));
    const Spec& spec = keep.back();
    if (std::string e = check_spec(spec, fns); !e.empty()) {
        DiffResult r;
        r.name = test;
        r.error = e;
        return r;
    }
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
                case 'i': aa[i] = ab[i] = g.v ? r.below(g.v) : r.next(); break;
                case 'f': aa[i] = ab[i] = 0x3F800000u + r.below(0x01000000) - (r.below(4) << 30); break;
                default: aa[i] = ab[i] = r.next(); break;
            }
        }
        const void* sa = f.conv == THIS ? a.blk[0].data() : nullptr;
        const void* sb = f.conv == THIS ? b.blk[0].data() : nullptr;
        const RegOutAny x = call_any(*f.orig, sa, aa, n, f.ret == FLT);
        const RegOutAny y = call_any(f.mine, sb, ab, n, f.ret == FLT);
        std::string bad;
        char t[200];
        if (f.ret == FLT) {
            if (!same_float(x.st0, y.st0)) {
                snprintf(t, sizeof t, " st0 (float result: original %g, rewrite %g)", x.st0, y.st0);
                bad += t;
            }
        } else if (f.ret == LOW16 ? (x.eax & 0xFFFF) != (y.eax & 0xFFFF) : a.rel(x.eax) != b.rel(y.eax)) {
            snprintf(t, sizeof t, " eax (return value: original 0x%X, rewrite 0x%X; 0xA0A0A0A0 = the caller's EAX untouched)",
                     static_cast<unsigned>(x.eax), static_cast<unsigned>(y.eax));
            bad += t;
        }
        if (x.popped != y.popped) {
            snprintf(t, sizeof t, " pops (original removes %u bytes of stack arguments, rewrite %u: fix nargs / THIS-CDECL-STD)",
                     static_cast<unsigned>(x.popped), static_cast<unsigned>(y.popped));
            bad += t;
        }
        if (x.edx == kSentEDX && y.edx != kSentEDX) bad += " edx";
        if (f.conv != THIS && x.ecx == kSentECX && y.ecx != kSentECX) bad += " ecx";
        bad += a.diff(b);
        return bad.empty() ? Ok() : MismatchText(std::string(f.name) + ":" + bad);
    });
}

}  // namespace game::leaf

// Register a rewrite and get its table entry: LEAF_THIS(addr, fn, nargs, sig) after the function definition;
// in the test: {"fn", (void**)&o_fn, NFSU2_ENTRY(fn), THIS, INT, {{'i', 4}}}
#define LEAF_SIG_(sig, ...) sig
#define LEAF_THIS(addr, fn, nargs, ...) \
    namespace { void* o_##fn = nullptr; } \
    NFSU2_REWRITE_THIS(addr, fn, nargs, LEAF_SIG_(__VA_ARGS__, ""), o_##fn);  /* ends itself: a missing ; after LEAF_* is fine */
#define LEAF_CDECL(addr, fn, nargs, ...) \
    namespace { void* o_##fn = nullptr; } \
    NFSU2_REWRITE_N(addr, fn, nargs, LEAF_SIG_(__VA_ARGS__, ""), o_##fn);  /* ends itself: a missing ; after LEAF_* is fine */
#define LEAF_STD(addr, fn, nargs, ...) \
    namespace { void* o_##fn = nullptr; } \
    NFSU2_REWRITE_STD(addr, fn, nargs, LEAF_SIG_(__VA_ARGS__, ""), o_##fn);  /* ends itself: a missing ; after LEAF_* is fine */
// the original leaves EAX (partly) as the caller had it: our function gets it as an extra LAST parameter
#define LEAF_THIS_EAX(addr, fn, nargs, ...) \
    namespace { void* o_##fn = nullptr; } \
    NFSU2_REWRITE_THIS_EAX(addr, fn, nargs, LEAF_SIG_(__VA_ARGS__, ""), o_##fn);  /* ends itself: a missing ; after LEAF_* is fine */
#define LEAF_CDECL_EAX(addr, fn, nargs, ...) \
    namespace { void* o_##fn = nullptr; } \
    NFSU2_REWRITE_N_EAX(addr, fn, nargs, LEAF_SIG_(__VA_ARGS__, ""), o_##fn);  /* ends itself: a missing ; after LEAF_* is fine */
#define LEAF_STD_EAX(addr, fn, nargs, ...) \
    namespace { void* o_##fn = nullptr; } \
    NFSU2_REWRITE_STD_EAX(addr, fn, nargs, LEAF_SIG_(__VA_ARGS__, ""), o_##fn);  /* ends itself: a missing ; after LEAF_* is fine */
#define LEAF_FN(fn, conv, ...) ::game::leaf::Fn(#fn, &o_##fn, NFSU2_ENTRY(fn), ::game::leaf::conv, ::game::leaf::__VA_ARGS__)

using namespace game::leaf;  // the rewrite files use the helpers unqualified

// Ghidra's type names, which drafts copy from the decompilation (MSVC: long double is a 64-bit double)
using undefined = uint8_t;
using undefined1 = uint8_t;
using undefined2 = uint16_t;
using undefined4 = uint32_t;
using undefined8 = uint64_t;
using float10 = long double;
using byte = uint8_t;
using ushort = uint16_t;
using uint = uint32_t;
using ulong = uint32_t;
