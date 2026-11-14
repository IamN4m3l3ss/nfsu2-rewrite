// EA bMath helpers (NA v1.2), rewritten from the disassembly. All __cdecl.
//
// Floats: the game thread runs x87 at 24-bit precision (CW 0x007F, measured), so each x87 + - * / fsqrt rounds like
// SSE single precision and plain `float` code matches bit for bit — PROVIDED the operation order is the same and
// values stay in normal ranges. (x87 keeps a 15-bit exponent in registers: products that overflow/underflow float
// would differ. Diff inputs therefore stay within realistic game magnitudes, 1e-3..1e5.) Operation order below is
// copied from the x87 sequences, e.g. bDistBetween sums (dz*dz + dy*dy) + dx*dx.
#include <cstring>
#include <string>

#include "../diff.h"
#include "../regs.h"
#include "../rewrite.h"

namespace {

// ---- FPU observation (2026-11-14): the user felt handling change with these enabled. Record the x87 control word
// at each real call. In passthrough mode every float rewrite calls the original, so enabling them changes nothing.
bool g_passthrough = false;  // measurement done 2026-11-14: 3.24M live calls, all at 24-bit precision
uint32_t g_cw_hist[4] = {};  // by precision control: 0=24-bit, 2=53-bit, 3=64-bit
uint32_t g_cw_other = 0;
struct NoPassthrough {  // diff tests must test our maths, not the passthrough
    bool saved = g_passthrough;
    NoPassthrough() { g_passthrough = false; }
    ~NoPassthrough() { g_passthrough = saved; }
};
inline void note_cw() {
    unsigned short cw;
    __asm { fnstcw cw }
    ++g_cw_hist[(cw >> 8) & 3];
    if ((cw & 0x0C3F) != 0x003F) ++g_cw_other;  // rounding not nearest or exceptions unmasked
}

struct V2 { float x, y; };
struct V3 { float x, y, z; };
struct M4 { float m[16]; };

// ---------------------------------------------------------------- bRandom 0x0043C1C0 (102 callers): the game RNG
constexpr uintptr_t kRandomSeed = 0x007EA624;
using Rand_t = uint32_t(__cdecl*)(uint32_t);
Rand_t o_bRandom = nullptr;
uint32_t __cdecl bRandom(uint32_t range) {
    uint32_t& seed = game_var<uint32_t>(kRandomSeed);
    const uint32_t result = seed % range;
    const uint32_t s = seed ^ 0x1D872B41u;
    const uint32_t t = s ^ (s >> 5);
    seed = (t << 27) ^ t ^ s;
    return result;
}

// ---------------------------------------------------------------- bIdentity 0x0043C780 (98 callers)
using Ident_t = M4*(__cdecl*)(M4*);
Ident_t o_bIdentity = nullptr;
M4* __cdecl bIdentity(M4* m) {
    for (int i = 0; i < 16; ++i) m->m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    return m;
}

// ---------------------------------------------------------------- bDistBetween 0x0043CE10 (74 callers)
using Dist_t = float(__cdecl*)(const V3*, const V3*);
Dist_t o_bDistBetween = nullptr;
float __cdecl bDistBetween(const V3* a, const V3* b) {
    note_cw();
    if (g_passthrough) return o_bDistBetween(a, b);
    const float dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return __builtin_sqrtf((dz * dz + dy * dy) + dx * dx);
}

// ---------------------------------------------------------------- bNormalize2 0x0043CE50 (39 callers)
using Norm2_t = V2*(__cdecl*)(V2*, const V2*);
Norm2_t o_bNormalize2 = nullptr;
V2* __cdecl bNormalize2(V2* out, const V2* in) {
    note_cw();
    if (g_passthrough) return o_bNormalize2(out, in);
    const float len = __builtin_sqrtf(in->y * in->y + in->x * in->x);
    if (len == 0.0f) {
        out->x = 1.0f;
        out->y = 0.0f;
        return out;
    }
    const float inv = 1.0f / len;
    const float y = in->y;
    out->x = inv * in->x;
    out->y = inv * y;
    return out;
}

// ---------------------------------------------------------------- bNormalize 0x0043CF10 (97 callers)
using Norm3_t = V3*(__cdecl*)(V3*, const V3*);
Norm3_t o_bNormalize = nullptr;
V3* __cdecl bNormalize(V3* out, const V3* in) {
    note_cw();
    if (g_passthrough) return o_bNormalize(out, in);
    const float len = __builtin_sqrtf((in->x * in->x + in->y * in->y) + in->z * in->z);
    if (len == 0.0f) {
        out->x = 1.0f;
        out->y = 0.0f;
        out->z = 0.0f;
        return out;
    }
    const float inv = 1.0f / len;
    const float y = in->y, z = in->z;  // read before writing: in-place use
    out->x = inv * in->x;
    out->y = inv * y;
    out->z = inv * z;
    return out;
}

// ---------------------------------------------------------------- bScaleAdd2 0x0043D090 / bScaleAdd 0x0043D0C0
using SA2_t = V2*(__cdecl*)(V2*, const V2*, const V2*, float);
SA2_t o_bScaleAdd2 = nullptr;
V2* __cdecl bScaleAdd2(V2* out, const V2* a, const V2* b, float s) {
    note_cw();
    if (g_passthrough) return o_bScaleAdd2(out, a, b, s);
    const float y = s * b->y + a->y, x = s * b->x + a->x;
    out->x = x;
    out->y = y;
    return out;
}

using SA3_t = V3*(__cdecl*)(V3*, const V3*, const V3*, float);
SA3_t o_bScaleAdd = nullptr;
V3* __cdecl bScaleAdd(V3* out, const V3* a, const V3* b, float s) {
    note_cw();
    if (g_passthrough) return o_bScaleAdd(out, a, b, s);
    const float y = s * b->y + a->y, z = s * b->z + a->z, x = s * b->x + a->x;
    out->x = x;
    out->y = y;
    out->z = z;
    return out;
}

// ---------------------------------------------------------------- bCross 0x0043D1E0 (85 callers)
using Cross_t = V3*(__cdecl*)(V3*, const V3*, const V3*);
Cross_t o_bCross = nullptr;
V3* __cdecl bCross(V3* out, const V3* a, const V3* b) {
    note_cw();
    if (g_passthrough) return o_bCross(out, a, b);
    const float y = b->x * a->z - b->z * a->x;
    const float z = b->y * a->x - a->y * b->x;
    const float x = b->z * a->y - b->y * a->z;
    out->x = x;
    out->y = y;
    out->z = z;
    return out;
}

// ---------------------------------------------------------------- bInitBounds2 0x0043D220 (11 callers)
using Bounds_t = V2*(__cdecl*)(V2*, V2*);  // returns max (left in EAX)
Bounds_t o_bInitBounds2 = nullptr;
V2* __cdecl bInitBounds2(V2* mn, V2* mx) {
    const uint32_t big = 0x7F7FFFFFu, nbig = 0xFF7FFFFFu;  // +/-FLT_MAX
    memcpy(&mn->x, &big, 4);
    memcpy(&mn->y, &big, 4);
    memcpy(&mx->x, &nbig, 4);
    memcpy(&mx->y, &nbig, 4);
    return mx;
}

// ---------------------------------------------------------------- test inputs
float rf(game::Rng& r) {
    switch (r.below(8)) {
        case 0: return 0.0f;
        case 1: return -0.0f;
        case 2: return static_cast<float>(static_cast<int>(r.below(201)) - 100);  // small integers
        default: {
            const float mag = static_cast<float>(r.next() & 0xFFFFFF) / 16777216.0f;  // [0,1)
            static const float scales[] = {1e-3f, 1e-1f, 1.0f, 10.0f, 1e3f, 1e5f};
            const float v = mag * scales[r.below(6)];
            return r.below(2) ? -v : v;
        }
    }
}
V3 rv3(game::Rng& r) { return {rf(r), rf(r), rf(r)}; }
V2 rv2(game::Rng& r) { return {rf(r), rf(r)}; }
template <class T> bool same_bits(const T& a, const T& b) { return memcmp(&a, &b, sizeof(T)) == 0; }
std::string fs(float f) {
    char b[48];
    uint32_t u;
    memcpy(&u, &f, 4);
    snprintf(b, sizeof b, "%.9g(0x%08x)", f, u);
    return b;
}
std::string v3s(const V3& v) { return "(" + fs(v.x) + "," + fs(v.y) + "," + fs(v.z) + ")"; }
std::string v2s(const V2& v) { return "(" + fs(v.x) + "," + fs(v.y) + ")"; }

}  // namespace

