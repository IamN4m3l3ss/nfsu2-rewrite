// Camera deps batch 3 (7), written from the PC machine code: eRotateX / eRotateY / eTranslate (a temporary matrix
// times through the D3DX multiply thunk 0x63CEDB), Car::MakeFlatMatrix, the two tCubic3D initialisers (0x4419A0,
// 0x441A20) and the 0x444C60 copy (header + 32 points).
#include "leaf.h"
#include "x87.h"

namespace {
using namespace x87;

inline uint32_t fbits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
inline uint32_t u_(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
using Trig = float(__cdecl*)(uint32_t);
using Mul = uint32_t(__stdcall*)(uint32_t, uint32_t, uint32_t);
constexpr uint32_t kOne = 0x3F800000u;

// 0x005BACF0 eRotateX(out, m, angle): out = m * rotX. EAX = the multiply's.
uint32_t __cdecl RotateX(uint32_t out, uint32_t m, uint32_t angle) {
    const float c = game_fn<Trig>(0x0043C370)(angle);
    const X s = ld(game_fn<Trig>(0x0043C2B0)(angle));
    alignas(16) uint32_t r[16] = {};
    r[0] = kOne, r[15] = kOne;
    r[5] = fbits(c), r[10] = fbits(c);
    r[6] = fbits(st(s));
    r[9] = fbits(st(negx(s)));
    return game_fn<Mul>(0x0063CEDB)(out, m, u_(r));
}
// 0x005BADA0 eRotateY(out, m, angle): out = m * rotY. EAX = the multiply's.
uint32_t __cdecl RotateY(uint32_t out, uint32_t m, uint32_t angle) {
    const float c = game_fn<Trig>(0x0043C370)(angle);
    const X s = ld(game_fn<Trig>(0x0043C2B0)(angle));
    alignas(16) uint32_t r[16] = {};
    r[5] = kOne, r[15] = kOne;
    r[2] = fbits(st(negx(s)));
    r[8] = fbits(st(s));
    r[0] = fbits(c), r[10] = fbits(c);
    return game_fn<Mul>(0x0063CEDB)(out, m, u_(r));
}
// 0x005BAE90 eTranslate(out, m, v): out = m * translate(v). EAX = the multiply's.
uint32_t __cdecl Translate(uint32_t out, uint32_t m, const uint32_t* v) {
    alignas(16) uint32_t r[16] = {};
    r[0] = r[5] = r[10] = r[15] = kOne;
    r[12] = v[0], r[13] = v[1], r[14] = v[2];
    return game_fn<Mul>(0x0063CEDB)(out, m, u_(r));
}

// 0x005ECC00 Car::MakeFlatMatrix(out): identity; forward x, y = -(+0xB0, +0xB4) when the up z (+0x98) is within
// 0x78435C of 1, else (+0x90, +0x94); normalised; side = up x forward; position +0x60. EAX = bCross's.
uint32_t __cdecl Car_MakeFlatMatrix(uint8_t* c, uint32_t out) {
    game_fn<uint32_t(__cdecl*)(uint32_t)>(0x0043C780)(out);
    uint32_t* o = reinterpret_cast<uint32_t*>(out);
    o[10] = kOne;
    const X d = absx(subrm(ld(at<float>(c, 0x98)), gu(0x00784250u)));
    const uint8_t ah = cmp(d, ld(*reinterpret_cast<const float*>(0x0078435Cu)));
    if (!parity_even(ah & 0x41)) {  // |1 - up.z| <= limit
        o[0] = fbits(st(negx(ld(at<float>(c, 0xB0)))));
        o[1] = fbits(st(negx(ld(at<float>(c, 0xB4)))));
    } else {
        o[0] = at<uint32_t>(c, 0x90);
        o[1] = at<uint32_t>(c, 0x94);
    }
    game_fn<uint32_t(__cdecl*)(uint32_t, uint32_t)>(0x0043CE50)(out, out);
    const uint32_t r = game_fn<uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x0043D1E0)(out + 0x10, out + 0x20, out);
    const X y = ld(at<float>(c, 0x64)), z = ld(at<float>(c, 0x68));
    const uint32_t x = at<uint32_t>(c, 0x60);
    *reinterpret_cast<float*>(&o[13]) = st(y);
    o[12] = x;
    *reinterpret_cast<float*>(&o[14]) = st(z);
    o[15] = kOne;
    return r;
}

// 0x004419A0 tCubic3D init(flags, a, b): each axis (+0 / +0x30 / +0x60) zeroed, state 0, flag word = flags,
// +0x24 / +0x28 = a / b. EAX = this.
uint32_t __cdecl Cubic3D_Init(uint8_t* s, uint32_t flags, uint32_t a, uint32_t b) {
    const uint16_t dx = static_cast<uint16_t>(flags);
    for (uint32_t k = 0; k < 3; ++k) {
        uint8_t* p = s + k * 0x30;
        at<uint32_t>(p, 0x24) = a;
        at<uint32_t>(p, 0x28) = b;
        for (uint32_t off = 0; off < 0x10; off += 4) at<uint32_t>(p, off) = 0;
        at<uint16_t>(p, 0x2C) = 0;
        at<uint16_t>(p, 0x2E) = dx;
    }
    return u_(s);
}
// 0x00441A20 tCubic3D init(flags, durations*, b): as 0x4419A0 with per-axis +0x24 from the vector (through the
// x87), in the original's store order. EAX = this.
uint32_t __cdecl Cubic3D_InitVec(uint8_t* s, uint32_t flags, const float* v, uint32_t b) {
    const uint16_t dx = static_cast<uint16_t>(flags);
    at<float>(s, 0x24) = st(ld(v[0]));
    for (uint32_t off = 0; off < 0x10; off += 4) at<uint32_t>(s, off) = 0;
    at<uint16_t>(s, 0x2C) = 0;
    at<uint32_t>(s, 0x28) = b;
    at<uint16_t>(s, 0x2E) = dx;
    const X v1 = ld(v[1]);
    at<uint16_t>(s, 0x5E) = dx;
    at<float>(s, 0x54) = st(v1);
    for (uint32_t off = 0x30; off < 0x40; off += 4) at<uint32_t>(s, off) = 0;
    at<uint16_t>(s, 0x5C) = 0;
    at<uint32_t>(s, 0x58) = b;
    at<float>(s, 0x84) = st(ld(v[2]));
    at<uint16_t>(s, 0x8E) = dx;
    for (uint32_t off = 0x60; off < 0x70; off += 4) at<uint32_t>(s, off) = 0;
    at<uint32_t>(s, 0x88) = b;
    at<uint16_t>(s, 0x8C) = 0;
    return u_(s);
}

// 0x00444C60 copy: 0x30 header bytes, then 32 points (x, y, z of each 16-byte entry at +0x30, through the x87; the
// 4th dword is left alone). EAX = this.
uint32_t __cdecl PointsCopy(uint8_t* d, const uint8_t* src) {
    for (uint32_t off = 0; off < 0x30; off += 4) at<uint32_t>(d, off) = at<uint32_t>(src, off);
    for (uint32_t i = 0; i < 32; ++i) {
        const uint32_t o = 0x30 + i * 0x10;
        const X y = ld(at<float>(src, o + 4)), z = ld(at<float>(src, o + 8)), x = ld(at<float>(src, o));
        at<float>(d, o) = st(x);
        at<float>(d, o + 4) = st(y);
        at<float>(d, o + 8) = st(z);
    }
    return u_(d);
}

}  // namespace

