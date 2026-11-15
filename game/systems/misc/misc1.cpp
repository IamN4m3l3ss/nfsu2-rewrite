// zMisc batch 1 (21 small functions), written from the PC machine code: easter eggs, joystick lookups, pause state,
// cubic curves, timer rounding, splines, replay data movers, demo filenames.
#include <cstring>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"
#include "x87.h"

namespace {

inline float fl(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint32_t fb(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
template <class T> T& at(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }

// ---------------------------------------------------------------- EasterEggs (instance 0x865930)
// 0x00576000 IsEasterEggUnlocked(type, id): entries [this+0xC] (0x34 each: type +0x20, id +0x24, unlocked byte +0x30),
// count [this+0x10]. EAX keeps the index arithmetic in its high bytes.
using Egg_t = uint32_t(__thiscall*)(void*, int32_t, int32_t);
Egg_t o_IsEggUnlocked = nullptr;
uint32_t __cdecl IsEggUnlocked(void* self, int32_t type, int32_t id) {
    const uint32_t n = at<uint32_t>(self, 0x10);
    uint32_t i = 0;
    if (n) {
        uint8_t* e = at<uint8_t*>(self, 0xC);
        for (; i < n; ++i)
            if (at<int32_t>(e, i * 0x34 + 0x20) == type && at<int32_t>(e, i * 0x34 + 0x24) == id)
                return ((i * 0x34) & 0xFFFFFF00u) | e[i * 0x34 + 0x30];
    }
    return i & 0xFFFFFF00u;
}
// 0x00579D70 IsPresetUnlocked(preset) = IsEasterEggUnlocked(6, preset)
using Egg1_t = uint32_t(__thiscall*)(void*, int32_t);
Egg1_t o_IsPresetUnlocked = nullptr;
uint32_t __cdecl IsPresetUnlocked(void* self, int32_t preset) { return IsEggUnlocked(self, 6, preset); }
// 0x00579D60 AreContestVinylsUnlocked = IsEasterEggUnlocked(5, 3) != 0 (setne al: high bytes kept)
using Egg0_t = uint32_t(__thiscall*)(void*);
Egg0_t o_AreContestVinylsUnlocked = nullptr;
uint32_t __cdecl AreContestVinylsUnlocked(void* self) {
    const uint32_t r = IsEggUnlocked(self, 5, 3);
    return (r & 0xFFFFFF00u) | ((r & 0xFF) != 0 ? 1u : 0u);
}

// ---------------------------------------------------------------- joystick
// 0x00571A60 IsJoystickPluggedIn(port): port type byte 0x864788[port] != 0xFF
using Port_t = uint32_t(__cdecl*)(int32_t);
Port_t o_IsJoystickPluggedIn = nullptr;
uint32_t __cdecl IsJoystickPluggedIn(int32_t port) { return *reinterpret_cast<uint8_t*>(0x00864788 + port) != 0xFF ? 1u : 0u; }
// 0x005794E0 IsJoyHandlerAdded(port, id): list per port at 0x8655A0 + port*0x1C8, node id at +8
using Handler_t = uint32_t(__cdecl*)(int32_t, int32_t);
Handler_t o_IsJoyHandlerAdded = nullptr;
uint32_t __cdecl IsJoyHandlerAdded(int32_t port, int32_t id) {
    uint8_t* const list = reinterpret_cast<uint8_t*>(0x008655A0 + port * 0x1C8);
    uint8_t* n = *reinterpret_cast<uint8_t**>(list);
    uint8_t* const end = game_fn<uint8_t*(__thiscall*)(void*)>(0x00502FD0)(list);  // list end (sentinel)
    for (; n != end; n = *reinterpret_cast<uint8_t**>(n))
        if (at<int32_t>(n, 8) == id) return 1;
    return 0;
}
// 0x00571AD0 MapEventNameToJoyEvent(name): table 0x7FABA8 {event, name} x 0xDF, bStrICmp
using Map_t = uint32_t(__cdecl*)(const char*);
Map_t o_MapEventNameToJoyEvent = nullptr;
uint32_t __cdecl MapEventNameToJoyEvent(const char* name) {
    for (int32_t i = 0; i < 0xDF; ++i)
        if (game_fn<int32_t(__cdecl*)(const char*, const char*)>(0x0043DCC0)(*reinterpret_cast<const char**>(0x007FABAC + i * 8), name) == 0)
            return *reinterpret_cast<uint32_t*>(0x007FABA8 + i * 8);
    return 0;
}

// ---------------------------------------------------------------- 0x00571250 GameFlowManager::IsPaused
using IsPaused_t = uint32_t(__cdecl*)();
IsPaused_t o_IsPaused = nullptr;
uint32_t __cdecl IsPaused() {
    const uint32_t r = game_fn<uint32_t(__thiscall*)(void*)>(0x005F00F0)(reinterpret_cast<void*>(0x0089CF48));
    if (r & 0xFF) return r & 0xFFFFFF00u;  // xor al, al
    const int32_t* gs = *reinterpret_cast<int32_t**>(0x008900D8);
    return (gs && *gs == 6) ? 1u : 0u;
}

// ---------------------------------------------------------------- cubic curves
// tCubic1D (0x30): value +0, target +8, coefficients +0x10..+0x1C, flags word +0x2C. tCubic3D = 3 x tCubic1D.
// 0x00574870 tCubic1D::GetVal(t) = ((a*t + b)*t + c)*t + d
using GetVal_t = float(__thiscall*)(void*, float);
GetVal_t o_Cubic1DGetVal = nullptr;
float __cdecl Cubic1DGetVal(void* self, float t) {
    return ((t * at<float>(self, 0x10) + at<float>(self, 0x14)) * t + at<float>(self, 0x18)) * t + at<float>(self, 0x1C);
}
// 0x00570AE0 tCubic3D set (x, y, z): value = v; flags = 2 unless v == target (ordered equal)
using Set3_t = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
Set3_t o_Cubic3DSet = nullptr;
void __cdecl Cubic3DSet(void* self, uint32_t x, uint32_t y, uint32_t z) {
    const uint32_t v[3] = {x, y, z};
    for (uint32_t k = 0; k < 3; ++k) {
        at<uint32_t>(self, k * 0x30) = v[k];
        if (!(fl(v[k]) == at<float>(self, k * 0x30 + 8))) at<uint16_t>(self, k * 0x30 + 0x2C) = 2;
    }
}
// 0x00574C90 tCubic3D::SetVal(const bVector3*)
using SetVal_t = void(__thiscall*)(void*, const float*);
SetVal_t o_Cubic3DSetVal = nullptr;
void __cdecl Cubic3DSetVal(void* self, const float* v) { Cubic3DSet(self, fb(v[0]), fb(v[1]), fb(v[2])); }
// 0x00574D50 tCubic3D::GetdVal(bVector3* out): the three +4 fields; EAX = out
using GetdVal_t = float*(__thiscall*)(void*, float*);
GetdVal_t o_Cubic3DGetdVal = nullptr;
float* __cdecl Cubic3DGetdVal(void* self, float* out) {
    at<uint32_t>(out, 0) = at<uint32_t>(self, 0x04);
    at<uint32_t>(out, 4) = at<uint32_t>(self, 0x34);
    at<uint32_t>(out, 8) = at<uint32_t>(self, 0x64);
    return out;
}

// ---------------------------------------------------------------- Timer (int ticks at 4000 Hz)
// 0x00577360 GetSecondsRounded(step): s = ticks/4000/step, rounded to nearest whole step (floor, +1 if frac >= .5),
// back to seconds via ticks: ceil(r*step*4000) / 4000. x87 throughout (fild of the raw tick count).
using Secs_t = float(__thiscall*)(void*, float);
Secs_t o_TimerSecondsRounded = nullptr;
float __cdecl TimerSecondsRounded(void* self, float step) {
    using namespace x87;
    const X k = ld(fl(0x3983126Fu));  // 0.00025f (0x784268)
    const X one = ld(1.0f), half = ld(0.5f), stepx = ld(step);
    const X s = div(mul(ild(at<int32_t>(self, 0)), k), stepx);
    const float sf = st(s);  // fst [esp]
    X f = ild(ftol(s));
    if (!(cmp(f, ld(sf)) & 0x41)) f = sub(f, one);  // trunc above s -> floor
    const float ff = st(f);
    X r = f;
    if (!(cmp(sub(ld(sf), ld(ff)), half) & 0x01)) r = add(ld(ff), one);
    r = mul(mul(r, stepx), ld(4000.0f));
    const float tf = st(r);
    X g = ild(ftol(r));
    if (less(cmp(g, ld(tf)))) g = add(g, one);
    return st(mul(g, k));
}
// 0x00577400 RoundTime(step): ticks = ftol(GetSecondsRounded(step) * 4000 + 0.5)
using Round_t = int32_t(__thiscall*)(void*, float);
Round_t o_TimerRoundTime = nullptr;
int32_t __cdecl TimerRoundTime(void* self, float step) {
    using namespace x87;
    const int32_t t = ftol(add(mul(ld(TimerSecondsRounded(self, step)), ld(4000.0f)), ld(0.5f)));
    at<int32_t>(self, 0) = t;
    return t;
}

// ---------------------------------------------------------------- splines
// Spline: points +0xC (16 bytes each), count +4, owned buffer +0x10, dirty +0x14, length +0x20
// 0x00573140 Spline::Spline(a, b, c)
using SplineCtor_t = void*(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
SplineCtor_t o_SplineCtor = nullptr;
void* __cdecl SplineCtor(void* s, uint32_t a, uint32_t b, uint32_t c) {
    at<uint32_t>(s, 0x8) = a;
    at<uint32_t>(s, 0x0) = b;
    at<uint32_t>(s, 0x14) = 1;
    at<uint32_t>(s, 0xC) = 0;
    at<uint32_t>(s, 0x10) = 0;
    at<uint32_t>(s, 0x4) = 0;
    at<uint32_t>(s, 0x18) = c;
    at<uint32_t>(s, 0x2C) = 0;
    at<uint32_t>(s, 0x20) = 0;
    at<uint32_t>(s, 0x24) = 0;
    at<uint32_t>(s, 0x28) = 0;
    return s;
}
// 0x00579770 Spline::SetControlPoints(points, n): frees an owned buffer first. EAX: points if it freed, else n.
using SetCP_t = uint32_t(__thiscall*)(void*, uint32_t, uint32_t);
SetCP_t o_SplineSetControlPoints = nullptr;
uint32_t __cdecl SplineSetControlPoints(void* s, uint32_t pts, uint32_t n) {
    if (void* owned = at<void*>(s, 0x10)) {
        game_fn<void(__cdecl*)(void*)>(0x0075C6DA)(owned);  // operator delete
        at<uint32_t>(s, 0x10) = 0;
        at<uint32_t>(s, 0xC) = pts;
        at<uint32_t>(s, 0x4) = n;
        return pts;
    }
    at<uint32_t>(s, 0xC) = pts;
    at<uint32_t>(s, 0x4) = n;
    return n;
}
// 0x00573540 Spline::LinearEstimateLength(): when dirty, sum of bDistBetween over consecutive points
using Len_t = float(__thiscall*)(void*);
Len_t o_SplineLinearLength = nullptr;
float __cdecl SplineLinearLength(void* s) {
    if (at<uint32_t>(s, 0x14)) {
        at<float>(s, 0x20) = 0.0f;
        for (int32_t i = 1; i < at<int32_t>(s, 4); ++i) {
            uint8_t* p = at<uint8_t*>(s, 0xC) + i * 16;
            const float d = game_fn<float(__cdecl*)(const void*, const void*)>(0x0043CE10)(p, p - 16);  // bDistBetween
            at<float>(s, 0x20) = d + at<float>(s, 0x20);
        }
        at<uint32_t>(s, 0x14) = 0;
    }
    return at<float>(s, 0x20);
}
// 0x005701D0 QuickSpline::SetNumControlPoints(n)
using SetN_t = void(__thiscall*)(void*, uint32_t);
SetN_t o_QuickSplineSetNum = nullptr;
void __cdecl QuickSplineSetNum(void* s, uint32_t n) {
    const uint16_t w = static_cast<uint16_t>(n);
    at<uint8_t>(s, 0x18) = 1;
    at<uint16_t>(s, 0x1E) = w;
    if (w == 0) {
        at<uint32_t>(s, 0xC) = 0;
        at<uint32_t>(s, 0x10) = 0;
        return;
    }
    switch (at<int32_t>(s, 4)) {
        case 0: at<uint32_t>(s, 0x10) = 0; at<float>(s, 0xC) = static_cast<float>(static_cast<int32_t>(w)); break;
        case 1: at<uint32_t>(s, 0x10) = 0; at<float>(s, 0xC) = static_cast<float>(static_cast<int32_t>(w) - 1); break;
        case 2: at<uint32_t>(s, 0x10) = 0xCB18967Fu; at<uint32_t>(s, 0xC) = 0x4B18967Fu; break;
        default: break;
    }
}
// 0x00574F40 cPoint::SplineSeek(point, t): both halves (point, point+0x30) through 0x574E30
using Seek_t = void(__cdecl*)(uint8_t*, uint32_t);
Seek_t o_PointSplineSeek = nullptr;
void __cdecl PointSplineSeek(uint8_t* p, uint32_t t) {
    using S = void(__cdecl*)(uint8_t*, uint32_t, uint32_t, uint32_t);
    game_fn<S>(0x00574E30)(p, t, 0, 0);
    game_fn<S>(0x00574E30)(p + 0x30, t, 0, 0);
}

// ---------------------------------------------------------------- 0x00571970 DemoModeReplay::GenerateFilename(dst, i)
// names at 0x7FAB40 (one entry; any other index -> 0); empty name -> dst = "". EAX: bSPrintf's, else i.
using GenName_t = uint32_t(__stdcall*)(char*, int32_t);
GenName_t o_GenerateFilename = nullptr;
uint32_t __cdecl GenerateFilename(char* dst, int32_t i) {
    const int32_t k = (i < 0 || i >= 1) ? 0 : i;
    const char* name = *reinterpret_cast<const char**>(0x007FAB40 + k * 4);
    if (*name)
        return static_cast<uint32_t>(game_fn<int32_t(__cdecl*)(char*, const char*, ...)>(0x004400D0)(dst, reinterpret_cast<const char*>(0x007A03F4), name));
    dst[0] = 0;
    return static_cast<uint32_t>(i);
}

// ---------------------------------------------------------------- ReplaySnapshot data movers
// recording +0x40, playback +0x41, position +0x1DC, end +0x1E4, buffer +0x1E8
// 0x00571710 MoveData16(void* src): recording -> 2 bytes into the buffer (position always advances); else
// MoveDataBuffer(src, 2, -1, 0)
using Move16_t = uint32_t(__thiscall*)(void*, uint8_t*);
Move16_t o_MoveData16 = nullptr;
uint32_t __cdecl MoveData16(void* s, uint8_t* src) {
    if (at<uint8_t>(s, 0x40)) {
        const int32_t pos = at<int32_t>(s, 0x1DC), next = pos + 2;
        if (next < at<int32_t>(s, 0x1E4)) {
            at<uint8_t>(s, 0x1E8 + pos) = src[0];
            at<uint8_t>(s, 0x1E9 + pos) = src[1];
        }
        at<int32_t>(s, 0x1DC) = next;
        return static_cast<uint32_t>(pos);
    }
    return game_fn<uint32_t(__thiscall*)(void*, uint8_t*, int32_t, int32_t, int32_t)>(0x00571640)(s, src, 2, -1, 0);
}
// 0x00578410 MoveDataCompress16(float* v, float range): k = 16384/range; int16 = ftol(k * v) through MoveData16;
// recording or playing back -> *v = (float)int16 / k
using MoveC16_t = void(__thiscall*)(void*, float*, float);
MoveC16_t o_MoveDataCompress16 = nullptr;
void __cdecl MoveDataCompress16(void* s, float* v, float range) {
    using namespace x87;
    const X k = div(ld(16384.0f), ld(range));
    int32_t tmp = ftol(mul(k, ld(*v)));
    MoveData16(s, reinterpret_cast<uint8_t*>(&tmp));
    if (at<uint8_t>(s, 0x41) | at<uint8_t>(s, 0x40)) *v = st(div(ild(static_cast<int16_t>(tmp)), k));
}

}  // namespace

NFSU2_REWRITE_THIS(0x00576000, IsEggUnlocked, 2, "bool EasterEggs::IsEasterEggUnlocked(int type, int id)", o_IsEggUnlocked);
NFSU2_REWRITE_THIS(0x00579D70, IsPresetUnlocked, 1, "bool EasterEggs::IsPresetUnlocked(int preset)", o_IsPresetUnlocked);
NFSU2_REWRITE_THIS(0x00579D60, AreContestVinylsUnlocked, 0, "bool EasterEggs::AreContestVinylsUnlocked()", o_AreContestVinylsUnlocked);
NFSU2_REWRITE_N(0x00571A60, IsJoystickPluggedIn, 1, "bool __cdecl IsJoystickPluggedIn(int port)", o_IsJoystickPluggedIn);
NFSU2_REWRITE_N(0x005794E0, IsJoyHandlerAdded, 2, "bool __cdecl IsJoyHandlerAdded(int port, int id)", o_IsJoyHandlerAdded);
NFSU2_REWRITE_N(0x00571AD0, MapEventNameToJoyEvent, 1, "int __cdecl MapEventNameToJoyEvent(const char*)", o_MapEventNameToJoyEvent);
NFSU2_REWRITE_N(0x00571250, IsPaused, 0, "bool __cdecl GameFlowManager::IsPaused()", o_IsPaused);
NFSU2_REWRITE_THIS(0x00574870, Cubic1DGetVal, 1, "float tCubic1D::GetVal(float t)", o_Cubic1DGetVal);
NFSU2_REWRITE_THIS(0x00570AE0, Cubic3DSet, 3, "void tCubic3D::SetVal(float x, float y, float z)", o_Cubic3DSet);
NFSU2_REWRITE_THIS(0x00574C90, Cubic3DSetVal, 1, "void tCubic3D::SetVal(const bVector3*)", o_Cubic3DSetVal);
NFSU2_REWRITE_THIS(0x00574D50, Cubic3DGetdVal, 1, "bVector3* tCubic3D::GetdVal(bVector3* out)", o_Cubic3DGetdVal);
NFSU2_REWRITE_THIS(0x00577360, TimerSecondsRounded, 1, "float Timer::GetSecondsRounded(float step)", o_TimerSecondsRounded);
NFSU2_REWRITE_THIS(0x00577400, TimerRoundTime, 1, "void Timer::RoundTime(float step)", o_TimerRoundTime);
NFSU2_REWRITE_THIS(0x00573140, SplineCtor, 3, "Spline::Spline(a, b, c)", o_SplineCtor);
NFSU2_REWRITE_THIS(0x00579770, SplineSetControlPoints, 2, "void Spline::SetControlPoints(bVector4* pts, int n)", o_SplineSetControlPoints);
NFSU2_REWRITE_THIS(0x00573540, SplineLinearLength, 0, "float Spline::LinearEstimateLength()", o_SplineLinearLength);
NFSU2_REWRITE_THIS(0x005701D0, QuickSplineSetNum, 1, "void QuickSpline::SetNumControlPoints(u16 n)", o_QuickSplineSetNum);
NFSU2_REWRITE_N(0x00574F40, PointSplineSeek, 2, "void __cdecl cPoint::SplineSeek(cPoint*, float t)", o_PointSplineSeek);
NFSU2_REWRITE_STD(0x00571970, GenerateFilename, 2, "void DemoModeReplay::GenerateFilename(char* dst, int i)", o_GenerateFilename);
NFSU2_REWRITE_THIS(0x00571710, MoveData16, 1, "void ReplaySnapshot::MoveData16(void* data)", o_MoveData16);
NFSU2_REWRITE_THIS(0x00578410, MoveDataCompress16, 2, "void ReplaySnapshot::MoveDataCompress16(float* v, float range)", o_MoveDataCompress16);

namespace {

float rf(game::Rng& r) {
    switch (r.below(8)) {
        case 0: return 0.0f;
        case 1: return static_cast<float>(static_cast<int32_t>(r.below(9)) - 4);
        default: return (static_cast<float>(r.next() >> 8) / 16777216.0f - 0.5f) * (r.below(2) ? 8.0f : 2000.0f);
    }
}
bool same(float a, float b) { return game::same_float(a, b); }

}  // namespace

NFSU2_DIFF_TEST(zMisc1) {
    return game::run_diff(ctx, "zMisc1", o_Cubic1DGetVal, [](game::Rng& r) -> game::Case {
        const uint32_t which = r.below(16);
        switch (which) {
            case 0: {  // easter eggs on the live object (read only)
                void* eggs = reinterpret_cast<void*>(0x00865930);
                const int32_t type = static_cast<int32_t>(r.below(12)), id = static_cast<int32_t>(r.below(40));
                if (o_IsEggUnlocked(eggs, type, id) != IsEggUnlocked(eggs, type, id)) return game::MismatchText("IsEasterEggUnlocked");
                if (o_IsPresetUnlocked(eggs, id) != IsPresetUnlocked(eggs, id)) return game::MismatchText("IsPresetUnlocked");
                return o_AreContestVinylsUnlocked(eggs) == AreContestVinylsUnlocked(eggs) ? game::Ok() : game::MismatchText("AreContestVinylsUnlocked");
            }
            case 1: {  // synthetic egg table
                std::vector<uint8_t> tab(0x34 * 8);
                for (auto& b : tab) b = static_cast<uint8_t>(r.below(4));
                uint8_t obj[0x14] = {};
                at<uint8_t*>(obj, 0xC) = tab.data();
                at<uint32_t>(obj, 0x10) = r.below(9);
                const int32_t type = static_cast<int32_t>(r.below(4)), id = static_cast<int32_t>(r.below(4));
                for (int k = 0; k < 8; ++k) { at<int32_t>(tab.data(), k * 0x34 + 0x20) = static_cast<int32_t>(r.below(4)); at<int32_t>(tab.data(), k * 0x34 + 0x24) = static_cast<int32_t>(r.below(4)); }
                return o_IsEggUnlocked(obj, type, id) == IsEggUnlocked(obj, type, id) ? game::Ok() : game::MismatchText("IsEasterEggUnlocked synthetic");
            }
            case 2: {
                const int32_t port = static_cast<int32_t>(r.below(4));
                if (o_IsJoystickPluggedIn(port) != IsJoystickPluggedIn(port)) return game::MismatchText("IsJoystickPluggedIn");
                const int32_t id = static_cast<int32_t>(r.below(64)) - 2, p2 = static_cast<int32_t>(r.below(2));
                if (o_IsJoyHandlerAdded(p2, id) != IsJoyHandlerAdded(p2, id)) return game::MismatchText("IsJoyHandlerAdded");
                return o_IsPaused() == IsPaused() ? game::Ok() : game::MismatchText("IsPaused");
            }
            case 3: {
                std::string name = r.below(3) ? std::string(*reinterpret_cast<const char**>(0x007FABAC + r.below(0xDF) * 8)) : "NOT_AN_EVENT";
                if (r.below(4) == 0)
                    for (char& ch : name) ch = static_cast<char>(r.below(2) ? (ch | 0x20) : ch);
                return o_MapEventNameToJoyEvent(name.c_str()) == MapEventNameToJoyEvent(name.c_str()) ? game::Ok() : game::MismatchText("MapEventNameToJoyEvent " + name);
            }
            case 4: {
                uint8_t c[0x30];
                for (uint32_t k = 0; k < 0x30; k += 4) at<float>(c, k) = rf(r);
                const float t = rf(r);
                return same(o_Cubic1DGetVal(c, t), Cubic1DGetVal(c, t)) ? game::Ok() : game::MismatchText("tCubic1D::GetVal");
            }
            case 5: case 6: {
                uint8_t a[0x90], b[0x90];
                for (uint32_t k = 0; k < 0x90; k += 4) at<float>(a, k) = rf(r);
                for (int k = 0; k < 3; ++k) if (r.below(3) == 0) at<float>(a, k * 0x30) = at<float>(a, k * 0x30 + 8);
                std::memcpy(b, a, sizeof a);
                float v[3] = {rf(r), rf(r), rf(r)};
                if (r.below(3) == 0) v[1] = at<float>(a, 0x38);
                if (which == 5) o_Cubic3DSetVal(a, v), Cubic3DSetVal(b, v);
                else o_Cubic3DSet(a, fb(v[0]), fb(v[1]), fb(v[2])), Cubic3DSet(b, fb(v[0]), fb(v[1]), fb(v[2]));
                float oa[3], ob[3];
                o_Cubic3DGetdVal(a, oa);
                Cubic3DGetdVal(b, ob);
                return std::memcmp(a, b, sizeof a) == 0 && std::memcmp(oa, ob, 12) == 0 ? game::Ok() : game::MismatchText("tCubic3D");
            }
            case 7: {  // Timer: ticks up to 2^31 (fild exactness above 2^24)
                int32_t ta = static_cast<int32_t>(r.below(2) ? r.below(400000) : r.next() >> 1), tb = ta;
                const float step = r.below(4) == 0 ? 0.001f : static_cast<float>(1 + r.below(100)) / 100.0f;
                if (!same(o_TimerSecondsRounded(&ta, step), TimerSecondsRounded(&tb, step))) return game::MismatchText("GetSecondsRounded");
                o_TimerRoundTime(&ta, step);
                TimerRoundTime(&tb, step);
                return ta == tb ? game::Ok() : game::MismatchText("RoundTime " + std::to_string(ta) + " vs " + std::to_string(tb));
            }
            case 8: {  // Spline ctor + SetControlPoints (owned buffer freed through the CRT)
                uint8_t a[0x30], b[0x30];
                std::memset(a, 0x5A, sizeof a);
                std::memset(b, 0x5A, sizeof b);
                const uint32_t x = r.next(), y = r.next(), z = r.next();
                o_SplineCtor(a, x, y, z);
                SplineCtor(b, x, y, z);
                if (std::memcmp(a, b, sizeof a)) return game::MismatchText("Spline::Spline");
                if (r.below(2)) {
                    at<void*>(a, 0x10) = game_fn<void*(__cdecl*)(uint32_t)>(0x0075C6C8)(16);
                    at<void*>(b, 0x10) = game_fn<void*(__cdecl*)(uint32_t)>(0x0075C6C8)(16);
                }
                const uint32_t pts = r.next(), n = r.below(50);
                const uint32_t ea = o_SplineSetControlPoints(a, pts, n), eb = SplineSetControlPoints(b, pts, n);
                return std::memcmp(a, b, sizeof a) == 0 && ea == eb ? game::Ok() : game::MismatchText("SetControlPoints");
            }
            case 9: {
                std::vector<float> pts(4 * 20);
                for (float& f : pts) f = rf(r);
                uint8_t a[0x30] = {}, b[0x30] = {};
                at<float*>(a, 0xC) = pts.data();
                at<int32_t>(a, 4) = static_cast<int32_t>(r.below(21));
                at<uint32_t>(a, 0x14) = r.below(4) ? 1 : 0;
                at<float>(a, 0x20) = rf(r);
                std::memcpy(b, a, sizeof a);
                const float la = o_SplineLinearLength(a), lb = SplineLinearLength(b);
                return same(la, lb) && std::memcmp(a, b, sizeof a) == 0 ? game::Ok() : game::MismatchText("LinearEstimateLength");
            }
            case 10: {
                uint8_t a[0x24], b[0x24];
                for (auto& x : a) x = static_cast<uint8_t>(r.next());
                at<int32_t>(a, 4) = static_cast<int32_t>(r.below(4));
                std::memcpy(b, a, sizeof a);
                const uint32_t n = r.below(4) == 0 ? 0 : r.next();
                o_QuickSplineSetNum(a, n);
                QuickSplineSetNum(b, n);
                return std::memcmp(a, b, sizeof a) == 0 ? game::Ok() : game::MismatchText("SetNumControlPoints");
            }
            case 11: {  // cPoint: two tCubic1D halves; mode word +0x2C in 0..2
                uint8_t a[0x60], b[0x60];
                for (uint32_t k = 0; k < 0x60; k += 4) at<float>(a, k) = rf(r);
                for (int h = 0; h < 2; ++h) {
                    at<int16_t>(a, h * 0x30 + 0x2C) = static_cast<int16_t>(r.below(3));
                    at<int16_t>(a, h * 0x30 + 0x2E) = static_cast<int16_t>(r.below(2));
                }
                std::memcpy(b, a, sizeof a);
                const float t = std::abs(rf(r)) + 0.01f;
                o_PointSplineSeek(a, fb(t));
                PointSplineSeek(b, fb(t));
                for (uint32_t k = 0; k < 0x60; k += 4)
                    if (!same(at<float>(a, k), at<float>(b, k))) return game::MismatchText("cPoint::SplineSeek");
                return game::Ok();
            }
            case 12: {
                char a[64], b[64];
                std::memset(a, 0x5A, sizeof a);
                std::memset(b, 0x5A, sizeof b);
                const int32_t i = static_cast<int32_t>(r.below(5)) - 2;
                const uint32_t ea = o_GenerateFilename(a, i), eb = GenerateFilename(b, i);
                return std::memcmp(a, b, sizeof a) == 0 && ea == eb ? game::Ok() : game::MismatchText("GenerateFilename");
            }
            default: {  // ReplaySnapshot movers, recording / playing back / idle
                std::vector<uint8_t> a(0x1E8 + 256), b;
                for (auto& x : a) x = static_cast<uint8_t>(r.next());
                at<uint8_t>(a.data(), 0x40) = static_cast<uint8_t>(r.below(2));
                at<uint8_t>(a.data(), 0x41) = static_cast<uint8_t>(r.below(2));
                at<int32_t>(a.data(), 0x1E4) = static_cast<int32_t>(r.below(256));
                at<int32_t>(a.data(), 0x1DC) = static_cast<int32_t>(r.below(256));
                if (!at<uint8_t>(a.data(), 0x40)) return game::Ok();  // playback reads through MoveDataBuffer (untested here)
                b = a;
                if (which == 13) {
                    uint8_t sa[2] = {static_cast<uint8_t>(r.next()), static_cast<uint8_t>(r.next())}, sb[2] = {sa[0], sa[1]};
                    o_MoveData16(a.data(), sa);
                    MoveData16(b.data(), sb);
                    return a == b ? game::Ok() : game::MismatchText("MoveData16");
                }
                float va = rf(r), vb = va;
                const float range = static_cast<float>(1 + r.below(2000)) / 10.0f;
                o_MoveDataCompress16(a.data(), &va, range);
                MoveDataCompress16(b.data(), &vb, range);
                return a == b && same(va, vb) ? game::Ok() : game::MismatchText("MoveDataCompress16");
            }
        }
    });
}

NFSU2_DIFF_TEST(regs_zMisc1) {  // the ones whose EAX callers read
    using game::arg;
    return game::run_diff(ctx, "regs_zMisc1", o_IsJoyHandlerAdded, [](game::Rng& r) -> game::Case {
        void* eggs = reinterpret_cast<void*>(0x00865930);
        const uint32_t port = r.below(4), id = r.below(60);
        const char* name = *reinterpret_cast<const char**>(0x007FABAC + r.below(0xDF) * 8);
        for (auto c : {game::check_regs("IsJoystickPluggedIn", (void*)o_IsJoystickPluggedIn, NFSU2_ENTRY(IsJoystickPluggedIn), {port}, false),
                       game::check_regs("IsJoyHandlerAdded", (void*)o_IsJoyHandlerAdded, NFSU2_ENTRY(IsJoyHandlerAdded), {port & 1, id}, false),
                       game::check_regs("IsPaused", (void*)o_IsPaused, NFSU2_ENTRY(IsPaused), {}, false),
                       game::check_regs("MapEventNameToJoyEvent", (void*)o_MapEventNameToJoyEvent, NFSU2_ENTRY(MapEventNameToJoyEvent), {arg(name)}, false),
                       game::check_regs_this("IsEasterEggUnlocked", (void*)o_IsEggUnlocked, NFSU2_ENTRY(IsEggUnlocked), eggs, eggs, {r.below(12), id}),
                       game::check_regs_this("AreContestVinylsUnlocked", (void*)o_AreContestVinylsUnlocked, NFSU2_ENTRY(AreContestVinylsUnlocked), eggs, eggs, {})})
            if (!c.ok) return c;
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(zMisc1_BROKEN) {  // harness self-check: GetVal with a coefficient order mix-up
    return game::run_diff(ctx, "zMisc1_BROKEN", o_Cubic1DGetVal, [](game::Rng& r) -> game::Case {
        uint8_t c[0x30];
        for (uint32_t k = 0; k < 0x30; k += 4) at<float>(c, k) = rf(r);
        const float t = rf(r);
        const float bad = ((t * at<float>(c, 0x14) + at<float>(c, 0x10)) * t + at<float>(c, 0x18)) * t + at<float>(c, 0x1C);
        return same(o_Cubic1DGetVal(c, t), bad) ? game::Ok() : game::MismatchText("coefficients");
    });
}
