// zPhysics batch 1 (45 small accessors), written from the PC machine code: rigid-body / collision-body getters and
// setters, constant returns, state copies, terrain lookups. Layouts as the instructions use them:
//   RigidBody-ish: +0x68 body (position +0x20, rotation +0x30, velocity +0xA0, acceleration +0xB0, angular +0xC0,
//   centre of gravity +0x1F0, inertia flag +0x158, tensors +0x170 / +0x1B0, mass inverse +0x168);
//   +0x148 / +0x60 surface (terrain type +0xD0, bounciness +0xCC, friction +0xE0, type byte +0xB).
#include <cstring>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"
#include "x87.h"

namespace {

template <class T> T& at(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }
inline uint8_t* ptr(void* p, uint32_t off) { return at<uint8_t*>(p, off); }
inline float gf(uintptr_t a) { return *reinterpret_cast<const float*>(a); }

using V_t = uint32_t(__thiscall*)(void*);
using V1_t = uint32_t(__thiscall*)(void*, uint32_t);
using V2_t = uint32_t(__thiscall*)(void*, uint32_t, uint32_t);
using F_t = float(__thiscall*)(void*);
using F1_t = float(__thiscall*)(void*, uint32_t);
using F2_t = float(__thiscall*)(void*, uint32_t, uint32_t);

// ---- field / pointer getters
V_t o_GetDampState5C = nullptr;   uint32_t __cdecl GetDampState5C(void* s) { return at<uint32_t>(s, 0x5C); }
V_t o_GetDampState60 = nullptr;   uint32_t __cdecl GetDampState60(void* s) { return at<uint32_t>(s, 0x60); }
F_t o_GetMaxBackupTime6C = nullptr; float __cdecl GetMaxBackupTime6C(void* s) { return at<float>(s, 0x6C); }
V_t o_GetState1C = nullptr;       uint32_t __cdecl GetState1C(void* s) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s)) + 0x1C; }
V_t o_GetState24 = nullptr;       uint32_t __cdecl GetState24(void* s) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s)) + 0x24; }
V_t o_LastForce = nullptr;        uint32_t __cdecl LastForce(void* s) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s)) + 0x100; }
V_t o_LastTorque = nullptr;       uint32_t __cdecl LastTorque(void* s) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s)) + 0x110; }
#define BODY_GETTER(fn, off) \
    V_t o_##fn = nullptr;    \
    uint32_t __cdecl fn(void* s) { return at<uint32_t>(s, 0x68) + (off); }
