// zCamera leaf batch 1 (26), written from the PC machine code: camera mover targets / car to follow, POV type,
// generic categories, select-car / cubic / showcase mover setters and resets, track-car spring force, FOV cubic
// init, drag-camera key tables (NIS groups, current track, parameters, neighbours, shared keys, key number).
// Table-driven leaf tests (game/framework/leaf.h); x87 maths through x87.h.
#include "leaf.h"
#include "x87.h"

namespace {
using namespace x87;

// ---- small queries ------------------------------------------------------------------------------------------

// 0x00441190 OutsidePovType(type): 0, 1 and 5 are outside views
uint32_t __cdecl OutsidePovType(uint32_t t) { return (t == 1 || t == 0 || t == 5) ? 1u : 0u; }
// 0x00441AE0 CameraMover::GetTarget: the view's target vector ([+0x1C] + 0x60)
uint32_t __cdecl Mover_GetTarget(uint8_t* s) { return at<uint32_t>(s, 0x1C) + 0x60; }
// 0x00444B90 OrbitCarCameraMover::GetTarget: the orbited car's position (+0x50) or the view's target
uint32_t __cdecl Orbit_GetTarget(uint8_t* s) {
    const uint32_t c = at<uint32_t>(s, 0xB0);
    return c ? c + 0x50 : at<uint32_t>(s, 0x1C) + 0x60;
}
// 0x00444CD0 TrackCarCameraMover::GetTarget: same with the tracked car at +0x438
uint32_t __cdecl TrackCar_GetTarget(uint8_t* s) {
    const uint32_t c = at<uint32_t>(s, 0x438);
    return c ? c + 0x50 : at<uint32_t>(s, 0x1C) + 0x60;
}
// 0x00444030 CameraMover::GetCarToFollow: [[+0x20] + 4], 0 without a target
uint32_t __cdecl Mover_GetCarToFollow(uint8_t* s) {
    const uint32_t t = at<uint32_t>(s, 0x20);
    return t ? at<uint32_t>(t, 4) : 0u;
}
// generic camera categories: 6 entries of 0x14 bytes at 0x7EC1F8 (id at +4, name at +8)
// 0x00441F70 GetGenericCategoryName(id): the name, or the default 0x7875A8
uint32_t __cdecl GenericCategoryName(uint32_t id) {
    for (uint32_t i = 0; i < 6; ++i)
        if (gu(0x007EC1FCu + i * 0x14) == id) return gu(0x007EC200u + i * 0x14);
    return 0x007875A8;
}
// 0x00441FA0 GetGenericCategory(id): the entry, or 0
uint32_t __cdecl GenericCategory(uint32_t id) {
    for (uint32_t i = 0; i < 6; ++i)
        if (gu(0x007EC1FCu + i * 0x14) == id) return 0x007EC1F8u + i * 0x14;
    return 0;
}

// ---- SelectCarCameraMover / CubicCameraMover / ShowcaseCameraMover ------------------------------------------

// 0x00444DC0 SelectCarCameraMover::IsCameraAtDefaultPosition: !(+0x148 < +0x14C)
uint32_t __cdecl SelectCar_IsAtDefault(uint8_t* s) {
    return (cmp(ld(at<float>(s, 0x148)), ld(at<float>(s, 0x14C))) & 0x01) ? 0u : 1u;
}
// 0x004454A0 SelectCarCameraMover::SetHRotateSpeed(v): only marks the mover active (+0x2C = 1 if set). EAX = old.
uint32_t __cdecl SelectCar_SetHRotateSpeed(uint8_t* s, uint32_t) {
    const uint32_t a = at<uint32_t>(s, 0x2C);
    if (a) at<uint32_t>(s, 0x2C) = 1;
    return a;
}
// 0x004454C0 / 0x004454E0 SetZoomSpeed / SetVRotateSpeed(v): if active, +0x2C = 1 and the speed field = v.
// EAX = v, or 0 when inactive.
uint32_t select_set(uint8_t* s, uint32_t off, uint32_t v) {
    if (!at<uint32_t>(s, 0x2C)) return 0;
    at<uint32_t>(s, 0x2C) = 1;
    at<uint32_t>(s, off) = v;
    return v;
}
uint32_t __cdecl SelectCar_SetZoomSpeed(uint8_t* s, uint32_t v) { return select_set(s, 0x140, v); }
uint32_t __cdecl SelectCar_SetVRotateSpeed(uint8_t* s, uint32_t v) { return select_set(s, 0x144, v); }
// 0x004455A0 SelectCarCameraMover::SetLookAtAxisRoll(r): +0xB8 = +0xF8 = +0x138 = r. EAX = r.
uint32_t __cdecl SelectCar_SetLookAtAxisRoll(uint8_t* s, uint32_t r) {
    at<uint32_t>(s, 0xB8) = r;
    at<uint32_t>(s, 0xF8) = r;
    at<uint32_t>(s, 0x138) = r;
    return r;
}
// 0x004455C0 SelectCarCameraMover::SkipAnimation: +0x14C = 0. EAX untouched.
uint32_t __cdecl SelectCar_SkipAnimation(uint8_t* s, uint32_t eax_in) {
    at<uint32_t>(s, 0x14C) = 0;
    return eax_in;
}
// 0x00445B10 CubicCameraMover::SetPovType(t): +0x48 = t, dirty +0x52. EAX = t.
uint32_t __cdecl Cubic_SetPovType(uint8_t* s, uint32_t t) {
    s[0x52] = 1;
    at<uint32_t>(s, 0x48) = t;
    return t;
}
// 0x0044DE60 CubicCameraMover::SetDisableLag(b): lag flag +0x50 = !b. AL = it, upper EAX untouched.
uint32_t __cdecl Cubic_SetDisableLag(uint8_t* s, uint32_t b, uint32_t eax_in) {
    const uint8_t v = static_cast<uint8_t>(b) == 0 ? 1 : 0;
    s[0x50] = v;
    return (eax_in & 0xFFFFFF00u) | v;
}
// 0x0044DE70 CubicCameraMover::GetLookbackAngle: 0x8000 (half a turn) while looking back (+0x51)
uint32_t __cdecl Cubic_GetLookbackAngle(uint8_t* s) { return s[0x51] ? 0x8000u : 0u; }

// reset a cubic to its desired state: value/slope from +8/+0xC, dirty word cleared; 3D cubics have three axes
void cubic_reset(uint8_t* c, int axes) {
    for (int k = 0; k < axes; ++k) {
        uint8_t* a = c + k * 0x30;
        at<uint32_t>(a, 0) = at<uint32_t>(a, 8);
        at<uint32_t>(a, 4) = at<uint32_t>(a, 0xC);
        at<uint16_t>(a, 0x2C) = 0;
    }
}
// 0x00445B20 CubicCameraMover::ResetState: the 3D cubics (+0x44, +0x38, +0x3C, +0x40) and the 1D one (+0x34)
// snap to their targets; the view (+0x1C) gets a reset flag +0x270 and the reset time +0x27C. EAX = that time.
uint32_t __cdecl Cubic_ResetState(uint8_t* s) {
    cubic_reset(ptr(s, 0x44), 3);
    cubic_reset(ptr(s, 0x34), 1);
    cubic_reset(ptr(s, 0x38), 3);
    cubic_reset(ptr(s, 0x3C), 3);
    cubic_reset(ptr(s, 0x40), 3);
    uint8_t* v = ptr(s, 0x1C);
    v[0x270] = 1;
    const uint32_t t = gu(0x007FB708u);
    at<uint32_t>(v, 0x27C) = t;
    return t;
}
// 0x00446670 ShowcaseCameraMover::ResetState: only the view's reset flag and time. EAX = the view.
uint32_t __cdecl Showcase_ResetState(uint8_t* s) {
    uint8_t* v = ptr(s, 0x1C);
    v[0x270] = 1;
    at<uint32_t>(v, 0x27C) = gu(0x007FB708u);
    return u(v);
}
// 0x00574D10 (tCubic3D setter): +0x24 = v.x (raw), +0x54 = v.y, +0x84 = v.z. EAX = v.x's bits.
uint32_t __cdecl Cubic3D_SetVal574D10(uint8_t* s, uint32_t vp) {
    const float* v = reinterpret_cast<const float*>(vp);
    const X z = ld(v[2]), y = ld(v[1]);
    const uint32_t x = *reinterpret_cast<const uint32_t*>(v);
    at<float>(s, 0x54) = st(y);
    at<uint32_t>(s, 0x24) = x;
    at<float>(s, 0x84) = st(z);
    return x;
}

// 0x00444910 CameraMover::FovCubicInit(tCubic1D* c): value = fov word [+0x1C]+0xC4 + zoom word +0x264 * 0x865154,
// slope = zoom * c+0x24; either differing from its target (+8 / +0xC) marks the cubic dirty (+0x2C = 2).
// EAX = high half of the slope's bits + the last x87 status word.
uint32_t __cdecl Mover_FovCubicInit(uint8_t* s, uint32_t cp) {
    uint8_t* v = ptr(s, 0x1C);
    uint8_t* c = reinterpret_cast<uint8_t*>(cp);
    const X zoom = ild(at<uint16_t>(v, 0x264));
    const X fov = ild(at<uint16_t>(v, 0xC4));
    const X val = add(fov, mul(zoom, ld(gf(0x00865154u))));
    const float slope = st(mul(zoom, ld(at<float>(c, 0x24))));
    at<float>(c, 0) = st(val);
    if (parity_even(static_cast<uint8_t>(cmpw(val, ld(at<float>(c, 8))) >> 8) & 0x44)) at<uint16_t>(c, 0x2C) = 2;
    uint32_t sb;
    std::memcpy(&sb, &slope, 4);
    const uint16_t sw = cmpw(ld(slope), ld(at<float>(c, 0xC)));
    at<uint32_t>(c, 4) = sb;
    if (parity_even(static_cast<uint8_t>(sw >> 8) & 0x44)) at<uint16_t>(c, 0x2C) = 2;
    return (sb & 0xFFFF0000u) | sw;
}

// 0x00444CF0 TrackCarCameraMover::CalcSpringForce(out, a, b): out = a * +0x60 + b * +0x64 (the b.x product stays on
// the x87 stack, the others go through floats). EAX = out.
uint32_t __cdecl TrackCar_CalcSpringForce(uint8_t* s, uint32_t op, uint32_t ap, uint32_t bp) {
    const float* a = reinterpret_cast<const float*>(ap);
    const float* b = reinterpret_cast<const float*>(bp);
    const X k1 = ld(at<float>(s, 0x64));
    const X bx = mul(k1, ld(b[0]));
    const float by = st(mul(ld(b[1]), k1)), bz = st(mul(ld(b[2]), k1));
    const X k0 = ld(at<float>(s, 0x60));
    const float ax = st(mul(k0, ld(a[0]))), ay = st(mul(ld(a[1]), k0)), az = st(mul(ld(a[2]), k0));
    float* out = reinterpret_cast<float*>(op);
    out[0] = st(add(ld(ax), bx));
    out[1] = st(add(ld(ay), ld(by)));
    out[2] = st(add(ld(az), ld(bz)));
    return op;
}

// ---- drag camera key tables (0x88-byte keys) -----------------------------------------------------------------

// 0x00442980 DragCameraManager::GetNisCameraGroup(id): the group (0x14 bytes at [+0x10], count +0x1C) with that id
uint32_t __cdecl Drag_GetNisCameraGroup(uint8_t* s, uint32_t id) {
    const int32_t n = at<int32_t>(s, 0x1C);
    for (int32_t i = 0; i < n; ++i)
        if (at<uint32_t>(ptr(s, 0x10), i * 0x14) == id) return at<uint32_t>(s, 0x10) + i * 0x14;
    return 0;
}
// 0x00442A10 DragCameraManager::GetCurrentTrack: the current index +0x1C3C in the table of the current mode +0x1C50
// (0: [+0x10] count +0x1C, 1: [+0x18] count +0x24, 2: [+0x14] count +0x20), if it is in range
uint32_t __cdecl Drag_GetCurrentTrack(uint8_t* s) {
    const uint32_t mode = at<uint32_t>(s, 0x1C50);
    const int32_t i = at<int32_t>(s, 0x1C3C);
    uint32_t cnt, base;
    if (mode == 0) cnt = 0x1C, base = 0x10;
    else if (mode == 1) cnt = 0x24, base = 0x18;
    else if (mode == 2) cnt = 0x20, base = 0x14;
    else return 0;
    int32_t c = i > 0 ? i : 0;
    const int32_t m = at<int32_t>(s, cnt) - 1;
    if (m < c) c = m;
    if (i != c) return 0;
    return at<uint32_t>(s, base) + static_cast<uint32_t>(i) * 0x14;
}
// 0x00442AE0 DragCameraTrack::GetKeyNumber(t): the last key (0x88 bytes from +0x2C, count signed byte +0x10)
// whose time is <= t, searching down; 0 if none
uint32_t __cdecl DragTrack_GetKeyNumber(uint8_t* s, uint32_t tb) {
    float t;
    std::memcpy(&t, &tb, 4);
    int32_t n = static_cast<int8_t>(s[0x10]) - 1;
    if (n <= 0) return static_cast<uint32_t>(n);
    do {
        const uint8_t ah = cmp(ld(at<float>(s, 0x2C + n * 0x88)), ld(t));
        if (!parity_even(ah & 0x41)) break;  // key < t or key == t
        --n;
    } while (n > 0);
    return static_cast<uint32_t>(n);
}
// 0x00442D00 DragCameraManager::GetParameter(i, track): a key's parameter. No track: the manager's own keys
// (+0x1A8 + i*0x88, count +0x1C2C, i clamped up from 0); with a track: its keys (+0x2C, count byte +0x10), 0 for
// i < 0. Past the end: 1 (0x784250).
float __cdecl Drag_GetParameter(uint8_t* s, uint32_t ib, uint32_t tp) {
    const int32_t i = static_cast<int32_t>(ib);
    if (!tp) {
        int32_t k = i > 0 ? i : 0;
        const int32_t n = at<int32_t>(s, 0x1C2C);
        if (k > n) k = n;
        return k < n ? st(ld(at<float>(s, 0x1A8 + k * 0x88))) : st(ld(gf(0x00784250u)));
    }
    if (i < 0) return st(ld(gf(0x007D7D3Cu)));
    if (i >= static_cast<int8_t>(at<uint8_t>(tp, 0x10))) return st(ld(gf(0x00784250u)));
    return st(ld(at<float>(tp, 0x2C + i * 0x88)));
}
// 0x00442EE0 DragCameraManager::GetNeighbour(key, forward, track): the key after/before `key` in the same table
// (a track's keys from +0x20, or the manager's from +0x19C), 0 past either end
uint32_t __cdecl Drag_GetNeighbour(uint8_t* s, uint32_t kp, uint32_t fwd, uint32_t tp) {
    const uint32_t base = tp ? tp + 0x20 : u(s) + 0x19C;
    const int32_t k = static_cast<int32_t>(kp - base) / 0x88;
    const int32_t j = k + (fwd ? 1 : -1);
    const int32_t last = (tp ? static_cast<int8_t>(at<uint8_t>(tp, 0x10)) : at<int32_t>(s, 0x1C2C)) - 1;
    const int32_t c = j > 0 ? j : 0;
    const int32_t e = last < c ? last : c;
    if (j != e) return 0;
    return base + static_cast<uint32_t>(j) * 0x88;
}
// 0x004434F0 DragCameraManager::KeysSharedSpace(a, i, b, j): same type bytes (+0, +4, +5) and positions (+0x18 +
// 12i, 3 floats) and targets (+0x30 + 12i) within 0x7845A0 of each other. AL = result.
uint32_t __cdecl Drag_KeysSharedSpace(uint32_t ap, uint32_t i, uint32_t bp, uint32_t j) {
    const uint8_t* a = reinterpret_cast<const uint8_t*>(ap);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(bp);
    if (a[0] != b[0] || a[4] != b[4] || a[5] != b[5]) return 0;
    const X eps = ld(gf(0x007845A0u));
    for (uint32_t k = 0; k < 3; ++k) {
        X d = sub(ld(at<float>(a, (i * 3 + 6 + k) * 4)), ld(at<float>(b, (j * 3 + 6 + k) * 4)));
        d.b[9] &= 0x7F;
        if (!(cmp(d, eps) & 0x41)) return 0;  // greater
        X e = sub(ld(at<float>(a, (i * 3 + 12 + k) * 4)), ld(at<float>(b, (j * 3 + 12 + k) * 4)));
        e.b[9] &= 0x7F;
        if (!(cmp(e, eps) & 0x41)) return 0;
    }
    return 1;
}
// 0x00443690 DragCameraManager::BackupTangentLength: copy the current key (+0x19C + index +0x1C30 * 0x88) to the
// working key at +0x2C. EAX = index * 0x88.
uint32_t __cdecl Drag_BackupTangentLength(uint8_t* s) {
    const uint32_t off = at<uint32_t>(s, 0x1C30) * 0x88;
    for (uint32_t k = 0; k < 0x22; ++k) at<uint32_t>(s, 0x2C + k * 4) = at<uint32_t>(s, off + 0x19C + k * 4);
    return off;
}

}  // namespace

