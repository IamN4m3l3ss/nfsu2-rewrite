// Diff harness (M7): run the original (through its MinHook trampoline) and our rewrite on the same inputs and
// compare. Runs inside the game process on the render thread (core RPC diff_test).
//
//   NFSU2_DIFF_TEST(bStrLen) {          // ctx: iterations, rng; returns a DiffResult
//       return run_diff(ctx, "bStrLen", o_bStrLen, [&](Rng& r) {
//           std::string s = r.str();
//           auto a = o_bStrLen(s.c_str()); auto b = bStrLen(s.c_str());
//           return a == b ? Ok() : Mismatch("s=" + show(s), a, b);
//       });
//   }
//
// For functions with side effects: compare the memory they write (separate output buffers per run, or
// MemSnapshot to save/restore game memory between the two runs).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace game {

// xorshift32: deterministic per seed, so a failing case can be reproduced exactly
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
    // a byte != 0, biased towards the interesting classes: letters, digits/punct, high bytes (>= 0x80)
    uint8_t byte() {
        switch (below(4)) {
            case 0: return static_cast<uint8_t>('a' + below(26));
            case 1: return static_cast<uint8_t>('A' + below(26));
            case 2: return static_cast<uint8_t>(0x80 + below(0x80));
            default: return static_cast<uint8_t>(1 + below(0x7F));
        }
    }
    std::string str(uint32_t max_len = 48) {
        std::string s;
        const uint32_t n = below(8) == 0 ? 0 : below(max_len + 1);  // 1 in 8 empty
        for (uint32_t i = 0; i < n; ++i) s += static_cast<char>(byte());
        return s;
    }
};

inline std::string show(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') {
            o += static_cast<char>(c);
        } else {
            char b[8];
            snprintf(b, sizeof b, "\\x%02x", c);
            o += b;
        }
    }
    return o + "\"";
}

struct Case {
    bool ok = true;
    std::string detail;
};
inline Case Ok() { return {}; }
template <class A, class B> Case Mismatch(const std::string& inputs, const A& orig, const B& mine) {
    return {false, inputs + " original=" + std::to_string(orig) + " rewrite=" + std::to_string(mine)};
}
inline Case MismatchText(const std::string& text) { return {false, text}; }

// Float comparison contract: bit-exact for every non-NaN value; where the original gives NaN, ours must give NaN
// (sign/payload may differ: x87 and SSE propagate NaN operands differently — see CLAUDE.md "Floating point").
inline bool same_float(float a, float b) {
    uint32_t ua, ub;
    memcpy(&ua, &a, 4);
    memcpy(&ub, &b, 4);
    return ua == ub || (a != a && b != b);
}

struct DiffResult {
    std::string name;
    uint32_t iterations = 0, mismatches = 0;
    std::vector<std::string> examples;  // first few failing cases
    std::string error;
};

struct DiffCtx {
    uint32_t iterations;
    uint32_t seed;
};

template <class Orig> DiffResult run_diff(const DiffCtx& ctx, const char* name, Orig original, const std::function<Case(Rng&)>& one) {
    DiffResult r;
    r.name = name;
    if (!original) {
        r.error = "original not hooked (trampoline missing)";
        return r;
    }
    Rng rng(ctx.seed);
    for (uint32_t i = 0; i < ctx.iterations; ++i) {
        ++r.iterations;
        Case c = one(rng);
        if (!c.ok && ++r.mismatches <= 5) r.examples.push_back(c.detail);
    }
    return r;
}

using DiffTestFn = DiffResult (*)(const DiffCtx&);
struct DiffTestReg {
    const char* name;
    DiffTestFn fn;
};
std::vector<DiffTestReg>& diff_registry();
struct DiffRegistrar {
    DiffRegistrar(const char* name, DiffTestFn fn) { diff_registry().push_back({name, fn}); }
};

}  // namespace game

#define NFSU2_DIFF_TEST(name)                                                      \
    static ::game::DiffResult difftest_##name(const ::game::DiffCtx& ctx);        \
    static ::game::DiffRegistrar g_diffreg_##name(#name, &difftest_##name);       \
    static ::game::DiffResult difftest_##name(const ::game::DiffCtx& ctx)
