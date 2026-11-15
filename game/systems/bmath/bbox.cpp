// Bounding boxes (EA core library, zBWare.cpp): the remaining bInitializeBoundingBox / bExpandBoundingBox /
// bBoundingBoxIsInside / bBoundingBoxOverlapping overloads (the 2D point ones are in bmath3.cpp).
// Written from the PC machine code (0x0043D470..0x0043D888). Comparisons follow the original's x87 flag tests exactly,
// including what happens with NaN (unordered): noted per function.
#include <cmath>
#include <cstring>
#include <string>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

struct V2 { float x, y; };
struct V3 { float x, y, z; };

// ---------------------------------------------------------------- 2D box vs box
// 0x0043D470 bBoundingBoxIsInside(minA, maxA, minB, maxB): box B inside box A. Per axis B.min >= A.min and
// B.max <= A.max, ordered (fcomp + test ah,1 / test ah,41h;jp: NaN fails).
using Box2_t = int(__cdecl*)(const V2*, const V2*, const V2*, const V2*);
Box2_t o_bBoxInside2 = nullptr;
int __cdecl bBoxInside2(const V2* minA, const V2* maxA, const V2* minB, const V2* maxB) {
    if (!(minB->x >= minA->x) || !(maxB->x <= maxA->x)) return 0;
    if (!(minB->y >= minA->y) || !(maxB->y <= maxA->y)) return 0;
    return 1;
}
// 0x0043D4C0 bBoundingBoxOverlapping(minA, maxA, minB, maxB): per axis B.min < A.max and B.max > A.min, strict
// and ordered (test ah,5;jp / test ah,41h;jne: NaN fails).
Box2_t o_bBoxOverlap2 = nullptr;
int __cdecl bBoxOverlap2(const V2* minA, const V2* maxA, const V2* minB, const V2* maxB) {
    if (!(minB->x < maxA->x) || !(maxB->x > minA->x)) return 0;
    if (!(minB->y < maxA->y) || !(maxB->y > minA->y)) return 0;
    return 1;
}

