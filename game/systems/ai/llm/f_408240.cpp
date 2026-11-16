// 0x00408240 RaceCarController::GetLookaheadTime, written from the PC machine code (local-model draft, diff-tested).
#include "leaf.h"

using namespace game::leaf;

namespace {
// Returns a float.
// Logic:
// 1. Compare global 0x7e8e0c (let's call it A) with global 0x783670 (B).
//    If A >= B (i.e., not A < B and not unordered), load A onto FPU stack.
//    Else (A < B or unordered), load B onto FPU stack.
// 2. Compare the loaded value (let's call it Val) with global 0x783b40 (C).
//    If Val < C or unordered, return C.
//    Else (Val >= C), return Val.
float __cdecl GetLookaheadTime() {
    float A = gf(0x7e8e0c);
    float B = gf(0x783670);
    float C = gf(0x783b40);

    float Val;

    // fcomp A, B; check flags
    // jne if (A < B) or unordered
    // We need to replicate the x87 fcomp behavior.
    // fcomp sets C0=1 if st0 < m, C3=1 if st0 == m.
    // test ah, 0x41 checks bits 0 and 6.
    // jne jumps if ANY of these bits are set.
    // So it jumps if (A < B) OR (A == B).
    // It does NOT jump if A > B.
    // Wait, let's re-read the test logic carefully.
    // test ah, 0x41. 0x41 is binary 0100 0001.
    // Bit 0 corresponds to C0 (st0 < m).
    // Bit 6 corresponds to C3 (st0 == m).
    // jne jumps if the result of the AND is non-zero.
    // So it jumps if (C0 is set) OR (C3 is set).
    // This means it jumps if A < B OR A == B.
    // It falls through if neither is set, i.e., A > B.

    if (A < B || A == B) {
        Val = B;
    } else {
        Val = A;
    }

    // fcom Val, C
    // test ah, 0x41
    // je 0x408276 (return Val)
    // If equal, it jumps to return Val.
    // So it returns Val if (Val == C) OR (Val > C) [since neither < nor == is set].
    // It returns C if (Val < C) OR (Val is unordered with C).

    if (Val == C || Val > C) {
        return Val;
    } else {
        return C;
    }
}
}  // namespace
LEAF_CDECL(0x00408240, GetLookaheadTime, 0, "float RaceCarController::GetLookaheadTime()");

NFSU2_DIFF_TEST(llm_408240) {
    // No arguments, reads globals.
    static const Spec spec{1, 0x100, {}, {}};
    static const std::vector<Fn> fns{LEAF_FN(GetLookaheadTime, CDECL, FLT)};
    return run(ctx, "llm_408240", spec, fns);
}
