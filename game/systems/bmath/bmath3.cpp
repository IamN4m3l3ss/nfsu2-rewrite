// More bMath (EA core library, zBWare.cpp): eMulMatrix/bMulMatrix, bMatrixToQuaternion, bFixATan, the seeded float
// bRandom, the 4D bNormalize, 2D bEqual / bounding boxes, bTan, bLength.
// Algorithms and x87 operation order adapted from nfsu2-gcpc (CC0, github.com/octavioCuatrochio/nfsu2-gcpc,
// port/game/bWare/bMath.cpp). They compute in 80-bit long double (MinGW); here the game's x87 runs at 24-bit
// precision (CLAUDE.md "Floating point"), so each operation rounds like float and plain float with the same
// operation order is bit-identical in normal ranges - proven by our diff tests (inputs: realistic magnitudes, see rf).
#include <cmath>
#include <cstring>
#include <string>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

struct V2 { float x, y; };
struct V3 { float x, y, z; };
struct V4 { float x, y, z, w; };
struct M4 { float m[16]; };

// ---------------------------------------------------------------- eMulMatrix 0x004013F0: out = a x b
// each element summed in the original's (unrolled) order, written through a temporary (out may alias a or b)
using MulM_t = void(__cdecl*)(M4*, const M4*, const M4*);
MulM_t o_eMulMatrix = nullptr;
void __cdecl eMulMatrix(M4* out, const M4* pa, const M4* pb) {
    const float* a = pa->m;
    const float* b = pb->m;
    float r[16];
    r[0] = (((a[0] * b[0]) + (a[3] * b[12])) + (a[2] * b[8])) + (b[4] * a[1]);
    r[1] = (((b[13] * a[3]) + (a[1] * b[5])) + (b[9] * a[2])) + (b[1] * a[0]);
    r[2] = (((b[10] * a[2]) + (b[2] * a[0])) + (a[3] * b[14])) + (b[6] * a[1]);
    r[3] = (((b[15] * a[3]) + (a[2] * b[11])) + (a[0] * b[3])) + (b[7] * a[1]);
    r[4] = (((a[6] * b[8]) + (a[7] * b[12])) + (b[4] * a[5])) + (a[4] * b[0]);
    r[5] = (((a[6] * b[9]) + (a[4] * b[1])) + (a[5] * b[5])) + (a[7] * b[13]);
    r[6] = (((a[7] * b[14]) + (a[6] * b[10])) + (b[6] * a[5])) + (a[4] * b[2]);
    r[7] = (((b[15] * a[7]) + (a[6] * b[11])) + (a[4] * b[3])) + (b[7] * a[5]);
    r[8] = (((a[10] * b[8]) + (a[11] * b[12])) + (b[4] * a[9])) + (a[8] * b[0]);
    r[9] = (((a[10] * b[9]) + (a[8] * b[1])) + (a[9] * b[5])) + (a[11] * b[13]);
    r[10] = (((a[11] * b[14]) + (a[10] * b[10])) + (b[6] * a[9])) + (a[8] * b[2]);
    r[11] = (((b[15] * a[11]) + (a[10] * b[11])) + (a[8] * b[3])) + (b[7] * a[9]);
    r[12] = (((a[14] * b[8]) + (a[15] * b[12])) + (b[4] * a[13])) + (a[12] * b[0]);
    r[13] = (((a[14] * b[9]) + (a[12] * b[1])) + (a[13] * b[5])) + (a[15] * b[13]);
    r[14] = (((a[15] * b[14]) + (a[14] * b[10])) + (b[6] * a[13])) + (a[12] * b[2]);
    r[15] = (((b[15] * a[15]) + (a[14] * b[11])) + (a[12] * b[3])) + (b[7] * a[13]);
    std::memcpy(out->m, r, sizeof r);
}