NFSU2_REWRITE_N(0x0043C1C0, bRandom, 1, "uint32_t __cdecl(uint32_t range); seed at 0x7EA624", o_bRandom);
NFSU2_REWRITE_N(0x0043C780, bIdentity, 1, "Matrix4* __cdecl(Matrix4*) -> m in EAX", o_bIdentity);
NFSU2_REWRITE_N(0x0043CE10, bDistBetween, 2, "float __cdecl(const bVector3*, const bVector3*)", o_bDistBetween);
NFSU2_REWRITE_N(0x0043CE50, bNormalize2, 2, "bVector2* __cdecl(bVector2* out, const bVector2* in) -> out in EAX", o_bNormalize2);
NFSU2_REWRITE_N(0x0043CF10, bNormalize, 2, "bVector3* __cdecl(bVector3* out, const bVector3* in) -> out in EAX", o_bNormalize);
NFSU2_REWRITE_N(0x0043D090, bScaleAdd2, 4, "bVector2* __cdecl(bVector2* out, const bVector2* a, const bVector2* b, float s) -> out in EAX", o_bScaleAdd2);
NFSU2_REWRITE_N(0x0043D0C0, bScaleAdd, 4, "bVector3* __cdecl(bVector3* out, const bVector3* a, const bVector3* b, float s) -> out in EAX", o_bScaleAdd);
NFSU2_REWRITE_N(0x0043D1E0, bCross, 3, "bVector3* __cdecl(bVector3* out, const bVector3* a, const bVector3* b) -> out in EAX", o_bCross);
NFSU2_REWRITE_N(0x0043D220, bInitBounds2, 2, "bVector2* __cdecl(bVector2* min, bVector2* max) -> max in EAX", o_bInitBounds2);