BODY_GETTER(BodyPosition, 0x20)
BODY_GETTER(BodyRotation, 0x30)
BODY_GETTER(BodyVelocity, 0xA0)
BODY_GETTER(BodyAcceleration, 0xB0)
BODY_GETTER(BodyAngularVelocity, 0xC0)
BODY_GETTER(BodyCenterOfGravity, 0x1F0)
#undef BODY_GETTER
V_t o_IsBlinking = nullptr;       uint32_t __cdecl IsBlinking(void* s) { return static_cast<uint32_t>(static_cast<int32_t>(at<int8_t>(s, 0x507))); }
V_t o_TerrainType60b = nullptr;   uint32_t __cdecl TerrainType60b(void* s) { return static_cast<uint32_t>(static_cast<int32_t>(at<int8_t>(ptr(s, 0x60), 0xB))); }
V_t o_TerrainType60 = nullptr;    uint32_t __cdecl TerrainType60(void* s) { return at<uint32_t>(ptr(s, 0x60), 0xD0); }
V_t o_TerrainType148 = nullptr;   uint32_t __cdecl TerrainType148(void* s) { return at<uint32_t>(ptr(s, 0x148), 0xD0); }
F_t o_BodyMassInverse = nullptr;  float __cdecl BodyMassInverse(void* s) { return at<float>(ptr(s, 0x68), 0x168); }
F1_t o_Bounciness = nullptr;      float __cdecl Bounciness(void* s, uint32_t) { return at<float>(ptr(s, 0x148), 0xCC); }
F1_t o_Friction = nullptr;        float __cdecl Friction(void* s, uint32_t) { return at<float>(ptr(s, 0x148), 0xE0); }
// 0x005A9D90: the body's inverse inertia tensor: +0x1B0 when +0x158 is set, else +0x170
V_t o_InertiaTensorInverse = nullptr;
uint32_t __cdecl InertiaTensorInverse(void* s) {
    uint8_t* b = ptr(s, 0x68);
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b)) + (at<uint32_t>(b, 0x158) ? 0x1B0u : 0x170u);
}
// ---- constants
V_t o_StaticRotation = nullptr;   uint32_t __cdecl StaticRotation(void*) { return 0x0086B520; }
V_t o_StaticInertia = nullptr;    uint32_t __cdecl StaticInertia(void*) { return 0x0086B450; }
V_t o_PhysicsExtrema = nullptr;   uint32_t __cdecl PhysicsExtrema(void*) { return 0x0086B3D0; }
V_t o_StateSize = nullptr;        uint32_t __cdecl StateSize(void*) { return 0x20; }
V_t o_ZeroIndices = nullptr;      uint32_t __cdecl ZeroIndices(void*) { return 0x007FC694; }
F_t o_MaxBackupTimeConst = nullptr; float __cdecl MaxBackupTimeConst(void*) { return gf(0x00784550); }
F_t o_MassInverseZero = nullptr;  float __cdecl MassInverseZero(void*) { return gf(0x007D7D3C); }
F2_t o_ConsiderImpulse = nullptr; float __cdecl ConsiderImpulse(void*, uint32_t, uint32_t) { return gf(0x00784250); }
F2_t o_ChangeInEnergy = nullptr;  float __cdecl ChangeInEnergy(void*, uint32_t, uint32_t) { return gf(0x007D7D3C); }
// ---- counters / flags
V1_t o_BeginCollision = nullptr;  uint32_t __cdecl BeginCollision(void* s, uint32_t) { ++at<uint8_t>(s, 0x18); return 0; }
V1_t o_EndCollision = nullptr;    uint32_t __cdecl EndCollision(void* s, uint32_t) { --at<uint8_t>(s, 0x18); return 0; }
V1_t o_AreCollisionsEnabledB = nullptr; uint32_t __cdecl AreCollisionsEnabledB(void* s, uint32_t) { return at<uint8_t>(s, 0x17) == 0 ? 1u : 0u; }
// 0x005B1C80: +0x150 == 0 (ordered) -> 1
V_t o_IsMoveable = nullptr;       uint32_t __cdecl IsMoveable(void* s) { return at<float>(s, 0x150) == 0.0f ? 1u : 0u; }
// ---- tables
// 0x00599600 GetNormalizedLevel(i) = table[i] / 3 (times the float 0.3333)
F1_t o_NormalizedLevel = nullptr;
float __cdecl NormalizedLevel(void* s, uint32_t i) {  // x87 product (extended exponent: denormal inputs round once)
    return x87::st(x87::mul(x87::ld(at<float>(s, i * 4)), x87::ld(gf(0x00786348))));
}
// 0x005995E0 GetUpgradeLevel(i) = ftol(table[i] + 0.5)
V1_t o_UpgradeLevel = nullptr;
uint32_t __cdecl UpgradeLevel(void* s, uint32_t i) {
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(at<float>(s, i * 4) + gf(0x00784260))));
}
// ---- state copies (EAX = what the original leaves)
V1_t o_SetState14 = nullptr;      uint32_t __cdecl SetState14(void* s, uint32_t p) { at<uint32_t>(s, 0x14) = *reinterpret_cast<uint32_t*>(p); return p; }
V1_t o_SetState14_58 = nullptr;
uint32_t __cdecl SetState14_58(void* s, uint32_t p) {
    at<uint32_t>(s, 0x14) = *reinterpret_cast<uint32_t*>(p);
    at<uint32_t>(s, 0x58) = 0;
    return p;
}
#define PAIR_GET(fn, off)                                                  \
    V1_t o_##fn = nullptr;                                                 \
    uint32_t __cdecl fn(void* s, uint32_t out) {                           \
        reinterpret_cast<uint32_t*>(out)[0] = at<uint32_t>(s, off);        \
        reinterpret_cast<uint32_t*>(out)[1] = at<uint32_t>(s, (off) + 4);  \
        return out;                                                        \
    }
#define PAIR_SET(fn, off)                                                  \
    V1_t o_##fn = nullptr;                                                 \
    uint32_t __cdecl fn(void* s, uint32_t in) {                            \
        at<uint32_t>(s, off) = reinterpret_cast<uint32_t*>(in)[0];         \
        at<uint32_t>(s, (off) + 4) = reinterpret_cast<uint32_t*>(in)[1];   \
        return reinterpret_cast<uint32_t*>(in)[1];                         \
    }