// ---------------------------------------------------------------- 3D
// 0x0043D510 bInitializeBoundingBox(min, max): empty box, min = +FLT_MAX, max = -FLT_MAX (stored as integers).
// Leaves `max` in EAX.
using Init3_t = V3*(__cdecl*)(V3*, V3*);
Init3_t o_bInitBox3 = nullptr;
V3* __cdecl bInitBox3(V3* mn, V3* mx) {
    const uint32_t big = 0x7F7FFFFFu, nbig = 0xFF7FFFFFu;
    std::memcpy(&mn->x, &big, 4);
    std::memcpy(&mn->y, &big, 4);
    std::memcpy(&mn->z, &big, 4);
    std::memcpy(&mx->x, &nbig, 4);
    std::memcpy(&mx->y, &nbig, 4);
    std::memcpy(&mx->z, &nbig, 4);
    return mx;
}
// 0x0043D540 bInitializeBoundingBox(min, max, p): box of one point (all three loaded first, so p may alias min/max).
// Leaves `max` in EAX.
using InitP3_t = V3*(__cdecl*)(V3*, V3*, const V3*);
InitP3_t o_bInitBoxPoint3 = nullptr;
V3* __cdecl bInitBoxPoint3(V3* mn, V3* mx, const V3* p) {
    const V3 v = *p;
    *mn = v;
    *mx = v;
    return mx;
}
// Expand: an axis changes only when the new value is strictly smaller (min) / bigger (max), ordered compares (a NaN
// is never written; min and max are tested independently). The original writes min first, then max.
inline void expand3(V3* mn, V3* mx, const V3& lo, const V3& hi) {
    if (lo.x < mn->x) mn->x = lo.x;
    if (lo.y < mn->y) mn->y = lo.y;
    if (lo.z < mn->z) mn->z = lo.z;
    if (hi.x > mx->x) mx->x = hi.x;
    if (hi.y > mx->y) mx->y = hi.y;
    if (hi.z > mx->z) mx->z = hi.z;
}
// 0x0043D570 bExpandBoundingBox(min, max, p). p is read completely before anything is written.
using Exp3_t = void(__cdecl*)(V3*, V3*, const V3*);
Exp3_t o_bExpandBox3 = nullptr;
void __cdecl bExpandBox3(V3* mn, V3* mx, const V3* p) {
    const V3 v = *p;
    expand3(mn, mx, v, v);
}
// 0x0043D610 bExpandBoundingBox(min, max, p, r): by the box p-r .. r+p (all six computed before writing; at the
// game's 24-bit x87 precision each rounds like float).
using Exp3R_t = void(__cdecl*)(V3*, V3*, const V3*, float);
Exp3R_t o_bExpandBoxR3 = nullptr;
void __cdecl bExpandBoxR3(V3* mn, V3* mx, const V3* p, float r) {
    const V3 lo{p->x - r, p->y - r, p->z - r}, hi{r + p->x, r + p->y, r + p->z};
    expand3(mn, mx, lo, hi);
}
// 0x0043D6E0 bExpandBoundingBox(min, max, p, q): min by p, max by q (both read before writing).
using Exp3PQ_t = void(__cdecl*)(V3*, V3*, const V3*, const V3*);
Exp3PQ_t o_bExpandBoxPQ3 = nullptr;
void __cdecl bExpandBoxPQ3(V3* mn, V3* mx, const V3* p, const V3* q) {
    const V3 lo = *p, hi = *q;
    expand3(mn, mx, lo, hi);
}
// 0x0043D7A0 bBoundingBoxIsInside(lo, hi, p, margin): per axis fails when lo-m > p or m+hi < p; an unordered
// compare (NaN) PASSES (test ah,41h;je / test ah,5;jnp).
using In3_t = int(__cdecl*)(const V3*, const V3*, const V3*, float);
In3_t o_bBoxInside3 = nullptr;
int __cdecl bBoxInside3(const V3* lo, const V3* hi, const V3* p, float m) {
    if (lo->x - m > p->x || m + hi->x < p->x) return 0;
    if (lo->y - m > p->y || m + hi->y < p->y) return 0;
    if (lo->z - m > p->z || m + hi->z < p->z) return 0;
    return 1;
}
// 0x0043D820 bBoundingBoxOverlapping(minA, maxA, minB, maxB): as the 2D one, three axes.
using Box3_t = int(__cdecl*)(const V3*, const V3*, const V3*, const V3*);
Box3_t o_bBoxOverlap3 = nullptr;
int __cdecl bBoxOverlap3(const V3* minA, const V3* maxA, const V3* minB, const V3* maxB) {
    if (!(minB->x < maxA->x) || !(maxB->x > minA->x)) return 0;
    if (!(minB->y < maxA->y) || !(maxB->y > minA->y)) return 0;
    if (!(minB->z < maxA->z) || !(maxB->z > minA->z)) return 0;
    return 1;
}

}  // namespace

NFSU2_REWRITE_N(0x0043D470, bBoxInside2, 4, "int __cdecl bBoundingBoxIsInside(minA, maxA, minB, maxB) 2D box in box", o_bBoxInside2);
NFSU2_REWRITE_N(0x0043D4C0, bBoxOverlap2, 4, "int __cdecl bBoundingBoxOverlapping(minA, maxA, minB, maxB) 2D", o_bBoxOverlap2);
NFSU2_REWRITE_N(0x0043D510, bInitBox3, 2, "void __cdecl bInitializeBoundingBox(bVector3* min, bVector3* max) empty", o_bInitBox3);
NFSU2_REWRITE_N(0x0043D540, bInitBoxPoint3, 3, "void __cdecl bInitializeBoundingBox(min, max, const bVector3* p)", o_bInitBoxPoint3);
NFSU2_REWRITE_N(0x0043D570, bExpandBox3, 3, "void __cdecl bExpandBoundingBox(min, max, const bVector3* p)", o_bExpandBox3);
NFSU2_REWRITE_N(0x0043D610, bExpandBoxR3, 4, "void __cdecl bExpandBoundingBox(min, max, const bVector3* p, float r)", o_bExpandBoxR3);
NFSU2_REWRITE_N(0x0043D6E0, bExpandBoxPQ3, 4, "void __cdecl bExpandBoundingBox(min, max, const bVector3* p, const bVector3* q)", o_bExpandBoxPQ3);
NFSU2_REWRITE_N(0x0043D7A0, bBoxInside3, 4, "int __cdecl bBoundingBoxIsInside(lo, hi, const bVector3* p, float margin)", o_bBoxInside3);
NFSU2_REWRITE_N(0x0043D820, bBoxOverlap3, 4, "int __cdecl bBoundingBoxOverlapping(minA, maxA, minB, maxB) 3D", o_bBoxOverlap3);

