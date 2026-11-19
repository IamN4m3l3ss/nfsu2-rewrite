// Camera batch 9 (12), written from the PC machine code: the look-at matrix (0x5BA9A0) and its users
// (IsoProjectionMatrix, TrackRouteEditorCameraMover::Update), ComputeBankedUpVector, the select-car camera's
// orientation wrappers, CubicCameraMover look-back / min distance to wall and their two helpers.
#include "leaf.h"
#include "x87.h"

namespace {
using namespace x87;

inline uint32_t fbits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
inline X gx(uint32_t addr) { return ld(*reinterpret_cast<const float*>(addr)); }
inline uint32_t u_(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
inline uint32_t zero() { return gu(0x007D7D3Cu); }  // the 0.0 constant the original multiplies by

using Cdecl2 = uint32_t(__cdecl*)(uint32_t, uint32_t);
using Cdecl3 = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t);
using Cdecl4 = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t);

// 0x005BA9A0 eCreateLookAtMatrix(out, eye, target, up): forward d = |target - eye|, right r = |(-up) x d|,
// v = |d x r|; out = rotation (columns r, v, d) with translation -eye pushed through it the way the original does
// (products with the 0.0 constant included, they decide NaN / inf results). EAX = CopyMatrix's.
uint32_t __cdecl LookAtMatrix(uint32_t out, const float* eye, const float* target, const float* up) {
    alignas(16) float d[3], u[3], r[3];
    for (int i = 0; i < 3; ++i) d[i] = st(subm(ld(target[i]), fbits(eye[i])));
    game_fn<Cdecl2>(0x0043CF10)(u_(d), u_(d));
    for (int i = 0; i < 3; ++i) u[i] = st(negx(ld(up[i])));
    game_fn<Cdecl3>(0x0043D1E0)(u_(r), u_(u), u_(d));
    float* v = u;  // the cross product d x r goes where -up was
    game_fn<Cdecl3>(0x0043D1E0)(u_(v), u_(d), u_(r));
    game_fn<Cdecl2>(0x0043CF10)(u_(r), u_(r));
    game_fn<Cdecl2>(0x0043CF10)(u_(v), u_(v));
    float* o = reinterpret_cast<float*>(out);
    o[0] = st(ld(r[0]));
    *reinterpret_cast<uint32_t*>(o + 2) = fbits(d[0]);
    *reinterpret_cast<uint32_t*>(o + 4) = fbits(r[1]);
    *reinterpret_cast<uint32_t*>(o + 5) = fbits(v[1]);
    *reinterpret_cast<uint32_t*>(o + 6) = fbits(d[1]);
    *reinterpret_cast<uint32_t*>(o + 8) = fbits(r[2]);
    *reinterpret_cast<uint32_t*>(o + 1) = fbits(v[0]);
    *reinterpret_cast<uint32_t*>(o + 9) = fbits(v[2]);
    *reinterpret_cast<uint32_t*>(o + 10) = fbits(d[2]);
    *reinterpret_cast<uint32_t*>(o + 3) = 0;
    *reinterpret_cast<uint32_t*>(o + 7) = 0;
    *reinterpret_cast<uint32_t*>(o + 11) = 0;
    *reinterpret_cast<uint32_t*>(o + 12) = 0;
    *reinterpret_cast<uint32_t*>(o + 13) = 0;
    *reinterpret_cast<uint32_t*>(o + 14) = 0;
    *reinterpret_cast<uint32_t*>(o + 15) = 0x3F800000u;
    const X ex = negx(ld(eye[0])), ey = negx(ld(eye[1])), ez = negx(ld(eye[2]));
    alignas(16) float m[16];
    m[3] = 0.0f, m[7] = 0.0f, m[11] = 0.0f;
    const X rz0 = mulm(ld(r[2]), zero()), ry0 = mulm(ld(r[1]), zero());
    m[0] = st(add(add(ld(r[0]), ry0), rz0));
    const X vz0 = mulm(ld(v[2]), zero());
    const float t1 = st(mulm(ld(v[1]), zero()));
    m[1] = st(add(addm(ld(v[0]), fbits(t1)), vz0));
    const X dz0 = mulm(ld(d[2]), zero());
    const float t2 = st(mulm(ld(d[1]), zero()));
    m[2] = st(add(addm(ld(d[0]), fbits(t2)), dz0));
    const float t3 = st(mulm(ld(r[0]), zero()));
    m[4] = st(add(addm(ld(r[1]), fbits(t3)), rz0));
    const float t4 = st(mulm(ld(v[0]), zero()));
    m[5] = st(add(addm(ld(v[1]), fbits(t4)), vz0));
    const float t5 = st(mulm(ld(d[0]), zero()));
    m[6] = st(add(addm(ld(d[1]), fbits(t5)), dz0));
    m[8] = st(add(addm(ld(r[2]), fbits(t3)), ry0));
    m[9] = st(addm(addm(ld(v[2]), fbits(t4)), fbits(t1)));
    m[10] = st(addm(addm(ld(d[2]), fbits(t5)), fbits(t2)));
    auto row = [&](const float* a) {
        return add(add(mul(ld(a[2]), ez), mulm(ey, fbits(a[1]))), mulm(ex, fbits(a[0])));
    };
    m[12] = st(row(r));
    m[13] = st(row(v));
    m[14] = st(row(d));
    m[15] = st(addm(add(add(mulm(ez, zero()), mulm(ey, zero())), mulm(ex, zero())), gu(0x00784250u)));
    return game_fn<Cdecl2>(0x004012B0)(out, u_(m));
}

