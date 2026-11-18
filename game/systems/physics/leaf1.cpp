// zPhysics leaf batch 1 (16), written from the PC machine code: CarState wheel contact/terrain, Collision queries,
// drive-train shift RPM / clutch, Mellovator state, rigid-body moment, wheel ground force,
// bRotateVector, GJK wafer point, static-body test velocities. One table-driven leaf test (game/framework/leaf.h).
#include "leaf.h"
#include "live.h"
#include "x87.h"

namespace {

// 0x00592040 CarState::SetWheelTouchingGround(int wheel, bool on): byte flag +0x350[wheel]; the touching count
// +0x3E4 goes up on off->on and down on on->off. EAX = wheel.
uint32_t __cdecl SetWheelTouchingGround(uint8_t* s, uint32_t wheel, uint32_t on) {
    const uint8_t old = s[0x350 + wheel];
    const uint8_t now = static_cast<uint8_t>(on);
    s[0x350 + wheel] = now;
    if (old == 0) {
        if (now != 0) ++at<uint32_t>(s, 0x3E4);
    } else if (now == 0) {
        --at<uint32_t>(s, 0x3E4);
    }
    return wheel;
}

// 0x00592080 CarState::SetWheelTerrain(int wheel, int terrain): +0x354[wheel] = terrain; +0x364[wheel] keeps the
// (signed) maximum. EAX = terrain.
uint32_t __cdecl SetWheelTerrain(uint8_t* s, uint32_t wheel, uint32_t t) {
    at<uint32_t>(s, 0x354 + wheel * 4) = t;
    if (static_cast<int32_t>(t) > at<int32_t>(s, 0x364 + wheel * 4)) at<uint32_t>(s, 0x364 + wheel * 4) = t;
    return t;
}

// 0x00592BD0 Collision::Update(float): still fresh when (now 0x86518C - stamp +0x0C) * 0x784268 < 0x7FBEE0
// (int converted exactly by fild, one rounding in fmul)
uint32_t __cdecl CollisionUpdate(uint8_t* s, uint32_t) {
    const int32_t dt = static_cast<int32_t>(gu(0x0086518Cu) - at<uint32_t>(s, 0x0C));
    const x87::X t = x87::mul(x87::ild(dt), x87::ld(gf(0x00784268u)));
    return x87::less(x87::cmp(t, x87::ld(gf(0x007FBEE0u)))) ? 1u : 0u;
}

// 0x00592C60 Collision::IsTwoCarCollision: both bodies ([+0x64]+0x238 / +0x23C) have type byte +0x11 == 0
uint32_t __cdecl IsTwoCarCollision(uint8_t* s) {
    uint8_t* c = ptr(s, 0x64);
    if (at<uint8_t>(ptr(c, 0x238), 0x11) != 0) return 0;
    return at<uint8_t>(ptr(c, 0x23C), 0x11) != 0 ? 0u : 1u;
}

// 0x00592C90 Collision::GetCollidingCars(Car** a, Car** b): each body of type 0 hands out its car (+0x130).
// EAX = the second body.
uint32_t __cdecl GetCollidingCars(uint8_t* s, uint32_t pa, uint32_t pb) {
    uint32_t* a = reinterpret_cast<uint32_t*>(pa);
    uint32_t* b = reinterpret_cast<uint32_t*>(pb);
    *a = 0;
    *b = 0;
    uint8_t* x = ptr(ptr(s, 0x64), 0x238);
    if (x[0x11] == 0) *a = at<uint32_t>(x, 0x130);
    uint8_t* y = ptr(ptr(s, 0x64), 0x23C);
    if (y[0x11] == 0) *b = at<uint32_t>(y, 0x130);
    return u(y);
}

// 0x00592D50 Collision::IsSmokeableInvolved: either body has type 1
uint32_t __cdecl IsSmokeableInvolved(uint8_t* s) {
    uint8_t* c = ptr(s, 0x64);
    if (at<uint8_t>(ptr(c, 0x238), 0x11) == 1) return 1;
    return at<uint8_t>(ptr(c, 0x23C), 0x11) == 1 ? 1u : 0u;
}

// 0x00592D80 Collision::GetSmokeable: the first body of type 1, else 0
uint32_t __cdecl GetSmokeable(uint8_t* s) {
    uint8_t* c = ptr(s, 0x64);
    uint8_t* x = ptr(c, 0x238);
    if (x[0x11] == 1) return u(x);
    uint8_t* y = ptr(c, 0x23C);
    return y[0x11] == 1 ? u(y) : 0u;
}

// 0x00592ED0 IsYawControlInducingCollisionBody(body): type (signed byte +0x11) 0..2 -> 1; 3 -> bit 1 of the signed
// byte [[body+0x60]+0x0A]; else 0
uint32_t __cdecl IsYawControlInducing(uint32_t body) {
    uint8_t* b = reinterpret_cast<uint8_t*>(body);
    const int32_t t = static_cast<int8_t>(b[0x11]);
    if (t < 0) return 0;
    if (t <= 2) return 1;
    if (t != 3) return 0;
    const int32_t f = static_cast<int8_t>(at<uint8_t>(ptr(b, 0x60), 0x0A));
    return (static_cast<uint32_t>(f) & 2u) >> 1;
}

// 0x005946E0 DriveTrain::GetShiftDownRPM(int gear, bool b): (b ? +0xD4 : +0x7C) + table +0xA8[gear]
float __cdecl GetShiftDownRPM(uint8_t* s, uint32_t gear, uint32_t b) {
    const float base = static_cast<uint8_t>(b) ? at<float>(s, 0xD4) : at<float>(s, 0x7C);
    return x87::st(x87::add(x87::ld(base), x87::ld(at<float>(s, 0xA8 + gear * 4))));
}

// 0x00594ED0 DriveTrain::ClutchMatched: |engine rpm [+0x20]+0x14 - clutch rpm +0x4C| <= 0x7FC028 (NaN: no)
uint32_t __cdecl ClutchMatched(uint8_t* s) {
    const x87::X d = x87::sub(x87::ld(at<float>(ptr(s, 0x20), 0x14)), x87::ld(at<float>(s, 0x4C)));
    x87::X a = d;  // fabs: clear the sign bit of the unrounded difference
    a.b[9] &= 0x7F;
    const uint8_t ah = x87::cmp(a, x87::ld(gf(0x007FC028u)));
    return x87::parity_even(ah & 0x41) ? 0u : 1u;
}

// 0x00598A70 Mellovator::SetState(a, b, c, d): +0x20..+0x2C = a..d; clears +0x30..+0x3C and +0xB4..+0xC0.
// EAX = this + 0x40 (the loop pointer).
uint32_t __cdecl MellovatorSetState(uint8_t* s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    at<uint32_t>(s, 0x20) = a;
    at<uint32_t>(s, 0x24) = b;
    at<uint32_t>(s, 0x28) = c;
    at<uint32_t>(s, 0x2C) = d;
    for (uint32_t i = 0; i < 4; ++i) {
        at<uint32_t>(s, 0x30 + i * 4 + 0x84) = 0;
        at<uint32_t>(s, 0x30 + i * 4) = 0;
    }
    return u(s) + 0x40;
}

// 0x0059BC20 RigidBody::ApplyMoment(const bVector3* m): torque +0x120..+0x128 += m. EAX = m.
uint32_t __cdecl ApplyMoment(uint8_t* s, uint32_t mp) {
    const float* m = reinterpret_cast<const float*>(mp);
    const float y = m[1], z = m[2];
    at<float>(s, 0x120) = x87::st(x87::add(x87::ld(at<float>(s, 0x120)), x87::ld(m[0])));
    at<float>(s, 0x124) = x87::st(x87::add(x87::ld(y), x87::ld(at<float>(s, 0x124))));
    at<float>(s, 0x128) = x87::st(x87::add(x87::ld(z), x87::ld(at<float>(s, 0x128))));
    return mp;
}

// 0x0059DAF0 Wheel::GetHorizontalGroundForceInCarFrame(bVector3* out): out = rows (+0xB0/+0xC0, +0xB4/+0xC4,
// +0xB8/+0xC8) dotted with the force (+0x124, +0x128). EAX = out.
uint32_t __cdecl GroundForceInCarFrame(uint8_t* s, uint32_t op) {
    using namespace x87;
    const X fy = ld(at<float>(s, 0x124)), fz = ld(at<float>(s, 0x128));
    const X x = add(mul(ld(at<float>(s, 0xC4)), fz), mul(ld(at<float>(s, 0xB4)), fy));
    const X y = add(mul(ld(at<float>(s, 0xC8)), fz), mul(ld(at<float>(s, 0xB8)), fy));
    const X z = add(mul(ld(at<float>(s, 0xC0)), fz), mul(fy, ld(at<float>(s, 0xB0))));
    float* out = reinterpret_cast<float*>(op);
    out[0] = st(z);
    out[1] = st(x);
    out[2] = st(y);
    return op;
}

// 0x0059DB50 bRotateVector(bVector3* out, const bMatrix4* m, const bVector3* v): out = v * m (3x3 part), each
// component stored before the next is computed. EAX = v.
uint32_t __cdecl RotateVector(uint32_t op, uint32_t mp, uint32_t vp) {
    using namespace x87;
    const float* m = reinterpret_cast<const float*>(mp);
    const float* v = reinterpret_cast<const float*>(vp);
    float* out = reinterpret_cast<float*>(op);
    out[0] = st(add(add(mul(ld(m[8]), ld(v[2])), mul(ld(m[4]), ld(v[1]))), mul(ld(m[0]), ld(v[0]))));
    out[1] = st(add(add(mul(ld(m[9]), ld(v[2])), mul(ld(m[1]), ld(v[0]))), mul(ld(m[5]), ld(v[1]))));
    out[2] = st(add(add(mul(ld(m[10]), ld(v[2])), mul(ld(m[2]), ld(v[0]))), mul(ld(m[6]), ld(v[1]))));
    return vp;
}

// 0x005A2010 GJKWafer::GetPoint(int i, Point* out): point i (48 bytes at +0x20): words +8..+0xE, dword +0x10,
// floats +0x14/+0x18 (through the x87). EAX = out.
uint32_t __cdecl WaferGetPoint(uint8_t* s, uint32_t i, uint32_t op) {
    uint8_t* p = s + i * 48 + 0x20;
    uint8_t* out = reinterpret_cast<uint8_t*>(op);
    for (uint32_t k = 8; k < 0x10; k += 2) at<uint16_t>(out, k) = at<uint16_t>(p, k);
    const float a = at<float>(p, 0x14), b = at<float>(p, 0x18);
    at<uint32_t>(out, 0x10) = at<uint32_t>(p, 0x10);
    at<float>(out, 0x14) = x87::st(x87::ld(a));
    at<float>(out, 0x18) = x87::st(x87::ld(b));
    return op;
}

// 0x005A9FF0 StaticCollisionBody::GetTestImpulseVelocities(bVector3* lin, bVector3* ang, ...): both = the static
// zero vector (0x86B390). EAX = ang.
uint32_t __cdecl TestImpulseVelocities(uint8_t*, uint32_t lp, uint32_t ap, uint32_t, uint32_t) {
    for (uint32_t p : {lp, ap}) {
        float* o = reinterpret_cast<float*>(p);
        const float x = gf(0x0086B390u), y = gf(0x0086B394u), z = gf(0x0086B398u);
        o[0] = x87::st(x87::ld(x));
        o[1] = x87::st(x87::ld(y));
        o[2] = x87::st(x87::ld(z));
    }
    return ap;
}

}  // namespace

