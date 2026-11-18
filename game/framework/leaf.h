// Table-driven diff test for small leaf functions (no calls, or calls whose effects are on the graph only).
// Each case builds two identical random object graphs: blocks of random floats/ints/zeros, linked by pointer fields
// given per batch (Link), with integer fields kept in range (IntField). Original and rewrite run on their own graph
// with the same arguments; compared: EAX (pointers as block+offset), ECX/EDX where the original preserves them,
// stack pops, st0 for float returns, and every block (link slots skipped, copied pointers compared by place).
#pragma once
#include <cstddef>
#include <cstdlib>
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
enum Ret { INT, PTR, FLT, NONE, LOW16, LOW8 };  // LOW8: only AL (bools whose upper EAX is leftover pointer bits)  // LOW16: only AX compared (upper EAX = a pointer's high half)  // NONE: EAX still compared (the caller's EAX passes through or is clobbered alike)
struct Arg {
    // 'i' below(v), 'j' signed: below(2v) - v, 'p' pointer to block v, 'q' pointer into block (v & 0xFF) at offset
    // ((v >> 8) & 0xFFF) + stride (v >> 20) * below(8), 'f' random float bits, 'r' random dword
    char kind = 0;
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

// A block: `size` usable bytes with a zeroed guard gap of kGuard bytes on each side, so pointers the code computes
// just before / past a block (handled with 0x100 slack in rel()) can never land in a neighbouring block, and a
// stray write into the gap shows up as a difference.
constexpr size_t kGuard = 0x200;
struct Blk {
    uint8_t* p;
    size_t n;
    uint8_t* data() const { return p; }
    size_t size() const { return n; }
};

struct Graph {
    const Spec* sp;
    std::vector<std::vector<uint8_t>> raw;
    std::vector<Blk> blk;
    void fill(const Spec& s, Rng& r, uint32_t nullmask) {
        sp = &s;
        raw.assign(s.blocks, {});
        blk.assign(s.blocks, {});
        for (size_t k = 0; k < raw.size(); ++k) {
            raw[k].assign(s.size + 2 * kGuard, 0);
            blk[k] = {raw[k].data() + kGuard, s.size};
            const Blk& b = blk[k];
            for (size_t i = 0; i < b.size(); i += 4) {
                const uint32_t kind = r.below(8);
                const uint32_t x = kind < 2 ? 0u
                                 : kind < 6 ? 0x3F800000u + r.below(0x01000000) - (r.below(4) << 30)  // floats +-1..4
                                 : kind < 7 ? r.below(16)
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
    // a pointer as (block, offset): inside a block first, then up to 0x100 before its start or past its end
    // (one-before / one-past-the-end pointers the code computes); blocks can sit back to back, so the exact pass
    // must come first
    int64_t rel(uint32_t v) const {
        for (uint32_t slack : {0u, 0x100u})
            for (size_t k = 0; k < blk.size(); ++k) {
                const uintptr_t b = reinterpret_cast<uintptr_t>(blk[k].data());
                if (v + slack >= b + 0 && v >= b - slack && v < b + blk[k].size() + slack)
                    return (static_cast<int64_t>(k + 1) << 32) | static_cast<uint32_t>(v - b);
            }
        return v;
    }
    std::string diff(const Graph& o) const {
        for (size_t k = 0; k < blk.size(); ++k) {
            // pointer fields (links, and pointers the code wrote) compare by where they point (block + offset);
            // the guard gaps are compared too
            const std::vector<uint8_t>& a = raw[k];
            const std::vector<uint8_t>& b = o.raw[k];
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
                snprintf(t, sizeof t, " mem[%d+0x%X] %08X/%08X", static_cast<int>(k),
                         static_cast<unsigned>(i - kGuard), ux, uy);
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
            if ((a.kind == 'p' && a.v >= static_cast<uint32_t>(s.blocks)) || (a.kind == 'q' && (a.v & 0xFF) >= static_cast<uint32_t>(s.blocks))) {
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
            else if (a.kind == 'q') nb = std::max(nb, static_cast<int>(a.v & 0xFF) + 1);
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
                case 'j': aa[i] = ab[i] = r.below(2 * g.v) - g.v; break;
                case 'q': {
                    const uint32_t off = ((g.v >> 8) & 0xFFF) + (g.v >> 20) * r.below(8);
                    aa[i] = u(a.blk[g.v & 0xFF].data()) + off, ab[i] = u(b.blk[g.v & 0xFF].data()) + off;
                    break;
                }
                case 'f': aa[i] = ab[i] = 0x3F800000u + r.below(0x01000000) - (r.below(4) << 30); break;
                default: aa[i] = ab[i] = r.next(); break;
            }
        }
        const void* sa = f.conv == THIS ? a.blk[0].data() : nullptr;
        const void* sb = f.conv == THIS ? b.blk[0].data() : nullptr;
        const RegOutAny x = call_any(*f.orig, sa, aa, n, f.ret == FLT);
        const RegOutAny y = call_any(f.mine, sb, ab, n, f.ret == FLT);
        std::string bad;
        char t[300];
        if (f.ret == FLT) {
            if (!same_float(x.st0, y.st0)) {
                snprintf(t, sizeof t, " st0 (float result: original %g, rewrite %g)", x.st0, y.st0);
                bad += t;
            }
        } else if (f.ret == LOW8    ? (x.eax & 0xFF) != (y.eax & 0xFF)
                   : f.ret == LOW16 ? (x.eax & 0xFFFF) != (y.eax & 0xFFFF)
                                    : (x.eax != y.eax && a.rel(x.eax) != b.rel(y.eax))) {  // equal bits are equal
            const int64_t ra = a.rel(x.eax), rb = b.rel(y.eax);
            snprintf(t, sizeof t, " eax (return value: original 0x%X, rewrite 0x%X; 0xA0A0A0A0 = the caller's EAX untouched; "
                     "as block:offset %d:%X / %d:%X)", static_cast<unsigned>(x.eax), static_cast<unsigned>(y.eax),
                     static_cast<int>(ra >> 32) - 1, static_cast<unsigned>(ra), static_cast<int>(rb >> 32) - 1, static_cast<unsigned>(rb));
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

// ---- Replay of captured real calls (probe `capture`, tools/game/capture.py): work/captures/<ADDR>.cap, found
// next to out/ (env NFSU2_GAME). Each recorded call is rebuilt twice: block 0 = the `this` snapshot, block 1+i = the
// snapshot of pointer argument i (pointer args then point at their copy; value args as recorded). Pointers INSIDE a
// snapshot still point into the live game (reads see live data, the same for both sides), so only use this for
// functions that write to `this` / their argument buffers. No capture file yet: 0 iterations, no mismatch.
namespace game::leaf {
inline std::string capture_path(uintptr_t addr) {
    const char* g = std::getenv("NFSU2_GAME");
    std::string p = g ? g : "";
    for (int up = 0; up < 2 && !p.empty(); ++up) p = p.substr(0, p.find_last_of("\\/"));  // ...\out\x.dll -> repo
    char t[64];
    snprintf(t, sizeof t, "\\work\\captures\\%06X.cap", static_cast<unsigned>(addr));
    return p + t;
}

inline DiffResult run_captured(const DiffCtx& ctx, const char* test, uintptr_t addr, const Fn& f) {
    DiffResult res;
    res.name = test;
    std::vector<uint8_t> file;
    if (FILE* fp = fopen(capture_path(addr).c_str(), "rb")) {
        uint8_t b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, fp)) > 0) file.insert(file.end(), b, b + n);
        fclose(fp);
    }
    if (file.size() < 24 || std::memcmp(file.data(), "NFCP", 4) != 0) return res;  // nothing recorded yet
    size_t at = 4;
    auto rd = [&](void* d, size_t n) {
        if (at + n > file.size()) return false;
        std::memcpy(d, file.data() + at, n);
        at += n;
        return true;
    };
    uint32_t ver, faddr, nargs, this_size, count;
    rd(&ver, 4), rd(&faddr, 4), rd(&nargs, 4), rd(&this_size, 4);
    std::vector<uint32_t> asz(nargs);
    for (auto& s : asz) rd(&s, 4);
    rd(&count, 4);
    if (!*f.orig) {
        res.error = "original not hooked";
        return res;
    }
    struct Rec {
        uint32_t ecx, eax, edx, mask;
        std::vector<uint32_t> args;
        std::vector<std::vector<uint8_t>> snaps;  // [0] this, [1+i] arg i
    };
    std::vector<Rec> recs;
    for (uint32_t k = 0; k < count; ++k) {
        Rec r;
        r.args.resize(nargs);
        if (!rd(&r.ecx, 4) || !rd(&r.eax, 4) || !rd(&r.edx, 4) || !rd(r.args.data(), nargs * 4) || !rd(&r.mask, 4)) break;
        r.snaps.resize(1 + nargs);
        r.snaps[0].resize(this_size);
        if (this_size && !rd(r.snaps[0].data(), this_size)) break;
        for (uint32_t i = 0; i < nargs; ++i) {
            r.snaps[1 + i].resize(asz[i]);
            if (asz[i] && !rd(r.snaps[1 + i].data(), asz[i])) break;
        }
        recs.push_back(std::move(r));
    }
    if (recs.empty()) return res;
    for (uint32_t it = 0; it < ctx.iterations; ++it) {
        const Rec& r = recs[(it + ctx.seed) % recs.size()];
        ++res.iterations;
        Graph g[2];
        Spec sp{static_cast<int>(1 + nargs), 0x10, {}, {}};
        for (auto& s : r.snaps) sp.size = std::max<uint32_t>(sp.size, static_cast<uint32_t>((s.size() + 0xF) & ~0xFu));
        for (Graph& gr : g) {
            gr.sp = &sp;
            gr.raw.assign(sp.blocks, {});
            gr.blk.assign(sp.blocks, {});
            for (int k = 0; k < sp.blocks; ++k) {
                gr.raw[k].assign(sp.size + 2 * kGuard, 0);
                gr.blk[k] = {gr.raw[k].data() + kGuard, sp.size};
                std::memcpy(gr.blk[k].data(), r.snaps[k].data(), r.snaps[k].size());
            }
        }
        uint32_t a0[16] = {}, a1[16] = {};
        for (uint32_t i = 0; i < nargs && i < 16; ++i) {
            const bool snap = asz[i] && (r.mask >> (1 + i) & 1);
            a0[i] = snap ? u(g[0].blk[1 + i].data()) : r.args[i];
            a1[i] = snap ? u(g[1].blk[1 + i].data()) : r.args[i];
        }
        const bool self = f.conv == THIS && this_size && (r.mask & 1);
        const void* s0 = f.conv == THIS ? (self ? g[0].blk[0].data() : reinterpret_cast<void*>(r.ecx)) : nullptr;
        const void* s1 = f.conv == THIS ? (self ? g[1].blk[0].data() : reinterpret_cast<void*>(r.ecx)) : nullptr;
        if (f.conv == THIS && !self) continue;  // `this` was not readable: replaying would touch the live object
        const RegOutAny x = call_any(*f.orig, s0, a0, static_cast<int>(nargs), f.ret == FLT);
        const RegOutAny y = call_any(f.mine, s1, a1, static_cast<int>(nargs), f.ret == FLT);
        std::string bad;
        if (f.ret == FLT ? !same_float(x.st0, y.st0)
                         : (x.eax != y.eax && g[0].rel(x.eax) != g[1].rel(y.eax) &&
                            !(f.ret == LOW8 && (x.eax & 0xFF) == (y.eax & 0xFF)) &&
                            !(f.ret == LOW16 && (x.eax & 0xFFFF) == (y.eax & 0xFFFF))))
            bad += f.ret == FLT ? " st0" : " eax";
        if (x.popped != y.popped) bad += " pops";
        bad += g[0].diff(g[1]);
        if (!bad.empty() && ++res.mismatches <= 5) res.examples.push_back(std::string(f.name) + " (captured call):" + bad);
    }
    return res;
}
}  // namespace game::leaf