NFSU2_DIFF_TEST(bRandom) {
    // stateful: run both from the same seed, compare result + next seed, then put the game's seed back
    uint32_t& seed = game_var<uint32_t>(kRandomSeed);
    const uint32_t saved = seed;
    game::DiffResult res = game::run_diff(ctx, "bRandom", o_bRandom, [&seed](game::Rng& r) {
        const uint32_t s0 = r.next(), range = 1 + (r.below(4) ? r.below(1000) : r.next() >> 1);
        seed = s0;
        const uint32_t a = o_bRandom(range), sa = seed;
        seed = s0;
        const uint32_t b = bRandom(range), sb = seed;
        if (a != b || sa != sb)
            return game::MismatchText("seed=" + std::to_string(s0) + " range=" + std::to_string(range) + " result " +
                                      std::to_string(a) + "/" + std::to_string(b) + " next seed " + std::to_string(sa) +
                                      "/" + std::to_string(sb));
        return game::Ok();
    });
    seed = saved;
    return res;
}

NFSU2_DIFF_TEST(bIdentity) {
    NoPassthrough np;
    return game::run_diff(ctx, "bIdentity", o_bIdentity, [](game::Rng& r) {
        M4 a, b;
        for (int i = 0; i < 16; ++i) a.m[i] = b.m[i] = rf(r);
        o_bIdentity(&a);
        bIdentity(&b);
        return same_bits(a, b) ? game::Ok() : game::MismatchText("matrices differ");
    });
}

NFSU2_DIFF_TEST(bDistBetween) {
    NoPassthrough np;
    return game::run_diff(ctx, "bDistBetween", o_bDistBetween, [](game::Rng& r) {
        V3 p = rv3(r), q = r.below(8) ? rv3(r) : p;
        float a = o_bDistBetween(&p, &q), b = bDistBetween(&p, &q);
        return same_bits(a, b) ? game::Ok() : game::MismatchText(v3s(p) + " " + v3s(q) + " -> " + fs(a) + " vs " + fs(b));
    });
}

NFSU2_DIFF_TEST(bNormalize2) {
    NoPassthrough np;
    return game::run_diff(ctx, "bNormalize2", o_bNormalize2, [](game::Rng& r) {
        V2 in = r.below(10) ? rv2(r) : V2{0.0f, 0.0f}, a{9, 9}, b{9, 9};
        o_bNormalize2(&a, &in);
        bNormalize2(&b, &in);
        return same_bits(a, b) ? game::Ok() : game::MismatchText(v2s(in) + " -> " + v2s(a) + " vs " + v2s(b));
    });
}