LEAF_THIS(0x00592040, SetWheelTouchingGround, 2, "void CarState::SetWheelTouchingGround(int, bool)");
LEAF_THIS(0x00592080, SetWheelTerrain, 2, "void CarState::SetWheelTerrain(int, int)");
LEAF_THIS(0x00592BD0, CollisionUpdate, 1, "bool Collision::Update(float)");
LEAF_THIS(0x00592C60, IsTwoCarCollision, 0, "bool Collision::IsTwoCarCollision()");
LEAF_THIS(0x00592C90, GetCollidingCars, 2, "void Collision::GetCollidingCars(Car**, Car**)");
LEAF_THIS(0x00592D50, IsSmokeableInvolved, 0, "bool Collision::IsSmokeableInvolved()");
LEAF_THIS(0x00592D80, GetSmokeable, 0, "CollisionBody* Collision::GetSmokeable()");
LEAF_CDECL(0x00592ED0, IsYawControlInducing, 1, "bool IsYawControlInducingCollisionBody(CollisionBody*)");
LEAF_THIS(0x005946E0, GetShiftDownRPM, 2, "float DriveTrain::GetShiftDownRPM(int gear, bool)");
LEAF_THIS(0x00594ED0, ClutchMatched, 0, "bool DriveTrain::ClutchMatched()");
LEAF_THIS(0x00598A70, MellovatorSetState, 4, "void Mellovator::SetState(int, int, int, int)");
LEAF_THIS(0x0059BC20, ApplyMoment, 1, "void RigidBody::ApplyMoment(const bVector3*)");
LEAF_THIS(0x0059DAF0, GroundForceInCarFrame, 1, "void Wheel::GetHorizontalGroundForceInCarFrame(bVector3*)");
LEAF_CDECL(0x0059DB50, RotateVector, 3, "void bRotateVector(bVector3*, const bMatrix4*, const bVector3*)");
LEAF_THIS(0x005A2010, WaferGetPoint, 2, "void GJKWafer::GetPoint(int, Point*)");
LEAF_THIS(0x005A9FF0, TestImpulseVelocities, 4, "void StaticCollisionBody::GetTestImpulseVelocities(bVector3*, bVector3*, ...)");

