// Helpers for code produced by tools/re/x87lift.py (straight-line x87 lifted to float C++, bit-exact at the game's
// 24-bit x87 precision). Memory through game pointers is accessed as raw bytes; int moves copy bits exactly.
#pragma once
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace lift {
inline float bitsf(uint32_t u) { return std::bit_cast<float>(u); }
inline uint32_t fbits(float f) { return std::bit_cast<uint32_t>(f); }
inline constexpr float f32(uint32_t u) { return std::bit_cast<float>(u); }
inline float ldf(const uint8_t* p, uint32_t off) { float f; memcpy(&f, p + off, 4); return f; }
inline uint32_t ldi(const uint8_t* p, uint32_t off) { uint32_t u; memcpy(&u, p + off, 4); return u; }
inline void stf(uint8_t* p, uint32_t off, float v) { memcpy(p + off, &v, 4); }
inline void sti(uint8_t* p, uint32_t off, uint32_t v) { memcpy(p + off, &v, 4); }
// x87 fchs/fabs flip/clear the sign bit even of NaN. LLVM may fold -(a*b) into a*(-b), which changes the sign of a
// NaN produced by the multiply; an empty asm barrier keeps the operation where the original has it.
inline float fchs(float x) {
    __asm__ volatile("" : "+x"(x));
    return -x;
}
inline float fabs_(float x) {
    __asm__ volatile("" : "+x"(x));
    return std::fabs(x);
}
// a float that went fld -> fstp without arithmetic: x87 turns a signalling NaN into a quiet one
inline float x87q(float f) {
    uint32_t u = fbits(f);
    if ((u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu)) u |= 0x00400000u;
    return bitsf(u);
}
}  // namespace lift
