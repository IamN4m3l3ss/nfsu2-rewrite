// Camera batch 6 (10), written from the PC machine code: camera mover constructors / destructors (rear-view mirror,
// autopilot, track route editor, select car), CubicCameraMover::Enable, the select-car / showcase 9-argument
// setters, CameraMoverRestartRace.
// Constructors take a view INDEX and look it up in the game's view table (0x832DE0 + index * 0x70): the tests pick
// the index whose slot lands on a test block that holds a fake view (spec post-hook + arg kind 'V'), so the real
// views are never touched.
#include "leaf.h"
#include "x87.h"

namespace {
using namespace x87;

using Ctor = uint32_t(__thiscall*)(void*, uint32_t, uint32_t);  // CameraMover::CameraMover(view, type)
inline uint32_t base_ctor(uint8_t* s, uint32_t view, uint32_t type) { return game_fn<Ctor>(0x0044AB90)(s, view, type); }
inline bool camera_frozen() { return gu(0x00828640u) != 0; }  // Camera::StopUpdating

// shared destructor tail: CameraMover's (disable + detach, the type-2 count). EAX = 0 or the detach's.
uint32_t mover_dtor(uint8_t* s) {
    at<uint32_t>(s, 0) = 0x00787CF0;
    uint32_t r = at<uint32_t>(s, 0x14);
    if (r) {
        uint8_t* cam = ptr(s, 0x1C);
        at<uint32_t>(s, 0x14) = 0;
        at<uint32_t>(s, 0x24) = at<uint32_t>(cam, 0x280);
        at<uint32_t>(s, 0x28) = at<uint32_t>(cam, 0x284);
        r = game_fn<uint32_t(__thiscall*)(void*, uint32_t)>(0x0048F760)(ptr(s, 0x18), u(s));
    }
    if (at<uint32_t>(s, 0xC) == 2) at<uint32_t>(0x00883DFCu, 0) -= 1;
    return r;
}

// 0x0044CDB0 SelectCarCameraMover::~SelectCarCameraMover
uint32_t __cdecl SelectCar_dtor(uint8_t* s) { return mover_dtor(s); }
// 0x0044DC50 TrackRouteEditorCameraMover::~TrackRouteEditorCameraMover (its own vtable first; the menu teardown
// UIWidgetMenu::Setup 0x4022C0 is an empty function on PC)
uint32_t __cdecl RouteEditor_dtor(uint8_t* s) {
    at<uint32_t>(s, 0) = 0x00787ED8;
    return mover_dtor(s);
}

// 0x00445A50 CubicCameraMover::Enable: CameraMover::Enable, then the cubic mover's enabled byte +0x54
uint32_t __cdecl Cubic_Enable(uint8_t* s) {
    const uint32_t r = game_fn<uint32_t(__thiscall*)(void*)>(0x00444040)(s);
    s[0x54] = 1;
    return r;
}

// 0x0044BA10 RearViewMirrorCameraMover::RearViewMirrorCameraMover(view, x): type 6, +0x2C = x, camera +0x280 = x
// and +0x284 = 0 (each unless the camera is frozen). EAX = this.
uint32_t __cdecl RearView_ctor(uint8_t* s, uint32_t view, uint32_t x) {
    base_ctor(s, view, 6);
    at<uint32_t>(s, 0) = 0x00787D98;
    at<uint32_t>(s, 0x2C) = x;
    if (!camera_frozen()) {
        at<uint32_t>(ptr(s, 0x1C), 0x280) = x;
        if (!camera_frozen()) at<uint32_t>(ptr(s, 0x1C), 0x284) = 0;
    }
    return u(s);
}
// 0x0044ED50 AutoPilotCameraMover::AutoPilotCameraMover(view): type 0x11, camera +0x280 = 0 (unless frozen)
uint32_t __cdecl AutoPilot_ctor(uint8_t* s, uint32_t view) {
    base_ctor(s, view, 0x11);
    at<uint32_t>(s, 0) = 0x00787FC8;
    if (!camera_frozen()) at<uint32_t>(ptr(s, 0x1C), 0x280) = 0;
    return u(s);
}
// 0x0044DBF0 TrackRouteEditorCameraMover::TrackRouteEditorCameraMover(view, ?): type 3 (its menu setup
// UIWidgetMenu::Setup is empty on PC)
uint32_t __cdecl RouteEditor_ctor(uint8_t* s, uint32_t view, uint32_t) {
    base_ctor(s, view, 3);
    at<uint32_t>(s, 0) = 0x00787ED8;
    return u(s);
}
// 0x0044CC30 SelectCarCameraMover::SelectCarCameraMover(view): type 0xB, active +0x2C = 1, the orientation block
// +0x40 and its three copies (+0x80 / +0xC0 / +0x100) cleared, zoom +0x150 = 0x787114, speeds and state cleared
uint32_t __cdecl SelectCar_ctor(uint8_t* s, uint32_t view) {
    base_ctor(s, view, 0xB);
    at<uint32_t>(s, 0) = 0x00787E38;
    at<uint32_t>(s, 0x2C) = 1;
    at<uint32_t>(s, 0x30) = 0;
    for (uint32_t i = 0; i < 16; ++i) at<uint32_t>(s, 0x40 + i * 4) = 0;
    for (uint32_t base : {0x80u, 0xC0u, 0x100u})
        for (uint32_t off : {0x0u, 0x10u, 0x14u, 0x18u, 0x20u, 0x24u, 0x28u, 0x30u, 0x34u, 0x38u}) at<uint32_t>(s, base + off) = 0;
    at<uint32_t>(s, 0x150) = gu(0x00787114u);
    at<uint32_t>(s, 0x140) = 0;
    at<uint32_t>(s, 0x144) = 0;
    at<uint32_t>(s, 0x148) = 0;
    at<uint32_t>(s, 0x14C) = 0;
    s[0x154] = 0;
    return u(s);
}

// 0x004463C0 ShowcaseCameraMover::SetShowcaseCameraParams(9 values): +0x2C/+0x30/+0x34, +0x40/+0x44/+0x48,
// +0x50/+0x54/+0x58. EAX = the last.
uint32_t __cdecl Showcase_SetParams(uint8_t* s, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                                    uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9) {
    const uint32_t v[9] = {a1, a2, a3, a4, a5, a6, a7, a8, a9};
    const uint32_t off[9] = {0x2C, 0x30, 0x34, 0x40, 0x44, 0x48, 0x50, 0x54, 0x58};
    for (int i = 0; i < 9; ++i) at<uint32_t>(s, off[i]) = v[i];
    return a9;
}
// 0x00445500 SelectCarCameraMover::SetDefaultOrientation(9 values): the default orientation +0x40 (rows +0x40,
// +0x50.., +0x60.., +0x70..), copied to the current one +0x80 (middle floats through the x87). EAX = +0x78.
uint32_t __cdecl SelectCar_SetDefaultOrientation(uint8_t* s, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                                                 uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9) {
    const uint32_t v[9] = {a1, a2, a3, a4, a5, a6, a7, a8, a9};
    const uint32_t off[9] = {0x40, 0x50, 0x54, 0x58, 0x60, 0x64, 0x68, 0x70, 0x74};
    for (int i = 0; i < 9; ++i) at<uint32_t>(s, off[i]) = v[i];
    at<uint32_t>(s, 0x80) = at<uint32_t>(s, 0x40);
    const X y = ld(at<float>(s, 0x54)), z = ld(at<float>(s, 0x58));
    at<uint32_t>(s, 0x90) = at<uint32_t>(s, 0x50);
    at<float>(s, 0x94) = st(y);
    at<float>(s, 0x98) = st(z);
    const X y2 = ld(at<float>(s, 0x64)), z2 = ld(at<float>(s, 0x68));
    at<uint32_t>(s, 0xA0) = at<uint32_t>(s, 0x60);
    at<float>(s, 0xA4) = st(y2);
    at<float>(s, 0xA8) = st(z2);
    at<uint32_t>(s, 0xB0) = at<uint32_t>(s, 0x70);
    at<uint32_t>(s, 0xB4) = at<uint32_t>(s, 0x74);
    at<uint32_t>(s, 0xB8) = at<uint32_t>(s, 0x78);
    return at<uint32_t>(s, 0x78);
}

// 0x00443D10 CameraMoverRestartRace: clear the restart flag 0x828650. EAX untouched.
uint32_t __cdecl RestartRace(uint32_t eax_in) {
    *reinterpret_cast<volatile uint32_t*>(0x00828650u) = 0;
    return eax_in;
}

// post-hook: block 2 becomes a fake view on its view-table slot: camera +0x40 = block 1, empty mover list +0x44
void fake_view(Graph& g) {
    Blk& v = g.blk[2];
    const uint32_t a = u(v.data());
    const uint32_t delta = (0x70u - (a - 0x00832DE0u) % 0x70u) % 0x70u;  // inside the guard gap
    v.p += delta;
    std::memset(v.p - 0x100, 0, v.n + 0x200);  // the compared window, the same in both runs (the shift differs)
    at<uint8_t*>(v.p, 0x40) = g.blk[1].data();
    at<uint8_t*>(v.p, 0x44) = v.p + 0x44;
    at<uint8_t*>(v.p, 0x48) = v.p + 0x44;
}

}  // namespace

