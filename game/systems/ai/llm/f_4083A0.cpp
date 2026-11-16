// 0x004083A0 RaceCarController::AllowedToCallGetUnstuckAction, written from the PC machine code (local-model draft, diff-tested).
#include "leaf.h"

using namespace game::leaf;

namespace {
// Returns 1 if abs(this->unstuckTimer) < threshold, else 0.
// threshold = gf(0x783b50)
// multiplier = gf(0x784268)
// timer = this->unstuckTimer (+0x708)
uint32_t __cdecl AllowedToCallGetUnstuckAction(void* s) {
    float timer = static_cast<float>(at<int32_t>(s, 0x708));
    float threshold = gf(0x783b50);
    float multiplier = gf(0x784268);
    
    // fild dword ptr [esp] -> st0 = timer
    // fmul dword ptr [0x784268] -> st0 = timer * multiplier
    float val = timer * multiplier;
    
    // fcomp dword ptr [0x783b50] -> compare st0 with threshold
    // st0 < threshold: ah bits C0 (0x01) set
    // st0 == threshold: ah bits C3 (0x40) set
    // Unordered (NaN): both bits set (0x41)
    
    // test ah, 0x41; jne -> jump if any of bit 0 or bit 6 is set
    // i.e., if st0 < threshold OR st0 == threshold (or unordered)
    
    // Note: fcomp does st0 - m. 
    // If st0 < m: C0=1, C3=0, C2=0 -> ah & 0x41 = 0x01 != 0 -> jump taken (return 1)
    // If st0 == m: C0=0, C3=1, C2=0 -> ah & 0x41 = 0x40 != 0 -> jump taken (return 1)
    // If st0 > m: C0=0, C3=0, C2=0 -> ah & 0x41 = 0 -> jump not taken (return 0)
    // If unordered: C0=1, C3=1, C2=1 -> ah & 0x41 = 0x41 != 0 -> jump taken (return 1)
    
    // So return 1 if val <= threshold or unordered.
    
    // Check for unordered (NaN)
    if (std::isnan(val) || std::isnan(threshold)) {
        return 1;
    }
    
    if (val <= threshold) {
        return 1;
    }
    
    return 0;
}
}  // namespace
LEAF_THIS(0x004083A0, AllowedToCallGetUnstuckAction, 0, "bool RaceCarController::AllowedToCallGetUnstuckAction()");

NFSU2_DIFF_TEST(llm_4083A0) {
    // block 0 = this; +0x708 is unstuckTimer (int32)
    static const Spec spec{1, 0x1000, {}, {{0, 0x708, 4, 0}}};
    static const std::vector<Fn> fns{LEAF_FN(AllowedToCallGetUnstuckAction, THIS, INT)};
    return run(ctx, "llm_4083A0", spec, fns);
}
