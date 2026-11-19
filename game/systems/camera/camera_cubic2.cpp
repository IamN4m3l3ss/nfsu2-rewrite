// Camera batch 8 (9), written from the PC machine code: tCubic1D::Update and its two derivative clamps,
// tCubic3D::Update, eCreateAxisRotationMatrix, CameraMover::SetEyeLook, CubicCameraMover::MaybePullBackToSeeChaseCar,
// SelectCarCameraMover::FindAnimationTime / SetDesiredOrientation.
// Several of these end with `fnstsw ax` and never set EAX's high half: it is the caller's (THIS_EAX stubs), and
// callers chain it through their calls, so the calls below pass EAX exactly as the original does.
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
inline X gx(uint32_t addr) { return ld(*reinterpret_cast<const float*>(addr)); }  // fld dword [const]
inline uint8_t ah_of(uint16_t sw) { return static_cast<uint8_t>(sw >> 8); }

// calls into game code with a chosen EAX (thiscall: callee pops its args)
uint32_t this1_eax(uint32_t fn, void* self, uint32_t eax_in, uint32_t a) {
    uint32_t r;
    __asm {
        push a
        mov ecx, self
        mov eax, eax_in
        mov edx, fn
        call edx
        mov r, eax
    }
    return r;
}
uint32_t this3_eax(uint32_t fn, void* self, uint32_t eax_in, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r;
    __asm {
        push c
        push b
        push a
        mov ecx, self
        mov eax, eax_in
        mov edx, fn
        call edx
        mov r, eax
    }
    return r;
}
struct EaxSt {
    uint32_t eax;
    float st0;
};
EaxSt this3_eax_f(uint32_t fn, void* self, uint32_t eax_in, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r;
    float f;
    __asm {
        push c
        push b
        push a
        mov ecx, self
        mov eax, eax_in
        mov edx, fn
        call edx
        mov r, eax
        fstp dword ptr [f]
    }
    return {r, f};
}
EaxSt cdecl1_f(uint32_t fn, uint32_t a) {
    uint32_t r;
    float f;
    __asm {
        push a
        mov edx, fn
        call edx
        add esp, 4
        mov r, eax
        fstp dword ptr [f]
    }
    return {r, f};
}

// ---------------------------------------------------------------- tCubic1D (val +0, dval +4, desired +8 / +0xC,
// coefficients +0x10..+0x1C, time +0x20, duration +0x24, end +0x28, state word +0x2C, flag word +0x2E)

// 0x005748C0 tCubic1D clamp of the first derivative to max (|slope at the start| above max: rescale desired dval)
uint32_t __cdecl Cubic1D_ClampFirstDerivative(uint8_t* s, uint32_t max, uint32_t eax_in) {
    const uint32_t dur = at<uint32_t>(s, 0x24);
    const X inv = divm(gx(0x00784250), dur);
    const X a = mulm(inv, dur);
    const X b = mulm(mulm(a, at<uint32_t>(s, 0x10)), gu(0x00787ADCu));
    const X f14 = ld(at<float>(s, 0x14));
    const X v = addm(mul(add(b, add(f14, f14)), a), at<uint32_t>(s, 0x18));
    const X w = mul(inv, v);
    const X aw = absx(w);
    const uint16_t sw = with_depth(cmpw(aw, ld(bitsf(max))), 2);
    if ((ah_of(sw) & 0x41) == 0) at<float>(s, 0xC) = st(mulm(mulm(div(aw, w), max), at<uint32_t>(s, 0x24)));
    return (eax_in & 0xFFFF0000u) | sw;
}

