// bWare ticker, random seed and memory-pool override (EA core library, zBWare.cpp), written from the PC machine code.
//   bGetTicker            0x0043BDF0  QueryPerformanceCounter >> TickerShift (0x8284E4), low 32 bits
//   bGetTickerDifference  0x0043BE20  (start, end) -> max(end - start, 0) * (1 << shift) * TickerScale (0x828028)
//   bGetTickerDifference  0x0043E5C0  (start) -> the same, against bGetTicker()
//   bSetRandomSeed        0x0043E6B0  scrambles a seed into *out
//   bSetMemoryPoolOverrideInfo 0x0043CB00  table[pool] = info (0x82802C)
#include <windows.h>

#include <string>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

volatile uint32_t* const kTickerShift = reinterpret_cast<volatile uint32_t*>(0x008284E4);
volatile float* const kTickerScale = reinterpret_cast<volatile float*>(0x00828028);
uint32_t* const kPoolOverride = reinterpret_cast<uint32_t*>(0x0082802C);

// __aullshr (0x75C600): 64-bit logical shift right by CL; >= 64 gives 0
uint64_t aullshr(uint64_t v, uint32_t cl) { return (cl & 0xFF) >= 64 ? 0 : v >> (cl & 0xFF); }

using Ticker_t = uint32_t(__cdecl*)();
Ticker_t o_bGetTicker = nullptr;
uint32_t __cdecl bGetTicker() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return static_cast<uint32_t>(aullshr(static_cast<uint64_t>(li.QuadPart), *kTickerShift));
}

// x87: fild (1 << (shift & 31)) [a power of two, or INT_MIN]; fimul d; fmul scale. At 24-bit precision fimul rounds
// d * 2^k once = float(d) * 2^k exactly; then one rounded multiply by the scale.
float difference(int32_t d) {
    if (d < 0) d = 0;
    const int32_t p = static_cast<int32_t>(1u << (*kTickerShift & 31));
    const float scaled = static_cast<float>(p) * static_cast<float>(d);
    return scaled * *kTickerScale;
}
using Diff2_t = float(__cdecl*)(uint32_t, uint32_t);
Diff2_t o_bGetTickerDifference2 = nullptr;
float __cdecl bGetTickerDifference2(uint32_t start, uint32_t end) {
    return difference(static_cast<int32_t>(end - start));
}
using Diff1_t = float(__cdecl*)(uint32_t);
Diff1_t o_bGetTickerDifference1 = nullptr;
float __cdecl bGetTickerDifference1(uint32_t start) {
    return difference(static_cast<int32_t>(bGetTicker() - start));
}

using Seed_t = uint32_t*(__cdecl*)(uint32_t, uint32_t*);
Seed_t o_bSetRandomSeed = nullptr;
uint32_t* __cdecl bSetRandomSeed(uint32_t seed, uint32_t* out) {
    const uint32_t x = seed ^ 0x1D872B41u;
    const uint32_t y = x ^ (x >> 5);
    *out = y ^ (y << 27) ^ x;
    return out;  // EAX = out
}

using PoolOv_t = uint32_t(__cdecl*)(uint32_t, uint32_t);
PoolOv_t o_bSetMemoryPoolOverrideInfo = nullptr;
uint32_t __cdecl bSetMemoryPoolOverrideInfo(uint32_t pool, uint32_t info) {
    kPoolOverride[pool] = info;
    return info;  // EAX = info
}

}  // namespace

NFSU2_REWRITE_N(0x0043BDF0, bGetTicker, 0, "unsigned __cdecl bGetTicker() QPC >> shift", o_bGetTicker);
NFSU2_REWRITE_N(0x0043BE20, bGetTickerDifference2, 2, "float __cdecl bGetTickerDifference(unsigned start, unsigned end)", o_bGetTickerDifference2);
NFSU2_REWRITE_N(0x0043E5C0, bGetTickerDifference1, 1, "float __cdecl bGetTickerDifference(unsigned start) vs now", o_bGetTickerDifference1);
NFSU2_REWRITE_N(0x0043E6B0, bSetRandomSeed, 2, "void __cdecl bSetRandomSeed(unsigned seed, unsigned* out)", o_bSetRandomSeed);
NFSU2_REWRITE_N(0x0043CB00, bSetMemoryPoolOverrideInfo, 2, "void __cdecl bSetMemoryPoolOverrideInfo(int pool, info)", o_bSetMemoryPoolOverrideInfo);

