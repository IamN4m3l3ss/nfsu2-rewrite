// zCamera batch 2 (14): functions that call verified maths helpers or drag-camera functions. Written from the PC
// machine code: lens / FOV conversions, SinController, drag-camera index / interval / handle / shared-key / track
// lookups, select-car step count, ice-camera FX gain, camera POV lookup, drag-camera eye / look keys.
// Callees are called through their game addresses (their own rewrite, when enabled, runs for both sides).
#include "leaf.h"
#include "live.h"
#include "x87.h"

namespace {
using namespace x87;

using IntFF = uint32_t(__cdecl*)(uint32_t, uint32_t);
using FltI = float(__cdecl*)(uint32_t);
inline uint32_t bATan(uint32_t x, uint32_t y) { return game_fn<IntFF>(0x0043E6D0)(x, y); }
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
// lens constant: film width * scale (0x7870A8 * 0x7870AC)
inline float lens_k() { return st(mul(ld(gf(0x007870A8u)), ld(gf(0x007870ACu)))); }

// 0x00443D20 ConvertLensLengthToFovAngle(lens) = 2 * bATan(lens, K)
uint32_t __cdecl LensToFov(uint32_t lens) { return (bATan(lens, fbits(lens_k())) & 0xFFFF) << 1; }

// 0x00443D50 ConvertFovAngleToLensLength(angle) = 0x7870A8 / tan(angle / 2) * 0x7870AC (bSin / bCos of the half
// angle, 16-bit)
float __cdecl FovToLens(uint32_t angle) {
    const uint32_t h = static_cast<uint16_t>(angle) >> 1;
    const float s = game_fn<FltI>(0x0043C2B0)(h);
    const float c = game_fn<FltI>(0x0043C370)(h);
    return st(mul(div(ld(gf(0x007870A8u)), div(ld(s), ld(c))), ld(gf(0x007870ACu))));
}

// 0x00443D80 ConvertLensDeltaToFovDelta(lens, delta): FOV(lens + delta * 0x78425C) - FOV(lens), as signed 16-bit
// angles, times 0x784258
float __cdecl LensDeltaToFovDelta(uint32_t lens, uint32_t delta) {
    const uint32_t a1 = (bATan(lens, fbits(lens_k())) & 0xFFFF) << 1;
    const float lens2 = st(add(mul(ld(bitsf(delta)), ld(gf(0x0078425Cu))), ld(bitsf(lens))));
    const uint32_t a2 = bATan(fbits(lens2), fbits(lens_k())) << 1;
    const int32_t d = static_cast<int16_t>(a2) - static_cast<int16_t>(a1);
    return st(mul(ild(d), ld(gf(0x00784258u))));
}

// 0x004466D0 SinController::Function(t) = lo + (hi - lo) * (sin(t) - 0x7842A0) * 0x784260 (lo +0xC, hi +0x10)
float __cdecl SinController_Function(uint8_t* s, uint32_t t) {
    const float sn = game_fn<FltI>(0x0043C350)(t);
    const X w = mul(sub(ld(sn), ld(gf(0x007842A0u))), ld(gf(0x00784260u)));
    return st(add(mul(w, sub(ld(at<float>(s, 0x10)), ld(at<float>(s, 0xC)))), ld(at<float>(s, 0xC))));
}

// ---- drag camera ------------------------------------------------------------------------------------------------

// 0x00442B20 DragCameraManager::GetDragIndex(t, track): the track's key number, or the manager's own last key
// (times every 0x88 from +0x1A8, count +0x1C2C) whose time is <= t
uint32_t __cdecl Drag_GetDragIndex(uint8_t* s, uint32_t t, uint32_t track) {
    if (track) return game_fn<uint32_t(__thiscall*)(void*, uint32_t)>(0x00442AE0)(reinterpret_cast<void*>(track), t);
    int32_t n = at<int32_t>(s, 0x1C2C) - 1;
    if (n <= 0) return static_cast<uint32_t>(n);
    do {
        if (!parity_even(cmp(ld(at<float>(s, 0x1A8 + n * 0x88)), ld(bitsf(t))) & 0x41)) break;
        --n;
    } while (n > 0);
    return static_cast<uint32_t>(n);
}

// 0x00442D70 DragCameraManager::GetIntervalSize(key, track) = parameter(k + 1) - parameter(k) for the key's index k
float __cdecl Drag_GetIntervalSize(uint8_t* s, uint32_t key, uint32_t track) {
    const int32_t k = static_cast<int32_t>(key - (track ? track + 0x20 : u(s) + 0x19C)) / 0x88;
    using P = float(__thiscall*)(void*, uint32_t, uint32_t);
    const float a = game_fn<P>(0x00442D00)(s, static_cast<uint32_t>(k + 1), track);
    const float b = game_fn<P>(0x00442D00)(s, static_cast<uint32_t>(k), track);
    return st(sub(ld(a), ld(b)));
}

// 0x0044A160 DragCameraManager::KeysShared(a, i, b, j): same times (+0x10[i]) and parameters (+0x48[i]), then
// KeysSharedSpace. AL = result.
uint32_t __cdecl Drag_KeysShared(uint32_t a, uint32_t i, uint32_t b, uint32_t j) {
    if (parity_even(cmp(ld(at<float>(a, 0x10 + i * 4)), ld(at<float>(b, 0x10 + j * 4))) & 0x44)) return 0;
    if (parity_even(cmp(ld(at<float>(a, 0x48 + i * 4)), ld(at<float>(b, 0x48 + j * 4))) & 0x44)) return 0;
    return game_fn<uint32_t(__stdcall*)(uint32_t, uint32_t, uint32_t, uint32_t)>(0x004434F0)(a, i, b, j);
}

// 0x0044A530 DragCameraManager::GetNextValidHandleIndex(step): next index (wrapping) in the current mode's table.
// Mode 1 skips handles whose track section (0x88A334[word +0]) is enabled (flag bit 6) with a different side bit
// (bit 1 vs word +2).
uint32_t __cdecl Drag_GetNextValidHandleIndex(uint8_t* s, uint32_t step) {
    using Mod = int32_t(__cdecl*)(int32_t, int32_t);
    const uint32_t mode = at<uint32_t>(s, 0x1C50);
    const int32_t cur = at<int32_t>(s, 0x1C3C);
    if (mode == 1) {
        int32_t r = game_fn<Mod>(0x0043C280)(cur + static_cast<int32_t>(step), at<int32_t>(s, 0x24));
        while (r != at<int32_t>(s, 0x1C3C)) {
            const uint8_t* e = ptr(s, 0x18) + r * 0x14;
            const uint32_t sec = gu(0x0088A334u + at<uint16_t>(e, 0) * 4u);
            if (sec) {
                const uint32_t f = at<uint32_t>(sec, 0x70);
                if (((f >> 6) & 1) && at<uint16_t>(e, 2) == ((f >> 1) & 1)) return static_cast<uint32_t>(r);
            }
            r = game_fn<Mod>(0x0043C280)(r + static_cast<int32_t>(step), at<int32_t>(s, 0x24));
        }
        return static_cast<uint32_t>(r);
    }
    int32_t n;
    if (mode == 0) n = at<int32_t>(s, 0x1C);
    else if (mode == 2) n = at<int32_t>(s, 0x20);
    else return static_cast<uint32_t>(cur);
    if (n == 0) return static_cast<uint32_t>(cur);
    return static_cast<uint32_t>(game_fn<Mod>(0x0043C280)(cur + static_cast<int32_t>(step), n));
}

// 0x00447C00 DragCameraGroup::GetTrack(name): the track in the group's list (head +0xC) with that name (+0x12)
uint32_t __cdecl DragGroup_GetTrack(uint8_t* s, uint32_t name) {
    uint8_t* head = s + 0xC;
    for (uint8_t* e = ptr(head, 0); e != head; e = ptr(e, 0))
        if (game_fn<IntFF>(0x0043DC30)(name, u(e) + 0x12) == 0) return u(e);
    return 0;
}

// 0x00447A30 / 0x00447A80 DragCameraData::GetEye / GetLook(i, out): key i's eye (+0x18) / look (+0x30) vector;
// y flipped when the track is mirrored (0x828649); type byte +4 / +5 == 3 -> the manager fixes the elevation.
// AL = the mirror flag (or the fix-up's result).
uint32_t drag_vec(uint8_t* s, uint32_t i, uint32_t op, uint32_t base, uint32_t typeoff) {
    float* out = reinterpret_cast<float*>(op);
    for (uint32_t k = 0; k < 3; ++k) at<uint32_t>(out, k * 4) = at<uint32_t>(s, base + i * 12 + k * 4);
    const uint8_t mirror = *reinterpret_cast<const uint8_t*>(0x00828649u);
    if (mirror) {
        X y = ld(out[1]);
        y.b[9] ^= 0x80;  // fchs
        out[1] = st(y);
    }
    if (s[typeoff] == 3) return game_fn<uint32_t(__thiscall*)(void*, uint32_t)>(0x004427C0)(reinterpret_cast<void*>(0x00828770u), op);
    return mirror;
}
uint32_t __cdecl DragData_GetEye(uint8_t* s, uint32_t i, uint32_t op) { return drag_vec(s, i, op, 0x18, 4); }
uint32_t __cdecl DragData_GetLook(uint8_t* s, uint32_t i, uint32_t op) { return drag_vec(s, i, op, 0x30, 5); }

// ---- select car / ice / camera info ---------------------------------------------------------------------------

// 0x004455D0 SelectCarCameraMover::FindNumberOfStepsToMoveVectorToDefault(a, b, speed): |b - a| / speed, 0 when
// equal (bEqual, eps 0x3A83126F) or speed == 0
float __cdecl SelectCar_FindSteps(uint32_t ap, uint32_t bp, uint32_t speed) {
    if (game_fn<uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x0043D190)(ap, bp, 0x3A83126Fu)) return gf(0x007D7D3Cu);
    if (!parity_even(cmp(ld(bitsf(speed)), ld(gf(0x007D7D3Cu))) & 0x44)) return gf(0x007D7D3Cu);
    const float* a = reinterpret_cast<const float*>(ap);
    const float* b = reinterpret_cast<const float*>(bp);
    const X dx = sub(ld(b[0]), ld(a[0])), dy = sub(ld(b[1]), ld(a[1])), dz = sub(ld(b[2]), ld(a[2]));
    const X sq = add(add(mul(dx, dx), mul(dz, dz)), mul(dy, dy));
    return st(div(x87::sqrt(sq), ld(bitsf(speed))));
}