// 0x004440D0 CameraMover::ComputeBankedUpVector(out, from, to, angle): rotate about |to - from| by angle; out =
// rows (0, 0, 1, 1)-style sums of the rotation matrix as the original adds them. EAX = out.
uint32_t __cdecl ComputeBankedUpVector(uint32_t out, const float* from, const float* to, uint32_t angle) {
    alignas(16) float d[3];
    alignas(16) uint32_t m[16];
    for (int i = 0; i < 3; ++i) d[i] = st(subm(ld(to[i]), fbits(from[i])));
    game_fn<Cdecl2>(0x0043CF10)(u_(d), u_(d));
    game_fn<Cdecl3>(0x005BA8B0)(u_(m), u_(d), angle);
    X c[3];
    for (int i = 0; i < 3; ++i)
        c[i] = addm(addm(mulm(addm(ld(*reinterpret_cast<float*>(&m[4 + i])), m[i]), zero()), m[12 + i]), m[8 + i]);
    float* o = reinterpret_cast<float*>(out);
    for (int i = 0; i < 3; ++i) o[i] = st(c[i]);
    return out;
}

// 0x0044DCE0 TrackRouteEditorCameraMover::Update: if the editor gives a position and direction (0x50D510, a stub
// returning 0 on PC), look from the position along the direction (up +Z) and set the camera with the frame time.
uint32_t __cdecl RouteEditor_Update(uint8_t* s) {
    alignas(16) float p[3], q[4];
    const uint32_t got = game_fn<Cdecl2>(0x0050D510)(u_(p), u_(q));
    if (!got) return got;
    alignas(16) float t[3], e[3], up[3] = {0.0f, 0.0f, 1.0f};
    alignas(16) float m[16];
    for (int i = 0; i < 3; ++i) {
        t[i] = st(addm(ld(q[i]), fbits(p[i])));
        *reinterpret_cast<uint32_t*>(&e[i]) = fbits(p[i]);
    }
    game_fn<Cdecl4>(0x005BA9A0)(u_(m), u_(e), u_(t), u_(up));
    return game_fn<uint32_t(__thiscall*)(void*, uint32_t, uint32_t)>(0x00447280)(ptr(s, 0x1C), u_(m), gu(0x00865198u));
}