namespace {

// realistic magnitudes plus the edge cases the flag tests care about: ties, +-0, infinities, NaN
float rf(game::Rng& r) {
    switch (r.below(14)) {
        case 0: return 0.0f;
        case 1: return -0.0f;
        case 2: return r.below(2) ? 1.0f : -1.0f;
        case 3: return r.below(2) ? INFINITY : -INFINITY;
        case 4: return NAN;
        case 5: return static_cast<float>(static_cast<int>(r.below(9)) - 4);  // small integers: many ties
        default: return (static_cast<float>(r.next() >> 8) / 16777216.0f - 0.5f) * (r.below(3) ? 8.0f : 8000.0f);
    }
}
V2 r2(game::Rng& r) { return {rf(r), rf(r)}; }
V3 r3(game::Rng& r) { return {rf(r), rf(r), rf(r)}; }
bool same3(const V3& a, const V3& b) {
    return game::same_float(a.x, b.x) && game::same_float(a.y, b.y) && game::same_float(a.z, b.z);
}

}  // namespace

NFSU2_DIFF_TEST(bBoundingBox) {
    return game::run_diff(ctx, "bBoundingBox", o_bBoxInside3, [](game::Rng& r) -> game::Case {
        const uint32_t which = r.below(9);
        switch (which) {
            case 0: case 1: {
                V2 a = r2(r), b = r2(r), c = r2(r), d = r2(r);
                if (r.below(3) == 0) c = a;  // ties
                if (r.below(3) == 0) d = b;
                const int x = which ? o_bBoxOverlap2(&a, &b, &c, &d) : o_bBoxInside2(&a, &b, &c, &d);
                const int y = which ? bBoxOverlap2(&a, &b, &c, &d) : bBoxInside2(&a, &b, &c, &d);
                return x == y ? game::Ok() : game::MismatchText(which ? "Overlapping 2D" : "IsInside 2D box");
            }
            case 2: {
                V3 ma = r3(r), xa = r3(r), mb = ma, xb = xa;
                V3* ra = o_bInitBox3(&ma, &xa);
                V3* rb = bInitBox3(&mb, &xb);
                return std::memcmp(&ma, &mb, 12) == 0 && std::memcmp(&xa, &xb, 12) == 0 && ra == &xa && rb == &xb
                           ? game::Ok() : game::MismatchText("bInitializeBoundingBox empty");
            }
            case 3: {
                V3 ma = r3(r), xa = r3(r), p = r3(r);
                V3 mb = ma, xb = xa;
                const bool alias = r.below(4) == 0;
                if (alias) {  // p is min itself
                    o_bInitBoxPoint3(&ma, &xa, &ma);
                    bInitBoxPoint3(&mb, &xb, &mb);
                } else {
                    o_bInitBoxPoint3(&ma, &xa, &p);
                    bInitBoxPoint3(&mb, &xb, &p);
                }
                return same3(ma, mb) && same3(xa, xb) ? game::Ok() : game::MismatchText("bInitializeBoundingBox point");
            }
            case 4: case 5: case 6: {
                V3 ma = r3(r), xa = r3(r), p = r3(r), q = r3(r);
                if (r.below(2)) o_bInitBox3(&ma, &xa);  // the usual start: empty box
                V3 mb = ma, xb = xa;
                const float rr = rf(r);
                if (which == 4) o_bExpandBox3(&ma, &xa, &p), bExpandBox3(&mb, &xb, &p);
                else if (which == 5) o_bExpandBoxR3(&ma, &xa, &p, rr), bExpandBoxR3(&mb, &xb, &p, rr);
                else o_bExpandBoxPQ3(&ma, &xa, &p, &q), bExpandBoxPQ3(&mb, &xb, &p, &q);
                return same3(ma, mb) && same3(xa, xb) ? game::Ok() : game::MismatchText("bExpandBoundingBox 3D " + std::to_string(which));
            }
            case 7: {
                V3 lo = r3(r), hi = r3(r), p = r3(r);
                if (r.below(3) == 0) p = r.below(2) ? lo : hi;
                const float m = r.below(3) ? 0.0f : rf(r);
                return o_bBoxInside3(&lo, &hi, &p, m) == bBoxInside3(&lo, &hi, &p, m) ? game::Ok() : game::MismatchText("IsInside 3D");
            }
            default: {
                V3 a = r3(r), b = r3(r), c = r3(r), d = r3(r);
                if (r.below(3) == 0) c = b;
                if (r.below(3) == 0) d = a;
                return o_bBoxOverlap3(&a, &b, &c, &d) == bBoxOverlap3(&a, &b, &c, &d) ? game::Ok() : game::MismatchText("Overlapping 3D");
            }
        }
    });
}