LEAF_CDECL(0x00441190, OutsidePovType, 1, "bool OutsidePovType(int)");
LEAF_THIS(0x00441AE0, Mover_GetTarget, 0, "bVector3* CameraMover::GetTarget()");
LEAF_THIS(0x00444B90, Orbit_GetTarget, 0, "bVector3* OrbitCarCameraMover::GetTarget()");
LEAF_THIS(0x00444CD0, TrackCar_GetTarget, 0, "bVector3* TrackCarCameraMover::GetTarget()");
LEAF_THIS(0x00444030, Mover_GetCarToFollow, 0, "Car* CameraMover::GetCarToFollow()");
LEAF_CDECL(0x00441F70, GenericCategoryName, 1, "const char* GetGenericCategoryName(int)");
LEAF_CDECL(0x00441FA0, GenericCategory, 1, "GenericCategory* GetGenericCategory(int)");
LEAF_THIS(0x00444DC0, SelectCar_IsAtDefault, 0, "bool SelectCarCameraMover::IsCameraAtDefaultPosition()");
LEAF_THIS(0x004454A0, SelectCar_SetHRotateSpeed, 1, "void SelectCarCameraMover::SetHRotateSpeed(float)");
LEAF_THIS(0x004454C0, SelectCar_SetZoomSpeed, 1, "void SelectCarCameraMover::SetZoomSpeed(float)");
LEAF_THIS(0x004454E0, SelectCar_SetVRotateSpeed, 1, "void SelectCarCameraMover::SetVRotateSpeed(float)");
LEAF_THIS(0x004455A0, SelectCar_SetLookAtAxisRoll, 1, "void SelectCarCameraMover::SetLookAtAxisRoll(float)");
LEAF_THIS_EAX(0x004455C0, SelectCar_SkipAnimation, 0, "void SelectCarCameraMover::SkipAnimation()");
LEAF_THIS(0x00445B10, Cubic_SetPovType, 1, "void CubicCameraMover::SetPovType(int)");
LEAF_THIS_EAX(0x0044DE60, Cubic_SetDisableLag, 1, "void CubicCameraMover::SetDisableLag(bool)");
LEAF_THIS(0x0044DE70, Cubic_GetLookbackAngle, 0, "unsigned short CubicCameraMover::GetLookbackAngle()");
LEAF_THIS(0x00445B20, Cubic_ResetState, 0, "void CubicCameraMover::ResetState()");
LEAF_THIS(0x00446670, Showcase_ResetState, 0, "void ShowcaseCameraMover::ResetState()");
LEAF_THIS(0x00574D10, Cubic3D_SetVal574D10, 1, "void tCubic3D setter (0x574D10)");
LEAF_THIS(0x00444910, Mover_FovCubicInit, 1, "void CameraMover::FovCubicInit(tCubic1D*)");
LEAF_THIS(0x00444CF0, TrackCar_CalcSpringForce, 3, "void TrackCarCameraMover::CalcSpringForce(bVector3*, const bVector3*, const bVector3*)");
LEAF_THIS(0x00442980, Drag_GetNisCameraGroup, 1, "DragCameraGroup* DragCameraManager::GetNisCameraGroup(int)");
LEAF_THIS(0x00442A10, Drag_GetCurrentTrack, 0, "DragCameraTrack* DragCameraManager::GetCurrentTrack()");
LEAF_THIS(0x00442AE0, DragTrack_GetKeyNumber, 1, "int DragCameraTrack::GetKeyNumber(float t)");
LEAF_THIS(0x00442D00, Drag_GetParameter, 2, "float DragCameraManager::GetParameter(int, DragCameraTrack*)");
LEAF_THIS(0x00442EE0, Drag_GetNeighbour, 3, "DragCameraKey* DragCameraManager::GetNeighbour(DragCameraKey*, bool forward, DragCameraTrack*)");
LEAF_STD(0x004434F0, Drag_KeysSharedSpace, 4, "bool DragCameraManager::KeysSharedSpace(key*, int, key*, int)");
LEAF_THIS(0x00443690, Drag_BackupTangentLength, 0, "void DragCameraManager::BackupTangentLength()");

