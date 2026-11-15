// bATan(x, y) 0x0043E6D0 (EA core library, zBWare.cpp): 16-bit angle of (x, y), written from the PC machine code.
//   quadrant q: x < 0 -> 1, y < 0 -> q ^= 3 (ordered compares)
//   r2 = x*x + y*y; if r2 <= 1e-4 (or unordered) the ratio is 0
//   |y| < |x|: a = bASin(|y| / sqrt(r2))            (y == 0 -> ratio 0)
//   else:      a = 0x4000 - bASin(|x| / sqrt(r2))   (x == 0 -> ratio 0)
//   q 0: a | 3: 0x10000 - a | 1: 0x8000 - a | 2: a + 0x8000
// EAX is read at 52 call sites: q 0 returns through `mov ax, word` (the high half is what bASin left in EAX).
// At the game's 24-bit x87 precision every step (incl. fsqrt) rounds like float. The r2 threshold compare uses the
// unrounded x87 value, which only differs from float when x*x or y*y overflows float (|x| > 1.8e19).
#include <cmath>
#include <string>

#include "diff.h"
#include "lift.h"
#include "regs.h"
#include "rewrite.h"

namespace {

using ATan_t = uint32_t(__cdecl*)(float, float);
ATan_t o_bATan = nullptr;
uint32_t __cdecl bATan(float x, float y) {
    uint32_t q = 0;
    if (x < 0.0f) q = 1;
    if (y < 0.0f) q ^= 3;
    const float ay = std::fabs(y), ax = std::fabs(x);
    const bool y_smaller = ay < ax;  // ordered: NaN -> the |x| branch
    const float r2 = x * x + y * y;
    float t = 0.0f;
    const auto asin_ = game_fn<uint32_t(__cdecl*)(float)>(0x0043C490);  // bASin
    uint32_t a;
    if (y_smaller) {
        if (r2 > lift::f32(0x38D1B717u) && y != 0.0f) t = ay / std::sqrt(r2);
        a = asin_(t);
    } else {
        if (r2 > lift::f32(0x38D1B717u) && x != 0.0f) t = ax / std::sqrt(r2);
        const uint32_t e = asin_(t);
        a = (e & 0xFFFF0000u) | ((0x4000u - e) & 0xFFFFu);  // what `mov ax, word [a]` leaves in EAX for q 0
        if (q) a = 0x4000u - e;
    }
    switch (q) {
        case 0: return a;
        case 3: return 0x10000u - a;
        case 1: return 0x8000u - a;
        default: return a + 0x8000u;
    }
}

}  // namespace

NFSU2_REWRITE_N(0x0043E6D0, bATan, 2, "uint16 __cdecl bATan(float x, float y)", o_bATan);

namespace {

float rf(game::Rng& r) {
    switch (r.below(10)) {
        case 0: return 0.0f;
        case 1: return -0.0f;
        case 2: return r.below(2) ? 1.0f : -1.0f;
        case 3: return (static_cast<float>(r.next() >> 8) / 16777216.0f - 0.5f) * 0.02f;  // around the 1e-4 threshold
        case 4: return NAN;
        default: return (static_cast<float>(r.next() >> 8) / 16777216.0f - 0.5f) * (r.below(3) ? 4.0f : 20000.0f);
    }
}

}  // namespace

NFSU2_DIFF_TEST(bATan) {
    return game::run_diff(ctx, "bATan", o_bATan, [](game::Rng& r) -> game::Case {
        const float x = rf(r), y = r.below(8) == 0 ? (r.below(2) ? x : -x) : rf(r);
        const uint32_t a = o_bATan(x, y), b = bATan(x, y);
        return a == b ? game::Ok() : game::MismatchText("bATan(" + std::to_string(x) + "," + std::to_string(y) + ") " + std::to_string(a) + " vs " + std::to_string(b));
    });
}

NFSU2_DIFF_TEST(regs_bATan) {
    return game::run_diff(ctx, "regs_bATan", o_bATan, [](game::Rng& r) {
        const float x = rf(r), y = rf(r);
        return game::check_regs("bATan", (void*)o_bATan, NFSU2_ENTRY(bATan), {game::bits(x), game::bits(y)}, false);
    });
}

NFSU2_DIFF_TEST(bATan_BROKEN) {  // harness self-check: quadrants 1 and 2 swapped
    return game::run_diff(ctx, "bATan_BROKEN", o_bATan, [](game::Rng& r) -> game::Case {
        const float x = -(1.0f + static_cast<float>(r.below(100))), y = static_cast<float>(r.below(100)) - 50.0f;
        const uint32_t ok = o_bATan(x, y);
        const uint32_t bad = o_bATan(-x, y);
        return ok == bad ? game::Ok() : game::MismatchText("quadrant");
    });
}