// 0x00446360 IceCameraMover::GetFxGain(out, t): out = value * (1 - t) + desired * t of the 2D cubic [+0x5C]
// (bScaleAdd). EAX = what bScaleAdd leaves (out).
uint32_t __cdecl Ice_GetFxGain(uint8_t* s, uint32_t op, uint32_t t) {
    float v[2], d[2];
    using Get = uint32_t(__thiscall*)(void*, void*);
    game_fn<Get>(0x00574BF0)(ptr(s, 0x5C), v);
    game_fn<Get>(0x00574C70)(ptr(s, 0x5C), d);
    const X k = sub(ld(gf(0x00784250u)), ld(bitsf(t)));
    float* out = reinterpret_cast<float*>(op);
    out[0] = st(mul(ld(v[0]), k));
    out[1] = st(mul(ld(v[1]), k));
    return game_fn<uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t)>(0x0043D090)(op, op, u(d), t);
}

// 0x00441140 CameraInfo::GetPov(id): the POV entry (16 bytes, id word +0) with that id: the second table
// (+0x70, count +4) in view mode 3, else the first (+0x10, count +0)
uint32_t __cdecl CameraInfo_GetPov(uint8_t* s, uint32_t id) {
    const bool alt = game_fn<uint32_t(__cdecl*)()>(0x005B98A0)() == 3;
    const int32_t n = at<int32_t>(s, alt ? 4 : 0);
    uint8_t* base = s + (alt ? 0x70 : 0x10);
    for (int32_t i = 0; i < n; ++i)
        if (static_cast<int32_t>(id) == static_cast<int16_t>(at<uint16_t>(base, i * 16))) return u(base) + i * 16;
    return 0;
}

}  // namespace

