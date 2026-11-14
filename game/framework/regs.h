// Register contract (learned the hard way, 2026-11-14): the game was built with MSVC whole-program optimisation, so
// callers may rely on registers a small function happens not to touch (e.g. a caller keeps ECX live across bSin, and
// callers use the output pointer bScaleAdd leaves in EAX). A rewrite must therefore leave EAX/ECX/EDX exactly as the
// original does — not just return the right value.
//
//  - Every rewrite is installed through a naked entry stub that saves/restores ECX and EDX around our C++ function
//    (NFSU2_REWRITE_N). EAX and the x87 return value (st0) pass through untouched.
//  - Functions that "return" a pointer in EAX without a declared return value must return it explicitly.
//  - call_regs() calls a function with sentinel EAX/ECX/EDX and captures them afterwards, so diff tests can check
//    the contract (check_regs).
#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>

#include "diff.h"

namespace game {

constexpr uint32_t kSentEAX = 0xA0A0A0A0, kSentECX = 0xC0C0C0C0, kSentEDX = 0xD0D0D0D0;

struct RegOut {
    uint32_t eax, ecx, edx;
    float st0;  // only if returns_float
};

// cdecl call with up to 8 dword args (floats passed as their bits)
RegOut call_regs(const void* fn, const uint32_t* args, int nargs, bool returns_float);

inline uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
template <class T> uint32_t arg(T* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
inline uint32_t arg(uint32_t v) { return v; }
inline uint32_t arg(int v) { return static_cast<uint32_t>(v); }
inline uint32_t arg(float f) { return bits(f); }

// The original (trampoline) and our installed entry stub get identical arguments. Registers the original leaves
// at their sentinel (= preserved) must be preserved by ours; EAX must match unless the function returns a float.
// compare_eax=false only for functions whose EAX is garbage AND no call site reads it (tools/re/regcontract.py
// shows "none") — e.g. bSinCos leaves the x87 status word there.
inline Case check_regs(const char* what, const void* orig, const void* mine, std::initializer_list<uint32_t> args,
                       bool returns_float, bool compare_eax = true) {
    const RegOut a = call_regs(orig, args.begin(), static_cast<int>(args.size()), returns_float);
    const RegOut b = call_regs(mine, args.begin(), static_cast<int>(args.size()), returns_float);
    std::string bad;
    if (!returns_float && compare_eax && a.eax != b.eax) bad += " eax " + std::to_string(a.eax) + "/" + std::to_string(b.eax);
    if (a.ecx == kSentECX && b.ecx != kSentECX) bad += " ecx not preserved";
    if (a.edx == kSentEDX && b.edx != kSentEDX) bad += " edx not preserved";
    if (returns_float && bits(a.st0) != bits(b.st0)) bad += " st0 differs";
    return bad.empty() ? Ok() : MismatchText(std::string(what) + ": register contract:" + bad);
}

}  // namespace game

// ---- entry stubs: push ecx, push edx, copy N args, call impl, pop, ret (cdecl: caller cleans its args)
// After the two pushes, arg i of N is at [esp + 12 + 4*i]; pushing them last-to-first keeps the source offset
// constant at 8 + 4*N.
#define NFSU2_PUSH_ARGS_1 __asm push dword ptr [esp + 12]
#define NFSU2_PUSH_ARGS_2 __asm push dword ptr [esp + 16] __asm push dword ptr [esp + 16]
#define NFSU2_PUSH_ARGS_3 \
    __asm push dword ptr [esp + 20] __asm push dword ptr [esp + 20] __asm push dword ptr [esp + 20]
#define NFSU2_PUSH_ARGS_4                                                                              \
    __asm push dword ptr [esp + 24] __asm push dword ptr [esp + 24] __asm push dword ptr [esp + 24] \
    __asm push dword ptr [esp + 24]

#define NFSU2_REWRITE_N(addr, fn, nargs, sig, original)                                     \
    static void* const g_impl_##fn = reinterpret_cast<void*>(&fn);                          \
    extern "C" __declspec(naked) void nfsu2_entry_##fn() {                                  \
        __asm push ecx __asm push edx NFSU2_PUSH_ARGS_##nargs __asm call dword ptr [g_impl_##fn] \
            __asm add esp, 4 * nargs __asm pop edx __asm pop ecx __asm ret                   \
    }                                                                                       \
    static ::game::Registrar g_reg_##fn(addr, #fn, sig, reinterpret_cast<void*>(&nfsu2_entry_##fn), \
                                        reinterpret_cast<void**>(&original), __FILE__)

#define NFSU2_ENTRY(fn) reinterpret_cast<const void*>(&nfsu2_entry_##fn)

// ---- entry stub that also hands the caller's EAX to our function as an extra LAST argument, for originals whose
// result keeps garbage bits from the entry EAX (e.g. `mov ax, cx` on a path where only `fnstsw ax` touched EAX).
// After push ecx/edx/eax, arg i of N is at [esp + 16 + 4*i]; pushed last-to-first the offset is 12 + 4*N.
#define NFSU2_PUSH_ARGS_E1 __asm push dword ptr [esp + 16]
#define NFSU2_PUSH_ARGS_E2 __asm push dword ptr [esp + 20] __asm push dword ptr [esp + 20]
#define NFSU2_REWRITE_N_EAX(addr, fn, nargs, sig, original)                                          \
    static void* const g_impl_##fn = reinterpret_cast<void*>(&fn);                                   \
    extern "C" __declspec(naked) void nfsu2_entry_##fn() {                                           \
        __asm push ecx __asm push edx __asm push eax NFSU2_PUSH_ARGS_E##nargs                         \
            __asm call dword ptr [g_impl_##fn] __asm add esp, 4 * (nargs + 1) __asm pop edx __asm pop ecx \
                __asm ret                                                                             \
    }                                                                                                \
    static ::game::Registrar g_reg_##fn(addr, #fn, sig, reinterpret_cast<void*>(&nfsu2_entry_##fn),  \
                                        reinterpret_cast<void**>(&original), __FILE__)