// ---------------------------------------------------------------- bMulMatrix 0x0043C990 = eMulMatrix(out, b, a)
MulM_t o_bMulMatrix4 = nullptr;
void __cdecl bMulMatrix4(M4* out, const M4* a, const M4* b) { game_fn<MulM_t>(0x004013F0)(out, b, a); }

// ---------------------------------------------------------------- bMatrixToQuaternion 0x0043CBD0
// trace > 0: w = s/2, s = sqrt(trace + 1); else the largest diagonal element i (strictly greater wins, NaN keeps the
// earlier one); s == 0 skips the 0.5/s division. Written through a temporary.
using M2Q_t = void(__cdecl*)(V4*, const M4*);
M2Q_t o_bMatrixToQuaternion = nullptr;
void __cdecl bMatrixToQuaternion(V4* q, const M4* pm) {
    const float* m = pm->m;
    const float trace = (m[5] + m[0]) + m[10];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f);
        q->w = 0.5f * s;
        const float r = 0.5f / s;
        q->x = (m[6] - m[9]) * r;
        q->y = (m[8] - m[2]) * r;
        q->z = (m[1] - m[4]) * r;
        return;
    }
    int i = 0;
    if (m[5] > m[0]) i = 1;
    if (m[10] > m[i * 5]) i = 2;
    const int j = (i + 1) % 3, k = (j + 1) % 3;
    float t[4];
    float s = std::sqrt(((m[i * 5] - m[j * 5]) - m[k * 5]) + 1.0f);
    t[i] = 0.5f * s;
    if (s != 0.0f) s = 0.5f / s;
    t[3] = (m[j * 4 + k] - m[k * 4 + j]) * s;
    t[j] = (m[j * 4 + i] + m[i * 4 + j]) * s;
    t[k] = (m[k * 4 + i] + m[i * 4 + k]) * s;
    std::memcpy(q, t, sizeof t);
}

// ---------------------------------------------------------------- bFixATan(ratio) 0x0043C650, bFixATan(x, y) 0x0043C6F0
// atan of a 16.16 ratio, piecewise linear over two uint16 tables (0x7EB1C0 below 4.0, 0x7EB2C8 below 32.0), a fixed
// segment below 256.0, 0x3FFF above; negative input -> negated result
using FixAtan1_t = int32_t(__cdecl*)(int32_t);
FixAtan1_t o_bFixATan1 = nullptr;
int32_t __cdecl bFixATan1(int32_t ratio) {
    const bool neg = ratio < 0;
    const int32_t x = neg ? static_cast<int32_t>(0u - static_cast<uint32_t>(ratio)) : ratio;
    uint32_t c, s, frac;
    if (x >= 0x200000) {
        if (x < 0x1000000) {
            frac = static_cast<uint32_t>(x >> 8);
            c = 0x3EBA;
            s = 0x3FD7;
        } else {
            c = s = 0x3FFF;
            frac = 0;
        }
    } else if (x >= 0x40000) {
        const uint16_t* t = reinterpret_cast<const uint16_t*>(0x007EB2C8) + (x >> 14);
        c = t[0];
        s = t[1];
        frac = static_cast<uint32_t>(x & 0x3FFF) << 2;
    } else {
        const uint16_t* t = reinterpret_cast<const uint16_t*>(0x007EB1C0) + (x >> 11);
        c = t[0];
        s = t[1];
        frac = static_cast<uint32_t>(x & 0x7FF) << 5;
    }
    const uint32_t r = static_cast<uint32_t>(static_cast<int32_t>((s - c) * frac) >> 16) + c;
    return static_cast<int32_t>(neg ? 0u - r : r);
}

