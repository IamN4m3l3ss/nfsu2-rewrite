// zCamera batch 4 (8), written from the PC machine code: cubic mover outside-POV test, ice camera FOV / FX tint /
// look / eye (blend of a cubic's value and its desired value), autopilot camera update, power-slide score, aperture
// number -> f-stop. Callees through their game addresses.
#include "leaf.h"
#include "live.h"
#include "x87.h"

namespace {
using namespace x87;

inline float bitsf(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
using GetF = float(__thiscall*)(void*, uint32_t);       // tCubic1D::GetVal(t)
using GetF0 = float(__thiscall*)(void*);                 // tCubic1D::GetValDesired()
using GetV = uint32_t(__thiscall*)(void*, void*);       // tCubic3D getters (out)
using GetVt = uint32_t(__thiscall*)(void*, void*, uint32_t);  // tCubic3D::GetVal(out, t)
inline uint32_t scale_add3(uint32_t out, uint32_t a, uint32_t b, uint32_t s) {  // bScaleAdd (3D) 0x43D0C0, EAX = out
    return game_fn<uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t)>(0x0043D0C0)(out, a, b, s);
}

// 0x00445A90 CubicCameraMover::OutsidePOV: the followed car's ([+0x2C]) POV +0x4C is an outside view (type 0, 1, 5)
uint32_t __cdecl Cubic_OutsidePOV(uint8_t* s) {
    const uint32_t pov = game_fn<uint32_t(__thiscall*)(void*, uint32_t)>(0x005ECCA0)(ptr(s, 0x2C), at<uint32_t>(s, 0x4C));
    const uint16_t t = at<uint16_t>(pov, 0);
    return (t == 1 || t == 0 || t == 5) ? 1u : 0u;
}

// 0x004462A0 IceCameraMover::GetFOV(t): animating ([+0x7C] flag +2): the FOV cubic's ([+0x38]) value at t; else
// value + (desired - value) style blend: desired * t + (1 - t) * value. Truncated to int (_ftol2).
uint32_t __cdecl Ice_GetFOV(uint8_t* s, uint32_t t) {
    uint8_t* fl = ptr(s, 0x7C);
    void* cubic = ptr(s, 0x38);
    if (fl && fl[2]) return static_cast<uint32_t>(ftol(ld(game_fn<GetF>(0x00574870)(cubic, t))));
    const float value = at<float>(cubic, 0);
    const float desired = game_fn<GetF0>(0x005748B0)(cubic);
    const X r = add(mul(ld(desired), ld(bitsf(t))), mul(sub(ld(gf(0x00784250u)), ld(bitsf(t))), ld(value)));
    return static_cast<uint32_t>(ftol(r));
}

// value * (1 - t) + desired * t of a 3D cubic (bScaleAdd into out). EAX = out.
uint32_t blend3(void* cubic, uint32_t op, uint32_t t) {
    float v[3], d[3];
    game_fn<GetV>(0x00574D30)(cubic, v);
    game_fn<GetV>(0x00574DD0)(cubic, d);
    const X k = sub(ld(gf(0x00784250u)), ld(bitsf(t)));
    float* out = reinterpret_cast<float*>(op);
    out[0] = st(mul(ld(v[0]), k));
    out[1] = st(mul(ld(v[1]), k));
    out[2] = st(mul(ld(v[2]), k));
    return scale_add3(op, op, u(d), t);
}
// 0x004462F0 IceCameraMover::GetFxTint(out, t): blend of the tint cubic [+0x58]
uint32_t __cdecl Ice_GetFxTint(uint8_t* s, uint32_t op, uint32_t t) { return blend3(ptr(s, 0x58), op, t); }
// 0x00446180 / 0x00446210 IceCameraMover::GetLook / GetEye(out, t): animating (flag +2 / +3): the cubic's value at t,
// else the blend. Look cubic [+0x30], eye cubic [+0x34].
uint32_t ice_vec(uint8_t* s, uint32_t op, uint32_t t, uint32_t flag, uint32_t cub) {
    uint8_t* fl = ptr(s, 0x7C);
    if (fl && fl[flag]) return game_fn<GetVt>(0x00574D70)(ptr(s, cub), reinterpret_cast<void*>(op), t);
    return blend3(ptr(s, cub), op, t);
}
uint32_t __cdecl Ice_GetLook(uint8_t* s, uint32_t op, uint32_t t) { return ice_vec(s, op, t, 2, 0x30); }
uint32_t __cdecl Ice_GetEye(uint8_t* s, uint32_t op, uint32_t t) { return ice_vec(s, op, t, 3, 0x34); }

// 0x0044ED90 AutoPilotCameraMover::Update: unless paused, copy the source's ([+0x20]) FOV +0x168 to the camera
// ([+0x1C]) +0xC4 (when Camera::StopUpdating 0x828640 is clear) and set the camera matrix from source +0x190 with the
// frame time 0x865198. EAX = IsPaused's / SetCameraMatrix's.
uint32_t __cdecl AutoPilot_Update(uint8_t* s) {
    const uint32_t paused = game_fn<uint32_t(__thiscall*)(void*)>(0x00571250)(reinterpret_cast<void*>(0x00865480u));
    if (static_cast<uint8_t>(paused)) return paused;
    if (gu(0x00828640u) == 0) at<uint16_t>(ptr(s, 0x1C), 0xC4) = at<uint16_t>(ptr(s, 0x20), 0x168);
    return game_fn<uint32_t(__thiscall*)(void*, uint32_t, uint32_t)>(0x00447280)(ptr(s, 0x1C), u(ptr(s, 0x20)) + 0x190,
                                                                                  gu(0x00865198u));
}

// 0x00441E90 GenericPowerSlideScore(car): above the speed 0x7844F8 (+0x430), clamp((|yaw| - 0x7875A4) * 0x7875A0 +
// 0x784358, 0, 1) (0 when the scaled slide is not positive), times 0x786FAC
float __cdecl PowerSlideScore(uint32_t car) {
    const X k = ld(gf(0x00786FACu));
    if (cmp(ld(at<float>(car, 0x430)), ld(gf(0x007844F8u))) & 0x41) return st(mul(k, ld(0.0f)));  // not faster
    const float yaw = game_fn<GetF0>(0x0059FA10)(reinterpret_cast<void*>(car + 0x40));
    X y = ld(yaw);
    y.b[9] &= 0x7F;  // fabs
    const X t = mul(sub(y, ld(gf(0x007875A4u))), ld(gf(0x007875A0u)));
    if (cmp(t, ld(gf(0x007D7D3Cu))) & 0x41) return st(mul(k, ld(0.0f)));
    const X t2 = add(t, ld(gf(0x00784358u)));
    const float t2f = st(t2);
    if (cmp(t2, ld(gf(0x007D7D3Cu))) & 0x41) return st(mul(k, ld(gf(0x007D7D3Cu))));
    const float one = gf(0x00784250u);
    const float val = parity_even(cmp(ld(one), ld(t2f)) & 0x05) ? t2f : one;  // 1 >= t2 (ordered) keeps t2
    return st(mul(k, ld(val)));
}

// 0x00443DF0 ConvertApertureNumberToFStop(n): the 1/3-stop table (1, 2^(1/6), ... 64) at round-half-up n,
// clamped to 0..36
constexpr uint32_t kFStops[37] = {
    0x3F800000, 0x3F8FACD6, 0x3FA14518, 0x3FB504F3, 0x3FCB2FF5, 0x3FE411F0, 0x40000000, 0x400FACD6, 0x40214518,
    0x403504F3, 0x404B2FF5, 0x406411F0, 0x40800000, 0x408FACD6, 0x40A14518, 0x40B504F3, 0x40CB2FF5, 0x40E411F0,
    0x41000000, 0x410FACD6, 0x41214518, 0x413504F3, 0x414B2FF5, 0x416411F0, 0x41800000, 0x418FACD6, 0x41A14518,
    0x41B504F3, 0x41CB2FF5, 0x41E411F0, 0x42000000, 0x420FACD6, 0x42214518, 0x423504F3, 0x424B2FF5, 0x426411F0,
    0x42800000};
float __cdecl ApertureToFStop(uint32_t nb) {
    int32_t i = ftol(add(ld(bitsf(nb)), ld(gf(0x00784260u))));
    if (i <= 0) i = 0;
    else if (i > 36) i = 36;
    return st(ld(bitsf(kFStops[i])));
}

}  // namespace

