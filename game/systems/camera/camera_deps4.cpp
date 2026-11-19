// Camera deps batch 4 (7), written from the PC machine code: CubicCameraMover::HighliteMode, a key-recorder append
// (0x447970), UIWidgetMenu::Setup (empty), CAnimScene::IsPlaying, CollisionBody::DisableCollisions,
// FuturePoint::GetFixup, a per-component clamp (0x443F70).
// Virtual calls in tests: the fake object's vtable slot +0x24 points at CAnimScene::IsPlaying, a real side-effect-
// free bool getter (it reads the object's +0x28), so both runs take the same paths.
#include "leaf.h"
#include "x87.h"

namespace {
using namespace x87;

inline uint32_t u_(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }

// 0x004307D0 CAnimScene::IsPlaying: state +0x28 not 0 / 1
uint32_t __cdecl AnimScene_IsPlaying(uint8_t* s) {
    const uint32_t v = at<uint32_t>(s, 0x28);
    return (v != 0 && v != 1) ? 1u : 0u;
}

// 0x00445AC0 CubicCameraMover::HighliteMode: the vtable's +0x24 bool, then the car [+0x2C] hydraulics bouncing /
// extended, or (car has [+0x38]) its +0x424 <= 2
uint32_t __cdecl Cubic_HighliteMode(uint8_t* s) {
    const uint32_t vfn = gu(at<uint32_t>(s, 0) + 0x24);
    if (!(reinterpret_cast<uint32_t(__thiscall*)(void*)>(vfn)(s) & 0xFF)) return 0;
    using Pred = uint32_t(__thiscall*)(void*);
    if (game_fn<Pred>(0x005ED290)(ptr(s, 0x2C)) & 0xFF) return 1;
    if (game_fn<Pred>(0x005ED300)(ptr(s, 0x2C)) & 0xFF) return 1;
    uint8_t* car = ptr(s, 0x2C);
    if (!at<uint32_t>(car, 0x38)) return 0;
    return at<int32_t>(car, 0x424) > 2 ? 0u : 1u;
}

// 0x00447970 append one dword ([p]) to the buffer [+0xC4] at write offset +0xCC (wrapping back to +0xC8 when
// p's word +2 - start + offset exceeds 0x1000), flag +0x2B = 1. EAX = the new offset.
uint32_t __cdecl Recorder_Append(uint8_t* s, const uint8_t* p) {
    s[0x2B] = 1;
    const int32_t e = static_cast<int32_t>(at<uint16_t>(p, 2)) - at<int32_t>(s, 0xC8) + at<int32_t>(s, 0xCC);
    if (e > 0x1000) at<uint32_t>(s, 0xCC) = at<uint32_t>(s, 0xC8);
    at<uint32_t>(at<uint32_t>(s, 0xC4) + at<uint32_t>(s, 0xCC), 0) = at<uint32_t>(p, 0);
    return at<uint32_t>(s, 0xCC) += 4;
}

// 0x004022C0 UIWidgetMenu::Setup: empty. EAX untouched.
uint32_t __cdecl UIWidgetMenu_Setup(uint32_t eax_in) { return eax_in; }

// 0x005930F0 CollisionBody::DisableCollisions: ++byte +0x17. EAX untouched.
uint32_t __cdecl CollisionBody_DisableCollisions(uint8_t* s, uint32_t eax_in) {
    ++s[0x17];
    return eax_in;
}

// 0x00401C40 FuturePoint::GetFixup(i): max(+0x14, +0x16 as u16 << 8, times 0x784278) times signed byte +0x22+i
// << 9 times the same scale
float __cdecl FuturePoint_GetFixup(const uint8_t* s, uint32_t i) {
    const uint32_t k = gu(0x00784278u);
    const X a = mulm(ild(static_cast<int32_t>(at<uint16_t>(s, 0x16)) << 8), k);
    const X b = mulm(ild(static_cast<int32_t>(at<uint16_t>(s, 0x14)) << 8), k);
    const float bf = st(b);
    X m = a;
    if ((cmp(b, a) & 0x41) == 0) m = ld(bf);
    const X c = mulm(ild(static_cast<int32_t>(static_cast<int8_t>(s[i + 0x22])) << 9), k);
    return st(mul(m, c));
}

// 0x00443F70 clamp(v, lo, hi): each component of v to [lo, hi] (max with lo first, then min with hi; NaN keeps the
// bound / the value as the original's compares do), in place. EAX = v.
uint32_t __cdecl ClampVector(float* v, const float* lo, const float* hi) {
    X m[3];
    for (int i = 0; i < 3; ++i) {
        const X l = ld(lo[i]);
        const uint32_t h = *reinterpret_cast<const uint32_t*>(&hi[i]);
        const float vf = st(ld(v[i]));
        const X vv = ld(v[i]);
        m[i] = (cmp(vv, l) & 0x41) == 0 ? ld(vf) : l;
        float hf;
        std::memcpy(&hf, &h, 4);
        if (!parity_even(cmp(ld(hf), m[i]) & 5)) m[i] = ld(hf);
    }
    for (int i = 0; i < 3; ++i) v[i] = st(m[i]);
    return u_(v);
}

void fake_vtable(Graph& g) {  // block 0's vtable -> block 3, whose slot +0x24 = CAnimScene::IsPlaying
    at<uint32_t>(g.blk[3].data(), 0x24) = 0x004307D0u;
    at<uint8_t*>(g.blk[0].data(), 0) = g.blk[3].data();
}

}  // namespace