PAIR_GET(GetState1Cc, 0x1C)
PAIR_SET(SetState1C, 0x1C)
PAIR_GET(GetState24c, 0x24)
PAIR_SET(SetState24, 0x24)
#undef PAIR_GET
#undef PAIR_SET
V1_t o_GetState20x4 = nullptr;
uint32_t __cdecl GetState20x4(void* s, uint32_t out) {
    std::memcpy(reinterpret_cast<void*>(out), static_cast<uint8_t*>(s) + 0x20, 16);
    return out;
}
V1_t o_SetState20x4 = nullptr;
uint32_t __cdecl SetState20x4(void* s, uint32_t in) {
    std::memcpy(static_cast<uint8_t*>(s) + 0x20, reinterpret_cast<void*>(in), 16);
    return reinterpret_cast<uint32_t*>(in)[3];
}
V1_t o_ZeroDerivative = nullptr;
uint32_t __cdecl ZeroDerivative(void*, uint32_t out) {
    reinterpret_cast<uint32_t*>(out)[0] = 0;
    reinterpret_cast<uint32_t*>(out)[1] = 0;
    return out;
}

}  // namespace

NFSU2_REWRITE_THIS(0x005B0010, GetDampState5C, 0, "int GetDampState() [+0x5C]", o_GetDampState5C);
NFSU2_REWRITE_THIS(0x005B0490, GetDampState60, 0, "int GetDampState() [+0x60]", o_GetDampState60);
NFSU2_REWRITE_THIS(0x005A9D70, GetMaxBackupTime6C, 0, "float GetMaxBackupTime() [+0x6C]", o_GetMaxBackupTime6C);
NFSU2_REWRITE_THIS(0x005A8740, GetState1C, 0, "void* GetState() this+0x1C", o_GetState1C);
NFSU2_REWRITE_THIS(0x005A8230, GetState24, 0, "void* GetState() this+0x24", o_GetState24);
NFSU2_REWRITE_THIS(0x005B04A0, LastForce, 0, "bVector3* GetLastAccumulatedForce() this+0x100", o_LastForce);
NFSU2_REWRITE_THIS(0x005B04B0, LastTorque, 0, "bVector3* GetLastAccumulatedTorque() this+0x110", o_LastTorque);
NFSU2_REWRITE_THIS(0x005B1BA0, BodyPosition, 0, "bVector3* GetPosition() body+0x20", o_BodyPosition);
NFSU2_REWRITE_THIS(0x005B1BD0, BodyRotation, 0, "bMatrix4* GetRotation() body+0x30", o_BodyRotation);
NFSU2_REWRITE_THIS(0x005B1BB0, BodyVelocity, 0, "bVector3* GetVelocity() body+0xA0", o_BodyVelocity);
NFSU2_REWRITE_THIS(0x005B1BC0, BodyAcceleration, 0, "bVector3* GetAcceleration() body+0xB0", o_BodyAcceleration);
NFSU2_REWRITE_THIS(0x005B1BE0, BodyAngularVelocity, 0, "bVector3* GetAngularVelocity() body+0xC0", o_BodyAngularVelocity);
NFSU2_REWRITE_THIS(0x005B1BF0, BodyCenterOfGravity, 0, "bVector3* GetCenterOfGravity() body+0x1F0", o_BodyCenterOfGravity);
NFSU2_REWRITE_THIS(0x005B1C00, IsBlinking, 0, "int IsBlinking() (s8)+0x507", o_IsBlinking);
NFSU2_REWRITE_THIS(0x00593D90, TerrainType60b, 0, "int GetTerrainType() (s8)[+0x60]+0xB", o_TerrainType60b);
NFSU2_REWRITE_THIS(0x005ADA80, TerrainType60, 0, "int GetTerrainType() [+0x60]+0xD0", o_TerrainType60);
NFSU2_REWRITE_THIS(0x005B1CA0, TerrainType148, 0, "int GetTerrainType() [+0x148]+0xD0", o_TerrainType148);
NFSU2_REWRITE_THIS(0x005A9D80, BodyMassInverse, 0, "float GetMassInverse() body+0x168", o_BodyMassInverse);
NFSU2_REWRITE_THIS(0x005B1CB0, Bounciness, 1, "float GetBounciness(...) [+0x148]+0xCC", o_Bounciness);
NFSU2_REWRITE_THIS(0x005B1CC0, Friction, 1, "float GetFriction(...) [+0x148]+0xE0", o_Friction);
NFSU2_REWRITE_THIS(0x005A9D90, InertiaTensorInverse, 0, "bMatrix4* GetInertiaTensorInverse() body+0x1B0/+0x170", o_InertiaTensorInverse);
NFSU2_REWRITE_THIS(0x005A9F40, StaticRotation, 0, "bMatrix4* GetRotation() static 0x86B520", o_StaticRotation);
NFSU2_REWRITE_THIS(0x005AA040, StaticInertia, 0, "bMatrix4* GetInertiaTensorInverse() static 0x86B450", o_StaticInertia);
NFSU2_REWRITE_THIS(0x00599430, PhysicsExtrema, 0, "void* GetPhysicsExtrema() 0x86B3D0", o_PhysicsExtrema);
NFSU2_REWRITE_THIS(0x005A80B0, StateSize, 0, "int GetSize() 0x20", o_StateSize);
NFSU2_REWRITE_THIS(0x005A80D0, ZeroIndices, 0, "int* GetZeroIndices() 0x7FC694", o_ZeroIndices);
NFSU2_REWRITE_THIS(0x005A9F50, MaxBackupTimeConst, 0, "float GetMaxBackupTime() [0x784550]", o_MaxBackupTimeConst);
NFSU2_REWRITE_THIS(0x005B7680, MassInverseZero, 0, "float GetMassInverse() 0", o_MassInverseZero);
NFSU2_REWRITE_THIS(0x005AA060, ConsiderImpulse, 2, "float ConsiderImpulse(...) 1", o_ConsiderImpulse);
NFSU2_REWRITE_THIS(0x005AA080, ChangeInEnergy, 2, "float ChangeInEnergy(...) 0", o_ChangeInEnergy);
NFSU2_REWRITE_THIS(0x005A0590, BeginCollision, 1, "void BeginCollision(...) ++[+0x18]", o_BeginCollision);
NFSU2_REWRITE_THIS(0x005929C0, EndCollision, 1, "void EndCollision(...) --[+0x18]", o_EndCollision);
NFSU2_REWRITE_THIS(0x005AA070, AreCollisionsEnabledB, 1, "bool AreCollisionsEnabled(...) [+0x17]==0", o_AreCollisionsEnabledB);
NFSU2_REWRITE_THIS(0x005B1C80, IsMoveable, 0, "bool IsMoveable() [+0x150]==0", o_IsMoveable);
NFSU2_REWRITE_THIS(0x00599600, NormalizedLevel, 1, "float GetNormalizedLevel(int i)", o_NormalizedLevel);
NFSU2_REWRITE_THIS(0x005995E0, UpgradeLevel, 1, "int GetUpgradeLevel(int i)", o_UpgradeLevel);
NFSU2_REWRITE_THIS(0x005AA720, SetState14, 1, "void SetState(const int*) [+0x14]", o_SetState14);
NFSU2_REWRITE_THIS(0x00595020, SetState14_58, 1, "void SetState(const int*) [+0x14], [+0x58]=0", o_SetState14_58);
NFSU2_REWRITE_THIS(0x005A8750, GetState1Cc, 1, "void GetState(out) [+0x1C..+0x20]", o_GetState1Cc);
NFSU2_REWRITE_THIS(0x005A8770, SetState1C, 1, "void SetState(in) [+0x1C..+0x20]", o_SetState1C);
NFSU2_REWRITE_THIS(0x005A8240, GetState24c, 1, "void GetState(out) [+0x24..+0x28]", o_GetState24c);
NFSU2_REWRITE_THIS(0x005A8260, SetState24, 1, "void SetState(in) [+0x24..+0x28]", o_SetState24);
NFSU2_REWRITE_THIS(0x005B02B0, GetState20x4, 1, "void GetState(out) 16 bytes from +0x20", o_GetState20x4);
NFSU2_REWRITE_THIS(0x005B02D0, SetState20x4, 1, "void SetState(in) 16 bytes to +0x20", o_SetState20x4);
NFSU2_REWRITE_THIS(0x0059E130, ZeroDerivative, 1, "void GetDerivative(out) = {0, 0}", o_ZeroDerivative);

