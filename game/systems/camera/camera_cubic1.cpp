// Camera batch 7 (10), written from the PC machine code: the remaining tCubic3D setters, the 4D vector transform,
// eInvertTransformationMatrix, and CameraMover::EyeCubicInit / LookCubicInit (start a camera cubic at the camera's
// eye / look point, optionally moved into a matrix's space). Plus the two one-jump thunks they call through.
#include "leaf.h"
#include "live.h"
#include "x87.h"

namespace {
using namespace x87;

inline uint32_t fbits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
inline float bitsf(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

// three per-axis "set field, flag 2 at +0x2C unless it equals the compared field" (axes 0x30 apart). The last
// fnstsw leaves the status word in AX over the z argument's high half.
uint32_t cubic_set3(uint8_t* s, uint32_t store, uint32_t compare, uint32_t x, uint32_t y, uint32_t z) {
    const uint32_t v[3] = {x, y, z};
    uint16_t sw = 0;
    for (uint32_t k = 0; k < 3; ++k) {
        sw = cmpw(ld(bitsf(v[k])), ld(at<float>(s, compare + k * 0x30)));
        at<uint32_t>(s, store + k * 0x30) = v[k];
        if (parity_even(static_cast<uint8_t>(sw >> 8) & 0x44)) at<uint16_t>(s, 0x2C + k * 0x30) = 2;
    }
    return (z & 0xFFFF0000u) | sw;
}

// 0x00570B40 tCubic3D::SetdVal(x, y, z): the +4 fields, compared with +0xC
uint32_t __cdecl Cubic3D_SetdVal3(uint8_t* s, uint32_t x, uint32_t y, uint32_t z) { return cubic_set3(s, 4, 0xC, x, y, z); }
// 0x00570BA0 tCubic3D::SetValDesired(x, y, z): the +8 fields, compared with +0
uint32_t __cdecl Cubic3D_SetValDesired3(uint8_t* s, uint32_t x, uint32_t y, uint32_t z) { return cubic_set3(s, 8, 0, x, y, z); }

using Set3 = uint32_t(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
// 0x00574CB0 tCubic3D::SetdVal(const bVector3*)
uint32_t __cdecl Cubic3D_SetdVal(uint8_t* s, const uint32_t* v) { return game_fn<Set3>(0x00570B40)(s, v[0], v[1], v[2]); }
// 0x00574CD0 tCubic3D::SetValDesired(const bVector3*)
uint32_t __cdecl Cubic3D_SetValDesired(uint8_t* s, const uint32_t* v) { return game_fn<Set3>(0x00570BA0)(s, v[0], v[1], v[2]); }

// 0x00486F50 out = v * M for a 4-vector (all four computed before any store; out may be v). EAX = out.
uint32_t __cdecl Transform4(uint32_t op, uint32_t mp, uint32_t vp) {
    const float* m = reinterpret_cast<const float*>(mp);
    const float* v = reinterpret_cast<const float*>(vp);
    auto mv = [&](int mi, int vi) { return mulm(ld(m[mi]), fbits(v[vi])); };  // fld m; fmul dword v
    X r[4];
    for (int c = 0; c < 4; ++c)
        r[c] = c == 0 ? add(add(add(mv(12, 3), mv(8, 2)), mv(4, 1)), mv(0, 0))
                      : add(add(add(mv(12 + c, 3), mv(c, 0)), mv(8 + c, 2)), mv(4 + c, 1));
    float* out = reinterpret_cast<float*>(op);
    for (int c = 0; c < 4; ++c) out[c] = st(r[c]);
    return op;
}

using Cdecl3 = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t);
// 0x0043C9B0 / 0x0043C9C0: one-jump thunks to 0x486F50 / 0x441000 (TransformPoint)
uint32_t __cdecl Thunk_Transform4(uint32_t a, uint32_t b, uint32_t c) { return game_fn<Cdecl3>(0x00486F50)(a, b, c); }
uint32_t __cdecl Thunk_TransformPoint(uint32_t a, uint32_t b, uint32_t c) { return game_fn<Cdecl3>(0x00441000)(a, b, c); }

// 0x005BAF90 eInvertTransformationMatrix(out, m): out = translate(-pos) * transpose(rotation), multiplied by the
// D3DX routine the exe dispatches through 0x803E74 (stdcall out, a, b). The rotation is copied with fld/fstp
// (m[0] with a mov), the position negated with fchs. Both temporaries keep the original's 16-byte phase. EAX = out.
uint32_t __cdecl InvertTransformationMatrix(uint32_t op, const float* m) {
    alignas(16) uint32_t buf[0x90 / 4] = {};
    uint32_t* r = buf + 3;   // rotation^T at phase 0xC
    uint32_t* t = buf + 19;  // translation at phase 0xC (0x4C)
    auto cp = [](float f) { return fbits(st(ld(f))); };
    r[0] = fbits(m[0]), r[1] = cp(m[4]), r[2] = cp(m[8]), r[3] = 0;
    r[4] = cp(m[1]), r[5] = cp(m[5]), r[6] = cp(m[9]), r[7] = 0;
    r[8] = cp(m[2]), r[9] = cp(m[6]), r[10] = cp(m[10]), r[11] = 0;
    r[12] = r[13] = r[14] = 0, r[15] = 0x3F800000u;
    for (int i = 0; i < 16; ++i) t[i] = (i % 5 == 0) ? 0x3F800000u : 0;
    for (int i = 0; i < 3; ++i) t[12 + i] = cp(m[12 + i]) ^ 0x80000000u;
    const uint32_t fn = gu(0x00803E74u);
    reinterpret_cast<uint32_t(__stdcall*)(uint32_t, const uint32_t*, const uint32_t*)>(fn)(op, t, r);
    return op;
}

// CameraMover::EyeCubicInit / LookCubicInit(cubic, matrix or 0, offset or 0): p = camera point + camera velocity *
// 0x865154 (0x43D0C0), moved by the matrix if given, is the cubic's value; the camera's velocity (minus the offset if
// given, x / y kept in x87 precision) times the cubic's +0x24, as (x, y, z, 0), rotated by the matrix if given, is its
// derivative. EAX = SetdVal's.
uint32_t cubic_init(uint8_t* s, uint8_t* cubic, uint32_t mat, const float* off, uint32_t pos, uint32_t vel) {
    uint8_t* cam = ptr(s, 0x1C);
    alignas(16) float p[4] = {};
    game_fn<uint32_t(__cdecl*)(float*, uint8_t*, uint8_t*, uint32_t)>(0x0043D0C0)(p, cam + pos, cam + vel, gu(0x00865154u));
    if (mat) game_fn<Cdecl3>(0x0043C9C0)(u(p), mat, u(p));
    game_fn<uint32_t(__thiscall*)(void*, const float*)>(0x00574C90)(cubic, p);
    X x = ld(at<float>(cam, vel)), y = ld(at<float>(cam, vel + 4));
    float z = st(ld(at<float>(cam, vel + 8)));  // went through fstp [esp+0x38] (a signalling NaN comes out quiet)
    if (off) {
        x = subm(x, fbits(off[0]));
        y = subm(y, fbits(off[1]));
        z = st(subrm(ld(off[2]), fbits(z)));  // fld off.z; fsubr [z]
    }
    const uint32_t k = at<uint32_t>(cubic, 0x24);  // copied with mov, used as fmul's memory operand
    alignas(16) float d[4] = {st(mulm(x, k)), st(mulm(y, k)), st(mulm(ld(z), k)), 0.0f};
    if (mat) game_fn<Cdecl3>(0x0043C9B0)(u(d), mat, u(d));
    return game_fn<uint32_t(__thiscall*)(void*, const float*)>(0x00574CB0)(cubic, d);
}
// 0x00444980 CameraMover::EyeCubicInit: the eye (camera +0x40, velocity +0x1E0)
uint32_t __cdecl Mover_EyeCubicInit(uint8_t* s, uint8_t* cubic, uint32_t mat, const float* off) {
    return cubic_init(s, cubic, mat, off, 0x40, 0x1E0);
}
// 0x00444A70 CameraMover::LookCubicInit: the look-at point (camera +0x60, velocity +0x200)
uint32_t __cdecl Mover_LookCubicInit(uint8_t* s, uint8_t* cubic, uint32_t mat, const float* off) {
    return cubic_init(s, cubic, mat, off, 0x60, 0x200);
}

// post-hooks: make the compared fields of the cubic (block 0) and the vector (block 1) 1.0, to reach "equal"
void equal_fields(Graph& g) {
    for (uint32_t k = 0; k < 3; ++k) {
        at<uint32_t>(g.blk[0].data(), k * 0x30) = 0x3F800000u;
        at<uint32_t>(g.blk[0].data(), 0xC + k * 0x30) = 0x3F800000u;
        at<uint32_t>(g.blk[1].data(), k * 4) = k == 1 ? 0x40000000u : 0x3F800000u;  // y differs
    }
}

}  // namespace

LEAF_THIS(0x00570B40, Cubic3D_SetdVal3, 3, "void tCubic3D::SetdVal(float x, float y, float z)");
LEAF_THIS(0x00570BA0, Cubic3D_SetValDesired3, 3, "void tCubic3D::SetValDesired(float x, float y, float z)");
LEAF_THIS(0x00574CB0, Cubic3D_SetdVal, 1, "void tCubic3D::SetdVal(const bVector3*)");
LEAF_THIS(0x00574CD0, Cubic3D_SetValDesired, 1, "void tCubic3D::SetValDesired(const bVector3*)");
LEAF_CDECL(0x00486F50, Transform4, 3, "bVector4* eMulVector(bVector4* out, const bMatrix4*, const bVector4*)");
LEAF_CDECL(0x0043C9B0, Thunk_Transform4, 3, "thunk -> 0x486F50");
LEAF_CDECL(0x0043C9C0, Thunk_TransformPoint, 3, "thunk -> 0x441000");
LEAF_CDECL(0x005BAF90, InvertTransformationMatrix, 2, "bMatrix4* eInvertTransformationMatrix(bMatrix4* out, const bMatrix4*)");
LEAF_THIS(0x00444980, Mover_EyeCubicInit, 3, "void CameraMover::EyeCubicInit(tCubic3D*, const bMatrix4*, const bVector3*)");
LEAF_THIS(0x00444A70, Mover_LookCubicInit, 3, "void CameraMover::LookCubicInit(tCubic3D*, const bMatrix4*, const bVector3*)");

NFSU2_DIFF_TEST(camera_cubic1_set) {  // 0 a cubic, 1 a vector
    static const Spec spec{2, 0x100, {}, {}};
    static const Spec eq = [] {
        Spec sp{2, 0x100, {}, {}};
        sp.post = &equal_fields;
        return sp;
    }();
    static const std::vector<Fn> fns{
        LEAF_FN(Cubic3D_SetdVal3, THIS, INT, {'f', 0}, {'f', 0}, {'f', 0}),
        LEAF_FN(Cubic3D_SetValDesired3, THIS, INT, {'f', 0}, {'f', 0}, {'f', 0}),
        LEAF_FN(Cubic3D_SetdVal, THIS, INT, {'p', 1}),
        LEAF_FN(Cubic3D_SetValDesired, THIS, INT, {'p', 1}),
    };
    static const std::vector<Fn> fns_eq{
        LEAF_FN(Cubic3D_SetdVal3, THIS, INT, {'k', 0x3F800000u}, {'f', 0}, {'k', 0x3F800000u}),
        LEAF_FN(Cubic3D_SetValDesired3, THIS, INT, {'k', 0x3F800000u}, {'k', 0x3F800000u}, {'f', 0}),
        LEAF_FN(Cubic3D_SetdVal, THIS, INT, {'p', 1}),
        LEAF_FN(Cubic3D_SetValDesired, THIS, INT, {'p', 1}),
    };
    game::DiffResult a = run(ctx, "camera_cubic1_set", spec, fns);
    if (a.mismatches || !a.error.empty()) return a;
    game::DiffResult b = run(ctx, "camera_cubic1_set", eq, fns_eq);
    b.iterations += a.iterations;
    return b;
}

NFSU2_DIFF_TEST(camera_cubic1_matrix) {  // plain matrices / vectors
    static const Spec spec{4, 0x200, {}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(Transform4, CDECL, PTR, {'p', 1}, {'p', 2}, {'p', 3}),
        LEAF_FN(Transform4, CDECL, PTR, {'p', 1}, {'p', 2}, {'p', 1}),
        LEAF_FN(Thunk_Transform4, CDECL, PTR, {'p', 1}, {'p', 2}, {'p', 3}),
        LEAF_FN(Thunk_TransformPoint, CDECL, PTR, {'p', 1}, {'p', 2}, {'p', 3}),
        LEAF_FN(InvertTransformationMatrix, CDECL, PTR, {'p', 1}, {'p', 2}),
        LEAF_FN(InvertTransformationMatrix, CDECL, PTR, {'p', 1}, {'p', 1}),
    };
    return run(ctx, "camera_cubic1_matrix", spec, fns);
}

NFSU2_DIFF_TEST(camera_cubic1_init) {
    // 0 a mover (camera [+0x1C] -> 1), 2 the cubic, 3 a matrix, 4 an offset; matrix / offset also passed as 0
    static const Spec spec{5, 0x300, {{0, 0x1C, 1}}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(Mover_EyeCubicInit, THIS, INT, {'p', 2}, {'p', 3}, {'p', 4}),
        LEAF_FN(Mover_EyeCubicInit, THIS, INT, {'p', 2}, {'k', 0}, {'p', 4}),
        LEAF_FN(Mover_EyeCubicInit, THIS, INT, {'p', 2}, {'p', 3}, {'k', 0}),
        LEAF_FN(Mover_EyeCubicInit, THIS, INT, {'p', 2}, {'k', 0}, {'k', 0}),
        LEAF_FN(Mover_LookCubicInit, THIS, INT, {'p', 2}, {'p', 3}, {'p', 4}),
        LEAF_FN(Mover_LookCubicInit, THIS, INT, {'p', 2}, {'k', 0}, {'p', 4}),
        LEAF_FN(Mover_LookCubicInit, THIS, INT, {'p', 2}, {'p', 3}, {'k', 0}),
        LEAF_FN(Mover_LookCubicInit, THIS, INT, {'p', 2}, {'k', 0}, {'k', 0}),
    };
    return run(ctx, "camera_cubic1_init", spec, fns);
}

NFSU2_DIFF_TEST(camera_cubic1_BROKEN) {  // harness self-check: EyeCubicInit compared with LookCubicInit
    static const Spec spec{5, 0x300, {{0, 0x1C, 1}}, {}};
    static const std::vector<Fn> fns{Fn("Eye_vs_Look", &o_Mover_EyeCubicInit, NFSU2_ENTRY(Mover_LookCubicInit), THIS, INT, {'p', 2}, {'p', 3}, {'p', 4})};
    return run(ctx, "camera_cubic1_BROKEN", spec, fns);
}

LEAF_LIVE(0x00486F50, Transform4, CDECL, PTR, 0, 0x10, 0x40, 0x10);
LEAF_LIVE(0x005BAF90, InvertTransformationMatrix, CDECL, PTR, 0, 0x40, 0x40);
LEAF_LIVE(0x00574CB0, Cubic3D_SetdVal, THIS, INT, 0x90, 12);
LEAF_LIVE(0x00574CD0, Cubic3D_SetValDesired, THIS, INT, 0x90, 12);