LEAF_THIS(0x0044CDB0, SelectCar_dtor, 0, "SelectCarCameraMover::~SelectCarCameraMover()");
LEAF_THIS(0x0044DC50, RouteEditor_dtor, 0, "TrackRouteEditorCameraMover::~TrackRouteEditorCameraMover()");
LEAF_THIS(0x00445A50, Cubic_Enable, 0, "void CubicCameraMover::Enable()");
LEAF_THIS(0x0044BA10, RearView_ctor, 2, "RearViewMirrorCameraMover::RearViewMirrorCameraMover(int view, int)");
LEAF_THIS(0x0044ED50, AutoPilot_ctor, 1, "AutoPilotCameraMover::AutoPilotCameraMover(int view)");
LEAF_THIS(0x0044DBF0, RouteEditor_ctor, 2, "TrackRouteEditorCameraMover::TrackRouteEditorCameraMover(int view, ?)");
LEAF_THIS(0x0044CC30, SelectCar_ctor, 1, "SelectCarCameraMover::SelectCarCameraMover(int view)");
LEAF_THIS(0x004463C0, Showcase_SetParams, 9, "void ShowcaseCameraMover::SetShowcaseCameraParams(9 values)");
LEAF_THIS(0x00445500, SelectCar_SetDefaultOrientation, 9, "void SelectCarCameraMover::SetDefaultOrientation(9 values)");
LEAF_CDECL_EAX(0x00443D10, RestartRace, 0, "void CameraMoverRestartRace()");

