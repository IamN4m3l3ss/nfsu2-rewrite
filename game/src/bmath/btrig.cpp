// EA bMath trig + random float (NA v1.2). Angles are 16-bit fixed point (0x10000 = 2*pi, 0x4000 = 90 deg).
// bSin's polynomial was traced from the x87 stack; operation order kept exactly (see comments).
#include <cstring>
#include <string>

#include "../diff.h"
#include "../regs.h"
#include "../rewrite.h"

namespace {

// constants as stored in the exe (.rdata)
constexpr float kAngleToRad = 9.58738019107841e-05f;  // 0x784240 = 2*pi/65536
constexpr float k3PiOver2 = 4.71238899230957f;        // 0x786DA0
constexpr float k2Pi = 6.2831854820251465f;           // 0x786D9C
constexpr float kPiOver2 = 1.5707963705062866f;       // 0x784254
constexpr float kPi = 3.1415927410125732f;            // 0x786D98
constexpr float kC3 = 0.1666666716337204f;            // 0x786D94  1/3!
constexpr float kC5 = 0.008333333767950535f;          // 0x786D90  1/5!
constexpr float kC7 = 0.00019841270113829523f;        // 0x786D8C  1/7!
constexpr float kC9 = 2.7557318844628753e-06f;        // 0x786D88  1/9!
constexpr float kRadToAngle = 10430.3779296875f;      // 0x78423C = 65536/(2*pi)
constexpr float kTwoPowMinus31 = 4.656612873077393e-10f;  // 0x786CE8

float sin_impl(uint16_t a) {
    float x = static_cast<float>(a) * kAngleToRad;
    float s = 1.0f;
    if (!(x < k3PiOver2)) {
        x = x - k2Pi;
    } else if (!(x < kPiOver2)) {
        x = x - kPi;
        s = -1.0f;
    }
    const float x2 = x * x, x3 = x2 * x, x5 = x3 * x2, x7 = x5 * x2;
    float acc = x - x3 * kC3;
    acc = acc + x5 * kC5;
    acc = acc - x7 * kC7;
    const float x9 = x7 * x2;
    acc = acc + x9 * kC9;
    return acc * s;
}

// ---------------------------------------------------------------- bSin 0x0043C2B0 (56 callers)
using SinA_t = float(__cdecl*)(uint32_t);
SinA_t o_bSin = nullptr;
float __cdecl bSin(uint32_t angle) { return sin_impl(static_cast<uint16_t>(angle)); }

// ---------------------------------------------------------------- bCos 0x0043C370 (49 callers): bSin(a + 0x4000)
SinA_t o_bCos = nullptr;
float __cdecl bCos(uint32_t angle) { return sin_impl(static_cast<uint16_t>(angle + 0x4000)); }

// ---------------------------------------------------------------- bSinRad 0x0043C350 (57 callers)
// angle = _ftol2(rad * 65536/2pi): truncation toward zero, low 32 bits of the 64-bit result
using SinF_t = float(__cdecl*)(float);
SinF_t o_bSinRad = nullptr;
float __cdecl bSinRad(float rad) {
    const float t = rad * kRadToAngle;
    const int32_t a = static_cast<int32_t>(static_cast<int64_t>(t));
    return sin_impl(static_cast<uint16_t>(a));
}

// ---------------------------------------------------------------- bRandomFloat 0x0043C1F0 (33 callers)
constexpr uintptr_t kRandomSeed = 0x007EA624;
using RandF_t = float(__cdecl*)(float);
RandF_t o_bRandomFloat = nullptr;
float __cdecl bRandomFloat(float scale) {
    uint32_t& seed = game_var<uint32_t>(kRandomSeed);
    const uint32_t r = seed % 0x7FFFFFFFu;
    const uint32_t s = seed ^ 0x1D872B41u;
    const uint32_t t = s ^ (s >> 5);
    seed = (t << 27) ^ t ^ s;
    // fild of r (< 2^31, never negative) * 2^-31 (exact) * scale
    return (static_cast<float>(static_cast<int32_t>(r)) * kTwoPowMinus31) * scale;
}

std::string fs(float f) {
    char b[48];
    uint32_t u;
    memcpy(&u, &f, 4);
    snprintf(b, sizeof b, "%.9g(0x%08x)", f, u);
    return b;
}
bool same(float a, float b) { return memcmp(&a, &b, 4) == 0; }

}  // namespace