NFSU2_DIFF_TEST(bTicker) {
    return game::run_diff(ctx, "bTicker", o_bGetTickerDifference2, [](game::Rng& r) -> game::Case {
        // other shift/scale values too (restored after each case; tests run on the main thread)
        const uint32_t old_shift = *kTickerShift;
        const float old_scale = *kTickerScale;
        game::ScopedUndo undo([=] { *kTickerShift = old_shift; *kTickerScale = old_scale; });
        if (r.below(2)) *kTickerShift = r.below(4) ? r.below(12) : r.below(70);
        if (r.below(3) == 0) *kTickerScale = (static_cast<float>(r.next() >> 8) / 16777216.0f) * 1e-3f;
        switch (r.below(4)) {
            case 0: {  // time-dependent: original, ours, original must be in order
                const uint32_t a = o_bGetTicker(), b = bGetTicker(), c = o_bGetTicker();
                return (b - a) <= (c - a) ? game::Ok() : game::MismatchText("bGetTicker out of order " + std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c));
            }
            case 1: {
                const uint32_t start = o_bGetTicker() - r.below(100000);
                const float a = o_bGetTickerDifference1(start), b = bGetTickerDifference1(start), c = o_bGetTickerDifference1(start);
                return (a <= b && b <= c) ? game::Ok() : game::MismatchText("bGetTickerDifference(start) out of order");
            }
            case 2: {
                uint32_t s = r.next(), e = r.below(2) ? s + r.below(1000000) : r.next();
                if (r.below(8) == 0) e = s;
                return game::same_float(o_bGetTickerDifference2(s, e), bGetTickerDifference2(s, e))
                           ? game::Ok() : game::MismatchText("bGetTickerDifference(" + std::to_string(s) + "," + std::to_string(e) + ")");
            }
            default: {
                const uint32_t seed = r.next();
                uint32_t a = 0x5A5A5A5A, b = a;
                o_bSetRandomSeed(seed, &a);
                bSetRandomSeed(seed, &b);
                return a == b ? game::Ok() : game::MismatchText("bSetRandomSeed(" + std::to_string(seed) + ")");
            }
        }
    });
}

NFSU2_DIFF_TEST(regs_bTicker) {
    using game::arg;
    return game::run_diff(ctx, "regs_bTicker", o_bSetRandomSeed, [](game::Rng& r) {
        uint32_t a = 0, b = 0;
        const uint32_t s = r.next(), e = s + r.below(5000);
        // bGetTicker: EAX is time-dependent (compared in the diff test) -> only ECX/EDX here
        for (auto c : {game::check_regs("bGetTicker", (void*)o_bGetTicker, NFSU2_ENTRY(bGetTicker), {}, false, false),
                       game::check_regs("bGetTickerDifference2", (void*)o_bGetTickerDifference2, NFSU2_ENTRY(bGetTickerDifference2), {s, e}, true),
                       game::check_regs("bSetRandomSeed", (void*)o_bSetRandomSeed, NFSU2_ENTRY(bSetRandomSeed), {s, arg(&a)}, false),
                       game::check_regs("bSetRandomSeed2", (void*)o_bSetRandomSeed, NFSU2_ENTRY(bSetRandomSeed), {s, arg(&b)}, false)})
            if (!c.ok) return c;
        // bSetMemoryPoolOverrideInfo writes a game table: same slot, same value, restored
        const uint32_t pool = r.below(15), old = kPoolOverride[pool], info = r.next();
        game::ScopedUndo undo([=] { kPoolOverride[pool] = old; });
        const game::Case c = game::check_regs("bSetMemoryPoolOverrideInfo", (void*)o_bSetMemoryPoolOverrideInfo,
                                              NFSU2_ENTRY(bSetMemoryPoolOverrideInfo), {pool, info}, false);
        if (!c.ok) return c;
        return kPoolOverride[pool] == info ? game::Ok() : game::MismatchText("pool override not written");
    });
}

NFSU2_DIFF_TEST(bTicker_BROKEN) {  // harness self-check: difference without the negative clamp
    return game::run_diff(ctx, "bTicker_BROKEN", o_bGetTickerDifference2, [](game::Rng& r) -> game::Case {
        const uint32_t s = r.next(), e = r.next();
        const int32_t d = static_cast<int32_t>(e - s);
        const float bad = static_cast<float>(static_cast<int32_t>(1u << (*kTickerShift & 31))) * static_cast<float>(d) * *kTickerScale;
        return game::same_float(o_bGetTickerDifference2(s, e), bad) ? game::Ok() : game::MismatchText("no clamp");
    });
}
