// Camera batch 5 (5): attaching camera movers to views and their enable / disable / construction, written from the
// PC machine code. A view keeps its movers in a doubly linked list (head at view +0x44, a mover's link at mover +4:
// next +0, prev +4); every change recomputes the view mode (eUpdateViewMode 0x5C2120).
// Tests use fake views only (the real ones belong to the running game); the constructor's view path (which links
// into a real view) is covered by its callers in play, not by the unit test.
#include "leaf.h"
#include "live.h"

namespace {

inline uint32_t update_view_mode() { return game_fn<uint32_t(__cdecl*)()>(0x005C2120)(); }

// 0x0048F730 eView::AttachCameraMover(mover): push its link to the front of the view's list. EAX = eUpdateViewMode's.
uint32_t __cdecl View_AttachCameraMover(uint8_t* v, uint32_t m) {
    uint8_t* link = m ? reinterpret_cast<uint8_t*>(m) + 4 : nullptr;
    uint8_t* head = v + 0x44;
    uint8_t* first = ptr(head, 0);
    at<uint8_t*>(head, 0) = link;
    at<uint8_t*>(first, 4) = link;
    at<uint8_t*>(link, 4) = head;
    at<uint8_t*>(link, 0) = first;
    return update_view_mode();
}

// 0x0048F760 eView::UnattachCameraMover(mover): unlink it. EAX = eUpdateViewMode's.
uint32_t __cdecl View_UnattachCameraMover(uint8_t*, uint32_t m) {
    uint8_t* link = m ? reinterpret_cast<uint8_t*>(m) + 4 : nullptr;
    uint8_t* next = ptr(link, 0);
    uint8_t* prev = ptr(link, 4);
    at<uint8_t*>(prev, 0) = next;
    at<uint8_t*>(next, 4) = prev;
    return update_view_mode();
}

using Attach = uint32_t(__thiscall*)(void*, uint32_t);

// 0x00444040 CameraMover::Enable: once (+0x14): restore the camera's ([+0x1C]) +0x280/+0x284 from +0x24/+0x28
// (unless Camera::StopUpdating 0x828640, checked before each), attach to the view [+0x18], camera +0xBC = 0.5.
// EAX = the old flag, or the attach's.
uint32_t __cdecl Mover_Enable(uint8_t* s) {
    if (at<uint32_t>(s, 0x14)) return at<uint32_t>(s, 0x14);
    at<uint32_t>(s, 0x14) = 1;
    if (gu(0x00828640u) == 0) {
        at<uint32_t>(ptr(s, 0x1C), 0x280) = at<uint32_t>(s, 0x24);
        if (gu(0x00828640u) == 0) at<uint32_t>(ptr(s, 0x1C), 0x284) = at<uint32_t>(s, 0x28);
    }
    const uint32_t r = game_fn<Attach>(0x0048F730)(ptr(s, 0x18), u(s));
    at<uint32_t>(ptr(s, 0x1C), 0xBC) = 0x3F000000u;
    return r;
}

// 0x004440A0 CameraMover::Disable: if enabled: clear the flag, save the camera's +0x280/+0x284 to +0x24/+0x28,
// detach from the view. EAX = 0 (was not enabled) or the detach's.
uint32_t __cdecl Mover_Disable(uint8_t* s) {
    if (!at<uint32_t>(s, 0x14)) return 0;
    uint8_t* cam = ptr(s, 0x1C);
    at<uint32_t>(s, 0x14) = 0;
    at<uint32_t>(s, 0x24) = at<uint32_t>(cam, 0x280);
    at<uint32_t>(s, 0x28) = at<uint32_t>(cam, 0x284);
    return game_fn<Attach>(0x0048F760)(ptr(s, 0x18), u(s));
}

// 0x0044AB90 CameraMover::CameraMover(view, type): vtable 0x787CF0; view -1: no view; else the view's camera gets
// +0xC0 = 10000, its +0x280/+0x284 are kept, and the mover is enabled. Type 2 counts in 0x883DFC. EAX = this.
uint32_t __cdecl Mover_ctor(uint8_t* s, uint32_t view, uint32_t type) {
    at<uint32_t>(s, 0) = 0x00787CF0;
    at<uint32_t>(s, 0x10) = view;
    at<uint32_t>(s, 0x14) = 0;
    at<uint32_t>(s, 0xC) = type;
    at<uint32_t>(s, 0x20) = 0;
    if (view == 0xFFFFFFFFu) {
        at<uint32_t>(s, 0x18) = 0;
        at<uint32_t>(s, 0x1C) = 0;
        at<uint32_t>(s, 0x24) = 0;
        at<uint32_t>(s, 0x28) = 0;
    } else {
        const uint32_t v = game_fn<uint32_t(__cdecl*)(uint32_t)>(0x0048B1E0)(view);
        at<uint32_t>(s, 0x18) = v;
        uint8_t* cam = ptr(v, 0x40);
        at<uint8_t*>(s, 0x1C) = cam;
        at<uint32_t>(cam, 0xC0) = 0x461C4000u;  // 10000.0
        at<uint32_t>(s, 0x24) = at<uint32_t>(cam, 0x280);
        at<uint32_t>(s, 0x28) = at<uint32_t>(cam, 0x284);
        game_fn<uint32_t(__thiscall*)(void*)>(0x00444040)(s);
    }
    if (at<uint32_t>(s, 0xC) == 2) at<uint32_t>(0x00883DFCu, 0) += 1;
    return u(s);
}

}  // namespace

