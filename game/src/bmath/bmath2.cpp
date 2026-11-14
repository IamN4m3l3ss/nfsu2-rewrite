// EA bMath, batch 2 (NA v1.2). __cdecl, installed through register-preserving entry stubs (regs.h).
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "../diff.h"
#include "../regs.h"
#include "../rewrite.h"

namespace {

struct V2 { float x, y; };
struct V3 { float x, y, z; };

// ---------------------------------------------------------------- bNormalizeTo 0x0043CF90 (25 callers)
// out = in * (1 / (|in| / len)); |in| summed (x*x + y*y) + z*z. Zero -> (len, 0, 0). out in EAX.
using NT3_t = V3*(__cdecl*)(V3*, const V3*, float);
NT3_t o_bNormalizeTo = nullptr;
V3* __cdecl bNormalizeTo(V3* out, const V3* in, float len) {
    const float l = __builtin_sqrtf((in->x * in->x + in->y * in->y) + in->z * in->z) / len;
    if (l == 0.0f) {
        out->x = len;
        out->y = 0.0f;
        out->z = 0.0f;
        return out;
    }
    const float inv = 1.0f / l;
    const float y = in->y, z = in->z;
    out->x = inv * in->x;
    out->y = y * inv;
    out->z = z * inv;
    return out;
}

// ---------------------------------------------------------------- bNormalizeTo2 0x0043CEB0 (13 callers)
// 2D: |in| summed y*y + x*x, divided by len, stored as float. Zero -> (1, 0) (unlike the 3D version). out in EAX.
using NT2_t = V2*(__cdecl*)(V2*, const V2*, float);
NT2_t o_bNormalizeTo2 = nullptr;
V2* __cdecl bNormalizeTo2(V2* out, const V2* in, float len) {
    const float l = __builtin_sqrtf(in->y * in->y + in->x * in->x) / len;
    if (l == 0.0f) {
        out->x = 1.0f;
        out->y = 0.0f;
        return out;
    }
    const float inv = 1.0f / l;
    const float y = in->y;
    out->x = inv * in->x;
    out->y = inv * y;
    return out;
}

// ---------------------------------------------------------------- bEqualEps 0x0043D190 (10 callers)
// 1 if |a-b| <= eps on every axis. Written as !(d > eps): NaN counts as equal.
using Eq_t = int(__cdecl*)(const V3*, const V3*, float);
Eq_t o_bEqualEps = nullptr;
int __cdecl bEqualEps(const V3* a, const V3* b, float eps) {
    if (std::fabs(a->x - b->x) > eps) return 0;
    if (std::fabs(a->y - b->y) > eps) return 0;
    if (std::fabs(a->z - b->z) > eps) return 0;
    return 1;
}

// ---------------------------------------------------------------- bInBox2 0x0043D410 (17 callers)
// 1 if lo - m <= p <= hi + m on x and y (computed as lo.x - m and m + hi.x, in that order).
using InBox_t = int(__cdecl*)(const V2*, const V2*, const V2*, float);
InBox_t o_bInBox2 = nullptr;
int __cdecl bInBox2(const V2* lo, const V2* hi, const V2* p, float m) {
    if (lo->x - m > p->x) return 0;   // original: !(lo.x - m <= p.x) -> 0
    if (m + hi->x < p->x) return 0;   // original: (m + hi.x) < p.x -> 0
    if (lo->y - m > p->y) return 0;
    if (m + hi->y < p->y) return 0;
    return 1;
}

// ---------------------------------------------------------------- tests
float rf(game::Rng& r) {
    switch (r.below(10)) {
        case 0: return 0.0f;
        case 1: return -0.0f;
        case 2: return static_cast<float>(static_cast<int>(r.below(21)) - 10);
        case 3: return std::numeric_limits<float>::quiet_NaN();
        default: {
            const float mag = static_cast<float>(r.next() & 0xFFFFFF) / 16777216.0f;
            static const float scales[] = {1e-3f, 1e-1f, 1.0f, 10.0f, 1e3f, 1e5f};
            const float v = mag * scales[r.below(6)];
            return r.below(2) ? -v : v;
        }
    }
}
float rf_finite(game::Rng& r) {
    float f;
    do f = rf(r);
    while (f != f);
    return f;
}
template <class T> bool same(const T& a, const T& b) { return memcmp(&a, &b, sizeof(T)) == 0; }
std::string fs(float f) {
    char b[48];
    snprintf(b, sizeof b, "%.9g(0x%08x)", f, game::bits(f));
    return b;
}

}  // namespace