namespace {

// one object graph: the object, a body behind +0x68, surfaces behind +0x60 / +0x148, an in/out buffer
struct Graph {
    std::vector<uint8_t> obj = std::vector<uint8_t>(0x600), body = std::vector<uint8_t>(0x300),
                         s60 = std::vector<uint8_t>(0x100), s148 = std::vector<uint8_t>(0x100), io = std::vector<uint8_t>(32);
    void fill(game::Rng& r) {
        for (auto* v : {&obj, &body, &s60, &s148, &io})
            for (size_t i = 0; i < v->size(); i += 4) {
                const uint32_t x = r.below(4) == 0 ? 0u : (r.below(2) ? r.next() : 0x3F800000u + r.below(0x01000000));
                std::memcpy(v->data() + i, &x, 4);
            }
        link();
    }
    void link() {
        at<uint8_t*>(obj.data(), 0x68) = body.data();
        at<uint8_t*>(obj.data(), 0x60) = s60.data();
        at<uint8_t*>(obj.data(), 0x148) = s148.data();
    }
    // a value the function returned, as (region, offset) when it points into this graph
    int64_t rel(uint32_t v) const {
        const std::vector<uint8_t>* regions[] = {&obj, &body, &s60, &s148, &io};
        for (int k = 0; k < 5; ++k) {
            const uintptr_t b = reinterpret_cast<uintptr_t>(regions[k]->data());
            if (v >= b && v < b + regions[k]->size() + 0x400) return (static_cast<int64_t>(k + 1) << 32) | (v - b);
        }
        return v;
    }
    bool operator==(const Graph& o) const {
        auto strip = [](std::vector<uint8_t> v) {  // the link pointers differ by construction
            std::memset(v.data() + 0x60, 0, 4);
            std::memset(v.data() + 0x68, 0, 4);
            std::memset(v.data() + 0x148, 0, 4);
            return v;
        };
        return strip(obj) == strip(o.obj) && body == o.body && s60 == o.s60 && s148 == o.s148 && io == o.io;
    }
};

struct Fn {
    const char* name;
    const void* orig;
    const void* mine;
    int nargs;    // stack args
    int argkind;  // 0 none, 1 small index, 2 pointer into io
    bool eax;     // compare EAX
    bool flt;     // float result in st0 (typed call; the register caller would leave it on the x87 stack)
};

}  // namespace

