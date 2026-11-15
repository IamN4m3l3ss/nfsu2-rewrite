// bStrToFloat 0x0043DEF0 (EA core library, zBWare.cpp), written from the PC machine code.
//   [+|-] digits: v = d + v*10 | '.' digits: scale *= 0.1; frac += d*scale; a nonzero frac is rounded to float and
//   its bit pattern incremented by one (the original's nudge) before v += frac | 'e'/'E' bStrToLong(rest): v *= 10 or
//   0.1, |exp| times | '-': fchs.
// The original keeps every intermediate in x87 registers (24-bit precision, extended exponent) and returns st0
// unrounded. Each operation rounds like float, so plain float in the same order is bit-identical while values stay
// in float range (CLAUDE.md "Floating point"); strings whose intermediates leave it (e.g. 40 digits then e-10) can
// differ - game data has none. The diff test covers realistic magnitudes.
#include <string>

#include "diff.h"
#include "lift.h"
#include "regs.h"
#include "rewrite.h"

namespace {

using StrToFloat_t = float(__cdecl*)(const char*);
StrToFloat_t o_bStrToFloat = nullptr;
float __cdecl bStrToFloat(const char* s) {
    const bool neg = *s == '-';
    if (neg || *s == '+') ++s;
    float v = 0.0f;
    while (static_cast<signed char>(*s) >= '0' && static_cast<signed char>(*s) <= '9') {
        const float d = static_cast<float>(static_cast<signed char>(*s) - '0');
        v = d + v * 10.0f;
        ++s;
    }
    if (*s == '.') {
        float frac = 0.0f, scale = 1.0f;
        for (;;) {
            const signed char c = static_cast<signed char>(*++s);
            if (c < '0' || c > '9') break;
            scale = scale * lift::f32(0x3DCCCCCDu);  // 0.1f
            frac = frac + static_cast<float>(c - '0') * scale;
        }
        if (frac != 0.0f) frac = lift::bitsf(lift::fbits(frac) + 1);
        v = v + frac;
    }
    if (*s == 'e' || *s == 'E') {
        int32_t e = game_fn<int32_t(__cdecl*)(const char*)>(0x0043DE50)(s + 1);  // bStrToLong
        float m = 1.0f;
        if (e > 0)
            while (e--) m = m * 10.0f;
        else if (e < 0)
            for (e = -e; e--;) m = m * lift::f32(0x3DCCCCCDu);
        v = v * m;
    }
    return neg ? lift::fchs(v) : v;
}

}  // namespace

NFSU2_REWRITE_N(0x0043DEF0, bStrToFloat, 1, "float __cdecl bStrToFloat(const char*)", o_bStrToFloat);

namespace {

std::string random_number(game::Rng& r) {
    std::string s;
    switch (r.below(4)) {
        case 0: s = "-"; break;
        case 1: s = "+"; break;
        default: break;
    }
    for (int i = 0, n = r.below(9); i < n; ++i) s += static_cast<char>('0' + r.below(10));
    if (r.below(3)) {
        s += '.';
        for (int i = 0, n = r.below(10); i < n; ++i) s += static_cast<char>('0' + r.below(10));
    }
    if (r.below(4) == 0) {
        s += r.below(2) ? 'e' : 'E';
        if (r.below(2)) s += '-';
        s += std::to_string(r.below(20));
    }
    if (r.below(8) == 0) s += "xyz";             // trailing junk
    if (r.below(12) == 0) s = "\xB0" + s;        // high byte first (signed compare: not a digit)
    if (r.below(16) == 0) s = r.below(2) ? "" : ".";
    return s;
}

}  // namespace

NFSU2_DIFF_TEST(bStrToFloat) {
    return game::run_diff(ctx, "bStrToFloat", o_bStrToFloat, [](game::Rng& r) -> game::Case {
        const std::string s = random_number(r);
        const float a = o_bStrToFloat(s.c_str()), b = bStrToFloat(s.c_str());
        return game::same_float(a, b) ? game::Ok() : game::MismatchText("bStrToFloat(\"" + s + "\")");
    });
}

NFSU2_DIFF_TEST(regs_bStrToFloat) {  // no caller reads EAX; ECX/EDX and st0 checked
    return game::run_diff(ctx, "regs_bStrToFloat", o_bStrToFloat, [](game::Rng& r) {
        const std::string s = random_number(r);
        return game::check_regs("bStrToFloat", (void*)o_bStrToFloat, NFSU2_ENTRY(bStrToFloat), {game::arg(s.c_str())}, true);
    });
}

NFSU2_DIFF_TEST(bStrToFloat_BROKEN) {  // harness self-check: without the +1 nudge
    return game::run_diff(ctx, "bStrToFloat_BROKEN", o_bStrToFloat, [](game::Rng& r) -> game::Case {
        const std::string s = std::to_string(r.below(100)) + "." + std::to_string(1 + r.below(999));
        float v = 0.0f, frac = 0.0f, scale = 1.0f;
        const char* p = s.c_str();
        while (*p >= '0' && *p <= '9') v = static_cast<float>(*p++ - '0') + v * 10.0f;
        for (++p; *p >= '0' && *p <= '9'; ++p) {
            scale = scale * lift::f32(0x3DCCCCCDu);
            frac = frac + static_cast<float>(*p - '0') * scale;
        }
        return game::same_float(o_bStrToFloat(s.c_str()), v + frac) ? game::Ok() : game::MismatchText("no nudge");
    });
}