// 0x00444390 CameraMover::IsoProjectionMatrix(out, eye, target, shift): look at target with up = -(out's column
// 1), move the target in that view by shift (x, y scaled by its depth), back to world space, look again. EAX = the
// second look-at's.
uint32_t __cdecl IsoProjectionMatrix(uint32_t out, uint32_t eye, const float* tgt, const float* shift) {
    const float* a1 = reinterpret_cast<const float*>(out);
    alignas(16) float up[3] = {st(negx(ld(a1[1]))), st(negx(ld(a1[5]))), st(negx(ld(a1[9])))};
    alignas(16) float l[16], inv[16];
    game_fn<Cdecl4>(0x005BA9A0)(u_(l), eye, u_(tgt), u_(up));
    game_fn<Cdecl2>(0x005BAF90)(u_(inv), u_(l));
    const uint32_t tx = fbits(tgt[0]), ty = fbits(tgt[1]), tz = fbits(tgt[2]);
    const X px = addm(add(add(mulm(ld(l[8]), tz), mulm(ld(l[4]), ty)), mulm(ld(l[0]), tx)), fbits(l[12]));
    const X py = addm(add(add(mulm(ld(l[5]), ty), mulm(ld(l[9]), tz)), mulm(ld(l[1]), tx)), fbits(l[13]));
    const X pz = addm(add(add(mulm(ld(l[6]), ty), mulm(ld(l[10]), tz)), mulm(ld(l[2]), tx)), fbits(l[14]));
    const float PZ = st(pz);
    float PX = st(px);
    PX = st(subrm(mulm(ld(PZ), fbits(shift[0])), fbits(PX)));
    const X py2 = sub(py, mulm(ld(PZ), fbits(shift[1])));
    const X nx = addm(add(add(mul(ld(inv[4]), py2), mulm(ld(inv[8]), fbits(PZ))), mulm(ld(inv[0]), fbits(PX))), fbits(inv[12]));
    const X ny = addm(add(add(mul(ld(inv[5]), py2), mulm(ld(inv[1]), fbits(PX))), mulm(ld(inv[9]), fbits(PZ))), fbits(inv[13]));
    const X nz = addm(add(add(mul(ld(inv[6]), py2), mulm(ld(inv[2]), fbits(PX))), mulm(ld(inv[10]), fbits(PZ))), fbits(inv[14]));
    alignas(16) float n[3];
    n[2] = st(nz);
    n[0] = st(nx);
    n[1] = st(ny);
    return game_fn<Cdecl4>(0x005BA9A0)(out, eye, u_(n), u_(up));
}

// ---------------------------------------------------------------- SelectCarCameraMover orientation wrappers
using SetDesired = uint32_t(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                         uint32_t, uint32_t, uint32_t, uint32_t);
inline uint32_t set_desired(uint8_t* s, const uint32_t* v, uint32_t a, uint32_t b) {
    return game_fn<SetDesired>(0x00451200)(s, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], a, b);
}
inline uint32_t to_default(uint8_t* s) {  // the default orientation (+0x40, +0x50.., +0x60.., +0x70..)
    const uint32_t v[9] = {at<uint32_t>(s, 0x40), at<uint32_t>(s, 0x50), at<uint32_t>(s, 0x54),
                           at<uint32_t>(s, 0x58), at<uint32_t>(s, 0x60), at<uint32_t>(s, 0x64),
                           at<uint32_t>(s, 0x68), at<uint32_t>(s, 0x70), at<uint32_t>(s, 0x74)};
    return set_desired(s, v, 0, 0);
}
// 0x00453380 SelectCarCameraMover::MoveToDefault
uint32_t __cdecl SelectCar_MoveToDefault(uint8_t* s) { return to_default(s); }
// 0x004533B0 SelectCarCameraMover::MoveToNewDefaultOrientation(9 values): +0x154 = 1, then move there
uint32_t __cdecl SelectCar_MoveToNewDefault(uint8_t* s, uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4,
                                            uint32_t v5, uint32_t v6, uint32_t v7, uint32_t v8, uint32_t v9) {
    const uint32_t v[9] = {v1, v2, v3, v4, v5, v6, v7, v8, v9};
    s[0x154] = 1;
    return set_desired(s, v, 0, 0);
}
// 0x00453320 SelectCarCameraMover::StopLookingAtParts(bool back): if looking at parts (+0x30): zoom +0x150 back to
// 0x787114, optionally move to the default, +0x30 = 0. EAX = 0, the zoom with AL = back, or the move's.
uint32_t __cdecl SelectCar_StopLookingAtParts(uint8_t* s, uint32_t back) {
    if (!at<uint32_t>(s, 0x30)) return 0;
    uint32_t eax = gu(0x00787114u);
    at<uint32_t>(s, 0x150) = eax;
    eax = (eax & 0xFFFFFF00u) | (back & 0xFF);
    if (back & 0xFF) eax = to_default(s);
    at<uint32_t>(s, 0x30) = 0;
    return eax;
}
// 0x004532B0 SelectCarCameraMover::SetDesiredPartLookAtOrientation(9 values, a, b, bool near): zoom +0x150 from
// 0x787110 (near) or 0x787118, +0x30 = 1, then SetDesiredOrientation(9 values, a, b)
uint32_t __cdecl SelectCar_SetPartLookAt(uint8_t* s, uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4, uint32_t v5,
                                         uint32_t v6, uint32_t v7, uint32_t v8, uint32_t v9, uint32_t a, uint32_t b,
                                         uint32_t near) {
    at<uint32_t>(s, 0x150) = (near & 0xFF) ? gu(0x00787110u) : gu(0x00787118u);
    const uint32_t v[9] = {v1, v2, v3, v4, v5, v6, v7, v8, v9};
    at<uint32_t>(s, 0x30) = 1;
    return set_desired(s, v, a, b);
}