// 0x00574910 tCubic1D::ClampSecondDerivative(max)
uint32_t __cdecl Cubic1D_ClampSecondDerivative(uint8_t* s, uint32_t max, uint32_t eax_in) {
    const uint32_t k6 = gu(0x007859D0u);
    const X inv = divm(gx(0x00784250), at<uint32_t>(s, 0x24));
    const X f14 = ld(at<float>(s, 0x14));
    const float t2 = st(add(f14, f14));
    float acc = st(mul(mul(addm(mulm(mulm(mul(gx(0x007D7D3C), inv), at<uint32_t>(s, 0x10)), k6), fbits(t2)), inv), inv));
    const float aa = st(absx(ld(acc)));
    const X B = mul(inv, mul(addm(mulm(mulm(mulm(inv, at<uint32_t>(s, 0x24)), at<uint32_t>(s, 0x10)), k6), fbits(t2)), inv));
    const X aB = absx(B);
    bool clamped = false;
    if ((cmp(ld(aa), ld(bitsf(max))) & 0x41) == 0) {
        clamped = true;
        acc = st(mulm(divm(ld(aa), fbits(acc)), max));
    }
    const uint16_t sw = with_depth(cmpw(aB, ld(bitsf(max))), 2);
    X r = B;
    if ((ah_of(sw) & 0x41) == 0) {
        r = mulm(div(aB, B), max);
        clamped = true;
    }
    if (clamped) {
        const X g = ld(at<float>(s, 0x24));
        const X g2 = mul(g, g);
        const X ag = mul(ld(acc), g2);
        at<float>(s, 0x14) = st(mul(gx(0x00784260), ag));
        at<float>(s, 0x10) = st(mulm(sub(mul(g2, r), ag), gu(0x00786D94u)));
    }
    return (eax_in & 0xFFFF0000u) | sw;
}

// 0x005749E0 tCubic1D::Update(dt, max dval, max ddval): state 2 = (re)plan from val/dval to the desired ones (with
// the clamps for maxima > 0), 1 = run; advance time by dt / duration; at the end snap to the desired values.
uint32_t __cdecl Cubic1D_Update(uint8_t* s, uint32_t dt, uint32_t max1, uint32_t max2, uint32_t eax_in) {
    (void)eax_in;  // every path sets EAX before reading it
    const int32_t state = static_cast<int16_t>(at<uint16_t>(s, 0x2C));
    uint32_t eax = static_cast<uint32_t>(state);
    auto sw_in = [&](uint16_t sw) { eax = (eax & 0xFFFF0000u) | sw; return ah_of(sw); };
    if (state == 0) return 0;
    if (state != 1) {
        if (state != 2) return eax - 2;
        eax = 0;
        const bool keep = at<uint16_t>(s, 0x2E) != 0;
        at<uint32_t>(s, 0x20) = 0;
        if (!keep) at<uint16_t>(s, 0x2C) = 1;
        if ((sw_in(cmpw(ld(bitsf(max1)), gx(0x007D7D3C))) & 0x41) == 0)
            eax = this1_eax(0x005748C0, s, max1, max1);
        eax = at<uint32_t>(s, 0);  // mov eax, [ecx] (kept for the [+0x1C] copy)
        const X d = subm(ld(at<float>(s, 8)), at<uint32_t>(s, 0));
        const X e = addm(ld(at<float>(s, 0xC)), at<uint32_t>(s, 4));
        at<uint32_t>(s, 0x1C) = at<uint32_t>(s, 0);
        at<uint32_t>(s, 0x18) = at<uint32_t>(s, 4);
        at<float>(s, 0x10) = st(sub(e, add(d, d)));
        const X f4 = ld(at<float>(s, 4));
        at<float>(s, 0x14) = st(sub(subm(mulm(d, gu(0x00787ADCu)), at<uint32_t>(s, 0xC)), add(f4, f4)));
        if ((sw_in(cmpw(ld(bitsf(max2)), gx(0x007D7D3C))) & 0x41) == 0)
            eax = this1_eax(0x00574910, s, eax, max2);
    }
    if ((sw_in(cmpw(ld(at<float>(s, 0x24)), gx(0x007845A0))) & 0x41) == 0)
        at<float>(s, 0x20) = st(addm(divm(ld(bitsf(dt)), at<uint32_t>(s, 0x24)), at<uint32_t>(s, 0x20)));
    else
        at<uint32_t>(s, 0x20) = 0x3F800000u;
    if ((sw_in(cmpw(ld(at<float>(s, 0x20)), ld(at<float>(s, 0x28)))) & 1) == 0) {
        at<uint16_t>(s, 0x2C) = 0;
        if (sw_in(cmpw(ld(at<float>(s, 0x28)), gx(0x00784250))) & 1) return eax;
        at<uint32_t>(s, 0) = at<uint32_t>(s, 8);
        at<uint32_t>(s, 4) = at<uint32_t>(s, 0xC);
        return at<uint32_t>(s, 8);
    }
    const X t = ld(at<float>(s, 0x20));
    const X a = mulm(t, at<uint32_t>(s, 0x10));
    at<float>(s, 0) = st(addm(mul(addm(mul(addm(a, at<uint32_t>(s, 0x14)), t), at<uint32_t>(s, 0x18)), t), at<uint32_t>(s, 0x1C)));
    const X f14 = ld(at<float>(s, 0x14));
    at<float>(s, 4) = st(addm(mul(add(mulm(a, gu(0x00787ADCu)), add(f14, f14)), t), at<uint32_t>(s, 0x18)));
    return eax;
}

