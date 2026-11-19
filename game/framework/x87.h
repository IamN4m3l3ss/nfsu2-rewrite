// x87 primitives on 80-bit values, for rewrites whose original keeps intermediates in x87 registers where plain
// float would round differently (fild of ints above 2^24, values outside float range, 53-bit double inputs).
// Each primitive is one x87 instruction under the CURRENT control word - on the game thread that is the game's own
// (0x007F: 24-bit precision), exactly what the original ran under. Replace with a soft-float at port time.
#pragma once
#include <cstdint>

namespace x87 {

struct X {
    uint8_t b[10];
};

inline X ld(float f) {
    X r;
    __asm {
        fld dword ptr [f]
        fstp tbyte ptr [r]
    }
    return r;
}
inline X ldd(double d) {
    X r;
    __asm {
        fld qword ptr [d]
        fstp tbyte ptr [r]
    }
    return r;
}
inline X ild(int32_t i) {  // fild dword
    X r;
    __asm {
        fild dword ptr [i]
        fstp tbyte ptr [r]
    }
    return r;
}
inline X ild16(int16_t i) {  // fild word
    X r;
    __asm {
        fild word ptr [i]
        fstp tbyte ptr [r]
    }
    return r;
}
// Inputs go to the asm as NAMED local copies (fld tbyte ptr [la]): the compiler then knows the asm reads them.
// (Reading through a pointer in a register hid that, and inside loops clang dropped the stores - 2026-11-18.)
inline float st(const X& a) {  // fstp dword (rounds to float)
    float r;
    X la = a;
    __asm {
        fld tbyte ptr [la]
        fstp dword ptr [r]
    }
    return r;
}
#define NFSU2_X87_BIN(name, op)                     \
    inline X name(const X& a, const X& b) {         \
        X r;                                        \
        X la = a, lb = b;                           \
        __asm { fld tbyte ptr [la] }                \
        __asm { fld tbyte ptr [lb] }                \
        __asm { op st(1), st }                      \
        __asm { fstp tbyte ptr [r] }                \
        return r;                                   \
    }
NFSU2_X87_BIN(add, faddp)
NFSU2_X87_BIN(sub, fsubp)  // a - b
NFSU2_X87_BIN(mul, fmulp)
NFSU2_X87_BIN(div, fdivp)  // a / b
#undef NFSU2_X87_BIN
// `op dword ptr [mem]`: a float memory operand as the original used it. Unlike ld() + op, a signalling NaN in
// memory reaches the instruction unquieted, which decides which NaN payload comes out. Takes the float's bits.
#define NFSU2_X87_MEM(name, op)                     \
    inline X name(const X& a, uint32_t bits) {      \
        X r;                                        \
        X la = a;                                   \
        uint32_t lb = bits;                         \
        __asm { fld tbyte ptr [la] }                \
        __asm { op dword ptr [lb] }                 \
        __asm { fstp tbyte ptr [r] }                \
        return r;                                   \
    }
NFSU2_X87_MEM(addm, fadd)
NFSU2_X87_MEM(subm, fsub)    // a - mem
NFSU2_X87_MEM(subrm, fsubr)  // mem - a
NFSU2_X87_MEM(mulm, fmul)
NFSU2_X87_MEM(divm, fdiv)    // a / mem
NFSU2_X87_MEM(divrm, fdivr)  // mem / a
#undef NFSU2_X87_MEM
inline X sqrt(const X& a) {  // fsqrt
    X r;
    X la = a;
    __asm {
        fld tbyte ptr [la]
        fsqrt
        fstp tbyte ptr [r]
    }
    return r;
}
inline int32_t ftol(const X& a) {  // _ftol2: truncate toward zero, low 32 bits
    int64_t r;
    X la = a;
    __asm {
        fld tbyte ptr [la]
        fisttp qword ptr [r]
    }
    return static_cast<int32_t>(r);
}
inline X absx(const X& a) {  // fabs (clears the sign, NaN too)
    X r = a;
    r.b[9] &= 0x7F;
    return r;
}
// a status word from cmpw() as the original's fnstsw saw it with `depth` values still on its x87 stack (TOP field)
inline uint16_t with_depth(uint16_t sw, int depth) {
    const uint16_t top = static_cast<uint16_t>((((sw >> 11) & 7) - depth) & 7);
    return static_cast<uint16_t>((sw & ~0x3800) | (top << 11));
}
// fcom a, b -> AH condition bits (C0 0x01, C2 0x04, C3 0x40)
inline uint8_t cmp(const X& a, const X& b) {
    uint16_t sw;
    X la = a, lb = b;
    __asm {
        fld tbyte ptr [lb]
        fld tbyte ptr [la]
        fcompp
        fnstsw ax
        mov sw, ax
    }
    return static_cast<uint8_t>(sw >> 8);
}
// the full status word after `fcomp` (what `fnstsw ax` leaves in AX: condition codes, TOP, sticky exception flags)
inline uint16_t cmpw(const X& a, const X& b) {
    uint16_t sw;
    X la = a, lb = b;
    __asm {
        fld tbyte ptr [lb]
        fld tbyte ptr [la]
        fcompp
        fnstsw ax
        mov sw, ax
    }
    return sw;
}
// the original's flag tests: "test ah,M; jp/jnp" use the parity of (ah & M)
inline bool parity_even(uint8_t v) { return (__builtin_popcount(v) & 1) == 0; }
inline bool less(uint8_t ah) { return !parity_even(ah & 0x05); }  // test ah,5 -> exactly C0: a < b

}  // namespace x87