NFSU2_REWRITE_N(0x0043C2B0, bSin, 1, "float __cdecl(uint16 angle)", o_bSin);
NFSU2_REWRITE_N(0x0043C370, bCos, 1, "float __cdecl(uint16 angle)", o_bCos);
NFSU2_REWRITE_N(0x0043C350, bSinRad, 1, "float __cdecl(float radians)", o_bSinRad);
NFSU2_REWRITE_N(0x0043C1F0, bRandomFloat, 1, "float __cdecl(float scale); seed 0x7EA624", o_bRandomFloat);

// exhaustive: every one of the 65536 angles (plus random high bits that must be ignored)
NFSU2_DIFF_TEST(bSin) {
    game::DiffCtx all{65536, ctx.seed};
    uint32_t i = 0;
    return game::run_diff(all, "bSin", o_bSin, [&i](game::Rng& r) {
        const uint32_t a = (i++) | (r.below(4) == 0 ? (r.next() & 0xFFFF0000u) : 0);
        float x = o_bSin(a), y = bSin(a);
        return same(x, y) ? game::Ok() : game::MismatchText("angle=" + std::to_string(a) + " " + fs(x) + " vs " + fs(y));
    });
}

NFSU2_DIFF_TEST(bCos) {
    game::DiffCtx all{65536, ctx.seed};
    uint32_t i = 0;
    return game::run_diff(all, "bCos", o_bCos, [&i](game::Rng& r) {
        const uint32_t a = (i++) | (r.below(4) == 0 ? (r.next() & 0xFFFF0000u) : 0);
        float x = o_bCos(a), y = bCos(a);
        return same(x, y) ? game::Ok() : game::MismatchText("angle=" + std::to_string(a) + " " + fs(x) + " vs " + fs(y));
    });
}

NFSU2_DIFF_TEST(bSinRad) {
    return game::run_diff(ctx, "bSinRad", o_bSinRad, [](game::Rng& r) {
        float rad;
        switch (r.below(3)) {
            case 0: rad = (static_cast<float>(r.next() & 0xFFFFFF) / 16777216.0f - 0.5f) * 20.0f; break;  // +-10 rad
            case 1: rad = (static_cast<float>(r.next() & 0xFFFFFF) / 16777216.0f - 0.5f) * 2000.0f; break;
            default: rad = static_cast<float>(static_cast<int>(r.below(13)) - 6) * 1.5707964f;  // multiples of pi/2
        }
        float x = o_bSinRad(rad), y = bSinRad(rad);
        return same(x, y) ? game::Ok() : game::MismatchText("rad=" + fs(rad) + " " + fs(x) + " vs " + fs(y));
    });
}

NFSU2_DIFF_TEST(bRandomFloat) {
    uint32_t& seed = game_var<uint32_t>(kRandomSeed);
    const uint32_t saved = seed;
    game::DiffResult res = game::run_diff(ctx, "bRandomFloat", o_bRandomFloat, [&seed](game::Rng& r) {
        const uint32_t s0 = r.next();
        const float scale = r.below(4) ? static_cast<float>(1 + r.below(1000)) : static_cast<float>(r.next() & 0xFFFF) / 7.0f;
        seed = s0;
        const float a = o_bRandomFloat(scale);
        const uint32_t sa = seed;
        seed = s0;
        const float b = bRandomFloat(scale);
        const uint32_t sb = seed;
        if (!same(a, b) || sa != sb)
            return game::MismatchText("seed=" + std::to_string(s0) + " scale=" + fs(scale) + " " + fs(a) + " vs " + fs(b));
        return game::Ok();
    });
    seed = saved;
    return res;
}