// 0x00574DF0 tCubic3D::Update(dt, max dval, max ddval): the three axes (EAX chained through)
uint32_t __cdecl Cubic3D_Update(uint8_t* s, uint32_t dt, uint32_t m1, uint32_t m2, uint32_t eax_in) {
    uint32_t eax = this3_eax(0x005749E0, s, eax_in, dt, m1, m2);
    eax = this3_eax(0x005749E0, s + 0x30, eax, dt, m1, m2);
    return this3_eax(0x005749E0, s + 0x60, eax, dt, m1, m2);
}

// 0x005BA8B0 eCreateAxisRotationMatrix(out, axis, angle): Rodrigues rotation about a unit axis. EAX = out.
uint32_t __cdecl AxisRotationMatrix(uint32_t op, const float* axis, uint32_t angle) {
    using Trig = float(__cdecl*)(uint32_t);
    const float c = game_fn<Trig>(0x0043C370)(angle);
    const X s = ld(game_fn<Trig>(0x0043C2B0)(angle));
    const uint32_t cb = fbits(c);
    const X t = subm(gx(0x00784250), cb);
    const X x = ld(axis[0]), y = ld(axis[1]);
    const uint32_t zb = fbits(axis[2]);
    float* out = reinterpret_cast<float*>(op);
    const X xt = mul(x, t);
    const float yt = st(mul(y, t));
    out[3] = out[7] = out[11] = 0.0f;
    out[12] = out[13] = out[14] = 0.0f;
    *reinterpret_cast<uint32_t*>(out + 15) = 0x3F800000u;
    out[0] = st(addm(mul(xt, x), cb));
    const X xty = mul(xt, y);
    const X zs = mul(ld(bitsf(zb)), s);
    out[1] = st(sub(xty, zs));
    const float xtz = st(mulm(xt, zb));
    const X ys = mul(y, s);
    const float ysf = st(ys);
    out[2] = st(addm(ys, fbits(xtz)));
    out[4] = st(add(zs, xty));
    out[5] = st(addm(mul(ld(yt), y), cb));
    const X ytz = mulm(ld(yt), zb);
    const X xs = mul(x, s);
    out[6] = st(sub(ytz, xs));
    out[8] = st(subm(ld(xtz), fbits(ysf)));
    out[9] = st(add(xs, ytz));
    out[10] = st(addm(mul(mulm(ld(bitsf(zb)), zb), t), cb));
    return op;
}