LEAF_CDECL(0x005BACF0, RotateX, 3, "bMatrix4* eRotateX(bMatrix4* out, const bMatrix4*, u16 angle)");
LEAF_CDECL(0x005BADA0, RotateY, 3, "bMatrix4* eRotateY(bMatrix4* out, const bMatrix4*, u16 angle)");
LEAF_CDECL(0x005BAE90, Translate, 3, "bMatrix4* eTranslate(bMatrix4* out, const bMatrix4*, const bVector3*)");
LEAF_THIS(0x005ECC00, Car_MakeFlatMatrix, 1, "void Car::MakeFlatMatrix(bMatrix4* out)");
LEAF_THIS(0x004419A0, Cubic3D_Init, 3, "tCubic3D::tCubic3D(short flags, float duration, float)");
LEAF_THIS(0x00441A20, Cubic3D_InitVec, 3, "tCubic3D::tCubic3D(short flags, const bVector3* durations, float)");
LEAF_THIS(0x00444C60, PointsCopy, 1, "copy: header + 32 points");

NFSU2_DIFF_TEST(camera_deps3) {  // plain blocks
    static const Spec spec{4, 0x300, {}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(RotateX, CDECL, PTR, {'p', 1}, {'p', 2}, {'r', 0}),
        LEAF_FN(RotateY, CDECL, PTR, {'p', 1}, {'p', 2}, {'r', 0}),
        LEAF_FN(RotateX, CDECL, PTR, {'p', 1}, {'p', 1}, {'r', 0}),
        LEAF_FN(Translate, CDECL, PTR, {'p', 1}, {'p', 2}, {'p', 3}),
        LEAF_FN(Car_MakeFlatMatrix, THIS, INT, {'p', 1}),
        LEAF_FN(Cubic3D_Init, THIS, PTR, {'r', 0}, {'r', 0}, {'r', 0}),
        LEAF_FN(Cubic3D_InitVec, THIS, PTR, {'r', 0}, {'p', 1}, {'r', 0}),
        LEAF_FN(PointsCopy, THIS, PTR, {'p', 1}),
    };
    return run(ctx, "camera_deps3", spec, fns);
}

NFSU2_DIFF_TEST(camera_deps3_flat) {  // MakeFlatMatrix with the up z near 1 half the time (block 0 = the car)
    static const Spec spec = [] {
        Spec sp{2, 0x100, {}, {}};
        sp.post = [](Graph& g) {
            uint8_t* c = g.blk[0].data();
            if (c[0] & 1) at<float>(c, 0x98) = (c[1] & 1) ? 1.0f : 0.9999f;
        };
        return sp;
    }();
    static const std::vector<Fn> fns{LEAF_FN(Car_MakeFlatMatrix, THIS, INT, {'p', 1})};
    return run(ctx, "camera_deps3_flat", spec, fns);
}

NFSU2_DIFF_TEST(camera_deps3_BROKEN) {  // harness self-check: rotate X compared with rotate Y
    static const Spec spec{4, 0x100, {}, {}};
    static const std::vector<Fn> fns{Fn("RotX_vs_RotY", &o_RotateX, NFSU2_ENTRY(RotateY), CDECL, PTR, {'p', 1}, {'p', 2}, {'r', 0})};
    return run(ctx, "camera_deps3_BROKEN", spec, fns);
}