NFSU2_DIFF_TEST(bNormalize) {
    NoPassthrough np;
    return game::run_diff(ctx, "bNormalize", o_bNormalize, [](game::Rng& r) {
        V3 in = r.below(10) ? rv3(r) : V3{0, 0, 0}, a{9, 9, 9}, b{9, 9, 9};
        if (r.below(4) == 0) {  // in place
            a = in;
            b = in;
            o_bNormalize(&a, &a);
            bNormalize(&b, &b);
        } else {
            o_bNormalize(&a, &in);
            bNormalize(&b, &in);
        }
        return same_bits(a, b) ? game::Ok() : game::MismatchText(v3s(in) + " -> " + v3s(a) + " vs " + v3s(b));
    });
}

NFSU2_DIFF_TEST(bScaleAdd2) {
    NoPassthrough np;
    return game::run_diff(ctx, "bScaleAdd2", o_bScaleAdd2, [](game::Rng& r) {
        V2 x = rv2(r), y = rv2(r), a{}, b{};
        const float s = rf(r);
        o_bScaleAdd2(&a, &x, &y, s);
        bScaleAdd2(&b, &x, &y, s);
        return same_bits(a, b) ? game::Ok() : game::MismatchText(v2s(x) + " " + v2s(y) + " s=" + fs(s));
    });
}

NFSU2_DIFF_TEST(bScaleAdd) {
    NoPassthrough np;
    return game::run_diff(ctx, "bScaleAdd", o_bScaleAdd, [](game::Rng& r) {
        V3 x = rv3(r), y = rv3(r), a{}, b{};
        const float s = rf(r);
        o_bScaleAdd(&a, &x, &y, s);
        bScaleAdd(&b, &x, &y, s);
        return same_bits(a, b) ? game::Ok() : game::MismatchText(v3s(x) + " " + v3s(y) + " s=" + fs(s));
    });
}

NFSU2_DIFF_TEST(bCross) {
    NoPassthrough np;
    return game::run_diff(ctx, "bCross", o_bCross, [](game::Rng& r) {
        V3 x = rv3(r), y = rv3(r), a{}, b{};
        if (r.below(4) == 0) {  // aliased output
            a = x;
            b = x;
            o_bCross(&a, &a, &y);
            bCross(&b, &b, &y);
        } else {
            o_bCross(&a, &x, &y);
            bCross(&b, &x, &y);
        }
        return same_bits(a, b) ? game::Ok() : game::MismatchText(v3s(x) + " x " + v3s(y) + " -> " + v3s(a) + " vs " + v3s(b));
    });
}

NFSU2_DIFF_TEST(bInitBounds2) {
    NoPassthrough np;
    return game::run_diff(ctx, "bInitBounds2", o_bInitBounds2, [](game::Rng& r) {
        V2 a0 = rv2(r), a1 = rv2(r), b0 = a0, b1 = a1;
        o_bInitBounds2(&a0, &a1);
        bInitBounds2(&b0, &b1);
        return same_bits(a0, b0) && same_bits(a1, b1) ? game::Ok() : game::MismatchText("bounds differ");
    });
}

// Harness self-check for floats: same formula, different summation order. Must report (some) mismatches — bit-exact
// matching needs the original's operation order, not just the same maths.
namespace {
float __cdecl bDistBetween_REORDERED(const V3* a, const V3* b) {
    const float dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return __builtin_sqrtf((dx * dx + dy * dy) + dz * dz);
}
}  // namespace

NFSU2_DIFF_TEST(bDistBetween_BROKEN) {
    NoPassthrough np;
    return game::run_diff(ctx, "bDistBetween_BROKEN", o_bDistBetween, [](game::Rng& r) {
        V3 p = rv3(r), q = rv3(r);
        float a = o_bDistBetween(&p, &q), b = bDistBetween_REORDERED(&p, &q);
        return same_bits(a, b) ? game::Ok() : game::MismatchText(v3s(p) + " " + v3s(q) + " -> " + fs(a) + " vs " + fs(b));
    });
}