NFSU2_DIFF_TEST(regs_bBoundingBox) {
    using game::arg;
    return game::run_diff(ctx, "regs_bBoundingBox", o_bBoxOverlap3, [](game::Rng& r) {
        V2 a2 = r2(r), b2 = r2(r), c2 = r2(r), d2 = r2(r);
        V3 a = r3(r), b = r3(r), c = r3(r), d = r3(r);
        V3 a1 = a, b1 = b, a2v = a, b2v = b;
        const float m = rf(r);
        // predicates: EAX read by callers; init: EAX = max (compared); expand: no caller reads EAX
        for (auto cs : {game::check_regs("Inside2", (void*)o_bBoxInside2, NFSU2_ENTRY(bBoxInside2), {arg(&a2), arg(&b2), arg(&c2), arg(&d2)}, false),
                        game::check_regs("Overlap2", (void*)o_bBoxOverlap2, NFSU2_ENTRY(bBoxOverlap2), {arg(&a2), arg(&b2), arg(&c2), arg(&d2)}, false),
                        game::check_regs("Inside3", (void*)o_bBoxInside3, NFSU2_ENTRY(bBoxInside3), {arg(&a), arg(&b), arg(&c), game::bits(m)}, false),
                        game::check_regs("Overlap3", (void*)o_bBoxOverlap3, NFSU2_ENTRY(bBoxOverlap3), {arg(&a), arg(&b), arg(&c), arg(&d)}, false),
                        game::check_regs("Init3", (void*)o_bInitBox3, NFSU2_ENTRY(bInitBox3), {arg(&a1), arg(&b1)}, false),
                        game::check_regs("InitP3", (void*)o_bInitBoxPoint3, NFSU2_ENTRY(bInitBoxPoint3), {arg(&a2v), arg(&b2v), arg(&c)}, false),
                        game::check_regs("Expand3", (void*)o_bExpandBox3, NFSU2_ENTRY(bExpandBox3), {arg(&a1), arg(&b1), arg(&c)}, false, false),
                        game::check_regs("ExpandR3", (void*)o_bExpandBoxR3, NFSU2_ENTRY(bExpandBoxR3), {arg(&a1), arg(&b1), arg(&c), game::bits(m)}, false, false),
                        game::check_regs("ExpandPQ3", (void*)o_bExpandBoxPQ3, NFSU2_ENTRY(bExpandBoxPQ3), {arg(&a1), arg(&b1), arg(&c), arg(&d)}, false, false)})
            if (!cs.ok) return cs;
        return game::Ok();
    });
}

// deliberately broken copy (harness self-check): IsInside 3D with the NaN rule flipped
NFSU2_DIFF_TEST(bBoundingBox_BROKEN) {
    return game::run_diff(ctx, "bBoundingBox_BROKEN", o_bBoxInside3, [](game::Rng& r) -> game::Case {
        V3 lo = r3(r), hi = r3(r), p = r3(r);
        const float m = 0.0f;
        const int bad = !(lo.x - m <= p.x) || !(m + hi.x >= p.x) || !(lo.y - m <= p.y) || !(m + hi.y >= p.y) ||
                                !(lo.z - m <= p.z) || !(m + hi.z >= p.z) ? 0 : 1;
        return o_bBoxInside3(&lo, &hi, &p, m) == bad ? game::Ok() : game::MismatchText("IsInside 3D (NaN rule flipped)");
    });
}