NFSU2_DIFF_TEST(physics_leaf1) {
    // 0 this, 1 collision info (+0x64), 2/3 the two bodies, 4/5 their +0x60 data, 6 engine (+0x20), 7-9 buffers
    static const Spec spec{10, 0x800,
                           {{0, 0x64, 1}, {1, 0x238, 2}, {1, 0x23C, 3}, {2, 0x60, 4}, {3, 0x60, 5}, {0, 0x20, 6}},
                           {{2, 0x11, 1, 6}, {3, 0x11, 1, 6}}};
    static const std::vector<Fn> fns{
        LEAF_FN(SetWheelTouchingGround, THIS, INT, {'i', 4}, {'i', 3}),
        LEAF_FN(SetWheelTerrain, THIS, INT, {'i', 4}, {'r', 0}),
        LEAF_FN(CollisionUpdate, THIS, INT, {'r', 0}),
        LEAF_FN(IsTwoCarCollision, THIS, INT),
        LEAF_FN(GetCollidingCars, THIS, PTR, {'p', 7}, {'p', 8}),
        LEAF_FN(IsSmokeableInvolved, THIS, INT),
        LEAF_FN(GetSmokeable, THIS, PTR),
        LEAF_FN(IsYawControlInducing, CDECL, INT, {'p', 2}),
        LEAF_FN(GetShiftDownRPM, THIS, FLT, {'i', 6}, {'i', 3}),
        LEAF_FN(ClutchMatched, THIS, INT),
        LEAF_FN(MellovatorSetState, THIS, PTR, {'r', 0}, {'r', 0}, {'r', 0}, {'r', 0}),
        LEAF_FN(ApplyMoment, THIS, PTR, {'p', 7}),
        LEAF_FN(GroundForceInCarFrame, THIS, PTR, {'p', 7}),
        LEAF_FN(RotateVector, CDECL, PTR, {'p', 7}, {'p', 8}, {'p', 9}),
        LEAF_FN(WaferGetPoint, THIS, PTR, {'i', 8}, {'p', 7}),
        LEAF_FN(TestImpulseVelocities, THIS, PTR, {'p', 7}, {'p', 8}, {'r', 0}, {'r', 0}),
    };
    return run(ctx, "physics_leaf1", spec, fns);
}