// 0x00444B60 CameraMover::SetEyeLook(eye cubic, look cubic, matrix, offset): both cubic inits. EAX = the look's.
uint32_t __cdecl Mover_SetEyeLook(uint8_t* s, uint32_t eye, uint32_t look, uint32_t mat, uint32_t off) {
    using Init = uint32_t(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
    game_fn<Init>(0x00444980)(s, eye, mat, off);
    return game_fn<Init>(0x00444A70)(s, look, mat, off);
}

// 0x00446040 CubicCameraMover::MaybePullBackToSeeChaseCar(matrix): the chase car's point (+0x30 -> +0x60) in the
// matrix's space; pull-back = x - (|xy| / 0.5) / tan(fov / 2) (fov from [+0x34] +8, binary angle), kept in
// [-30, x], becomes the desired value of the cubic [+0x38]. EAX = 0 without a car, else bCos's high half + status.
uint32_t __cdecl Cubic_MaybePullBack(uint8_t* s, uint32_t mat) {
    const uint32_t car = at<uint32_t>(s, 0x30);
    if (!car) return 0;
    alignas(16) float p[4] = {};
    game_fn<uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x0043C9C0)(u(p), mat, car + 0x60);
    const X h = sqrt(add(mulm(ld(p[1]), fbits(p[1])), mulm(ld(p[0]), fbits(p[0]))));
    const float dist = st(divm(h, gu(0x00787214u)));
    const uint16_t ax = static_cast<uint16_t>(ftol(ld(at<float>(ptr(s, 0x34), 8))));
    const uint32_t ang = static_cast<uint16_t>(ax >> 1);
    const float sn = game_fn<float(__cdecl*)(uint32_t)>(0x0043C2B0)(ang);
    const EaxSt cs = cdecl1_f(0x0043C370, ang);
    X v = subrm(divrm(divrm(ld(cs.st0), fbits(sn)), fbits(dist)), fbits(p[0]));
    const float lo = bitsf(fbits(st(gx(0x00787218))) ^ 0x80000000u);
    if (cmp(v, ld(lo)) & 0x41) v = ld(lo);
    if (!parity_even(cmp(ld(p[0]), v) & 5)) v = ld(p[0]);
    uint8_t* cubic = ptr(s, 0x38);
    at<float>(cubic, 8) = st(v);
    const uint16_t sw = cmpw(v, ld(at<float>(cubic, 0)));
    if (parity_even(ah_of(sw) & 0x44)) at<uint16_t>(cubic, 0x2C) = 2;
    return (cs.eax & 0xFFFF0000u) | sw;
}

// 0x0044D190 SelectCarCameraMover::FindAnimationTime(a, b): the longest of |a - b|, the three per-axis times
// (|current - desired| / (frame time * 50), 0 when equal) and the two vector step counts, times 0.0055, into +0x14C
// (at least +0x150); +0x148 = 0. EAX = the second step count's high half + status.
uint32_t __cdecl SelectCar_FindAnimationTime(uint8_t* s, uint32_t a, uint32_t b) {
    const float t0 = st(absx(subm(ld(bitsf(a)), b)));
    auto axis = [&](uint32_t cur, uint32_t des) -> float {
        const X fc = ld(at<float>(s, cur));
        if (!parity_even(cmp(fc, ld(at<float>(s, des))) & 0x44)) return 0.0f;
        const X d = absx(subrm(fc, at<uint32_t>(s, des)));
        if (!parity_even(cmp(d, gx(0x007D7D3C)) & 0x44)) return 0.0f;
        return st(div(d, mulm(gx(0x00865198), gu(0x0078711Cu))));
    };
    const float t1 = axis(0x100, 0xC0);
    const float t2 = axis(0x130, 0xF0);
    const float t3 = axis(0x134, 0xF4);
    const uint32_t steps = 0x004455D0;
    const float k1 = st(mulm(gx(0x007EB5E4), gu(0x00865198u)));
    const float n1 = this3_eax_f(steps, s, u(s) + 0x110, u(s) + 0xD0, u(s) + 0x110, fbits(k1)).st0;
    const float k2 = st(mulm(gx(0x007EB5E4), gu(0x00865198u)));
    const EaxSt r2 = this3_eax_f(steps, s, u(s) + 0xE0, u(s) + 0xE0, u(s) + 0x120, fbits(k2));
    const float n2 = r2.st0;
    X m = ld(t0);
    if ((cmp(ld(t1), ld(t0)) & 0x41) == 0) m = ld(t1);
    for (float v : {n1, n2, t2, t3})
        if ((cmp(ld(v), m) & 0x41) == 0) m = ld(v);
    const X r = mulm(m, gu(0x0078710Cu));
    at<uint32_t>(s, 0x148) = 0;
    at<float>(s, 0x14C) = st(r);
    const uint16_t sw = cmpw(r, ld(at<float>(s, 0x150)));
    if (!parity_even(ah_of(sw) & 5)) at<uint32_t>(s, 0x14C) = at<uint32_t>(s, 0x150);
    return (r2.eax & 0xFFFF0000u) | sw;
}

