// zPhysics batch 2 (14), written from the PC machine code: collision enable/query, static-vs-dynamic mass/inertia,
// idling / drive-wheel tests, yaw, velocity set, impulse-energy change, derivative, state forwarders.
#include <cmath>
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
inline uint32_t u(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
inline float q(float f) { return x87::st(x87::ld(f)); }  // fld/fstp: a signalling NaN comes out quiet

using V_t = uint32_t(__thiscall*)(void*);
using V1_t = uint32_t(__thiscall*)(void*, uint32_t);
using V2_t = uint32_t(__thiscall*)(void*, uint32_t, uint32_t);
using F_t = float(__thiscall*)(void*);
using F2_t = float(__thiscall*)(void*, uint32_t, uint32_t);

// 0x005931F0 AreCollisionsEnabled(...): [+0x17] == 0 and [[+0x60]+8] != 0
V1_t o_CollisionsEnabledA = nullptr;
uint32_t __cdecl CollisionsEnabledA(void* s, uint32_t) { return (at<uint8_t>(s, 0x17) == 0 && at<uint32_t>(ptr(s, 0x60), 8) != 0) ? 1u : 0u; }
// 0x0059E5A0 AreCollisionsEnabled(...) of a car: asleep ([+0x506] == 0) -> [+0x17] == 0, else as above
V1_t o_CollisionsEnabledB = nullptr;
uint32_t __cdecl CollisionsEnabledB(void* s, uint32_t a) {
    if (at<uint8_t>(s, 0x506) == 0) return at<uint8_t>(s, 0x17) == 0 ? 1u : 0u;
    return CollisionsEnabledA(s, a);
}
// 0x00593100 EnableCollisions(bool on): on -> [+0x17] = 0; off -> [+0x17]-- unless already 0 (a disable count)
V1_t o_EnableCollisions = nullptr;
uint32_t __cdecl EnableCollisions(void* s, uint32_t on) {
    if (static_cast<uint8_t>(on)) at<uint8_t>(s, 0x17) = 0;
    else if (at<uint8_t>(s, 0x17)) --at<uint8_t>(s, 0x17);
    return 0;
}
// 0x005B1C10 GetMassInverse: [+0x150] == 0 -> body+0x168, else 0
F_t o_MassInverseOrZero = nullptr;
float __cdecl MassInverseOrZero(void* s) { return at<float>(s, 0x150) == 0.0f ? at<float>(ptr(s, 0x68), 0x168) : gf(0x007D7D3C); }
// 0x005B1C40 GetInertiaTensorInverse: [+0x150] == 0 -> body+0x1B0/+0x170, else the static 0x86B450
V_t o_InertiaOrStatic = nullptr;
uint32_t __cdecl InertiaOrStatic(void* s) {
    if (!(at<float>(s, 0x150) == 0.0f)) return 0x0086B450;
    uint8_t* b = ptr(s, 0x68);
    return u(b) + (at<uint32_t>(b, 0x158) ? 0x1B0u : 0x170u);
}
// 0x00594E90 Idling: engine [+0x20]: rpm +0x78 < K (0x78758C) and throttle*k + 1e-4 >= [+0x4C]
V_t o_Idling = nullptr;
uint32_t __cdecl Idling(void* s) {
    uint8_t* e = ptr(s, 0x20);
    if (!(at<float>(e, 0x78) < gf(0x0078758C))) return 0;
    const float t = *reinterpret_cast<float*>(ptr(e, 0x18)) * gf(0x007A1D38) + gf(0x007843EC);
    return (t < at<float>(s, 0x4C) || t != t || at<float>(s, 0x4C) != at<float>(s, 0x4C)) ? 0u : 1u;
}
// 0x00592870 IsDriveWheel(i): drive bias [[this]+0x1C0]: < 0x784554 -> front wheels (0, 1); <= 0x7850EC -> all;
// above -> rear (2, 3). Unordered compares fall to "all".
V1_t o_IsDriveWheel = nullptr;
uint32_t __cdecl IsDriveWheel(void* s, uint32_t i) {
    const float b = at<float>(ptr(s, 0), 0x1C0);
    if (b < gf(0x00784554)) return (i == 1 || i == 0) ? 1u : 0u;
    if (!(b > gf(0x007850EC))) return 1;
    return (i == 2 || i == 3) ? 1u : 0u;
}
// 0x0059FA10 GetYaw: speed +0x3EC < 0x7A1824 -> 0, else GetYawAlways (0x591D90)
F_t o_GetYaw = nullptr;
float __cdecl GetYaw(void* s) {
    if (at<float>(s, 0x3EC) < gf(0x007A1824)) return gf(0x007D7D3C);
    return game_fn<F_t>(0x00591D90)(s);
}
// 0x00592160 SetVelocity(v): +0x30 = v (x raw, y/z through the x87), +0x480 = 0, +0x3F0 = |v| (x*x + z*z + y*y)
V1_t o_SetVelocity = nullptr;
uint32_t __cdecl SetVelocity(void* s, uint32_t vp) {
    const float* v = reinterpret_cast<const float*>(vp);
    at<uint32_t>(s, 0x30) = *reinterpret_cast<const uint32_t*>(v);
    at<float>(s, 0x34) = q(v[1]);
    at<float>(s, 0x38) = q(v[2]);
    at<uint8_t>(s, 0x480) = 0;
    using namespace x87;  // extended exponent: x*x must not overflow where the original's doesn't
    const X x = ld(v[0]), y = ld(v[1]), z = ld(v[2]);
    at<float>(s, 0x3F0) = st(x87::sqrt(add(add(mul(x, x), mul(z, z)), mul(y, y))));
    return vp;
}
// 0x005A9FA0 GetCurrentVelocities(out_linear, out_angular): both get the globals 0x86B390..98 (x87 copies)
V2_t o_CurrentVelocities = nullptr;
uint32_t __cdecl CurrentVelocities(void*, uint32_t a, uint32_t b) {
    for (uint32_t p : {a, b}) {
        float* o = reinterpret_cast<float*>(p);
        o[0] = q(gf(0x0086B390));
        o[1] = q(gf(0x0086B394));
        o[2] = q(gf(0x0086B398));
    }
    return b;
}
// 0x005A0A00 ImpulseEnergyChange(impulse, point): body.GetTestImpulseEnergy(impulse, point) - current kinetic energy
// (0.5 * v.lin, 0.5 * w.ang, summed in the original's order)
F2_t o_ImpulseEnergyChange = nullptr;
// the callee's st0 kept at full x87 width (a C++ float return would round it before the subtraction)
x87::X call_energy(void* body, uint32_t a, uint32_t b) {
    x87::X r;
    __asm {
        push b
        push a
        mov ecx, body
        mov eax, 0x0059BFD0
        call eax
        fstp tbyte ptr [r]
    }
    return r;
}
float __cdecl ImpulseEnergyChange(void* s, uint32_t a, uint32_t b) {
    using namespace x87;
    uint8_t* B = ptr(s, 0x68);
    auto f = [&](uint32_t off) { return ld(at<float>(B, off)); };
    const X half = ld(gf(0x00784260));
    const X lin = mul(add(add(mul(f(0x78), f(0xA8)), mul(f(0x74), f(0xA4))), mul(f(0x70), f(0xA0))), half);
    const X ang = mul(add(add(mul(f(0x88), f(0xC8)), mul(f(0x84), f(0xC4))), mul(f(0x80), f(0xC0))), half);
    const float now = st(add(lin, ang));  // fstp dword [esp]
    return st(sub(call_energy(B, a, b), ld(now)));
}
// 0x005949B0 GetDerivative(out): state 3 -> 0; else rate = [[+0x18]+0x1C] * [+0xE8], times (x+1) when x = [+0x68] > 0,
// times 0x7A18D4 when x < 0 and gear [+0x38] >= 3; then times table 0x7FC298[[+0x44]]
V1_t o_EngineDerivative = nullptr;
uint32_t __cdecl EngineDerivative(void* s, uint32_t outp) {
    float* out = reinterpret_cast<float*>(outp);
    if (at<int32_t>(s, 0x60) == 3) {
        *reinterpret_cast<uint32_t*>(out) = 0;
        return 0;
    }
    using namespace x87;
    X d = mul(ld(at<float>(ptr(s, 0x18), 0x1C)), ld(at<float>(s, 0xE8)));
    const float x = at<float>(s, 0x68);
    if (x > 0.0f) d = mul(d, add(ld(x), ld(gf(0x00784250))));
    else if (x < 0.0f && at<int32_t>(s, 0x38) >= 3) d = mul(d, ld(gf(0x007A18D4)));
    *out = st(mul(d, ld(*reinterpret_cast<float*>(0x007FC298 + at<int32_t>(s, 0x44) * 4))));
    return 0;
}
// 0x005A8150 SetState(state): (this+0x20) = state;  0x005A80E0 GetState(out): out = (this+0x20)
using Assign_t = uint32_t(__thiscall*)(void*, void*);
V1_t o_SetStateFwd = nullptr;
uint32_t __cdecl SetStateFwd(void* s, uint32_t st) { return game_fn<Assign_t>(0x005A80F0)(static_cast<uint8_t*>(s) + 0x20, reinterpret_cast<void*>(st)); }
V1_t o_GetStateFwd = nullptr;
uint32_t __cdecl GetStateFwd(void* s, uint32_t out) { return game_fn<Assign_t>(0x005A80F0)(reinterpret_cast<void*>(out), static_cast<uint8_t*>(s) + 0x20); }

}  // namespace