NFSU2_DIFF_TEST(physics_leaf1_BROKEN) {  // harness self-check: SetWheelTouchingGround compared with SetWheelTerrain
    static const Spec spec{1, 0x800, {}, {}};
    static const std::vector<Fn> fns{Fn("Touching_vs_Terrain", &o_SetWheelTerrain, NFSU2_ENTRY(SetWheelTouchingGround),
                                        THIS, INT, {'i', 4}, {'i', 3})};
    return run(ctx, "physics_leaf1_BROKEN", spec, fns);
}

// Real inputs recorded during play (tools/game/capture.py ADDR ...), replayed original vs rewrite. Without a capture
// file a test runs 0 cases.
NFSU2_DIFF_TEST(physics_leaf1_captured) {
    static const std::vector<std::pair<uintptr_t, Fn>> fns{
        {0x00592040, LEAF_FN(SetWheelTouchingGround, THIS, INT)},
        {0x00592080, LEAF_FN(SetWheelTerrain, THIS, INT)},
        {0x005946E0, LEAF_FN(GetShiftDownRPM, THIS, FLT)},
        {0x00594ED0, LEAF_FN(ClutchMatched, THIS, INT)},
    };
    game::DiffResult all;
    all.name = "physics_leaf1_captured";
    for (const auto& [addr, f] : fns) {
        const game::DiffResult r = run_captured(ctx, "physics_leaf1_captured", addr, f);
        all.iterations += r.iterations;
        all.mismatches += r.mismatches;
        for (const auto& e : r.examples)
            if (all.examples.size() < 5) all.examples.push_back(e);
        if (!r.error.empty()) all.error = r.error;
    }
    return all;
}