#define F(n, na, ak, e) {#n, nullptr, NFSU2_ENTRY(n), na, ak, e, false}
#define FL(n, na, ak) {#n, nullptr, NFSU2_ENTRY(n), na, ak, false, true}

NFSU2_DIFF_TEST(zPhysics1) {
    static Fn fns[] = {
        F(GetDampState5C, 0, 0, true), F(GetDampState60, 0, 0, true), F(GetState1C, 0, 0, true), F(GetState24, 0, 0, true),
        F(LastForce, 0, 0, true), F(LastTorque, 0, 0, true), F(BodyPosition, 0, 0, true), F(BodyRotation, 0, 0, true),
        F(BodyVelocity, 0, 0, true), F(BodyAcceleration, 0, 0, true), F(BodyAngularVelocity, 0, 0, true),
        F(BodyCenterOfGravity, 0, 0, true), F(IsBlinking, 0, 0, true), F(TerrainType60b, 0, 0, true),
        F(TerrainType60, 0, 0, true), F(TerrainType148, 0, 0, true), F(InertiaTensorInverse, 0, 0, true),
        F(StaticRotation, 0, 0, true), F(StaticInertia, 0, 0, true), F(PhysicsExtrema, 0, 0, true), F(StateSize, 0, 0, true),
        F(ZeroIndices, 0, 0, true), F(BeginCollision, 1, 1, false), F(EndCollision, 1, 1, false),
        F(AreCollisionsEnabledB, 1, 1, true), F(IsMoveable, 0, 0, true), F(UpgradeLevel, 1, 1, true),
        F(SetState14, 1, 2, true), F(SetState14_58, 1, 2, true), F(GetState1Cc, 1, 2, true), F(SetState1C, 1, 2, true),
        F(GetState24c, 1, 2, true), F(SetState24, 1, 2, true), F(GetState20x4, 1, 2, true), F(SetState20x4, 1, 2, true),
        F(ZeroDerivative, 1, 2, true),
        // float returns: EAX is garbage, st0 compared below
        FL(GetMaxBackupTime6C, 0, 0), FL(BodyMassInverse, 0, 0), FL(Bounciness, 1, 1), FL(Friction, 1, 1),
        FL(MaxBackupTimeConst, 0, 0), FL(MassInverseZero, 0, 0), FL(ConsiderImpulse, 2, 1), FL(ChangeInEnergy, 2, 1),
        FL(NormalizedLevel, 1, 1)};
    static bool init = false;
    if (!init) {  // the originals (trampolines) are only known after registration
        const void* origs[] = {o_GetDampState5C, o_GetDampState60, o_GetState1C, o_GetState24, o_LastForce, o_LastTorque,
                               o_BodyPosition, o_BodyRotation, o_BodyVelocity, o_BodyAcceleration, o_BodyAngularVelocity,
                               o_BodyCenterOfGravity, o_IsBlinking, o_TerrainType60b, o_TerrainType60, o_TerrainType148,
                               o_InertiaTensorInverse, o_StaticRotation, o_StaticInertia, o_PhysicsExtrema, o_StateSize,
                               o_ZeroIndices, o_BeginCollision, o_EndCollision, o_AreCollisionsEnabledB, o_IsMoveable,
                               o_UpgradeLevel, o_SetState14, o_SetState14_58, o_GetState1Cc, o_SetState1C, o_GetState24c,
                               o_SetState24, o_GetState20x4, o_SetState20x4, o_ZeroDerivative, o_GetMaxBackupTime6C,
                               o_BodyMassInverse, o_Bounciness, o_Friction, o_MaxBackupTimeConst, o_MassInverseZero,
                               o_ConsiderImpulse, o_ChangeInEnergy, o_NormalizedLevel};
        for (size_t i = 0; i < sizeof fns / sizeof fns[0]; ++i) fns[i].orig = origs[i];
        init = true;
    }
    return game::run_diff(ctx, "zPhysics1", o_IsBlinking, [](game::Rng& r) -> game::Case {
        const Fn& f = fns[r.below(sizeof fns / sizeof fns[0])];
        Graph a, b;
        game::Rng r2 = r;
        a.fill(r);
        b.fill(r2);
        // IsMoveable: make the tested float exactly zero sometimes; tables: small indices
        if (r.below(3) == 0) { at<uint32_t>(a.obj.data(), 0x150) = 0; at<uint32_t>(b.obj.data(), 0x150) = 0; }
        uint32_t args[2];
        for (int i = 0; i < f.nargs; ++i) args[i] = f.argkind == 1 ? r.below(8) : 0;
        uint32_t argsb[2] = {args[0], args[1]};
        if (f.argkind == 2) {
            args[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a.io.data()));
            argsb[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b.io.data()));
        }
        std::string bad;
        if (f.flt) {
            float x, y;
            const uintptr_t fo = reinterpret_cast<uintptr_t>(f.orig), fm = reinterpret_cast<uintptr_t>(f.mine);
            if (f.nargs == 0) x = game_fn<F_t>(fo)(a.obj.data()), y = game_fn<F_t>(fm)(b.obj.data());
            else if (f.nargs == 1) x = game_fn<F1_t>(fo)(a.obj.data(), args[0]), y = game_fn<F1_t>(fm)(b.obj.data(), argsb[0]);
            else x = game_fn<F2_t>(fo)(a.obj.data(), args[0], args[1]), y = game_fn<F2_t>(fm)(b.obj.data(), argsb[0], argsb[1]);
            if (!game::same_float(x, y)) bad += " st0";
        } else {
            const game::RegOutThis ra = game::call_regs_this(f.orig, a.obj.data(), args, f.nargs);
            const game::RegOutThis rb = game::call_regs_this(f.mine, b.obj.data(), argsb, f.nargs);
            if (f.eax && a.rel(ra.eax) != b.rel(rb.eax)) bad += " eax";
            if (ra.popped != rb.popped) bad += " pops";
            if (ra.edx == game::kSentEDX && rb.edx != game::kSentEDX) bad += " edx";
        }
        if (!(a == b)) bad += " memory";
        return bad.empty() ? game::Ok() : game::MismatchText(std::string(f.name) + ":" + bad);
    });
}
#undef F
#undef FL

NFSU2_DIFF_TEST(zPhysics1_BROKEN) {  // harness self-check: the body getters with the wrong offset
    return game::run_diff(ctx, "zPhysics1_BROKEN", o_BodyVelocity, [](game::Rng& r) -> game::Case {
        Graph g;
        g.fill(r);
        return o_BodyVelocity(g.obj.data()) == BodyAcceleration(g.obj.data()) ? game::Ok() : game::MismatchText("offset");
    });
}