NFSU2_REWRITE_THIS(0x005931F0, CollisionsEnabledA, 1, "bool AreCollisionsEnabled(...)", o_CollisionsEnabledA);
NFSU2_REWRITE_THIS(0x0059E5A0, CollisionsEnabledB, 1, "bool AreCollisionsEnabled(...) car (asleep check)", o_CollisionsEnabledB);
NFSU2_REWRITE_THIS(0x00593100, EnableCollisions, 1, "void EnableCollisions(bool)", o_EnableCollisions);
NFSU2_REWRITE_THIS(0x005B1C10, MassInverseOrZero, 0, "float GetMassInverse() (0 when static)", o_MassInverseOrZero);
NFSU2_REWRITE_THIS(0x005B1C40, InertiaOrStatic, 0, "bMatrix4* GetInertiaTensorInverse() (static 0x86B450)", o_InertiaOrStatic);
NFSU2_REWRITE_THIS(0x00594E90, Idling, 0, "bool Engine::Idling()", o_Idling);
NFSU2_REWRITE_THIS(0x00592870, IsDriveWheel, 1, "bool IsDriveWheel(int wheel)", o_IsDriveWheel);
NFSU2_REWRITE_THIS(0x0059FA10, GetYaw, 0, "float CarState::GetYaw()", o_GetYaw);
NFSU2_REWRITE_THIS(0x00592160, SetVelocity, 1, "void SetVelocity(const bVector3*)", o_SetVelocity);
NFSU2_REWRITE_THIS(0x005A9FA0, CurrentVelocities, 2, "void GetCurrentVelocities(bVector3*, bVector3*)", o_CurrentVelocities);
NFSU2_REWRITE_THIS(0x005A0A00, ImpulseEnergyChange, 2, "float ImpulseEnergyChange(const bVector3* impulse, const bVector3* point)", o_ImpulseEnergyChange);
NFSU2_REWRITE_THIS(0x005949B0, EngineDerivative, 1, "void GetDerivative(float* out)", o_EngineDerivative);
NFSU2_REWRITE_THIS(0x005A8150, SetStateFwd, 1, "void SetState(const RigidBodyState*) -> this+0x20", o_SetStateFwd);
NFSU2_REWRITE_THIS(0x005A80E0, GetStateFwd, 1, "void GetState(RigidBodyState* out) <- this+0x20", o_GetStateFwd);