NFSU2_DIFF_TEST(camera_leaf1) {
    // 0 mover; 1 view (+0x1C); 2 follow target (+0x20, nullable); 3 orbited car (+0xB0, nullable); 4 tracked car
    // (+0x438, nullable); 5-9 cubics (+0x34 1D, +0x38/+0x3C/+0x40/+0x44 3D); 10-12 buffers / vectors / keys
    static const Spec spec = [] {
        Spec sp{13, 0x500,
                {{0, 0x1C, 1}, {0, 0x20, 2, true}, {0, 0xB0, 3, true}, {0, 0x438, 4, true}, {0, 0x34, 5}, {0, 0x38, 6},
                 {0, 0x3C, 7}, {0, 0x40, 8}, {0, 0x44, 9}},
                {{0, 0x2C, 4, 2}, {0, 0x51, 1, 2}}};
        for (int blk : {11, 12}) sp.ints.push_back({blk, 0, 1, 2}), sp.ints.push_back({blk, 4, 1, 2}), sp.ints.push_back({blk, 5, 1, 2});
        return sp;
    }();
    static const std::vector<Fn> fns{
        LEAF_FN(OutsidePovType, CDECL, INT, {'i', 8}),
        LEAF_FN(Mover_GetTarget, THIS, PTR),
        LEAF_FN(Orbit_GetTarget, THIS, PTR),
        LEAF_FN(TrackCar_GetTarget, THIS, PTR),
        LEAF_FN(Mover_GetCarToFollow, THIS, PTR),
        LEAF_FN(GenericCategoryName, CDECL, PTR, {'i', 12}),
        LEAF_FN(GenericCategory, CDECL, PTR, {'i', 12}),
        LEAF_FN(SelectCar_IsAtDefault, THIS, INT),
        LEAF_FN(SelectCar_SetHRotateSpeed, THIS, INT, {'f', 0}),
        LEAF_FN(SelectCar_SetZoomSpeed, THIS, INT, {'f', 0}),
        LEAF_FN(SelectCar_SetVRotateSpeed, THIS, INT, {'f', 0}),
        LEAF_FN(SelectCar_SetLookAtAxisRoll, THIS, INT, {'r', 0}),
        LEAF_FN(SelectCar_SkipAnimation, THIS, INT),
        LEAF_FN(Cubic_SetPovType, THIS, INT, {'i', 8}),
        LEAF_FN(Cubic_SetDisableLag, THIS, INT, {'i', 3}),
        LEAF_FN(Cubic_GetLookbackAngle, THIS, INT),
        LEAF_FN(Cubic_ResetState, THIS, INT),
        LEAF_FN(Showcase_ResetState, THIS, PTR),
        LEAF_FN(Mover_FovCubicInit, THIS, INT, {'p', 10}),
        LEAF_FN(TrackCar_CalcSpringForce, THIS, PTR, {'p', 10}, {'p', 11}, {'p', 12}),
        LEAF_FN(Drag_KeysSharedSpace, STD, LOW8, {'p', 11}, {'i', 4}, {'p', 12}, {'i', 4}),
    };
    return run(ctx, "camera_leaf1", spec, fns);
}