// Live check specs (game/framework/live.h): `this` bytes, then bytes behind each pointer argument (0 = a value).
LEAF_LIVE(0x00592040, SetWheelTouchingGround, THIS, INT, 0x400, 0, 0);
LEAF_LIVE(0x00592080, SetWheelTerrain, THIS, INT, 0x400, 0, 0);
LEAF_LIVE(0x00592BD0, CollisionUpdate, THIS, INT, 0x20, 0);
LEAF_LIVE(0x00592C60, IsTwoCarCollision, THIS, INT, 0x80);
LEAF_LIVE(0x00592C90, GetCollidingCars, THIS, PTR, 0x80, 4, 4);
LEAF_LIVE(0x00592D50, IsSmokeableInvolved, THIS, INT, 0x80);
LEAF_LIVE(0x00592D80, GetSmokeable, THIS, PTR, 0x80);
LEAF_LIVE(0x00592ED0, IsYawControlInducing, CDECL, INT, 0, 0x64);
LEAF_LIVE(0x005946E0, GetShiftDownRPM, THIS, FLT, 0x100, 0, 0);
LEAF_LIVE(0x00594ED0, ClutchMatched, THIS, INT, 0x100);
LEAF_LIVE(0x00598A70, MellovatorSetState, THIS, PTR, 0xD0, 0, 0, 0, 0);
LEAF_LIVE(0x0059BC20, ApplyMoment, THIS, PTR, 0x140, 12);
LEAF_LIVE(0x0059DAF0, GroundForceInCarFrame, THIS, PTR, 0x140, 12);
LEAF_LIVE(0x0059DB50, RotateVector, CDECL, PTR, 0, 12, 0x40, 12);
LEAF_LIVE(0x005A2010, WaferGetPoint, THIS, PTR, 0x400, 0, 0x1C);
LEAF_LIVE(0x005A9FF0, TestImpulseVelocities, THIS, PTR, 0, 12, 12, 0, 0);