LEAF_THIS(0x00445A90, Cubic_OutsidePOV, 0, "bool CubicCameraMover::OutsidePOV()");
LEAF_THIS(0x004462A0, Ice_GetFOV, 1, "int IceCameraMover::GetFOV(float t)");
LEAF_THIS(0x004462F0, Ice_GetFxTint, 2, "void IceCameraMover::GetFxTint(bVector3* out, float t)");
LEAF_THIS(0x00446180, Ice_GetLook, 2, "void IceCameraMover::GetLook(bVector3* out, float t)");
LEAF_THIS(0x00446210, Ice_GetEye, 2, "void IceCameraMover::GetEye(bVector3* out, float t)");
LEAF_THIS(0x0044ED90, AutoPilot_Update, 0, "void AutoPilotCameraMover::Update()");
LEAF_CDECL(0x00441E90, PowerSlideScore, 1, "float GenericPowerSlideScore(Car*)");
LEAF_CDECL(0x00443DF0, ApertureToFStop, 1, "float ConvertApertureNumberToFStop(float)");

NFSU2_DIFF_TEST(camera_calls2_ice) {
    // 0 ice mover: flags [+0x7C] (1, nullable; bytes +2/+3), 1D FOV cubic [+0x38] (2), 3D cubics [+0x30] look (3),
    // [+0x34] eye (4), [+0x58] tint (5); 6 output
    static const Spec spec{7, 0x200,
                           {{0, 0x7C, 1, true}, {0, 0x38, 2}, {0, 0x30, 3}, {0, 0x34, 4}, {0, 0x58, 5}},
                           {{1, 2, 1, 2}, {1, 3, 1, 2}}};
    static const std::vector<Fn> fns{
        LEAF_FN(Ice_GetFOV, THIS, INT, {'f', 0}),
        LEAF_FN(Ice_GetFxTint, THIS, PTR, {'p', 6}, {'f', 0}),
        LEAF_FN(Ice_GetLook, THIS, PTR, {'p', 6}, {'f', 0}),
        LEAF_FN(Ice_GetEye, THIS, PTR, {'p', 6}, {'f', 0}),
    };
    return run(ctx, "camera_calls2_ice", spec, fns);
}