// ---------------------------------------------------------------- look-back
// 0x005D7AE0 FlushAccumulationBuffer: sets the flag 0x883E34. EAX untouched.
uint32_t __cdecl FlushAccumulationBuffer(uint32_t eax_in) {
    *reinterpret_cast<volatile uint32_t*>(0x00883E34u) = 1;
    return eax_in;
}
// 0x005EB520 Player::UseBirdsEye: +0x2B == 0
uint32_t __cdecl Player_UseBirdsEye(uint8_t* s) { return s[0x2B] == 0 ? 1u : 0u; }
// 0x00445A30 CubicCameraMover::SetLookBack(bool): a change flushes the motion-blur accumulation; +0x51 = it.
// EAX: AL = old ^ new over the caller's EAX.
uint32_t __cdecl Cubic_SetLookBack(uint8_t* s, uint32_t b, uint32_t eax_in) {
    const uint8_t x = static_cast<uint8_t>(s[0x51] ^ static_cast<uint8_t>(b));
    if (x) game_fn<void(__cdecl*)()>(0x005D7AE0)();
    s[0x51] = static_cast<uint8_t>(b);
    return (eax_in & 0xFFFFFF00u) | x;
}
// 0x00445A60 CubicCameraMover::MinDistToWall: looking back with a player [+0x20] in bird's-eye -> 0x787094, else
// 0x787090
float __cdecl Cubic_MinDistToWall(uint8_t* s) {
    if (s[0x51] && at<uint32_t>(s, 0x20) && (game_fn<uint32_t(__thiscall*)(void*)>(0x005EB520)(ptr(s, 0x20)) & 0xFF))
        return *reinterpret_cast<const float*>(0x00787094u);
    return *reinterpret_cast<const float*>(0x00787090u);
}

}  // namespace

LEAF_CDECL(0x005BA9A0, LookAtMatrix, 4, "bMatrix4* eCreateLookAtMatrix(bMatrix4* out, const bVector3* eye, const bVector3* target, const bVector3* up)");
LEAF_CDECL(0x004440D0, ComputeBankedUpVector, 4, "bVector3* CameraMover::ComputeBankedUpVector(bVector3* out, const bVector3*, const bVector3*, u16 angle)");
LEAF_THIS(0x0044DCE0, RouteEditor_Update, 0, "void TrackRouteEditorCameraMover::Update()");
LEAF_STD(0x00444390, IsoProjectionMatrix, 4, "void CameraMover::IsoProjectionMatrix(bMatrix4* out, const bVector3* eye, const bVector3* target, const bVector2* shift)");
LEAF_THIS(0x00453380, SelectCar_MoveToDefault, 0, "void SelectCarCameraMover::MoveToDefault()");
LEAF_THIS(0x004533B0, SelectCar_MoveToNewDefault, 9, "void SelectCarCameraMover::MoveToNewDefaultOrientation(9 values)");
LEAF_THIS(0x00453320, SelectCar_StopLookingAtParts, 1, "void SelectCarCameraMover::StopLookingAtParts(bool)");
LEAF_THIS(0x004532B0, SelectCar_SetPartLookAt, 12, "void SelectCarCameraMover::SetDesiredPartLookAtOrientation(9 values, float, float, bool)");
LEAF_CDECL_EAX(0x005D7AE0, FlushAccumulationBuffer, 0, "void FlushAccumulationBuffer()");
LEAF_THIS(0x005EB520, Player_UseBirdsEye, 0, "bool Player::UseBirdsEye()");
LEAF_THIS_EAX(0x00445A30, Cubic_SetLookBack, 1, "void CubicCameraMover::SetLookBack(bool)");
LEAF_THIS(0x00445A60, Cubic_MinDistToWall, 0, "float CubicCameraMover::MinDistToWall()");