NFSU2_DIFF_TEST(camera_leaf1_plain) {  // reads raw floats of this: no pointer fields
    static const Spec spec{2, 0x400, {}, {}};
    static const std::vector<Fn> fns{LEAF_FN(Cubic3D_SetVal574D10, THIS, INT, {'p', 1})};
    return run(ctx, "camera_leaf1_plain", spec, fns);
}

NFSU2_DIFF_TEST(camera_leaf1_drag) {
    // 0 manager (0x2000): groups [+0x10] (1), track tables [+0x14] (3) / [+0x18] (4), counts, mode, indices;
    // 2 a track (key count byte +0x10, keys from +0x20)
    static const Spec spec = [] {
        Spec sp{5, 0x2000, {{0, 0x10, 1}, {0, 0x14, 3}, {0, 0x18, 4}},
                {{0, 0x1C, 4, 8}, {0, 0x20, 4, 8}, {0, 0x24, 4, 8}, {0, 0x1C50, 4, 4}, {0, 0x1C3C, 4, 10, 0xFFFFFFFEu},
                 {0, 0x1C2C, 4, 9}, {0, 0x1C30, 4, 8}, {2, 0x10, 1, 10}}};
        for (uint32_t i = 0; i < 8; ++i) sp.ints.push_back({1, i * 0x14, 4, 4});
        return sp;
    }();
    static const std::vector<Fn> fns{
        LEAF_FN(Drag_GetNisCameraGroup, THIS, PTR, {'i', 4}),
        LEAF_FN(Drag_GetCurrentTrack, THIS, PTR),
        LEAF_FN(Drag_GetParameter, THIS, FLT, {'j', 3}, {'i', 1}),
        LEAF_FN(Drag_GetParameter, THIS, FLT, {'j', 3}, {'p', 2}),
        LEAF_FN(Drag_GetNeighbour, THIS, PTR, {'q', 0u | (0x19Cu << 8) | (0x88u << 20)}, {'i', 2}, {'i', 1}),
        LEAF_FN(Drag_GetNeighbour, THIS, PTR, {'q', 2u | (0x20u << 8) | (0x88u << 20)}, {'i', 2}, {'p', 2}),
        LEAF_FN(Drag_BackupTangentLength, THIS, INT),
    };
    return run(ctx, "camera_leaf1_drag", spec, fns);
}

NFSU2_DIFF_TEST(camera_leaf1_track) {  // a DragCameraTrack: key count byte +0x10, key times every 0x88 from +0x2C
    static const Spec spec{1, 0x900, {}, {{0, 0x10, 1, 12}}};
    static const std::vector<Fn> fns{LEAF_FN(DragTrack_GetKeyNumber, THIS, INT, {'f', 0})};
    return run(ctx, "camera_leaf1_track", spec, fns);
}

NFSU2_DIFF_TEST(camera_leaf1_BROKEN) {  // harness self-check: OrbitCar target compared with TrackCar target
    static const Spec spec{5, 0x500, {{0, 0x1C, 1}, {0, 0xB0, 3, true}, {0, 0x438, 4, true}}, {}};
    static const std::vector<Fn> fns{Fn("Orbit_vs_TrackCar", &o_Orbit_GetTarget, NFSU2_ENTRY(TrackCar_GetTarget), THIS, PTR)};
    return run(ctx, "camera_leaf1_BROKEN", spec, fns);
}