namespace {

// object graph: pointer fields +0x00, +0x18, +0x20, +0x60, +0x68, +0x148 to their own blocks; the +0x20 block's +0x18
// points to one more float; two in/out buffers
struct Graph {
    std::vector<std::vector<uint8_t>> blk;
    enum { OBJ, P0, P18, P20, P60, BODY, P148, T20, IO1, IO2, N };
    void fill(game::Rng& r) {
        blk.assign(N, {});
        for (int k = 0; k < N; ++k) {
            blk[k].resize(k == OBJ ? 0x600 : 0x300);
            for (size_t i = 0; i < blk[k].size(); i += 4) {
                const uint32_t x = r.below(4) == 0 ? 0u : (r.below(3) ? 0x3F800000u + r.below(0x01000000) - (r.below(2) << 31) : r.next());
                std::memcpy(blk[k].data() + i, &x, 4);
            }
        }
        // ints the code indexes or compares
        at<int32_t>(blk[OBJ].data(), 0x44) = static_cast<int32_t>(r.below(4));
        at<int32_t>(blk[OBJ].data(), 0x60) = static_cast<int32_t>(r.below(5));
        at<int32_t>(blk[OBJ].data(), 0x38) = static_cast<int32_t>(r.below(6));
        at<uint8_t>(blk[OBJ].data(), 0x17) = static_cast<uint8_t>(r.below(3));
        at<uint8_t>(blk[OBJ].data(), 0x506) = static_cast<uint8_t>(r.below(2));
        if (r.below(3) == 0) at<uint32_t>(blk[OBJ].data(), 0x150) = 0;
        link();
    }
    void link() {
        const std::pair<uint32_t, int> l[] = {{0x00, P0}, {0x18, P18}, {0x20, P20}, {0x60, P60}, {0x68, BODY}, {0x148, P148}};
        for (auto [off, k] : l) at<uint8_t*>(blk[OBJ].data(), off) = blk[k].data();
        at<uint8_t*>(blk[P20].data(), 0x18) = blk[T20].data();
        at<uint32_t>(blk[P60].data(), 8) = at<uint32_t>(blk[P60].data(), 8) & 1;  // pointer-or-null flag
    }
    int64_t rel(uint32_t v) const {
        for (int k = 0; k < N; ++k) {
            const uintptr_t b = reinterpret_cast<uintptr_t>(blk[k].data());
            if (v >= b && v < b + blk[k].size() + 0x400) return (static_cast<int64_t>(k + 1) << 32) | (v - b);
        }
        return v;
    }
    bool same_as(const Graph& o) const {
        for (int k = 0; k < N; ++k) {
            std::vector<uint8_t> a = blk[k], b = o.blk[k];
            if (k == OBJ)
                for (uint32_t off : {0x00u, 0x18u, 0x20u, 0x60u, 0x68u, 0x148u}) std::memset(a.data() + off, 0, 4), std::memset(b.data() + off, 0, 4);
            if (k == P20) std::memset(a.data() + 0x18, 0, 4), std::memset(b.data() + 0x18, 0, 4);
            for (size_t i = 0; i < a.size(); i += 4) {
                float x, y;
                std::memcpy(&x, a.data() + i, 4);
                std::memcpy(&y, b.data() + i, 4);
                if (!std::memcmp(&x, &y, 4) || (x != x && y != y)) continue;  // NaN payloads may differ
                uint32_t ux, uy;  // a copied link pointer: same place in each graph
                std::memcpy(&ux, &x, 4);
                std::memcpy(&uy, &y, 4);
                if (rel(ux) == o.rel(uy) && rel(ux) > 0xFFFFFFFFll) continue;
                return false;
            }
        }
        return true;
    }
};

struct Fn {
    const char* name;
    const void* orig;
    const void* mine;
    int nargs;
    int argkind;  // 0 none, 1 small int, 2 io pointers
    bool eax, flt;
};

}  // namespace