using FixAtan2_t = int32_t(__cdecl*)(int32_t, int32_t);
FixAtan2_t o_bFixATan2 = nullptr;
int32_t __cdecl bFixATan2(int32_t x, int32_t y) {  // integer atan2(y, x); |x| <= |y| >> 14 -> 0x4000
    int q = 0;
    if (x < 0) {
        q = 1;
        x = static_cast<int32_t>(0u - static_cast<uint32_t>(x));
    }
    if (y < 0) {
        q ^= 3;
        y = static_cast<int32_t>(0u - static_cast<uint32_t>(y));
    }
    uint32_t a;
    if (x <= (y >> 14)) {
        a = 0x4000;
    } else {
        int32_t r;
        if (x == 0) {
            r = (y == 0) ? 0 : static_cast<int32_t>(static_cast<uint32_t>(y <= 0) + 0x7FFFFFFFu);  // only y == INT_MIN
        } else {
            const int64_t p = static_cast<int64_t>(0x7FFFFFFF / x) * y;
            r = static_cast<int32_t>(static_cast<uint32_t>(p >> 16) << 1);
        }
        a = static_cast<uint16_t>(game_fn<FixAtan1_t>(0x0043C650)(r));
    }
    switch (q) {
        case 0: return static_cast<int32_t>(a);
        case 1: return static_cast<int32_t>(0x8000u - a);
        case 2: return static_cast<int32_t>(a - 0x8000u);
        default: return static_cast<int32_t>(0u - a);
    }
}

// ---------------------------------------------------------------- bRandom(float range, uint32* seed) 0x0043C170
using RandFS_t = float(__cdecl*)(float, uint32_t*);
RandFS_t o_bRandomFloatSeeded = nullptr;
float __cdecl bRandomFloatSeeded(float range, uint32_t* seed) {
    const uint32_t r = *seed % 0x7FFFFFFFu;
    const uint32_t s = *seed ^ 0x1D872B41u;
    const uint32_t t = s ^ (s >> 5);
    *seed = (t << 27) ^ t ^ s;
    return (static_cast<float>(static_cast<int32_t>(r)) * 4.656612873077393e-10f) * range;  // * 2^-31 (exact)
}

// ---------------------------------------------------------------- bNormalize(bVector4*, const bVector4*) 0x0043D010
// out.xyz = in.xyz / |in| (4D length); length exactly 0 -> (1,0,0); w not written
using Norm4_t = void(__cdecl*)(V4*, const V4*);
Norm4_t o_bNormalize4 = nullptr;
void __cdecl bNormalize4(V4* out, const V4* in) {
    const float x = in->x, y = in->y, z = in->z, w = in->w;
    const float len = std::sqrt(((x * x + y * y) + z * z) + w * w);
    if (len == 0.0f) {
        out->x = 1.0f;
        out->y = 0.0f;
        out->z = 0.0f;
        return;
    }
    const float inv = 1.0f / len;
    const float iz = in->z;
    const float iy = in->y;
    out->x = inv * in->x;
    out->y = iy * inv;
    out->z = iz * inv;
}

// ---------------------------------------------------------------- bEqual(bVector2) 0x0043D150 (NaN counts as equal)
using Eq2_t = int32_t(__cdecl*)(const V2*, const V2*, float);
Eq2_t o_bEqual2 = nullptr;
int32_t __cdecl bEqual2(const V2* a, const V2* b, float eps) {
    if (std::fabs(a->x - b->x) > eps) return 0;
    return !(std::fabs(a->y - b->y) > eps);
}