LEAF_CDECL(0x00443D20, LensToFov, 1, "unsigned short ConvertLensLengthToFovAngle(float lens)");
LEAF_CDECL(0x00443D50, FovToLens, 1, "float ConvertFovAngleToLensLength(unsigned short angle)");
LEAF_CDECL(0x00443D80, LensDeltaToFovDelta, 2, "float ConvertLensDeltaToFovDelta(float lens, float delta)");
LEAF_THIS(0x004466D0, SinController_Function, 1, "float SinController::Function(float t)");
LEAF_THIS(0x00442B20, Drag_GetDragIndex, 2, "int DragCameraManager::GetDragIndex(float t, DragCameraTrack*)");
LEAF_THIS(0x00442D70, Drag_GetIntervalSize, 2, "float DragCameraManager::GetIntervalSize(DragCameraKey*, DragCameraTrack*)");
LEAF_STD(0x0044A160, Drag_KeysShared, 4, "bool DragCameraManager::KeysShared(key*, int, key*, int)");
LEAF_THIS(0x0044A530, Drag_GetNextValidHandleIndex, 1, "int DragCameraManager::GetNextValidHandleIndex(int step)");
LEAF_THIS(0x00447C00, DragGroup_GetTrack, 1, "DragCameraTrack* DragCameraGroup::GetTrack(const char* name)");
LEAF_THIS(0x00447A30, DragData_GetEye, 2, "void DragCameraData::GetEye(int key, bVector3* out)");
LEAF_THIS(0x00447A80, DragData_GetLook, 2, "void DragCameraData::GetLook(int key, bVector3* out)");
LEAF_STD(0x004455D0, SelectCar_FindSteps, 3, "float SelectCarCameraMover::FindNumberOfStepsToMoveVectorToDefault(const bVector3*, const bVector3*, float)");
LEAF_THIS(0x00446360, Ice_GetFxGain, 2, "void IceCameraMover::GetFxGain(bVector2* out, float t)");
LEAF_THIS(0x00441140, CameraInfo_GetPov, 1, "CameraPov* CameraInfo::GetPov(int id)");

