// Player car telemetry. Pointer chain and offsets: see docs/structs/car.md (first found in the user's Camera
// Toolkit fork notes, each field re-verified live on 2026-11-14).
//   CarState* = *(0x0089CCF8); assembly = CarState+0x34
//   assembly+0x04 vehicle (vtable 0x00784FC8): +0x1E4 gear (0 R?, 1 N, 2 = 1st, ...)
//   assembly+0x20 RigidBody (vtable 0x007A20C4, tag 'Rigi'): +0x20 pos, +0x30/+0x40/+0x50 forward/up/side rows,
//                 +0x70 linear velocity, +0x80 angular velocity
//   assembly+0x48 Engine: +0x14 engine speed in rad/s (idle 83.8 = 800 rpm)
#include "probe.h"

#include <cmath>
#include <stdexcept>

namespace probe {
namespace {

constexpr uintptr_t kPlayerCarState = 0x0089CCF8;
constexpr uintptr_t kGameState = 0x008654A4;
constexpr uintptr_t kDeltaTime = 0x00865198;
constexpr uint32_t kRigidBodyVtbl = 0x007A20C4;
constexpr uint32_t kVehicleVtbl = 0x00784FC8;

template <class T> bool rd(uintptr_t a, T& out) { return mem_read(a, &out, sizeof out); }

}  // namespace

bool car_read(CarSample& s) {
    uint32_t cs = 0, asm_ = 0, rigid = 0, veh = 0, eng = 0, vt = 0;
    if (!rd(kPlayerCarState, cs) || !cs) return false;
    if (!rd(cs + 0x34, asm_) || !asm_) return false;
    if (!rd(asm_ + 0x20, rigid) || !rd(rigid, vt) || vt != kRigidBodyVtbl) return false;
    if (!rd(asm_ + 0x04, veh) || !rd(veh, vt) || vt != kVehicleVtbl) return false;
    if (!rd(asm_ + 0x48, eng) || !eng) return false;
    float m[12];
    if (!rd(rigid + 0x20, s.pos) || !mem_read(rigid + 0x30, m, sizeof m) || !rd(rigid + 0x70, s.vel) ||
        !rd(rigid + 0x80, s.angvel) || !rd(veh + 0x1E4, s.gear) || !rd(eng + 0x14, s.engine_rad_s))
        return false;
    for (int i = 0; i < 3; ++i) {
        s.forward[i] = m[i];
        s.up[i] = m[4 + i];
        s.side[i] = m[8 + i];
    }
    rd(kGameState, s.game_state);
    rd(kDeltaTime, s.dt);
    return true;
}

json car_sample_json(const CarSample& s) {
    auto v3 = [](const float* v) { return json::array({v[0], v[1], v[2]}); };
    const float speed = std::sqrt(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1] + s.vel[2] * s.vel[2]);
    return {{"frame", s.frame},
            {"game_state", s.game_state},
            {"dt", s.dt},
            {"pos", v3(s.pos)},
            {"forward", v3(s.forward)},
            {"up", v3(s.up)},
            {"side", v3(s.side)},
            {"vel", v3(s.vel)},
            {"angvel", v3(s.angvel)},
            {"speed_ms", speed},
            {"speed_kmh", speed * 3.6f},
            {"gear", s.gear},
            {"rpm", s.engine_rad_s * 60.0f / 6.28318531f}};
}

json car_state() {
    CarSample s;
    s.frame = current_frame();
    if (!car_read(s)) throw std::runtime_error("player car not available (not in game, or pointer chain invalid)");
    return car_sample_json(s);
}

}  // namespace probe
