// EA bMath inverse trig (NA v1.2), angles in 16-bit units (0x4000 = 90 deg). Table driven:
//   0x7EA628: 8-byte entries {uint16 angle; pad; float slope} (bASin), 0x7EACB0: byte table indexed backwards (bATan2).
// Both return values whose UPPER bits are leftovers (from _ftol2, or from the caller's EAX) — reproduced exactly,
// because some callers use the full 32-bit EAX (see DEVLOG). Tests compare full EAX through call_regs.
#include <cstring>
#include <string>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

inline int32_t ftol2(float f) { return static_cast<int32_t>(static_cast<int64_t>(f)); }

// ---------------------------------------------------------------- bASin 0x0043C490 (13 callers)
using ASin_t = uint32_t(__cdecl*)(float);
ASin_t o_bASin = nullptr;
uint32_t __cdecl bASin(float x) {
    bool neg = false;
    if (x < 0.0f) {
        neg = true;
        x = -x;
    }
    if (!(x < 1.0f)) return neg ? 0xC000u : 0x4000u;
    const int32_t v = ftol2(x * 65536.0f);
    int32_t edx = 0x8000, ecx = 0x8000, esi = 0;
    if (v >= ecx) {
        while (esi < 11) {
            edx >>= 1;
            ecx += edx;
            ++esi;
            if (v < ecx) break;
        }
    }
    ecx -= edx;
    const int32_t edi = ecx;
    const int32_t seg = (v - edi) >> (11 - esi);
    edx >>= 4;
    const int32_t base = edx * seg + edi;
    const uint32_t off = static_cast<uint32_t>((esi << 4) + seg) << 3;
    const float t = static_cast<float>(base) * 1.52587890625e-05f;  // exact (power of two)
    const float slope = game_var<float>(0x007EA62C + off);
    const int32_t r = ftol2(((x - t) * slope) * -65536.0f);
    const uint16_t cx = static_cast<uint16_t>(game_var<uint16_t>(0x007EA628 + off) - static_cast<uint16_t>(r));
    if (neg) return 0x10000u - cx;
    return (static_cast<uint32_t>(r) & 0xFFFF0000u) | cx;
}

// ---------------------------------------------------------------- bATan2 0x0043C570 (8 callers)
using ATan2_t = uint32_t(__cdecl*)(float, float);
ATan2_t o_bATan2 = nullptr;
uint32_t __cdecl bATan2(float a, float b, uint32_t entry_eax) {
    int esi = 0;
    if (a < 0.0f) {
        esi = 1;
        a = -a;
    }
    if (b < 0.0f) {
        esi ^= 3;
        b = -b;
    }
    uint32_t eax = entry_eax, ecx;
    if (a > b) {
        const int32_t r = ftol2((b / a) * -256.0f);
        const uint32_t p = 0x007EACB0u - static_cast<uint32_t>(r);
        ecx = ((p & 0xFFFF0000u) | game_var<uint8_t>(p)) << 5;
        eax = static_cast<uint32_t>(r);
    } else if (b > a) {
        const int32_t r = ftol2((a / b) * -256.0f);
        const uint32_t p = 0x007EACB0u - static_cast<uint32_t>(r);
        eax = ((static_cast<uint32_t>(r) & 0xFFFF0000u) | game_var<uint8_t>(p)) << 5;
        ecx = 0x4000u - eax;
    } else {  // equal, or NaN
        ecx = (b == 0.0f) ? 0u : 0x2000u;
    }
    switch (esi) {
        case 0: return (eax & 0xFFFF0000u) | (ecx & 0xFFFFu);
        case 3: return 0u - ecx;
        case 1: return 0x8000u - ecx;
        default: return ecx + 0x8000u;
    }
}

float rf(game::Rng& r) {
    switch (r.below(8)) {
        case 0: return 0.0f;
        case 1: return -0.0f;
        case 2: return static_cast<float>(static_cast<int>(r.below(9)) - 4) * 0.25f;  // exact steps incl. +-1
        default: {
            const float mag = static_cast<float>(r.next() & 0xFFFFFF) / 16777216.0f;
            static const float scales[] = {1e-3f, 1e-1f, 1.0f, 1.0f, 10.0f, 1e3f};
            const float v = mag * scales[r.below(6)];
            return r.below(2) ? -v : v;
        }
    }
}
std::string fs(float f) {
    char b[48];
    snprintf(b, sizeof b, "%.9g(0x%08x)", f, game::bits(f));
    return b;
}

}  // namespace

NFSU2_REWRITE_N(0x0043C490, bASin, 1, "uint32 __cdecl(float) -> angle; upper bits from _ftol2 (callers use full EAX)", o_bASin);
NFSU2_REWRITE_N_EAX(0x0043C570, bATan2, 2, "uint32 __cdecl(float, float) -> angle; equal case keeps caller's EAX upper half", o_bATan2);

// value + register contract in one: full EAX/ECX/EDX through the sentinel stub
NFSU2_DIFF_TEST(bASin) {
    using game::arg;
    return game::run_diff(ctx, "bASin", o_bASin, [](game::Rng& r) {
        float x = rf(r);
        if (r.below(4) == 0) x = static_cast<float>(r.next() & 0xFFFF) / 65536.0f;  // dense in [0,1)
        if (r.below(2)) x = -x;
        game::Case c = game::check_regs("bASin", (void*)o_bASin, NFSU2_ENTRY(bASin), {arg(x)}, false);
        if (!c.ok) c.detail += " x=" + fs(x);
        return c;
    });
}

NFSU2_DIFF_TEST(bATan2) {
    using game::arg;
    return game::run_diff(ctx, "bATan2", o_bATan2, [](game::Rng& r) {
        float a = rf(r), b = r.below(8) ? rf(r) : a;  // sometimes equal
        if (r.below(8) == 0) b = -a;
        game::Case c = game::check_regs("bATan2", (void*)o_bATan2, NFSU2_ENTRY(bATan2), {arg(a), arg(b)}, false);
        if (!c.ok) c.detail += " a=" + fs(a) + " b=" + fs(b);
        return c;
    });
}
