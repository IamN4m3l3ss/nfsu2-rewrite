// bInvertMatrix 0x0043C7C0 (8 callers): affine 4x4 inverse — 3x3 by cofactors / determinant, translation
// -(R^-1 t), last column (0,0,0,1). Body lifted by tools/re/x87lift.py (exact x87 operation order); diff-tested.
#include <string>

#include "../diff.h"
#include "../lift.h"
#include "../regs.h"
#include "../rewrite.h"

namespace {
struct M4 { float m[16]; };
// lifted from 0x43c7c0 by tools/re/x87lift.py — verify with a diff test
// at ret: fpu stack depth 0 (st0 = -); eax=('ptr', 'p0') ecx=('val', '0u') edx=('i', 't12')
static void bInvertMatrix_lifted(uint32_t a0, uint32_t a1) {
    using namespace lift;
    const float a0f = bitsf(a0); (void)a0f;
    const float a1f = bitsf(a1); (void)a1f;
    uint8_t* const p0 = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(a0));
    uint8_t* const p1 = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(a1));
    const float t0 = ldf(p1, 0x0);
    const uint32_t t1 = ldi(p1, 0x14);
    const float t2 = ldf(p1, 0x4);
    const uint32_t t3 = ldi(p1, 0x10);
    const float t4 = ldf(p1, 0x8);
    const float t5 = ldf(p1, 0x18);
    const uint32_t t6 = ldi(p1, 0x24);
    const float t7 = ldf(p1, 0x28);
    const float t8 = bitsf(t1) * t7;
    const uint32_t t9 = ldi(p1, 0x20);
    const float t10 = bitsf(t6) * t5;
    const uint32_t t11 = ldi(p1, 0x30);
    const uint32_t t12 = ldi(p1, 0x34);
    const uint32_t t13 = ldi(p1, 0x38);
    const float t14 = t8 - t10;
    const float t15 = t7 * bitsf(t3);
    sti(p0, 0xc, 0u);
    const float t16 = bitsf(t9) * t5;
    const float t17 = t15 - t16;
    const float t18 = bitsf(t6) * bitsf(t3);
    const float t19 = bitsf(t9) * bitsf(t1);
    const float t20 = t18 - t19;
    const float t21 = t14 * t0;
    const float t22 = t17 * t2;
    const float t23 = t21 - t22;
    const float t24 = t20 * t4;
    const float t25 = t23 + t24;
    const float t26 = f32(0x3F800000u) /* 1.0 @ 0x784250 */ / t25;
    const float t27 = t14 * t26;
    stf(p0, 0x0, t27);
    const float t28 = t7 * t2;
    const float t29 = bitsf(t6) * t4;
    const float t30 = t28 - t29;
    const float t31 = t30 * t26;
    const float t32 = fchs(t31);
    stf(p0, 0x4, t32);
    const float t33 = t5 * t2;
    const float t34 = bitsf(t1) * t4;
    const float t35 = t33 - t34;
    const float t36 = t35 * t26;
    stf(p0, 0x8, t36);
    const float t37 = t17 * t26;
    const float t38 = fchs(t37);
    stf(p0, 0x10, t38);
    const float t39 = t7 * t0;
    const float t40 = bitsf(t9) * t4;
    const float t41 = t39 - t40;
    sti(p0, 0x1c, 0u);
    sti(p0, 0x2c, 0u);
    sti(p0, 0x3c, 0x3f800000u);
    const float t42 = t41 * t26;
    stf(p0, 0x14, t42);
    const float t43 = t5 * t0;
    const float t44 = bitsf(t3) * t4;
    const float t45 = t43 - t44;
    const float t46 = t45 * t26;
    const float t47 = fchs(t46);
    stf(p0, 0x18, t47);
    const float t48 = t20 * t26;
    stf(p0, 0x20, t48);
    const float t49 = bitsf(t6) * t0;
    const float t50 = bitsf(t9) * t2;
    const float t51 = t49 - t50;
    const float t52 = t51 * t26;
    const float t53 = fchs(t52);
    stf(p0, 0x24, t53);
    const float t54 = t0 * bitsf(t1);
    const float t55 = bitsf(t3) * t2;
    const float t56 = t54 - t55;
    const float t57 = t56 * t26;
    stf(p0, 0x28, t57);
    const float t58 = t48 * bitsf(t13);
    const float t59 = t38 * bitsf(t12);
    const float t60 = t58 + t59;
    const float t61 = t27 * bitsf(t11);
    const float t62 = t60 + t61;
    const float t63 = fchs(t62);
    stf(p0, 0x30, t63);
    const float t64 = t53 * bitsf(t13);
    const float t65 = t42 * bitsf(t12);
    const float t66 = t64 + t65;
    const float t67 = t32 * bitsf(t11);
    const float t68 = t66 + t67;
    const float t69 = fchs(t68);
    stf(p0, 0x34, t69);
    const float t70 = t57 * bitsf(t13);
    const float t71 = t47 * bitsf(t12);
    const float t72 = t70 + t71;
    const float t73 = t36 * bitsf(t11);
    const float t74 = t72 + t73;
    const float t75 = fchs(t74);
    stf(p0, 0x38, t75);
}