NFSU2_DIFF_TEST(camera_movers1_ctor) {
    // 0 the mover being built, 1 the camera, 2 a fake view on its table slot (post-hook)
    static const Spec spec = [] {
        Spec sp{3, 0x300, {}, {}};
        sp.post = &fake_view;
        return sp;
    }();
    static const std::vector<Fn> fns{
        LEAF_FN(RearView_ctor, THIS, PTR, {'V', 2}, {'r', 0}),
        LEAF_FN(AutoPilot_ctor, THIS, PTR, {'V', 2}),
        LEAF_FN(RouteEditor_ctor, THIS, PTR, {'V', 2}, {'r', 0}),
        LEAF_FN(SelectCar_ctor, THIS, PTR, {'V', 2}),
    };
    return run(ctx, "camera_movers1_ctor", spec, fns);
}

NFSU2_DIFF_TEST(camera_movers1_mover) {
    // 0 a mover: view [+0x18] (1, list head +0x44 -> 4), camera [+0x1C] (2), its link +4 -> 3 / +8 -> 5; enabled
    // +0x14; type +0xC 0/1 (type 2 counts in a game global)
    static const Spec spec{6, 0x300, {{0, 0x18, 1}, {0, 0x1C, 2}, {1, 0x44, 4}, {0, 4, 3}, {0, 8, 5}},
                           {{0, 0x14, 4, 2}, {0, 0xC, 4, 2}}};
    static const std::vector<Fn> fns{
        LEAF_FN(SelectCar_dtor, THIS, INT),
        LEAF_FN(RouteEditor_dtor, THIS, INT),
        LEAF_FN(Cubic_Enable, THIS, INT),
        LEAF_FN(Showcase_SetParams, THIS, INT, {'r', 0}, {'r', 0}, {'r', 0}, {'r', 0}, {'r', 0}, {'r', 0}, {'r', 0}, {'r', 0}, {'r', 0}),
        LEAF_FN(SelectCar_SetDefaultOrientation, THIS, INT, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}, {'f', 0}),
    };
    return run(ctx, "camera_movers1_mover", spec, fns);
}

NFSU2_DIFF_TEST(camera_movers1_restart) {  // the flag 0x828650 is saved and restored around every case
    return game::run_diff(ctx, "camera_movers1_restart", o_RestartRace, [](game::Rng& r) -> game::Case {
        volatile uint32_t& flag = *reinterpret_cast<volatile uint32_t*>(0x00828650u);
        const uint32_t saved = flag;
        const uint32_t v = r.next();
        flag = v;
        const game::RegOutAny x = game::call_any(o_RestartRace, nullptr, nullptr, 0, false);
        const uint32_t fa = flag;
        flag = v;
        const game::RegOutAny y = game::call_any(NFSU2_ENTRY(RestartRace), nullptr, nullptr, 0, false);
        const uint32_t fb = flag;
        flag = saved;
        if (fa != fb || x.eax != y.eax || x.popped != y.popped || (x.ecx == game::kSentECX && y.ecx != game::kSentECX))
            return game::MismatchText("RestartRace");
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(camera_movers1_BROKEN) {  // harness self-check: AutoPilot constructor compared with SelectCar's
    static const Spec spec = [] {
        Spec sp{3, 0x300, {}, {}};
        sp.post = &fake_view;
        return sp;
    }();
    static const std::vector<Fn> fns{Fn("AutoPilot_vs_SelectCar", &o_AutoPilot_ctor, NFSU2_ENTRY(SelectCar_ctor), THIS, PTR, {'V', 2})};
    return run(ctx, "camera_movers1_BROKEN", spec, fns);
}