// 0x00451200 SelectCarCameraMover::SetDesiredOrientation(9 values, a, b): current orientation (+0x80..) becomes the
// start (+0xC0..; middle floats through the x87), the 9 values the target (+0x100..), then FindAnimationTime(a, b).
uint32_t __cdecl SelectCar_SetDesiredOrientation(uint8_t* s, uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4,
                                                  uint32_t v5, uint32_t v6, uint32_t v7, uint32_t v8, uint32_t v9,
                                                  uint32_t a, uint32_t b) {
    at<uint32_t>(s, 0x2C) = 0;
    at<uint32_t>(s, 0xC0) = at<uint32_t>(s, 0x80);
    for (uint32_t row : {0x10u, 0x20u}) {
        const X y = ld(at<float>(s, 0x84 + row)), z = ld(at<float>(s, 0x88 + row));
        at<uint32_t>(s, 0xC0 + row) = at<uint32_t>(s, 0x80 + row);
        at<float>(s, 0xC4 + row) = st(y);
        at<float>(s, 0xC8 + row) = st(z);
    }
    at<uint32_t>(s, 0xF0) = at<uint32_t>(s, 0xB0);
    at<uint32_t>(s, 0xF4) = at<uint32_t>(s, 0xB4);
    at<uint32_t>(s, 0xF8) = at<uint32_t>(s, 0xB8);
    const uint32_t v[9] = {v1, v2, v3, v4, v5, v6, v7, v8, v9};
    const uint32_t off[9] = {0x100, 0x110, 0x114, 0x118, 0x120, 0x124, 0x128, 0x130, 0x134};
    for (int i = 0; i < 9; ++i) at<uint32_t>(s, off[i]) = v[i];
    return game_fn<uint32_t(__thiscall*)(void*, uint32_t, uint32_t)>(0x0044D190)(s, a, b);
}

}  // namespace

LEAF_THIS_EAX(0x005748C0, Cubic1D_ClampFirstDerivative, 1, "void tCubic1D::ClampFirstDerivative(float max)");
LEAF_THIS_EAX(0x00574910, Cubic1D_ClampSecondDerivative, 1, "void tCubic1D::ClampSecondDerivative(float max)");
LEAF_THIS_EAX(0x005749E0, Cubic1D_Update, 3, "void tCubic1D::Update(float dt, float max_dval, float max_ddval)");
LEAF_THIS_EAX(0x00574DF0, Cubic3D_Update, 3, "void tCubic3D::Update(float dt, float max_dval, float max_ddval)");
LEAF_CDECL(0x005BA8B0, AxisRotationMatrix, 3, "bMatrix4* eCreateAxisRotationMatrix(bMatrix4* out, const bVector3* axis, u16 angle)");
LEAF_THIS(0x00444B60, Mover_SetEyeLook, 4, "void CameraMover::SetEyeLook(tCubic3D* eye, tCubic3D* look, const bMatrix4*, const bVector3*)");
LEAF_THIS(0x00446040, Cubic_MaybePullBack, 1, "void CubicCameraMover::MaybePullBackToSeeChaseCar(const bMatrix4*)");
LEAF_THIS(0x0044D190, SelectCar_FindAnimationTime, 2, "void SelectCarCameraMover::FindAnimationTime(float, float)");
LEAF_THIS(0x00451200, SelectCar_SetDesiredOrientation, 11, "void SelectCarCameraMover::SetDesiredOrientation(9 values, float, float)");