// ---------------------------------------------------------------- bounding boxes (2D)
using BBox1_t = void(__cdecl*)(V2*, V2*, const V2*);
BBox1_t o_bInitBoundsPoint = nullptr;
void __cdecl bInitBoundsPoint(V2* mn, V2* mx, const V2* p) {  // 0x0043D250: box of one point
    const float x = p->x, y = p->y;
    mn->x = x;
    mn->y = y;
    mx->x = x;
    mx->y = y;
}
// min = (min < v) ? min : v, max = (max > v) ? max : v per axis (NaN takes v)
inline void expand(V2* mn, V2* mx, float lx, float ly, float hx, float hy) {
    const float a = (mn->x < lx) ? mn->x : lx, b = (mn->y < ly) ? mn->y : ly;
    const float c = (mx->x > hx) ? mx->x : hx, d = (mx->y > hy) ? mx->y : hy;
    mn->x = a;
    mn->y = b;
    mx->x = c;
    mx->y = d;
}
BBox1_t o_bExpandBounds = nullptr;
void __cdecl bExpandBounds(V2* mn, V2* mx, const V2* p) { expand(mn, mx, p->x, p->y, p->x, p->y); }  // 0x0043D270
using BBoxR_t = void(__cdecl*)(V2*, V2*, const V2*, float);
BBoxR_t o_bExpandBoundsR = nullptr;
void __cdecl bExpandBoundsR(V2* mn, V2* mx, const V2* p, float r) {  // 0x0043D2F0
    expand(mn, mx, p->x - r, p->y - r, r + p->x, r + p->y);
}
using BBox2_t = void(__cdecl*)(V2*, V2*, const V2*, const V2*);
BBox2_t o_bExpandBounds2 = nullptr;
void __cdecl bExpandBounds2(V2* mn, V2* mx, const V2* p, const V2* q) { expand(mn, mx, p->x, p->y, q->x, q->y); }  // 0x0043D380

// ---------------------------------------------------------------- bTan 0x00440F50, bLength 0x00440F70 (out-of-line inlines)
using Tan_t = float(__cdecl*)(uint32_t);
Tan_t o_bTan = nullptr;
float __cdecl bTan(uint32_t angle) {
    const float s = game_fn<float(__cdecl*)(uint32_t)>(0x0043C2B0)(angle);  // bSin (float-rounded)
    return s / game_fn<float(__cdecl*)(uint32_t)>(0x0043C370)(angle);       // bCos
}
using Len_t = float(__cdecl*)(const V3*);
Len_t o_bLength = nullptr;
float __cdecl bLength(const V3* v) { return std::sqrt((v->x * v->x + v->y * v->y) + v->z * v->z); }

}  // namespace

NFSU2_REWRITE_N(0x004013F0, eMulMatrix, 3, "void __cdecl(bMatrix4* out, const bMatrix4* a, const bMatrix4* b) out = a x b", o_eMulMatrix);
NFSU2_REWRITE_N(0x0043C990, bMulMatrix4, 3, "void __cdecl bMulMatrix(out, a, b) = eMulMatrix(out, b, a)", o_bMulMatrix4);
NFSU2_REWRITE_N(0x0043CBD0, bMatrixToQuaternion, 2, "void __cdecl(bVector4* q, const bMatrix4* m)", o_bMatrixToQuaternion);
NFSU2_REWRITE_N(0x0043C650, bFixATan1, 1, "int __cdecl bFixATan(int ratio16_16)", o_bFixATan1);
NFSU2_REWRITE_N(0x0043C6F0, bFixATan2, 2, "int __cdecl bFixATan(int x, int y)", o_bFixATan2);
NFSU2_REWRITE_N(0x0043C170, bRandomFloatSeeded, 2, "float __cdecl bRandom(float range, uint32* seed)", o_bRandomFloatSeeded);
NFSU2_REWRITE_N(0x0043D010, bNormalize4, 2, "void __cdecl bNormalize(bVector4* out, const bVector4* in)", o_bNormalize4);
NFSU2_REWRITE_N(0x0043D150, bEqual2, 3, "int __cdecl bEqual(const bVector2*, const bVector2*, float eps)", o_bEqual2);
NFSU2_REWRITE_N(0x0043D250, bInitBoundsPoint, 3, "void __cdecl bInitializeBoundingBox(min, max, const bVector2* p)", o_bInitBoundsPoint);
NFSU2_REWRITE_N(0x0043D270, bExpandBounds, 3, "void __cdecl bExpandBoundingBox(min, max, p)", o_bExpandBounds);
NFSU2_REWRITE_N(0x0043D2F0, bExpandBoundsR, 4, "void __cdecl bExpandBoundingBox(min, max, p, float r)", o_bExpandBoundsR);
NFSU2_REWRITE_N(0x0043D380, bExpandBounds2, 4, "void __cdecl bExpandBoundingBox(min, max, p, q)", o_bExpandBounds2);
NFSU2_REWRITE_N(0x00440F50, bTan, 1, "float __cdecl bTan(uint16 angle)", o_bTan);
NFSU2_REWRITE_N(0x00440F70, bLength, 1, "float __cdecl bLength(const bVector3*)", o_bLength);