// report: the counts arrive as the "examples" of a diff test (no extra API needed)
NFSU2_DIFF_TEST(fpu_cw_report) {
    game::DiffResult r;
    r.name = "fpu_cw_report";
    r.examples.push_back("calls with precision 24-bit=" + std::to_string(g_cw_hist[0]) + " 53-bit=" +
                         std::to_string(g_cw_hist[2]) + " 64-bit=" + std::to_string(g_cw_hist[3]) +
                         " other-rounding/exceptions=" + std::to_string(g_cw_other) +
                         " passthrough=" + std::to_string(g_passthrough));
    (void)ctx;
    return r;
}

NFSU2_DIFF_TEST(regs_bmath) {
    using game::arg;
    uint32_t& seed = game_var<uint32_t>(kRandomSeed);
    const uint32_t saved = seed;
    const bool pt = g_passthrough;
    g_passthrough = false;  // check our maths, not the passthrough
    game::DiffResult res = game::run_diff(ctx, "regs_bmath", o_bDistBetween, [&seed](game::Rng& r) {
        V3 a = rv3(r), b = rv3(r), o{};
        V2 a2 = rv2(r), b2 = rv2(r), o2{}, mn{}, mx{};
        M4 m{};
        const float s = rf(r);
        const uint32_t s0 = r.next(), range = 1 + r.below(1000);
        seed = s0;  // both calls of check_regs see the same seed? no: each call advances it -> compare EAX per call
        const game::RegOut ra = game::call_regs((void*)o_bRandom, &range, 1, false);
        seed = s0;
        const game::RegOut rb = game::call_regs(NFSU2_ENTRY(bRandom), &range, 1, false);
        if (ra.eax != rb.eax || (ra.ecx == game::kSentECX && rb.ecx != game::kSentECX) ||
            (ra.edx == game::kSentEDX && rb.edx != game::kSentEDX))
            return game::MismatchText("bRandom: register contract");
        for (auto c : {game::check_regs("bIdentity", (void*)o_bIdentity, NFSU2_ENTRY(bIdentity), {arg(&m)}, false),
                       game::check_regs("bDistBetween", (void*)o_bDistBetween, NFSU2_ENTRY(bDistBetween), {arg(&a), arg(&b)}, true),
                       game::check_regs("bNormalize2", (void*)o_bNormalize2, NFSU2_ENTRY(bNormalize2), {arg(&o2), arg(&a2)}, false),
                       game::check_regs("bNormalize", (void*)o_bNormalize, NFSU2_ENTRY(bNormalize), {arg(&o), arg(&a)}, false),
                       game::check_regs("bScaleAdd2", (void*)o_bScaleAdd2, NFSU2_ENTRY(bScaleAdd2), {arg(&o2), arg(&a2), arg(&b2), arg(s)}, false),
                       game::check_regs("bScaleAdd", (void*)o_bScaleAdd, NFSU2_ENTRY(bScaleAdd), {arg(&o), arg(&a), arg(&b), arg(s)}, false),
                       game::check_regs("bCross", (void*)o_bCross, NFSU2_ENTRY(bCross), {arg(&o), arg(&a), arg(&b)}, false),
                       game::check_regs("bInitBounds2", (void*)o_bInitBounds2, NFSU2_ENTRY(bInitBounds2), {arg(&mn), arg(&mx)}, false)})
            if (!c.ok) return c;
        return game::Ok();
    });
    seed = saved;
    g_passthrough = pt;
    return res;
}

// Harness self-check for the register contract: today's actual bug (bScaleAdd written as `void`, EAX garbage).
namespace {
void __cdecl bScaleAdd_void(V3* out, const V3* a, const V3* b, float s) { bScaleAdd(out, a, b, s); }
}  // namespace
NFSU2_DIFF_TEST(regs_BROKEN) {
    using game::arg;
    NoPassthrough np;
    return game::run_diff(ctx, "regs_BROKEN", o_bScaleAdd, [](game::Rng& r) {
        V3 a = rv3(r), b = rv3(r), o{};
        const float s = rf(r);
        return game::check_regs("bScaleAdd(void)", (void*)o_bScaleAdd, reinterpret_cast<const void*>(&bScaleAdd_void),
                                {arg(&o), arg(&a), arg(&b), arg(s)}, false);
    });
}