NFSU2_DIFF_TEST(camera_calls2_pov) {
    // 0 cubic mover: car [+0x2C] (1) -> [+0xC] (2) whose +0x720 is a CameraInfo; POV id +0x4C
    static const Spec spec = [] {
        Spec sp{3, 0x900, {{0, 0x2C, 1}, {1, 0xC, 2}}, {{0, 0x4C, 4, 4}, {2, 0x720, 4, 6, 1}, {2, 0x724, 4, 6, 1}}};
        for (uint32_t i = 0; i < 6; ++i)
            sp.ints.push_back({2, 0x730 + i * 16, 2, 4}), sp.ints.push_back({2, 0x790 + i * 16, 2, 4}),
                sp.ints.push_back({2, 0x732 + i * 16, 2, 7}), sp.ints.push_back({2, 0x792 + i * 16, 2, 7});
        // every car has POV 0 (the fallback); without it the original reads through null too
        sp.ints.push_back({2, 0x730, 2, 1}), sp.ints.push_back({2, 0x790, 2, 1});
        return sp;
    }();
    static const std::vector<Fn> fns{LEAF_FN(Cubic_OutsidePOV, THIS, INT)};
    return run(ctx, "camera_calls2_pov", spec, fns);
}

NFSU2_DIFF_TEST(camera_calls2_autopilot) {  // 0 mover: camera [+0x1C] (1, a Camera), source [+0x20] (2)
    static const Spec spec{3, 0x300, {{0, 0x1C, 1}, {0, 0x20, 2}}, {}};
    static const std::vector<Fn> fns{LEAF_FN(AutoPilot_Update, THIS, INT)};
    return run(ctx, "camera_calls2_autopilot", spec, fns);
}

NFSU2_DIFF_TEST(camera_calls2_plain) {
    static const Spec spec{2, 0x500, {}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(PowerSlideScore, CDECL, FLT, {'p', 1}),
        LEAF_FN(ApertureToFStop, CDECL, FLT, {'f', 0}),
        LEAF_FN(ApertureToFStop, CDECL, FLT, {'j', 40}),  // small integers as float bits: denormals / zero
    };
    return run(ctx, "camera_calls2_plain", spec, fns);
}

NFSU2_DIFF_TEST(camera_calls2_BROKEN) {  // harness self-check: GetLook compared with GetEye
    static const Spec spec{7, 0x200, {{0, 0x7C, 1, true}, {0, 0x30, 3}, {0, 0x34, 4}}, {{1, 2, 1, 2}, {1, 3, 1, 2}}};
    static const std::vector<Fn> fns{Fn("Look_vs_Eye", &o_Ice_GetLook, NFSU2_ENTRY(Ice_GetEye), THIS, PTR, {'p', 6}, {'f', 0})};
    return run(ctx, "camera_calls2_BROKEN", spec, fns);
}

// Live check specs (game/framework/live.h). AutoPilot::Update writes the live camera through a pointer: no spec.
LEAF_LIVE(0x004462A0, Ice_GetFOV, THIS, INT, 0x80, 0);
LEAF_LIVE(0x004462F0, Ice_GetFxTint, THIS, PTR, 0x80, 12, 0);
LEAF_LIVE(0x00446180, Ice_GetLook, THIS, PTR, 0x80, 12, 0);
LEAF_LIVE(0x00446210, Ice_GetEye, THIS, PTR, 0x80, 12, 0);
LEAF_LIVE(0x00445A90, Cubic_OutsidePOV, THIS, INT, 0x60);
LEAF_LIVE(0x00441E90, PowerSlideScore, CDECL, FLT, 0, 0x440);
LEAF_LIVE(0x00443DF0, ApertureToFStop, CDECL, FLT, 0, 0);