// ============================================================================================ diff tests
namespace {

// Inputs within realistic game magnitudes (|v| ~ 1e-3..1e5) plus exact 0, +-1, +-inf and NaN - the same contract as
// bmath.cpp: inside x87 registers the exponent is wider, so e.g. x*x of a 1e30 vector stays finite there but
// overflows a float; arbitrary bit patterns (first version of this test) showed exactly that in bNormalize(4)/bLength.
float rf(game::Rng& r) {
    switch (r.below(12)) {
        case 0: return 0.0f;
        case 1: return r.below(2) ? 1.0f : -1.0f;
        case 2: return r.below(2) ? INFINITY : -INFINITY;
        case 3: return r.below(4) ? (static_cast<float>(r.next() >> 8) / 16777216.0f - 0.5f) * 2e5f : NAN;
        default: return (static_cast<float>(r.next() >> 8) / 16777216.0f - 0.5f) * (r.below(3) ? 4.0f : 4000.0f);
    }
}
bool same(float a, float b) { return game::same_float(a, b); }
bool same_mem(const float* a, const float* b, int n) {
    for (int i = 0; i < n; ++i)
        if (!same(a[i], b[i])) return false;
    return true;
}

}  // namespace

NFSU2_DIFF_TEST(bMath3) {
    return game::run_diff(ctx, "bMath3", o_eMulMatrix, [](game::Rng& r) -> game::Case {
        const uint32_t which = r.below(13);
        M4 A, B, OA, OB;
        for (float& f : A.m) f = rf(r);
        for (float& f : B.m) f = rf(r);
        std::memset(&OA, 0x5A, sizeof OA);
        OB = OA;
        switch (which) {
            case 0: o_eMulMatrix(&OA, &A, &B); eMulMatrix(&OB, &A, &B); return same_mem(OA.m, OB.m, 16) ? game::Ok() : game::MismatchText("eMulMatrix");
            case 1: {  // aliasing: out == a
                M4 X = A, Y = A;
                o_eMulMatrix(&X, &X, &B);
                eMulMatrix(&Y, &Y, &B);
                return same_mem(X.m, Y.m, 16) ? game::Ok() : game::MismatchText("eMulMatrix alias");
            }
            case 2: o_bMulMatrix4(&OA, &A, &B); bMulMatrix4(&OB, &A, &B); return same_mem(OA.m, OB.m, 16) ? game::Ok() : game::MismatchText("bMulMatrix");
            case 3: {
                V4 qa, qb;
                std::memset(&qa, 0x5A, sizeof qa);
                qb = qa;
                if (r.below(2)) {  // a real rotation-ish matrix (trace > 0 and the other branches)
                    for (int i = 0; i < 16; ++i) A.m[i] = (static_cast<float>(r.below(2001)) - 1000.0f) / 1000.0f;
                }
                o_bMatrixToQuaternion(&qa, &A);
                bMatrixToQuaternion(&qb, &A);
                return same_mem(&qa.x, &qb.x, 4) ? game::Ok() : game::MismatchText("bMatrixToQuaternion");
            }
            case 4: {
                const int32_t v = r.below(3) ? static_cast<int32_t>(r.next()) >> r.below(24) : static_cast<int32_t>(r.next());
                return o_bFixATan1(v) == bFixATan1(v) ? game::Ok() : game::MismatchText("bFixATan(" + std::to_string(v) + ")");
            }
            case 5: {
                const int32_t x = static_cast<int32_t>(r.next()) >> r.below(31), y = static_cast<int32_t>(r.next()) >> r.below(31);
                const int32_t xx = r.below(20) == 0 ? 0 : x, yy = r.below(20) == 0 ? static_cast<int32_t>(0x80000000u) : y;
                return o_bFixATan2(xx, yy) == bFixATan2(xx, yy) ? game::Ok() : game::MismatchText("bFixATan(" + std::to_string(xx) + "," + std::to_string(yy) + ")");
            }
            case 6: {
                uint32_t sa = r.next(), sb = sa;
                const float range = rf(r);
                const float a = o_bRandomFloatSeeded(range, &sa), b = bRandomFloatSeeded(range, &sb);
                return same(a, b) && sa == sb ? game::Ok() : game::MismatchText("bRandom(float, seed)");
            }
            case 7: {
                V4 in{rf(r), rf(r), rf(r), rf(r)}, oa, ob;
                if (r.below(10) == 0) in = {0, 0, 0, 0};
                std::memset(&oa, 0x5A, sizeof oa);
                ob = oa;
                o_bNormalize4(&oa, &in);
                bNormalize4(&ob, &in);
                return same_mem(&oa.x, &ob.x, 4) ? game::Ok() : game::MismatchText("bNormalize(4)");
            }
            case 8: {
                V2 a{rf(r), rf(r)}, b{a.x + rf(r) * 1e-3f, a.y + rf(r) * 1e-3f};
                const float eps = std::fabs(rf(r)) * 1e-3f;
                return o_bEqual2(&a, &b, eps) == bEqual2(&a, &b, eps) ? game::Ok() : game::MismatchText("bEqual(2)");
            }
            case 9: case 10: case 11: {
                V2 mna{rf(r), rf(r)}, mxa{rf(r), rf(r)}, p{rf(r), rf(r)}, q{rf(r), rf(r)};
                V2 mnb = mna, mxb = mxa;
                const float rr = rf(r);
                if (which == 9) o_bExpandBounds(&mna, &mxa, &p), bExpandBounds(&mnb, &mxb, &p);
                else if (which == 10) o_bExpandBoundsR(&mna, &mxa, &p, rr), bExpandBoundsR(&mnb, &mxb, &p, rr);
                else if (r.below(2)) o_bExpandBounds2(&mna, &mxa, &p, &q), bExpandBounds2(&mnb, &mxb, &p, &q);
                else o_bInitBoundsPoint(&mna, &mxa, &p), bInitBoundsPoint(&mnb, &mxb, &p);
                return same_mem(&mna.x, &mnb.x, 2) && same_mem(&mxa.x, &mxb.x, 2) ? game::Ok() : game::MismatchText("bounding box " + std::to_string(which));
            }
            default: {
                if (r.below(2)) {
                    const uint32_t ang = r.next();
                    return same(o_bTan(ang), bTan(ang)) ? game::Ok() : game::MismatchText("bTan " + std::to_string(ang & 0xFFFF));
                }
                V3 v{rf(r), rf(r), rf(r)};
                return same(o_bLength(&v), bLength(&v)) ? game::Ok() : game::MismatchText("bLength");
            }
        }
    });
}

NFSU2_DIFF_TEST(regs_bmath3) {
    using game::arg;
    return game::run_diff(ctx, "regs_bmath3", o_bFixATan2, [](game::Rng& r) {
        V2 a{rf(r), rf(r)}, b{rf(r), rf(r)};
        for (auto c : {game::check_regs("bFixATan1", (void*)o_bFixATan1, NFSU2_ENTRY(bFixATan1), {r.next()}, false),
                       game::check_regs("bFixATan2", (void*)o_bFixATan2, NFSU2_ENTRY(bFixATan2), {r.next() >> 3, r.next() >> 3}, false),
                       game::check_regs("bEqual2", (void*)o_bEqual2, NFSU2_ENTRY(bEqual2), {arg(&a), arg(&b), game::bits(1e-3f)}, false)})
            if (!c.ok) return c;
        return game::Ok();
    });
}
