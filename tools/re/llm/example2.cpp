// 0x00592080 CarState::SetWheelTerrain, written from the PC machine code (local-model draft, diff-tested).
#include "leaf.h"

using namespace game::leaf;

namespace {
// terrain[i] (+0x354) = t; the max seen (+0x364) rises to t (signed compare). EAX = t on return.
uint32_t __cdecl SetWheelTerrain(void* s, uint32_t i, uint32_t t) {
    at<uint32_t>(s, 0x354 + i * 4) = t;
    if (static_cast<int32_t>(t) > at<int32_t>(s, 0x364 + i * 4)) at<uint32_t>(s, 0x364 + i * 4) = t;
    return t;
}
}  // namespace
LEAF_THIS(0x00592080, SetWheelTerrain, 2, "void CarState::SetWheelTerrain(int wheel, int terrain)");

NFSU2_DIFF_TEST(llm_592080) {
    static const Spec spec{1, 0x400, {}, {}};
    // wheel index 0..3; terrain any value ('r')
    static const std::vector<Fn> fns{LEAF_FN(SetWheelTerrain, THIS, INT, {'i', 4}, {'r', 0})};
    return run(ctx, "llm_592080", spec, fns);
}