#define F(n, na, ak, e) {#n, nullptr, NFSU2_ENTRY(n), na, ak, e, false}
#define FL(n, na, ak) {#n, nullptr, NFSU2_ENTRY(n), na, ak, false, true}
NFSU2_DIFF_TEST(zPhysics2) {
    static Fn fns[] = {F(CollisionsEnabledA, 1, 1, true), F(CollisionsEnabledB, 1, 1, true), F(EnableCollisions, 1, 1, false),
                       FL(MassInverseOrZero, 0, 0), F(InertiaOrStatic, 0, 0, true), F(Idling, 0, 0, true),
                       F(IsDriveWheel, 1, 1, true), FL(GetYaw, 0, 0), F(SetVelocity, 1, 2, false),
                       F(CurrentVelocities, 2, 2, false), FL(ImpulseEnergyChange, 2, 2), F(EngineDerivative, 1, 2, false),
                       F(SetStateFwd, 1, 2, false), F(GetStateFwd, 1, 2, false)};
    static bool init = false;
    if (!init) {
        const void* o[] = {o_CollisionsEnabledA, o_CollisionsEnabledB, o_EnableCollisions, o_MassInverseOrZero, o_InertiaOrStatic,
                           o_Idling, o_IsDriveWheel, o_GetYaw, o_SetVelocity, o_CurrentVelocities, o_ImpulseEnergyChange,
                           o_EngineDerivative, o_SetStateFwd, o_GetStateFwd};
        for (size_t i = 0; i < sizeof fns / sizeof fns[0]; ++i) fns[i].orig = o[i];
        init = true;
    }
    return game::run_diff(ctx, "zPhysics2", o_Idling, [](game::Rng& r) -> game::Case {
        const Fn& f = fns[r.below(sizeof fns / sizeof fns[0])];
        Graph a, b;
        game::Rng r2 = r;
        a.fill(r);
        b.fill(r2);
        uint32_t args[2] = {r.below(5), r.below(5)}, argsb[2] = {args[0], args[1]};
        if (f.argkind == 2) {
            args[0] = u(a.blk[Graph::IO1].data()), args[1] = u(a.blk[Graph::IO2].data());
            argsb[0] = u(b.blk[Graph::IO1].data()), argsb[1] = u(b.blk[Graph::IO2].data());
        }
        std::string bad;
        void* oa = a.blk[Graph::OBJ].data();
        void* ob = b.blk[Graph::OBJ].data();
        if (f.flt) {
            const uintptr_t fo = reinterpret_cast<uintptr_t>(f.orig), fm = reinterpret_cast<uintptr_t>(f.mine);
            const float x = f.nargs == 0 ? game_fn<F_t>(fo)(oa) : game_fn<F2_t>(fo)(oa, args[0], args[1]);
            const float y = f.nargs == 0 ? game_fn<F_t>(fm)(ob) : game_fn<F2_t>(fm)(ob, argsb[0], argsb[1]);
            if (!game::same_float(x, y)) bad += " st0";
        } else {
            const game::RegOutThis ra = game::call_regs_this(f.orig, oa, args, f.nargs);
            const game::RegOutThis rb = game::call_regs_this(f.mine, ob, argsb, f.nargs);
            if (f.eax && a.rel(ra.eax) != b.rel(rb.eax)) bad += " eax";
            if (ra.popped != rb.popped) bad += " pops";
            if (ra.edx == game::kSentEDX && rb.edx != game::kSentEDX) bad += " edx";
        }
        if (!a.same_as(b)) bad += " memory";
        return bad.empty() ? game::Ok() : game::MismatchText(std::string(f.name) + ":" + bad);
    });
}
#undef F
#undef FL

NFSU2_DIFF_TEST(zPhysics2_BROKEN) {  // harness self-check: IsDriveWheel with front/rear swapped
    return game::run_diff(ctx, "zPhysics2_BROKEN", o_IsDriveWheel, [](game::Rng& r) -> game::Case {
        Graph g;
        g.fill(r);
        const uint32_t i = r.below(4);
        const uint32_t sw = i < 2 ? i + 2 : i - 2;
        return o_IsDriveWheel(g.blk[Graph::OBJ].data(), i) == IsDriveWheel(g.blk[Graph::OBJ].data(), sw) ? game::Ok() : game::MismatchText("front/rear");
    });
}