NFSU2_DIFF_TEST(camera_lookat_math) {  // plain matrices / vectors (blocks 1..4)
    static const Spec spec{5, 0x100, {}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(LookAtMatrix, CDECL, PTR, {'p', 1}, {'p', 2}, {'p', 3}, {'p', 4}),
        LEAF_FN(LookAtMatrix, CDECL, PTR, {'p', 1}, {'p', 1}, {'p', 3}, {'p', 1}),  // out aliasing eye / up
        LEAF_FN(ComputeBankedUpVector, CDECL, PTR, {'p', 1}, {'p', 2}, {'p', 3}, {'r', 0}),
        LEAF_FN(IsoProjectionMatrix, STD, INT, {'p', 1}, {'p', 2}, {'p', 3}, {'p', 4}),
    };
    return run(ctx, "camera_lookat_math", spec, fns);
}

NFSU2_DIFF_TEST(camera_lookat_movers) {
    // 0 a mover (camera [+0x1C] -> 1, player [+0x20] -> 2 or 0; look-back byte +0x51; select-car fields)
    static const Spec spec{3, 0x200, {{0, 0x1C, 1}, {0, 0x20, 2, true}}, {{0, 0x51, 1, 2}, {0, 0x30, 4, 2}, {2, 0x2B, 1, 2}}};
    static const std::vector<Fn> fns{
        LEAF_FN(RouteEditor_Update, THIS, INT),
        LEAF_FN(SelectCar_MoveToDefault, THIS, INT),
        LEAF_FN(SelectCar_MoveToNewDefault, THIS, INT, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}),
        LEAF_FN(SelectCar_StopLookingAtParts, THIS, INT, {'r', 0}),
        LEAF_FN(SelectCar_StopLookingAtParts, THIS, INT, {'i', 2}),
        LEAF_FN(SelectCar_SetPartLookAt, THIS, INT, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0},
                {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'r', 0}),
        LEAF_FN(Player_UseBirdsEye, THIS, INT),
        LEAF_FN(Cubic_SetLookBack, THIS, INT, {'r', 0}),
        LEAF_FN(Cubic_SetLookBack, THIS, INT, {'i', 2}),
        LEAF_FN(Cubic_MinDistToWall, THIS, FLT),
        LEAF_FN(FlushAccumulationBuffer, CDECL, INT),
    };
    volatile uint32_t& flush = *reinterpret_cast<volatile uint32_t*>(0x00883E34u);
    const uint32_t saved = flush;
    game::DiffResult r = run(ctx, "camera_lookat_movers", spec, fns);
    flush = saved;  // both sides set it (a motion-blur reset): put it back
    return r;
}

NFSU2_DIFF_TEST(camera_lookat_BROKEN) {  // harness self-check: MoveToDefault compared with StopLookingAtParts
    static const Spec spec{3, 0x200, {{0, 0x1C, 1}}, {{0, 0x30, 4, 2}}};
    static const std::vector<Fn> fns{Fn("Default_vs_Stop", &o_SelectCar_MoveToDefault, NFSU2_ENTRY(SelectCar_StopLookingAtParts), THIS, INT)};
    return run(ctx, "camera_lookat_BROKEN", spec, fns);
}