NFSU2_REWRITE_N(0x0043CF90, bNormalizeTo, 3, "bVector3* __cdecl(bVector3* out, const bVector3* in, float len) -> out in EAX", o_bNormalizeTo);
NFSU2_REWRITE_N(0x0043CEB0, bNormalizeTo2, 3, "bVector2* __cdecl(bVector2* out, const bVector2* in, float len) -> out in EAX", o_bNormalizeTo2);
NFSU2_REWRITE_N(0x0043D190, bEqualEps, 3, "int __cdecl(const bVector3*, const bVector3*, float eps)", o_bEqualEps);
NFSU2_REWRITE_N(0x0043D410, bInBox2, 4, "int __cdecl(const bVector2* lo, const bVector2* hi, const bVector2* p, float margin)", o_bInBox2);

NFSU2_DIFF_TEST(bNormalizeTo) {
    return game::run_diff(ctx, "bNormalizeTo", o_bNormalizeTo, [](game::Rng& r) {
        V3 in = r.below(10) ? V3{rf_finite(r), rf_finite(r), rf_finite(r)} : V3{0, 0, 0}, a{9, 9, 9}, b{9, 9, 9};
        const float len = rf_finite(r);
        V3* ra = o_bNormalizeTo(&a, &in, len);
        V3* rb = bNormalizeTo(&b, &in, len);
        if (!same(a, b) || (ra == &a) != (rb == &b))
            return game::MismatchText("in=(" + fs(in.x) + "," + fs(in.y) + "," + fs(in.z) + ") len=" + fs(len));
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(bNormalizeTo2) {
    return game::run_diff(ctx, "bNormalizeTo2", o_bNormalizeTo2, [](game::Rng& r) {
        V2 in = r.below(10) ? V2{rf_finite(r), rf_finite(r)} : V2{0, 0}, a{9, 9}, b{9, 9};
        const float len = rf_finite(r);
        o_bNormalizeTo2(&a, &in, len);
        bNormalizeTo2(&b, &in, len);
        if (!same(a, b)) return game::MismatchText("in=(" + fs(in.x) + "," + fs(in.y) + ") len=" + fs(len));
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(bEqualEps) {
    return game::run_diff(ctx, "bEqualEps", o_bEqualEps, [](game::Rng& r) {
        V3 a{rf(r), rf(r), rf(r)}, b = a;
        for (float* f : {&b.x, &b.y, &b.z})
            if (r.below(2)) *f += rf_finite(r) * (r.below(2) ? 1e-4f : 1.0f);
        const float eps = r.below(8) ? std::fabs(rf_finite(r)) : rf(r);
        int x = o_bEqualEps(&a, &b, eps), y = bEqualEps(&a, &b, eps);
        return x == y ? game::Ok() : game::Mismatch("eps=" + fs(eps), x, y);
    });
}

NFSU2_DIFF_TEST(bInBox2) {
    return game::run_diff(ctx, "bInBox2", o_bInBox2, [](game::Rng& r) {
        V2 lo{rf_finite(r), rf_finite(r)}, hi{lo.x + std::fabs(rf_finite(r)), lo.y + std::fabs(rf_finite(r))}, p;
        switch (r.below(4)) {
            case 0: p = lo; break;  // exactly on the corner
            case 1: p = hi; break;
            default: p = {rf_finite(r), rf_finite(r)};
        }
        const float m = r.below(3) ? std::fabs(rf_finite(r)) : rf(r);
        int x = o_bInBox2(&lo, &hi, &p, m), y = bInBox2(&lo, &hi, &p, m);
        return x == y ? game::Ok() : game::Mismatch("p=(" + fs(p.x) + "," + fs(p.y) + ") m=" + fs(m), x, y);
    });
}

NFSU2_DIFF_TEST(regs_bmath2) {
    using game::arg;
    return game::run_diff(ctx, "regs_bmath2", o_bEqualEps, [](game::Rng& r) {
        V3 a{rf_finite(r), rf_finite(r), rf_finite(r)}, b{rf_finite(r), rf_finite(r), rf_finite(r)}, o{};
        V2 a2{rf_finite(r), rf_finite(r)}, b2{a2.x + 1, a2.y + 1}, p2{rf_finite(r), rf_finite(r)}, o2{};
        const float s = rf_finite(r);
        for (auto c : {game::check_regs("bNormalizeTo", (void*)o_bNormalizeTo, NFSU2_ENTRY(bNormalizeTo), {arg(&o), arg(&a), arg(s)}, false),
                       game::check_regs("bNormalizeTo2", (void*)o_bNormalizeTo2, NFSU2_ENTRY(bNormalizeTo2), {arg(&o2), arg(&a2), arg(s)}, false),
                       game::check_regs("bEqualEps", (void*)o_bEqualEps, NFSU2_ENTRY(bEqualEps), {arg(&a), arg(&b), arg(std::fabs(s))}, false),
                       game::check_regs("bInBox2", (void*)o_bInBox2, NFSU2_ENTRY(bInBox2), {arg(&a2), arg(&b2), arg(&p2), arg(std::fabs(s))}, false)})
            if (!c.ok) return c;
        return game::Ok();
    });
}