NFSU2_DIFF_TEST(camera_calls1) {  // functions on plain data (no pointer fields read as numbers)
    // 0 object; 1-3 vectors / keys / outputs; 4 2D cubic for the ice camera (+0x5C)
    static const Spec spec = [] {
        Spec sp{5, 0x400, {{0, 0x5C, 4}}, {{0, 0, 4, 6}, {0, 4, 4, 6}}};
        for (uint32_t i = 0; i < 6; ++i) sp.ints.push_back({0, 0x10 + i * 16, 2, 4}), sp.ints.push_back({0, 0x70 + i * 16, 2, 4});
        for (int blk : {1, 2}) sp.ints.push_back({blk, 0, 1, 2}), sp.ints.push_back({blk, 4, 1, 2}), sp.ints.push_back({blk, 5, 1, 2});
        return sp;
    }();
    static const std::vector<Fn> fns{
        LEAF_FN(LensToFov, CDECL, INT, {'f', 0}),
        LEAF_FN(FovToLens, CDECL, FLT, {'r', 0}),
        LEAF_FN(LensDeltaToFovDelta, CDECL, FLT, {'f', 0}, {'f', 0}),
        LEAF_FN(SinController_Function, THIS, FLT, {'f', 0}),
        LEAF_FN(Drag_KeysShared, STD, LOW8, {'p', 1}, {'i', 4}, {'p', 2}, {'i', 4}),
        LEAF_FN(SelectCar_FindSteps, STD, FLT, {'p', 1}, {'p', 2}, {'f', 0}),
        LEAF_FN(SelectCar_FindSteps, STD, FLT, {'p', 1}, {'p', 1}, {'f', 0}),
        LEAF_FN(Ice_GetFxGain, THIS, PTR, {'p', 3}, {'f', 0}),
        LEAF_FN(CameraInfo_GetPov, THIS, PTR, {'i', 4}),
    };
    return run(ctx, "camera_calls1", spec, fns);
}

