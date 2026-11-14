// EA bMath: matrix * point, seeded random, integer/float modulo (NA v1.2). __cdecl, entry stubs (regs.h).
#include <cmath>
#include <cstring>
#include <string>

#include "../diff.h"
#include "../regs.h"
#include "../rewrite.h"

namespace {

struct V3 { float x, y, z; };
struct M4 { float m[16]; };  // row-major as stored: m[0..3] row 0, ..., m[12..14] translation

// ---------------------------------------------------------------- bMulMatrix 0x00441000 (122 callers via 0x43C9C0)
// out = M * v (point, with translation). Per component: ((m[8+c]*z + m[4+c]*y) + m[c]*x) + m[12+c], except the y
// component groups as ((m9*z + m1*x) + m5*y) and z as ((m10*z + m2*x) + m6*y) — copied from the x87 code.
using MulM_t = V3*(__cdecl*)(V3*, const M4*, const V3*);
MulM_t o_bMulMatrix = nullptr;
V3* __cdecl bMulMatrix(V3* out, const M4* M, const V3* v) {
    const float* m = M->m;
    const float x = ((m[8] * v->z + m[4] * v->y) + m[0] * v->x) + m[12];
    const float y = ((m[9] * v->z + m[1] * v->x) + m[5] * v->y) + m[13];
    const float z = ((m[10] * v->z + m[2] * v->x) + m[6] * v->y) + m[14];
    out->x = x;
    out->y = y;
    out->z = z;
    return out;
}

// ---------------------------------------------------------------- bRandomSeeded 0x0043C140 (8 callers)
using RandS_t = uint32_t(__cdecl*)(uint32_t, uint32_t*);
RandS_t o_bRandomSeeded = nullptr;
uint32_t __cdecl bRandomSeeded(uint32_t range, uint32_t* seed) {
    const uint32_t result = *seed % range;
    const uint32_t s = *seed ^ 0x1D872B41u;
    const uint32_t t = s ^ (s >> 5);
    *seed = (t << 27) ^ t ^ s;
    return result;
}

// ---------------------------------------------------------------- bMod 0x0043C280 (8 callers)
// m = |b|; c = (a/m)*m; if a < 0: c -= m; return a - c. Quirk: a negative multiple of b returns |b|, not 0.
using Mod_t = int(__cdecl*)(int, int);
Mod_t o_bMod = nullptr;
int __cdecl bMod(int a, int b) {
    const uint32_t m = b < 0 ? 0u - static_cast<uint32_t>(b) : static_cast<uint32_t>(b);
    const int32_t sm = static_cast<int32_t>(m);
    const int32_t q = (a == INT32_MIN && sm == -1) ? a : a / sm;  // (sm == -1 cannot happen; keeps UB away)
    uint32_t c = static_cast<uint32_t>(q) * m;
    if (a < 0) c -= m;
    return static_cast<int>(static_cast<uint32_t>(a) - c);
}

// ---------------------------------------------------------------- bFMod 0x0043C240 (11 callers)
// t = a/|b|; f = _ftol2(t) (truncate); if f > t: f -= 1; return (t - f) * |b|. Valid for |t| < 2^31.
using FMod_t = float(__cdecl*)(float, float);
FMod_t o_bFMod = nullptr;
float __cdecl bFMod(float a, float b) {
    const float ab = std::fabs(b);
    const float t = a / ab;
    float f = static_cast<float>(static_cast<int32_t>(static_cast<int64_t>(t)));  // exact for |t| < 2^24; beyond, t is integral
    if (f > t) f = f - 1.0f;
    return (t - f) * ab;
}

// ---------------------------------------------------------------- tests
float rf(game::Rng& r) {
    switch (r.below(8)) {
        case 0: return 0.0f;
        case 1: return -0.0f;
        case 2: return static_cast<float>(static_cast<int>(r.below(21)) - 10);
        default: {
            const float mag = static_cast<float>(r.next() & 0xFFFFFF) / 16777216.0f;
            static const float scales[] = {1e-3f, 1e-1f, 1.0f, 10.0f, 1e3f, 1e5f};
            const float v = mag * scales[r.below(6)];
            return r.below(2) ? -v : v;
        }
    }
}
template <class T> bool same(const T& a, const T& b) { return memcmp(&a, &b, sizeof(T)) == 0; }
std::string fs(float f) {
    char b[48];
    snprintf(b, sizeof b, "%.9g(0x%08x)", f, game::bits(f));
    return b;
}

}  // namespace

NFSU2_REWRITE_N(0x00441000, bMulMatrix, 3, "bVector3* __cdecl(bVector3* out, const bMatrix4*, const bVector3*) -> out in EAX", o_bMulMatrix);
NFSU2_REWRITE_N(0x0043C140, bRandomSeeded, 2, "uint32_t __cdecl(uint32_t range, uint32_t* seed)", o_bRandomSeeded);
NFSU2_REWRITE_N(0x0043C280, bMod, 2, "int __cdecl(int a, int b)", o_bMod);
NFSU2_REWRITE_N(0x0043C240, bFMod, 2, "float __cdecl(float a, float b)", o_bFMod);

