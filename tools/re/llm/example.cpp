// 0x00592C60 Collision::IsTwoCarCollision, written from the PC machine code (local-model draft, diff-tested).
#include "leaf.h"

using namespace game::leaf;

namespace {
// both bodies [[+0x64]+0x238] / [[+0x64]+0x23C] have type byte +0x11 == 0 -> 1, else 0
uint32_t __cdecl IsTwoCarCollision(void* s) {
    uint8_t* c = ptr(s, 0x64);
    if (at<uint8_t>(ptr(c, 0x238), 0x11) != 0) return 0;
    if (at<uint8_t>(ptr(c, 0x23C), 0x11) != 0) return 0;
    return 1;
}
}  // namespace
LEAF_THIS(0x00592C60, IsTwoCarCollision, 0, "bool Collision::IsTwoCarCollision()");

NFSU2_DIFF_TEST(llm_592C60) {
    // block 0 = this; +0x64 -> block 1; block 1 +0x238 -> block 2, +0x23C -> block 3; type bytes kept small
    static const Spec spec{4, 0x400, {{0, 0x64, 1}, {1, 0x238, 2}, {1, 0x23C, 3}}, {{2, 0x11, 1, 3}, {3, 0x11, 1, 3}}};
    static const std::vector<Fn> fns{LEAF_FN(IsTwoCarCollision, THIS, INT)};
    return run(ctx, "llm_592C60", spec, fns);
}