LEAF_THIS(0x0048F730, View_AttachCameraMover, 1, "void eView::AttachCameraMover(CameraMover*)");
LEAF_THIS(0x0048F760, View_UnattachCameraMover, 1, "void eView::UnattachCameraMover(CameraMover*)");
LEAF_THIS(0x00444040, Mover_Enable, 0, "void CameraMover::Enable()");
LEAF_THIS(0x004440A0, Mover_Disable, 0, "void CameraMover::Disable()");
LEAF_THIS(0x0044AB90, Mover_ctor, 2, "CameraMover::CameraMover(int view, int type)");

NFSU2_DIFF_TEST(camera_attach_view) {
    // 0 a fake view (list head +0x44 -> link record 2), 1 a mover (its link at +4: next -> 3, prev -> 4)
    static const Spec spec{5, 0x100, {{0, 0x44, 2}, {1, 4, 3}, {1, 8, 4}}, {}};
    static const std::vector<Fn> fns{
        LEAF_FN(View_AttachCameraMover, THIS, INT, {'p', 1}),
        LEAF_FN(View_UnattachCameraMover, THIS, INT, {'p', 1}),
    };
    return run(ctx, "camera_attach_view", spec, fns);
}

NFSU2_DIFF_TEST(camera_attach_mover) {
    // 0 a mover: view [+0x18] (1, a fake view whose list head +0x44 -> 4), camera [+0x1C] (2); its own link +4 ->
    // 3 / +8 -> 5 (for Disable); enabled flag +0x14
    static const Spec spec{6, 0x300, {{0, 0x18, 1}, {0, 0x1C, 2}, {1, 0x44, 4}, {0, 4, 3}, {0, 8, 5}}, {{0, 0x14, 4, 2}}};
    static const std::vector<Fn> fns{
        LEAF_FN(Mover_Enable, THIS, INT),
        LEAF_FN(Mover_Disable, THIS, INT),
    };
    return run(ctx, "camera_attach_mover", spec, fns);
}

NFSU2_DIFF_TEST(camera_attach_ctor) {  // the "no view" path (view -1); type 0 / 1 (type 2 counts in a game global)
    static const Spec spec{1, 0x100, {}, {}};
    static const std::vector<Fn> fns{LEAF_FN(Mover_ctor, THIS, PTR, {'k', 0xFFFFFFFFu}, {'i', 2})};  // never a real view
    return run(ctx, "camera_attach_ctor", spec, fns);
}

NFSU2_DIFF_TEST(camera_attach_BROKEN) {  // harness self-check: Attach compared with Unattach
    static const Spec spec{5, 0x100, {{0, 0x44, 2}, {1, 4, 3}, {1, 8, 4}}, {}};
    static const std::vector<Fn> fns{Fn("Attach_vs_Unattach", &o_View_AttachCameraMover, NFSU2_ENTRY(View_UnattachCameraMover), THIS, INT, {'p', 1})};
    return run(ctx, "camera_attach_BROKEN", spec, fns);
}