LEAF_THIS(0x004307D0, AnimScene_IsPlaying, 0, "bool CAnimScene::IsPlaying()");
LEAF_THIS(0x00445AC0, Cubic_HighliteMode, 0, "bool CubicCameraMover::HighliteMode()");
LEAF_THIS(0x00447970, Recorder_Append, 1, "void (append a dword to the +0xC4 buffer)");
LEAF_CDECL_EAX(0x004022C0, UIWidgetMenu_Setup, 0, "void UIWidgetMenu::Setup()");
LEAF_THIS_EAX(0x005930F0, CollisionBody_DisableCollisions, 0, "void CollisionBody::DisableCollisions()");
LEAF_THIS(0x00401C40, FuturePoint_GetFixup, 1, "float FuturePoint::GetFixup(int)");
LEAF_CDECL(0x00443F70, ClampVector, 3, "bVector3* bClamp(bVector3* v, const bVector3* lo, const bVector3* hi)");

NFSU2_DIFF_TEST(camera_deps4_mover) {
    // 0 a cubic mover (vtable -> 3, car [+0x2C] -> 1 whose +0x30 -> 2, [+0x38] may be 0); state +0x28 for IsPlaying
    static const Spec spec = [] {
        Spec sp{4, 0x500, {{0, 0x2C, 1}, {1, 0x30, 2}, {1, 0x38, 2, true}},
                {{0, 0x28, 4, 3}, {1, 0x424, 4, 5}}};
        sp.post = &fake_vtable;
        return sp;
    }();
    static const std::vector<Fn> fns{
        LEAF_FN(Cubic_HighliteMode, THIS, INT),
        LEAF_FN(AnimScene_IsPlaying, THIS, INT),
        LEAF_FN(CollisionBody_DisableCollisions, THIS, INT),
    };
    return run(ctx, "camera_deps4_mover", spec, fns);
}

NFSU2_DIFF_TEST(camera_deps4_misc) {
    // 0 a recorder (buffer [+0xC4] -> 1 (0x1200 bytes), start +0xC8 / offset +0xCC below 0x800), 2 / 3 vectors
    static const Spec spec{4, 0x1200, {{0, 0xC4, 1}}, {{0, 0xC8, 4, 0x800}, {0, 0xCC, 4, 0x800}}};
    // (GetFixup runs with this = block 0: its bytes +0x14..+0x41 hold no links)
    static const std::vector<Fn> fns{
        LEAF_FN(Recorder_Append, THIS, INT, {'p', 2}),
        LEAF_FN(UIWidgetMenu_Setup, CDECL, INT),
        LEAF_FN(FuturePoint_GetFixup, THIS, FLT, {'i', 32}),  // on the link-free block 3
        LEAF_FN(ClampVector, CDECL, PTR, {'p', 2}, {'p', 3}, {'p', 1}),
        LEAF_FN(ClampVector, CDECL, PTR, {'p', 2}, {'p', 2}, {'p', 3}),
    };
    return run(ctx, "camera_deps4_misc", spec, fns);
}

NFSU2_DIFF_TEST(camera_deps4_BROKEN) {  // harness self-check: IsPlaying compared with HighliteMode
    static const Spec spec = [] {
        Spec sp{4, 0x500, {{0, 0x2C, 1}, {1, 0x30, 2}, {1, 0x38, 2, true}}, {{0, 0x28, 4, 3}, {1, 0x424, 4, 5}}};
        sp.post = &fake_vtable;
        return sp;
    }();
    static const std::vector<Fn> fns{Fn("IsPlaying_vs_Highlite", &o_AnimScene_IsPlaying, NFSU2_ENTRY(Cubic_HighliteMode), THIS, INT)};
    return run(ctx, "camera_deps4_BROKEN", spec, fns);
}