NFSU2_DIFF_TEST(bMulMatrix) {
    return game::run_diff(ctx, "bMulMatrix", o_bMulMatrix, [](game::Rng& r) {
        M4 M;
        for (float& f : M.m) f = rf(r);
        V3 v{rf(r), rf(r), rf(r)}, a{}, b{};
        if (r.below(4) == 0) {  // in place
            a = b = v;
            o_bMulMatrix(&a, &M, &a);
            bMulMatrix(&b, &M, &b);
        } else {
            o_bMulMatrix(&a, &M, &v);
            bMulMatrix(&b, &M, &v);
        }
        return same(a, b) ? game::Ok() : game::MismatchText("v=(" + fs(v.x) + "," + fs(v.y) + "," + fs(v.z) + ")");
    });
}

NFSU2_DIFF_TEST(bRandomSeeded) {
    return game::run_diff(ctx, "bRandomSeeded", o_bRandomSeeded, [](game::Rng& r) {
        const uint32_t s0 = r.next(), range = 1 + (r.below(4) ? r.below(1000) : r.next() >> 1);
        uint32_t sa = s0, sb = s0;
        const uint32_t a = o_bRandomSeeded(range, &sa), b = bRandomSeeded(range, &sb);
        return (a == b && sa == sb) ? game::Ok()
                                    : game::MismatchText("seed=" + std::to_string(s0) + " range=" + std::to_string(range));
    });
}

NFSU2_DIFF_TEST(bMod) {
    return game::run_diff(ctx, "bMod", o_bMod, [](game::Rng& r) {
        int a, b;
        switch (r.below(4)) {
            case 0: a = static_cast<int>(r.below(201)) - 100; b = static_cast<int>(r.below(21)) - 10; break;
            case 1: b = static_cast<int>(r.below(19)) - 9; a = b * (static_cast<int>(r.below(21)) - 10); break;  // multiples
            case 2: a = INT32_MIN + static_cast<int>(r.below(3)); b = r.below(2) ? INT32_MIN : static_cast<int>(r.next()); break;
            default: a = static_cast<int>(r.next()); b = static_cast<int>(r.next());
        }
        if (b == 0) b = 7;
        int x = o_bMod(a, b), y = bMod(a, b);
        return x == y ? game::Ok() : game::Mismatch("a=" + std::to_string(a) + " b=" + std::to_string(b), x, y);
    });
}

NFSU2_DIFF_TEST(bFMod) {
    return game::run_diff(ctx, "bFMod", o_bFMod, [](game::Rng& r) {
        float a = rf(r), b = rf(r);
        if (b == 0.0f) b = 1.5f;
        if (std::fabs(a / b) >= 2.0e9f) a = 1.0f;  // documented validity range |a/b| < 2^31
        float x = o_bFMod(a, b), y = bFMod(a, b);
        return same(x, y) ? game::Ok() : game::MismatchText("a=" + fs(a) + " b=" + fs(b) + " -> " + fs(x) + " vs " + fs(y));
    });
}

NFSU2_DIFF_TEST(regs_bmatrix) {
    using game::arg;
    return game::run_diff(ctx, "regs_bmatrix", o_bMulMatrix, [](game::Rng& r) {
        M4 M;
        for (float& f : M.m) f = rf(r);
        V3 v{rf(r), rf(r), rf(r)}, o{};
        uint32_t seed = r.next();
        const uint32_t s0 = seed, range = 1 + r.below(1000);
        float b = rf(r);
        if (b == 0.0f) b = 2.5f;
        const float a = rf(r);
        int ia = static_cast<int>(r.next()), ib = static_cast<int>(r.below(100)) + 1;
        seed = s0;
        game::Case c1 = game::check_regs("bMulMatrix", (void*)o_bMulMatrix, NFSU2_ENTRY(bMulMatrix), {arg(&o), arg(&M), arg(&v)}, false);
        if (!c1.ok) return c1;
        const uint32_t rargs[2] = {range, arg(&seed)};
        const game::RegOut ra = game::call_regs((void*)o_bRandomSeeded, rargs, 2, false);
        seed = s0;
        const game::RegOut rb = game::call_regs(NFSU2_ENTRY(bRandomSeeded), rargs, 2, false);
        if (ra.eax != rb.eax) return game::MismatchText("bRandomSeeded: eax");
        for (auto c : {game::check_regs("bMod", (void*)o_bMod, NFSU2_ENTRY(bMod), {arg(ia), arg(ib)}, false),
                       game::check_regs("bFMod", (void*)o_bFMod, NFSU2_ENTRY(bFMod), {arg(a), arg(b)}, true)})
            if (!c.ok) return c;
        return game::Ok();
    });
}
