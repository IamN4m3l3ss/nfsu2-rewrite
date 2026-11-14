// Frame time.
//
// 0x0057EAD0 UpdateFrameTime: void __cdecl(float real_dt). Only caller 0x005812ED passes the real elapsed time
// (QueryPerformanceCounter delta, 0x0043BE20, scaled). Writes a frame time to 0x00865198, which reads exactly 1/60
// every frame at the WidescreenFix 60 fps limit.
// NOTE (2026-11-14): halving 0x00865198 here (measured 0.008333) did NOT visibly slow the game for the user while
// driving, so it is not the simulation's step; what reads it is unknown. Don't call it "the" delta time.
//
// Status: M6 hot-reload demonstrator — calls the original, then applies kTimeScale (1.0 = no change).
#include "../regs.h"
#include "../rewrite.h"

namespace {

constexpr uintptr_t kDeltaTime = 0x00865198;
constexpr float kTimeScale = 1.0f;

using UpdateFrameTime_t = void(__cdecl*)(float);
UpdateFrameTime_t o_UpdateFrameTime = nullptr;

void __cdecl UpdateFrameTime(float real_dt) {
    o_UpdateFrameTime(real_dt);
    game_var<float>(kDeltaTime) *= kTimeScale;
}

}  // namespace

NFSU2_REWRITE_N(0x0057EAD0, UpdateFrameTime, 1, "void __cdecl(float real_dt)", o_UpdateFrameTime);
