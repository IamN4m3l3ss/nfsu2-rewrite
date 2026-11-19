// Camera deps batch 2 (24), written from the PC machine code: small car / player / matrix helpers the camera movers
// call (hydraulics state, bounding box, camera target car, POV type, route), eCreateRotationZ / eRotateZ, the two
// D3DX dispatch thunks, vector helpers, and the two one-jump thunks to the CRT's operator new / delete
// (0x575620 / 0x575630; the KB's names MEM_allocz / __builtin_vec_delete are the jump targets').
#include "leaf.h"
#include "x87.h"

namespace {
using namespace x87;

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
inline uint32_t u_(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
inline X gx(uint32_t addr) { return ld(*reinterpret_cast<const float*>(addr)); }
inline uint16_t vs_zero(uint32_t addr) { return cmpw(ld(*reinterpret_cast<const float*>(addr)), gx(0x007D7D3C)); }
inline bool positive(uint16_t sw) { return ((sw >> 8) & 0x41) == 0; }

// 0x005ED290 Car::AreHydraulicsBouncing: any of the four floats [+0x30] +0x1EC..+0x1F8 > 0. AL = that, AH = the
// last compare's status high byte, the rest the caller's.
uint32_t __cdecl Car_AreHydraulicsBouncing(uint8_t* s, uint32_t eax_in) {
    const uint32_t b = at<uint32_t>(s, 0x30);
    uint16_t sw = 0;
    for (uint32_t off = 0x1EC; off <= 0x1F8; off += 4) {
        sw = vs_zero(b + off);
        if (positive(sw)) return (eax_in & 0xFFFF0000u) | (sw & 0xFF00u) | 1;
    }
    return (eax_in & 0xFFFF0000u) | (sw & 0xFF00u);
}

// 0x005ED300 Car::AreHydraulicsExtended: any of +0x3F8 / [+0x30]+0x1FC / +0x3FC / +0x200 / +0x400 / +0x204 / +0x404 /
// +0x208 > 0 (in that order). After the first check EAX's high half is 0 (the original computes offsets in EAX).
uint32_t __cdecl Car_AreHydraulicsExtended(uint8_t* s, uint32_t eax_in) {
    const uint32_t c = u_(s);
    uint16_t sw = vs_zero(c + 0x3F8);
    if (positive(sw)) return (eax_in & 0xFFFF0000u) | (sw & 0xFF00u) | 1;
    const uint32_t b = at<uint32_t>(s, 0x30);
    const uint32_t seq[7] = {b + 0x1FC, c + 0x3FC, b + 0x200, c + 0x400, b + 0x204, c + 0x404, b + 0x208};
    for (int i = 0; i < 7; ++i) {
        sw = vs_zero(seq[i]);
        if (positive(sw)) return (sw & 0xFF00u) | 1;
    }
    return sw & 0xFF00u;
}

// 0x005EC240 Car::GetLocalMiddle: [+0x3C] + 0x110
uint32_t __cdecl Car_GetLocalMiddle(uint8_t* s) { return at<uint32_t>(s, 0x3C) + 0x110; }

// 0x005EC250 Car::GetCarSpaceBoundingBox(min, max): from [+0x3C] +0xE0 / +0xF0 (y, z through the x87). EAX = max.
uint32_t __cdecl Car_GetCarSpaceBoundingBox(uint8_t* s, float* mn, float* mx) {
    const uint8_t* b = ptr(s, 0x3C);
    const X y0 = ld(at<float>(b, 0xE4)), z0 = ld(at<float>(b, 0xE8));
    *reinterpret_cast<uint32_t*>(mn) = at<uint32_t>(b, 0xE0);
    mn[1] = st(y0);
    mn[2] = st(z0);
    const uint32_t x1 = at<uint32_t>(b, 0xF0);
    const X y1 = ld(at<float>(b, 0xF4)), z1 = ld(at<float>(b, 0xF8));
    *reinterpret_cast<uint32_t*>(mx) = x1;
    mx[1] = st(y1);
    mx[2] = st(z1);
    return u_(mx);
}

// 0x0040F690 Car::GetRoute: [+0xC] ? the section table entry 0x88A334[[+0xC] +0xC & 0x7FF] : 0
uint32_t __cdecl Car_GetRoute(uint8_t* s) {
    const uint32_t p = at<uint32_t>(s, 0xC);
    if (!p) return 0;
    return gu(0x0088A334u + (at<uint32_t>(p, 0xC) & 0x7FF) * 4);
}
// 0x005EC0A0: Car::GetRoute of [+0x2C] (tail jump)
uint32_t __cdecl Owner_GetCarRoute(uint8_t* s) { return game_fn<uint32_t(__thiscall*)(void*)>(0x0040F690)(ptr(s, 0x2C)); }

// 0x005EAC70 Player::GetCameraTargetCar: [+4], or (flag +0x24A) the car of [[0x890118] +0xBD0] when there is one
uint32_t __cdecl Player_GetCameraTargetCar(uint8_t* s) {
    uint32_t eax = at<uint32_t>(s, 4);
    if (s[0x24A]) {
        const uint32_t o = at<uint32_t>(gu(0x00890118u), 0xBD0);
        if (o) eax = gu(o);
    }
    return eax;
}

// 0x00502500 Player::GetPovType(int): its jump table 0x50252C (case bodies return 2 / 4 / 1 / 5 / 0)
uint32_t __cdecl Player_GetPovType(uint32_t v) {
    if (v > 4) return 0;
    switch (gu(0x0050252Cu + v * 4)) {
        case 0x00502510: return 2;
        case 0x00502516: return 4;
        case 0x0050251C: return 1;
        case 0x00502522: return 5;
        default: return 0;  // 0x502528
    }
}
// 0x005EB400: Player::GetPovType of the entry for index +0x10 in the table 0x83AA74 (stride 40)
uint32_t __cdecl Player_PovTypeOfSlot(uint8_t* s) {
    return game_fn<uint32_t(__cdecl*)(uint32_t)>(0x00502500)(gu(0x0083AA74u + at<uint32_t>(s, 0x10) * 40));
}

// 0x005BAC90 eCreateRotationZ(out, angle): EAX = out
uint32_t __cdecl CreateRotationZ(uint32_t out, uint32_t angle) {
    using Trig = float(__cdecl*)(uint32_t);
    const float c = game_fn<Trig>(0x0043C370)(angle);
    const X s = ld(game_fn<Trig>(0x0043C2B0)(angle));
    uint32_t* o = reinterpret_cast<uint32_t*>(out);
    o[0] = fbits(st(ld(c)));
    o[1] = fbits(st(s));
    o[5] = fbits(c);
    o[4] = fbits(st(negx(s)));
    for (int i : {2, 3, 6, 7, 8, 9, 11, 12, 13, 14}) o[i] = 0;
    o[10] = o[15] = 0x3F800000u;
    return out;
}
// 0x0063CEDB / 0x0063D12C: jumps through the D3DX dispatch slots 0x803E74 (multiply, 3 args) / 0x803F00 (2 args)
uint32_t __cdecl D3DX_Thunk3(uint32_t a, uint32_t b, uint32_t c) {  // (LEAF_STD bodies are __cdecl)
    return reinterpret_cast<uint32_t(__stdcall*)(uint32_t, uint32_t, uint32_t)>(gu(0x00803E74u))(a, b, c);
}
uint32_t __cdecl D3DX_Thunk2(uint32_t a, uint32_t b) {
    return reinterpret_cast<uint32_t(__stdcall*)(uint32_t, uint32_t)>(gu(0x00803F00u))(a, b);
}
// 0x005BAE60 eRotateZ(out, m, angle): out = m * rotZ(angle) (via 0x63CEDB). EAX = its.
uint32_t __cdecl RotateZ(uint32_t out, uint32_t m, uint32_t angle) {
    alignas(16) uint32_t r[16];
    game_fn<uint32_t(__cdecl*)(uint32_t, uint32_t)>(0x005BAC90)(u_(r), angle);
    return game_fn<uint32_t(__stdcall*)(uint32_t, uint32_t, uint32_t)>(0x0063CEDB)(out, m, u_(r));
}
// 0x005BAF80 (out, m) through 0x63D12C. EAX = its.
uint32_t __cdecl D3DX_Call2(uint32_t a, uint32_t b) {
    return game_fn<uint32_t(__stdcall*)(uint32_t, uint32_t)>(0x0063D12C)(a, b);
}

// 0x00401250 bVector3 (this) minus: out = this - b (y, z, x computed, then stored x, y, z). EAX = out.
uint32_t __cdecl Vec_Sub(const float* s, float* out, const float* b) {
    const X y = subm(ld(s[1]), fbits(b[1])), z = subm(ld(s[2]), fbits(b[2])), x = subm(ld(s[0]), fbits(b[0]));
    out[0] = st(x);
    out[1] = st(y);
    out[2] = st(z);
    return u_(out);
}
// 0x004011B0 out = a - b (y with both operands loaded). EAX = out.
uint32_t __cdecl Vec_Sub3(float* out, const float* a, const float* b) {
    const X ay = ld(a[1]), az = ld(a[2]), by = ld(b[1]);
    const uint32_t bz = fbits(b[2]);
    const X x = subm(ld(a[0]), fbits(b[0]));
    out[0] = st(x);
    out[1] = st(sub(ay, by));
    out[2] = st(subm(az, bz));
    return u_(out);
}
// 0x00401050 max(a, b): a if a > b, else b
float __cdecl FloatMax(uint32_t a, uint32_t b) {
    return st((cmp(ld(bitsf(a)), ld(bitsf(b))) & 0x41) == 0 ? ld(bitsf(a)) : ld(bitsf(b)));
}
// 0x00441920: +0x1C94 != 0 (NaN counts as nonzero)
uint32_t __cdecl NonzeroAt1C94(uint8_t* s) {
    return parity_even(cmp(ld(at<float>(s, 0x1C94)), gx(0x007D7D3C)) & 0x44) ? 1u : 0u;
}
// 0x00444D70 copy a 3x3 orientation + position (rows +0 / +0x10 / +0x20 / +0x30, 3 floats each; y, z of rows 1-2
// through the x87) from src into this. EAX = this.
uint32_t __cdecl Orient_Copy(uint8_t* d, const uint8_t* src) {
    at<uint32_t>(d, 0) = at<uint32_t>(src, 0);
    for (uint32_t row : {0x10u, 0x20u}) {
        const X y = ld(at<float>(src, row + 4)), z = ld(at<float>(src, row + 8));
        at<uint32_t>(d, row) = at<uint32_t>(src, row);
        at<float>(d, row + 4) = st(y);
        at<float>(d, row + 8) = st(z);
    }
    at<uint32_t>(d, 0x30) = at<uint32_t>(src, 0x30);
    at<uint32_t>(d, 0x34) = at<uint32_t>(src, 0x34);
    at<uint32_t>(d, 0x38) = at<uint32_t>(src, 0x38);
    return u_(d);
}
// 0x004418A0 seconds since the world tick stamp +0x210 (0 / 0x7FFFFFFF = never: 0)
float __cdecl SecondsSince210(uint8_t* s) {
    const int32_t t = at<int32_t>(s, 0x210);
    if (t == 0 || t == 0x7FFFFFFF) return st(gx(0x007D7D3C));
    return st(mulm(ild(static_cast<int32_t>(gu(0x0086518Cu) - static_cast<uint32_t>(t))), gu(0x00784268u)));
}
// 0x005D4520 +0x3C - +0x38, plus the route length 0x88F2B0 when negative
float __cdecl RouteSpan(uint8_t* s) {
    const X v = subm(ld(at<float>(s, 0x3C)), at<uint32_t>(s, 0x38));
    if (!parity_even(cmp(v, gx(0x007D7D3C)) & 5)) return st(addm(v, gu(0x0088F2B0u)));
    return st(v);
}
// 0x004031C0 CarMover::DoSnapshot(x): empty (ret 4)
uint32_t __cdecl CarMover_DoSnapshot(uint8_t*, uint32_t, uint32_t eax_in) { return eax_in; }

// 0x00575620 / 0x00575630: jumps to the CRT's operator new (0x75C6C8) / operator delete (0x75C6DA)
uint32_t __cdecl Thunk_New(uint32_t n) { return game_fn<uint32_t(__cdecl*)(uint32_t)>(0x0075C6C8)(n); }
uint32_t __cdecl Thunk_Delete(uint32_t p) { return game_fn<uint32_t(__cdecl*)(uint32_t)>(0x0075C6DA)(p); }

void fill_car(Graph& g) {  // floats near 0 so the "> 0" checks go both ways
    for (int k : {0, 1}) {
        uint8_t* b = g.blk[k].data();
        for (uint32_t off = 0x1E0; off < 0x410; off += 4) {
            if (off == 0x248) continue;  // keeps the player flag +0x24A
            const uint32_t h = (off * 2654435761u) ^ at<uint32_t>(b, 0);
            at<float>(b, off) = (h & 3) == 0 ? 0.0f : (h & 3) == 1 ? -1.0f : (h & 3) == 2 ? 0.5f : bitsf(0x80000000u);
        }
    }
}

}  // namespace

LEAF_THIS_EAX(0x005ED290, Car_AreHydraulicsBouncing, 0, "bool Car::AreHydraulicsBouncing()");
LEAF_THIS_EAX(0x005ED300, Car_AreHydraulicsExtended, 0, "bool Car::AreHydraulicsExtended()");
LEAF_THIS(0x005EC240, Car_GetLocalMiddle, 0, "bVector3* Car::GetLocalMiddle()");
LEAF_THIS(0x005EC250, Car_GetCarSpaceBoundingBox, 2, "void Car::GetCarSpaceBoundingBox(bVector3* min, bVector3* max)");
LEAF_THIS(0x0040F690, Car_GetRoute, 0, "TrackRoute* Car::GetRoute()");
LEAF_THIS(0x005EC0A0, Owner_GetCarRoute, 0, "TrackRoute* (this->+0x2C)->GetRoute()");
LEAF_THIS(0x005EAC70, Player_GetCameraTargetCar, 0, "Car* Player::GetCameraTargetCar()");
LEAF_CDECL(0x00502500, Player_GetPovType, 1, "int Player::GetPovType(int)");
LEAF_THIS(0x005EB400, Player_PovTypeOfSlot, 0, "int Player::GetPovType of slot +0x10");
LEAF_CDECL(0x005BAC90, CreateRotationZ, 2, "bMatrix4* eCreateRotationZ(bMatrix4* out, u16 angle)");
LEAF_STD(0x0063CEDB, D3DX_Thunk3, 3, "thunk: jmp [0x803E74] (D3DX matrix multiply)");
LEAF_STD(0x0063D12C, D3DX_Thunk2, 2, "thunk: jmp [0x803F00] (D3DX, 2 args)");
LEAF_CDECL(0x005BAE60, RotateZ, 3, "bMatrix4* eRotateZ(bMatrix4* out, const bMatrix4*, u16 angle)");
LEAF_CDECL(0x005BAF80, D3DX_Call2, 2, "bMatrix4* (out, const bMatrix4*) via 0x63D12C");
LEAF_THIS(0x00401250, Vec_Sub, 2, "bVector3* bVector3::operator-(bVector3* out, const bVector3&)");
LEAF_CDECL(0x004011B0, Vec_Sub3, 3, "bVector3* bSub(bVector3* out, const bVector3*, const bVector3*)");
LEAF_CDECL(0x00401050, FloatMax, 2, "float bMax(float, float)");
LEAF_THIS(0x00441920, NonzeroAt1C94, 0, "bool (+0x1C94 != 0)");
LEAF_THIS(0x00444D70, Orient_Copy, 1, "Orientation& Orientation::operator=(const Orientation&)");
LEAF_THIS(0x004418A0, SecondsSince210, 0, "float seconds since tick +0x210");
LEAF_THIS(0x005D4520, RouteSpan, 0, "float span +0x3C - +0x38 (wrapped by the route length)");
LEAF_THIS_EAX(0x004031C0, CarMover_DoSnapshot, 1, "void CarMover::DoSnapshot(x)");
LEAF_CDECL(0x00575620, Thunk_New, 1, "void* operator new(unsigned) (thunk)");
LEAF_CDECL(0x00575630, Thunk_Delete, 1, "void operator delete(void*) (thunk)");

NFSU2_DIFF_TEST(camera_deps2_car) {
    // 0 a car (+0x30 -> 1, +0x3C -> 2, +0xC -> 3 or 0, +0x2C -> 0 itself), 4/5 vectors; player flag +0x24A
    static const Spec spec = [] {
        Spec sp{6, 0x2000, {{0, 0x30, 1}, {0, 0x3C, 2}, {0, 0xC, 3, true}, {0, 0x2C, 0}, {0, 4, 4}},
                {{0, 0x24A, 1, 2}, {0, 0x10, 4, 4}}};
        sp.post = &fill_car;
        return sp;
    }();
    static const std::vector<Fn> fns{
        LEAF_FN(Car_AreHydraulicsBouncing, THIS, INT),
        LEAF_FN(Car_AreHydraulicsExtended, THIS, INT),
        LEAF_FN(Car_GetLocalMiddle, THIS, PTR),
        LEAF_FN(Car_GetCarSpaceBoundingBox, THIS, PTR, {'p', 4}, {'p', 5}),
        LEAF_FN(Car_GetRoute, THIS, INT),
        LEAF_FN(Owner_GetCarRoute, THIS, INT),
        LEAF_FN(Player_GetCameraTargetCar, THIS, PTR),
        LEAF_FN(Player_PovTypeOfSlot, THIS, INT),
        LEAF_FN(NonzeroAt1C94, THIS, INT),
        LEAF_FN(SecondsSince210, THIS, FLT),
        LEAF_FN(CarMover_DoSnapshot, THIS, INT, {'r', 0}),
        LEAF_FN(Player_GetPovType, CDECL, INT, {'i', 8}),
    };
    return run(ctx, "camera_deps2_car", spec, fns);
}

NFSU2_DIFF_TEST(camera_deps2_math) {  // matrices / vectors
    static const Spec spec{4, 0x100, {}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(CreateRotationZ, CDECL, PTR, {'p', 1}, {'r', 0}),
        LEAF_FN(D3DX_Thunk3, STD, PTR, {'p', 1}, {'p', 2}, {'p', 3}),
        LEAF_FN(D3DX_Thunk2, STD, PTR, {'p', 1}, {'p', 2}),
        LEAF_FN(RotateZ, CDECL, PTR, {'p', 1}, {'p', 2}, {'r', 0}),
        LEAF_FN(D3DX_Call2, CDECL, PTR, {'p', 1}, {'p', 2}),
        LEAF_FN(Vec_Sub, THIS, PTR, {'p', 2}, {'p', 3}),
        LEAF_FN(Vec_Sub3, CDECL, PTR, {'p', 1}, {'p', 2}, {'p', 3}),
        LEAF_FN(Vec_Sub3, CDECL, PTR, {'p', 1}, {'p', 1}, {'p', 1}),
        LEAF_FN(FloatMax, CDECL, FLT, {'f', 0}, {'f', 0}),
        LEAF_FN(Orient_Copy, THIS, PTR, {'p', 1}),
        LEAF_FN(RouteSpan, THIS, FLT),  // here: +0x38 / +0x3C hold links in the car spec
    };
    return run(ctx, "camera_deps2_math", spec, fns);
}

NFSU2_DIFF_TEST(camera_deps2_alloc) {  // the CRT thunks: blocks come back usable and go back to the heap
    return game::run_diff(ctx, "camera_deps2_alloc", o_Thunk_New, [](game::Rng& r) -> game::Case {
        const uint32_t n = 1 + r.below(4096);
        const game::RegOutAny a = game::call_any(o_Thunk_New, nullptr, &n, 1, false);
        const game::RegOutAny b = game::call_any(NFSU2_ENTRY(Thunk_New), nullptr, &n, 1, false);
        bool ok = a.eax && b.eax && a.popped == b.popped;
        if (ok) {
            std::memset(reinterpret_cast<void*>(a.eax), 0x5A, n);
            std::memset(reinterpret_cast<void*>(b.eax), 0x5A, n);
        }
        const uint32_t pa = a.eax, pb = b.eax;
        const game::RegOutAny da = game::call_any(o_Thunk_Delete, nullptr, &pa, 1, false);
        const game::RegOutAny db = game::call_any(NFSU2_ENTRY(Thunk_Delete), nullptr, &pb, 1, false);
        ok = ok && da.popped == db.popped;
        return ok ? game::Ok() : game::MismatchText("new/delete thunks");
    });
}

NFSU2_DIFF_TEST(camera_deps2_BROKEN) {  // harness self-check: bouncing compared with extended
    static const Spec spec = [] {
        Spec sp{3, 0x2000, {{0, 0x30, 1}, {0, 0x3C, 2}}, {}};
        sp.post = &fill_car;
        return sp;
    }();
    static const std::vector<Fn> fns{Fn("Bouncing_vs_Extended", &o_Car_AreHydraulicsBouncing, NFSU2_ENTRY(Car_AreHydraulicsExtended), THIS, INT)};
    return run(ctx, "camera_deps2_BROKEN", spec, fns);
}