NFSU2_DIFF_TEST(camera_cubic2_1d) {  // 0 a tCubic1D (state 0..3, flag 0/1), or three of them (a tCubic3D)
    static const Spec spec{1, 0x100, {}, {{0, 0x2C, 2, 4}, {0, 0x2E, 2, 2}, {0, 0x5C, 2, 4}, {0, 0x5E, 2, 2}, {0, 0x8C, 2, 4}, {0, 0x8E, 2, 2}}};
    static const std::vector<Fn> fns{
        LEAF_FN(Cubic1D_ClampFirstDerivative, THIS, INT, {'f', 0}),
        LEAF_FN(Cubic1D_ClampSecondDerivative, THIS, INT, {'f', 0}),
        LEAF_FN(Cubic1D_Update, THIS, INT, {'f', 0}, {'f', 0}, {'f', 0}),
        LEAF_FN(Cubic3D_Update, THIS, INT, {'f', 0}, {'f', 0}, {'f', 0}),
    };
    return run(ctx, "camera_cubic2_1d", spec, fns);
}

NFSU2_DIFF_TEST(camera_cubic2_mover) {
    // 0 a mover: camera [+0x1C] (1), chase car [+0x30] (2, may be 0), fov holder [+0x34] (3), cubic [+0x38] (4);
    // 5 / 6 cubics, 7 a matrix, 8 an offset / axis
    static const Spec spec{9, 0x300, {{0, 0x1C, 1}, {0, 0x30, 2, true}, {0, 0x34, 3}, {0, 0x38, 4}}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(AxisRotationMatrix, CDECL, PTR, {'p', 7}, {'p', 8}, {'r', 0}),
        LEAF_FN(AxisRotationMatrix, CDECL, PTR, {'p', 8}, {'p', 8}, {'i', 0x10000}),
        LEAF_FN(Mover_SetEyeLook, THIS, INT, {'p', 5}, {'p', 6}, {'p', 7}, {'p', 8}),
        LEAF_FN(Mover_SetEyeLook, THIS, INT, {'p', 5}, {'p', 6}, {'k', 0}, {'k', 0}),
        LEAF_FN(Cubic_MaybePullBack, THIS, INT, {'p', 7}),
    };
    return run(ctx, "camera_cubic2_mover", spec, fns);
}

NFSU2_DIFF_TEST(camera_cubic2_selectcar) {  // 0 a SelectCarCameraMover (orientations +0x80..+0x150)
    static const Spec spec{1, 0x200, {}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(SelectCar_FindAnimationTime, THIS, INT, {'f', 0}, {'f', 0}),
        LEAF_FN(SelectCar_SetDesiredOrientation, THIS, INT, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0},
                {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}),
    };
    return run(ctx, "camera_cubic2_selectcar", spec, fns);
}

NFSU2_DIFF_TEST(camera_cubic2_BROKEN) {  // harness self-check: the first-derivative clamp compared with the second
    static const Spec spec{1, 0x100, {}, {}};
    static const std::vector<Fn> fns{Fn("Clamp1_vs_Clamp2", &o_Cubic1D_ClampFirstDerivative, NFSU2_ENTRY(Cubic1D_ClampSecondDerivative), THIS, INT, {'f', 0})};
    return run(ctx, "camera_cubic2_BROKEN", spec, fns);
}