NFSU2_DIFF_TEST(camera_calls1_drag) {
    // 0 manager (0x2000): keys every 0x88 from +0x19C (times +0x1A8), counts +0x1C/+0x20/+0x24 (handle tables),
    // mode +0x1C50, current +0x1C3C, key count +0x1C2C, handle table [+0x18]; 1 handle entries; 2 a track.
    // The current index stays below the mode-1 count (6..7): otherwise GetNextValidHandleIndex(0) never ends,
    // in the original too (the game never calls it that way)
    static const Spec spec = [] {
        Spec sp{3, 0x2000, {{0, 0x18, 1}},
                {{0, 0x1C, 4, 8}, {0, 0x20, 4, 8}, {0, 0x24, 4, 2, 6}, {0, 0x1C50, 4, 4}, {0, 0x1C3C, 4, 6},
                 {0, 0x1C2C, 4, 9}, {2, 0x10, 1, 10}}};
        // section numbers 64..319 of the live section table 0x88A334 (1500 entries; empty and loaded ones in free
        // roam): past its end the next global follows, which the game never indexes
        for (uint32_t i = 0; i < 8; ++i) sp.ints.push_back({1, i * 0x14, 2, 256, 64}), sp.ints.push_back({1, i * 0x14 + 2, 2, 2});
        return sp;
    }();
    static const std::vector<Fn> fns{
        LEAF_FN(Drag_GetDragIndex, THIS, INT, {'f', 0}, {'i', 1}),
        LEAF_FN(Drag_GetDragIndex, THIS, INT, {'f', 0}, {'p', 2}),
        LEAF_FN(Drag_GetIntervalSize, THIS, FLT, {'q', 0u | (0x19Cu << 8) | (0x88u << 20)}, {'i', 1}),
        LEAF_FN(Drag_GetIntervalSize, THIS, FLT, {'q', 2u | (0x20u << 8) | (0x88u << 20)}, {'p', 2}),
        LEAF_FN(Drag_GetNextValidHandleIndex, THIS, INT, {'j', 3}),
    };
    return run(ctx, "camera_calls1_drag", spec, fns);
}

NFSU2_DIFF_TEST(camera_calls1_group) {  // a group with a one-track list (head +0xC), names at track +0x12
    static const Spec spec{3, 0x200, {{0, 0xC, 1}, {1, 0, 0, false, 0xC}}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(DragGroup_GetTrack, THIS, PTR, {'p', 2}),
        LEAF_FN(DragGroup_GetTrack, THIS, PTR, {'q', 1u | (0x12u << 8)}),
    };
    return run(ctx, "camera_calls1_group", spec, fns);
}

NFSU2_DIFF_TEST(camera_calls1_data) {  // DragCameraData: type bytes +4/+5, eye keys from +0x18, look keys from +0x30
    static const Spec spec{2, 0x200, {}, {{0, 4, 1, 4}, {0, 5, 1, 4}}};
    static const std::vector<Fn> fns{
        LEAF_FN(DragData_GetEye, THIS, LOW8, {'i', 8}, {'p', 1}),
        LEAF_FN(DragData_GetLook, THIS, LOW8, {'i', 8}, {'p', 1}),
    };
    return run(ctx, "camera_calls1_data", spec, fns);
}

NFSU2_DIFF_TEST(camera_calls1_BROKEN) {  // harness self-check: GetEye compared with GetLook
    static const Spec spec{2, 0x200, {}, {}};
    static const std::vector<Fn> fns{Fn("GetEye_vs_GetLook", &o_DragData_GetEye, NFSU2_ENTRY(DragData_GetLook), THIS, LOW8, {'i', 8}, {'p', 1})};
    return run(ctx, "camera_calls1_BROKEN", spec, fns);
}

// Live check specs (game/framework/live.h): `this` bytes, then bytes behind each pointer argument (0 = a value).
LEAF_LIVE(0x00443D20, LensToFov, CDECL, INT, 0, 0);
LEAF_LIVE(0x00443D50, FovToLens, CDECL, FLT, 0, 0);
LEAF_LIVE(0x00443D80, LensDeltaToFovDelta, CDECL, FLT, 0, 0, 0);
LEAF_LIVE(0x004466D0, SinController_Function, THIS, FLT, 0x20, 0);
LEAF_LIVE(0x00441140, CameraInfo_GetPov, THIS, PTR, 0x200, 0);