using Inv_t = M4*(__cdecl*)(M4*, const M4*);
Inv_t o_bInvertMatrix = nullptr;
M4* __cdecl bInvertMatrix(M4* out, const M4* in) {
    bInvertMatrix_lifted(game::arg(out), game::arg(const_cast<M4*>(in)));
    return out;
}

float rf(game::Rng& r, float scale) {
    const float v = (static_cast<float>(r.next() & 0xFFFFFF) / 16777216.0f - 0.5f) * 2.0f * scale;
    return v;
}
void random_affine(game::Rng& r, M4& m) {
    for (float& f : m.m) f = 0.0f;
    // rotation-ish 3x3 (random, sometimes near singular), translation, w = (0,0,0,1)
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 3; ++c) m.m[row * 4 + c] = rf(r, 1.0f);
    if (r.below(8) == 0)  // nearly singular: row 2 ~ row 0
        for (int c = 0; c < 3; ++c) m.m[8 + c] = m.m[c] + rf(r, 1e-4f);
    if (r.below(16) == 0)  // exactly singular
        for (int c = 0; c < 3; ++c) m.m[8 + c] = m.m[c];
    for (int c = 0; c < 3; ++c) m.m[12 + c] = rf(r, r.below(2) ? 10.0f : 5000.0f);
    m.m[15] = 1.0f;
    if (r.below(8) == 0)
        for (int c = 0; c < 4; ++c) m.m[c * 4 + 3] = rf(r, 1.0f);  // garbage in the w column (ignored? check)
}
}  // namespace

NFSU2_REWRITE_N(0x0043C7C0, bInvertMatrix, 2, "bMatrix4* __cdecl(bMatrix4* out, const bMatrix4* in) -> out in EAX", o_bInvertMatrix);

NFSU2_DIFF_TEST(bInvertMatrix) {
    return game::run_diff(ctx, "bInvertMatrix", o_bInvertMatrix, [](game::Rng& r) {
        M4 in, a, b;
        random_affine(r, in);
        for (int i = 0; i < 16; ++i) a.m[i] = b.m[i] = rf(r, 3.0f);
        if (r.below(4) == 0) {
            a = b = in;
            o_bInvertMatrix(&a, &a);
            bInvertMatrix(&b, &b);
        } else {
            o_bInvertMatrix(&a, &in);
            bInvertMatrix(&b, &in);
        }
        int i = 0;
        while (i < 16 && game::same_float(a.m[i], b.m[i])) ++i;
        if (i == 16) return game::Ok();
        char buf[160];
        snprintf(buf, sizeof buf, "element %d: orig %.9g (0x%08x) mine %.9g (0x%08x)", i, a.m[i], game::bits(a.m[i]), b.m[i], game::bits(b.m[i]));
        return game::MismatchText(buf);
    });
}

NFSU2_DIFF_TEST(regs_binvert) {
    using game::arg;
    return game::run_diff(ctx, "regs_binvert", o_bInvertMatrix, [](game::Rng& r) {
        M4 in, out;
        random_affine(r, in);
        return game::check_regs("bInvertMatrix", (void*)o_bInvertMatrix, NFSU2_ENTRY(bInvertMatrix), {arg(&out), arg(&in)}, false);
    });
}