NFSU2_DIFF_TEST(regs_btrig) {
    using game::arg;
    uint32_t& seed = game_var<uint32_t>(kRandomSeed);
    const uint32_t saved = seed;
    game::DiffResult res = game::run_diff(ctx, "regs_btrig", o_bSin, [&seed](game::Rng& r) {
        const uint32_t ang = r.next();
        const float rad = (static_cast<float>(r.next() & 0xFFFFFF) / 16777216.0f - 0.5f) * 20.0f, scale = 1.0f + r.below(100);
        for (auto c : {game::check_regs("bSin", (void*)o_bSin, NFSU2_ENTRY(bSin), {ang}, true),
                       game::check_regs("bCos", (void*)o_bCos, NFSU2_ENTRY(bCos), {ang}, true),
                       game::check_regs("bSinRad", (void*)o_bSinRad, NFSU2_ENTRY(bSinRad), {arg(rad)}, true)})
            if (!c.ok) return c;
        const uint32_t s0 = r.next(), sc = arg(scale);
        seed = s0;
        const game::RegOut ra = game::call_regs((void*)o_bRandomFloat, &sc, 1, true);
        seed = s0;
        const game::RegOut rb = game::call_regs(NFSU2_ENTRY(bRandomFloat), &sc, 1, true);
        if (game::bits(ra.st0) != game::bits(rb.st0) || (ra.ecx == game::kSentECX && rb.ecx != game::kSentECX) ||
            (ra.edx == game::kSentEDX && rb.edx != game::kSentEDX))
            return game::MismatchText("bRandomFloat: register contract");
        return game::Ok();
    });
    seed = saved;
    return res;
}

// ---------------------------------------------------------------- bSinCos 0x0043C390 (16 callers)
// Same folding + sine series as bSin; cosine 1 - x2/2 + x4/4! - x6/6! + x8/8! - x10/10!, each even power formed as
// in the x87 code (x4 = x2*x2, x6 = x4*x2, x8 = x6*x2, x10 = x8*x2), both results times the same sign.
namespace {
constexpr float kHalf = 0.5f;                              // 0x784260
constexpr float kC4 = 0.0416666679084301f;                 // 0x786DB0  1/4!
constexpr float kC6 = 0.0013888889225199819f;              // 0x786DAC  1/6!
constexpr float kC8 = 2.4801587642286904e-05f;             // 0x786DA8  1/8!
constexpr float kC10 = 2.755731998149713e-07f;             // 0x786DA4  1/10!

using SinCos_t = void(__cdecl*)(float*, float*, uint32_t);
SinCos_t o_bSinCos = nullptr;
void __cdecl bSinCos(float* sin_out, float* cos_out, uint32_t angle) {
    float x = static_cast<float>(static_cast<uint16_t>(angle)) * kAngleToRad;
    float s = 1.0f;
    if (!(x < k3PiOver2)) {
        x = x - k2Pi;
    } else if (!(x < kPiOver2)) {
        x = x - kPi;
        s = -1.0f;
    }
    const float x2 = x * x, x3 = x2 * x, x4 = x2 * x2, x5 = x3 * x2, x6 = x4 * x2, x7 = x5 * x2, x8 = x6 * x2;
    float sn = x - x3 * kC3;
    sn = sn + x5 * kC5;
    sn = sn - x7 * kC7;
    const float x9 = x7 * x2;
    sn = sn + x9 * kC9;
    *sin_out = sn * s;
    float cs = 1.0f - kHalf * x2;
    cs = cs + x4 * kC4;
    cs = cs - x6 * kC6;
    cs = cs + x8 * kC8;
    const float x10 = x8 * x2;
    cs = cs - x10 * kC10;
    *cos_out = cs * s;
}
}  // namespace

NFSU2_REWRITE_N(0x0043C390, bSinCos, 3, "void __cdecl(float* sin, float* cos, uint16 angle)", o_bSinCos);

NFSU2_DIFF_TEST(bSinCos) {
    game::DiffCtx all{65536, ctx.seed};
    uint32_t i = 0;
    return game::run_diff(all, "bSinCos", o_bSinCos, [&i](game::Rng& r) {
        const uint32_t a = (i++) | (r.below(4) == 0 ? (r.next() & 0xFFFF0000u) : 0);
        float s1 = 9, c1 = 9, s2 = 9, c2 = 9;
        o_bSinCos(&s1, &c1, a);
        bSinCos(&s2, &c2, a);
        if (!same(s1, s2) || !same(c1, c2))
            return game::MismatchText("angle=" + std::to_string(a) + " sin " + fs(s1) + "/" + fs(s2) + " cos " + fs(c1) + "/" + fs(c2));
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(regs_bsincos) {
    using game::arg;
    return game::run_diff(ctx, "regs_bsincos", o_bSinCos, [](game::Rng& r) {
        float s = 0, c = 0;
        // void; leaves the FPU status word in EAX; regcontract: no caller reads EAX -> don't compare it
        return game::check_regs("bSinCos", (void*)o_bSinCos, NFSU2_ENTRY(bSinCos), {arg(&s), arg(&c), r.next()}, false,
                                /*compare_eax=*/false);
    });
}
