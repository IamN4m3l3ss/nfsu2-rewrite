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
inline float st(const X& a) {  // fstp dword (rounds to float)
    float r;
    const uint8_t* pa = a.b;
    __asm {
        mov eax, pa
        fld tbyte ptr [eax]
        fstp dword ptr [r]
    }
    return r;
}
#define NFSU2_X87_BIN(name, op)                     \
    inline X name(const X& a, const X& b) {         \
        X r;                                        \
        const uint8_t *pa = a.b, *pb = b.b;         \
        __asm { mov eax, pa }                       \
        __asm { mov ecx, pb }                       \
        __asm { fld tbyte ptr [eax] }               \
        __asm { fld tbyte ptr [ecx] }               \
        __asm { op st(1), st }                      \
        __asm { fstp tbyte ptr [r] }                \
        return r;                                   \
    }
NFSU2_X87_BIN(add, faddp)
NFSU2_X87_BIN(sub, fsubp)  // a - b
NFSU2_X87_BIN(mul, fmulp)
NFSU2_X87_BIN(div, fdivp)  // a / b
#undef NFSU2_X87_BIN
inline X sqrt(const X& a) {  // fsqrt
    X r;
    const uint8_t* pa = a.b;
    __asm {
        mov eax, pa
        fld tbyte ptr [eax]
        fsqrt
        fstp tbyte ptr [r]
    }
    return r;
}
inline int32_t ftol(const X& a) {  // _ftol2: truncate toward zero, low 32 bits
    int64_t r;
    const uint8_t* pa = a.b;
    __asm {
        mov eax, pa
        fld tbyte ptr [eax]
        fisttp qword ptr [r]
    }
    return static_cast<int32_t>(r);
}
// fcom a, b -> AH condition bits (C0 0x01, C2 0x04, C3 0x40)
inline uint8_t cmp(const X& a, const X& b) {
    uint16_t sw;
    const uint8_t *pa = a.b, *pb = b.b;
    __asm {
        mov eax, pa
        mov ecx, pb
        fld tbyte ptr [ecx]
        fld tbyte ptr [eax]
        fcompp
        fnstsw ax
        mov sw, ax
    }
    return static_cast<uint8_t>(sw >> 8);
}
// the original's flag tests: "test ah,M; jp/jnp" use the parity of (ah & M)
inline bool parity_even(uint8_t v) { return (__builtin_popcount(v) & 1) == 0; }
inline bool less(uint8_t ah) { return !parity_even(ah & 0x05); }  // test ah,5 -> exactly C0: a < b

}  // namespace x87
