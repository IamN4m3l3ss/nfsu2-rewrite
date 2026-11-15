// EA's printf (EA core library, zBWare.cpp), written from the PC machine code:
//   _bOutput      0x0043E950  the formatter (MSVC-CRT-style state machine + EA extensions)
//   bVSPrintf     0x0043FBC0  _bOutput into a buffer, timed into 0x828518 (float total) / 0x82851C (calls)
//   bSPrintf      0x004400D0  varargs wrapper
//   write_chars   0x0043CB40  n chars to the destination or the 128-byte ring buffer (0x828178, index 0x828514)
//   bIsValidPtr   0x0043BD60  !(IsBadReadPtr && IsBadWritePtr)
//
// Format: %[flags][width][.prec][h|l]conv. flags: '+' 1, ' ' 2, '-' 4, '0' 8, '#' 0x80, '$' 0x2000 (thousands
// grouping with the locale chars at 0x7EB3F0 decimal / 0x7EB3F1 separator / 0x7EB3F2 group size).
// conv: d i u o x X p b(binary) c s n, f e E g G (g/G = e/E), z (16.16 fixed point, default 2 decimals),
// v2 / v3 (bVector2/3 through "%*.*f" with the same width/precision).
// The state machine uses the CRT's character class / transition table at 0x786CD0 (read from the game's .rdata).
//
// Floats: the original converts on the x87 at the game's control word (24-bit precision, extended exponent),
// starting from the 53-bit double argument. The arithmetic below goes through small x87 primitives on 80-bit
// values (X87 below) so it rounds exactly the same; replace them with a soft-float at port time.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

// ---------------------------------------------------------------- x87 primitives (80-bit values in memory)
struct X {
    uint8_t b[10];
};
X x_ld(double d) {
    X r;
    __asm {
        fld qword ptr [d]
        fstp tbyte ptr [r]
    }
    return r;
}
X x_mul(const X& a, double c) {  // a * c (double operand)
    X r;
    const uint8_t* pa = a.b;
    __asm {
        mov eax, pa
        fld tbyte ptr [eax]
        fmul qword ptr [c]
        fstp tbyte ptr [r]
    }
    return r;
}
X x_mulx(const X& a, const X& b) {
    X r;
    const uint8_t *pa = a.b, *pb = b.b;
    __asm {
        mov eax, pa
        mov ecx, pb
        fld tbyte ptr [eax]
        fld tbyte ptr [ecx]
        fmulp st(1), st
        fstp tbyte ptr [r]
    }
    return r;
}
X x_div(const X& a, const X& b) {  // a / b
    X r;
    const uint8_t *pa = a.b, *pb = b.b;
    __asm {
        mov eax, pa
        mov ecx, pb
        fld tbyte ptr [eax]
        fld tbyte ptr [ecx]
        fdivp st(1), st
        fstp tbyte ptr [r]
    }
    return r;
}
X x_sub(const X& a, const X& b) {  // a - b
    X r;
    const uint8_t *pa = a.b, *pb = b.b;
    __asm {
        mov eax, pa
        mov ecx, pb
        fld tbyte ptr [eax]
        fld tbyte ptr [ecx]
        fsubp st(1), st
        fstp tbyte ptr [r]
    }
    return r;
}
X x_neg(const X& a) {
    X r = a;
    r.b[9] ^= 0x80;  // fchs
    return r;
}
X x_int(int32_t i) {  // fild
    X r;
    __asm {
        fild dword ptr [i]
        fstp tbyte ptr [r]
    }
    return r;
}
int32_t x_ftol(const X& a) {  // _ftol2: truncate toward zero, low 32 bits
    int64_t r;
    const uint8_t* pa = a.b;
    __asm {
        mov eax, pa
        fld tbyte ptr [eax]
        fisttp qword ptr [r]
    }
    return static_cast<int32_t>(r);
}
// fcom a, b -> condition bits (C0 0x01, C2 0x04, C3 0x40 in AH)
uint8_t x_cmp(const X& a, const X& b) {
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
uint8_t x_cmpd(const X& a, double c) { return x_cmp(a, x_ld(c)); }
inline bool parity_even(uint8_t v) { return (__builtin_popcount(v) & 1) == 0; }
// the original's tests: "test ah,M; jp/jnp" -> parity of (ah & M); "test ah,M; je/jne" -> zero test
inline bool only_c0(uint8_t ah) { return !parity_even(ah & 0x05); }  // test ah,5: exactly one of C0/C2 (= less)

// ---------------------------------------------------------------- output target
struct OutputInfo {
    char* dst;
    uint8_t to_ring;
    uint8_t pad[3];
    uint8_t ring_arg;  // +8: copied to 0x828150 when printing to the ring
};
uint8_t* const kRing = reinterpret_cast<uint8_t*>(0x00828178);
volatile int32_t* const kRingIdx = reinterpret_cast<volatile int32_t*>(0x00828514);
const uint8_t* const kLookup = reinterpret_cast<const uint8_t*>(0x00786CD0);
const char* lc_decimal() { return reinterpret_cast<const char*>(0x007EB3F0); }

inline void put(OutputInfo* o, char c) {
    if (o->to_ring) {
        kRing[*kRingIdx] = static_cast<uint8_t>(c);
        *kRingIdx = (*kRingIdx + 1) & 0x7F;
    } else if (o->dst) {
        *o->dst++ = c;
    }
}

// 0x0043CB40
using Write_t = void(__cdecl*)(OutputInfo*, const char*, int32_t, int32_t*);
Write_t o_WriteChars = nullptr;
void __cdecl WriteChars(OutputInfo* o, const char* s, int32_t n, int32_t* count) {
    if (o->to_ring) {
        if (!n) return;
        int32_t idx = *kRingIdx;
        do {
            const char c = *s;
            if (o->to_ring) {
                kRing[idx] = static_cast<uint8_t>(c);
                idx = (idx + 1) & 0x7F;
            } else if (o->dst) {
                *o->dst++ = c;
            }
            ++*count;
            ++s;
        } while (--n);
        *kRingIdx = idx;
        return;
    }
    *count += n;
    if (!o->dst || !n) return;
    do *o->dst++ = *s++; while (--n);
}

// 0x0043BD60
using Valid_t = int32_t(__cdecl*)(const void*);
Valid_t o_bIsValidPtr = nullptr;
int32_t __cdecl bIsValidPtr(const void* p) {
    using Is_t = BOOL(__stdcall*)(const void*, UINT_PTR);
    if (game_var<Is_t>(0x00783110)(p, 1) && game_var<Is_t>(0x0078310C)(p, 1)) return 0;
    return 1;
}

using SPrintf_t = int32_t(__cdecl*)(char*, const char*, ...);
inline SPrintf_t game_sprintf() { return game_fn<SPrintf_t>(0x004400D0); }

// ---------------------------------------------------------------- 0x0043E950 _bOutput
using Output_t = int32_t(__cdecl*)(OutputInfo*, const char*, va_list);
Output_t o_bOutput = nullptr;
int32_t __cdecl bOutput(OutputInfo* o, const char* fmt, va_list ap) {
    // locals named after the original's stack slots; they persist across conversions like the original's do
    uint32_t flags = 0;
    int32_t width = 0, prec = 0, count = 0, prefixlen = 0, textlen = 0, state = 0;
    char prefix[2] = {0, 0};
    char frame[0x48] = {};                 // [esp+0x44 .. 0x8B]: digit buffer (index = offset - 0x44)
    char* const buf = frame;
    char* text = nullptr;
    char vbuf[512];                        // %v: the original formats into its own frame here
    const char* p = fmt;
    if (o->to_ring) *reinterpret_cast<volatile uint8_t*>(0x00828150) = o->ring_arg;
    char c = *p;
    if (c == 0) goto done;
    for (;;) {
        ++p;
        if (count < 0) break;
        {
            const int32_t cls = (c >= 0x20 && c <= 0x7A) ? (static_cast<signed char>(kLookup[static_cast<signed char>(c)]) & 0xF) : 0;
            state = static_cast<signed char>(kLookup[0x20 + cls * 8 + state]) >> 4;
        }
        switch (state) {
            case 0:  // plain character
                put(o, c);
                ++count;
                goto next;
            case 1:  // '%'
                prefixlen = 0;
                width = 0;
                flags = 0;
                text = buf;  // [esp+0x44]
                prec = -1;
                goto next;
            case 2:
                switch (c) {
                    case ' ': flags |= 2; break;
                    case '#': flags |= 0x80; break;
                    case '$': flags |= 0x2000; break;
                    case '+': flags |= 1; break;
                    case '-': flags |= 4; break;
                    case '0': flags |= 8; break;
                    default: break;
                }
                goto next;
            case 3:
                if (c == '*') {
                    width = va_arg(ap, int32_t);
                    if (width < 0) {
                        flags |= 4;
                        width = -width;
                    }
                } else {
                    width = c + width * 10 - '0';
                }
                goto next;
            case 4:
                prec = 0;
                goto next;
            case 5:
                if (c == '*') {
                    prec = va_arg(ap, int32_t);
                    if (prec < 0) prec = -1;
                } else {
                    prec = c + prec * 10 - '0';
                }
                goto next;
            case 6:
                if (c == 'h') flags |= 0x20;
                else if (c == 'l') flags |= 0x10;
                goto next;
            case 7:
                break;
            default:
                goto next;
        }
        // ---- conversion
        {
            uint32_t hexadd = 0, base = 0;
            switch (c) {
                case 'c':
                    buf[0] = static_cast<char>(va_arg(ap, int32_t));
                    textlen = 1;
                    goto output;
                case 'x': hexadd = 0x27; goto hex;
                case 'p': prec = 8; hexadd = 7; goto hex;
                case 'X': hexadd = 7;
                hex:
                    if (flags & 0x80) {
                        prefix[0] = '0';
                        prefix[1] = static_cast<char>(hexadd + 0x51);
                        prefixlen = 2;
                    }
                    base = 16;
                    goto integer;
                case 'b': base = 2; goto integer;
                case 'o':
                    base = 8;
                    if (flags & 0x80) flags |= 0x200;
                    goto integer;
                case 'u': base = 10; goto integer;
                case 'd': case 'i':
                    flags |= 0x400;
                    base = 10;
                    goto integer;
                case 'e': case 'g': prefix[0] = 'e'; flags |= 0x800; goto flt;
                case 'E': case 'G': prefix[0] = 'E'; flags |= 0x800; goto flt;
                case 'f': goto flt;
                case 'n':
                    *va_arg(ap, int32_t*) = count;
                    flags |= 0x1000;
                    goto output;
                case 's': {
                    text = va_arg(ap, char*);
                    if (!text) text = *reinterpret_cast<char**>(0x007EB3E8);
                    else if (!bIsValidPtr(text)) text = *reinterpret_cast<char**>(0x007EB3EC);
                    goto strlen_prec;
                }
                case 'v': {
                    const char d = *p;  // [esp+0xcc] points after the 'v'
                    if (d != '2' && d != '3') {
                        text = reinterpret_cast<char*>(0x00786E30);  // "bPrintf invalid bVector"
                        goto strlen_prec;
                    }
                    const float* vec = va_arg(ap, const float*);
                    if (!bIsValidPtr(vec)) {
                        text = reinterpret_cast<char*>(0x00786DF4);  // "bPrintf invalid pointer"
                        goto strlen_prec;
                    }
                    int32_t n;
                    if (d == '3')
                        n = game_sprintf()(vbuf, reinterpret_cast<const char*>(0x00786E1C), width, prec, static_cast<double>(vec[0]),
                                           width, prec, static_cast<double>(vec[1]), width, prec, static_cast<double>(vec[2]));
                    else
                        n = game_sprintf()(vbuf, reinterpret_cast<const char*>(0x00786E0C), width, prec, static_cast<double>(vec[0]),
                                           width, prec, static_cast<double>(vec[1]));
                    textlen = 0;
                    WriteChars(o, vbuf, n + 1, &textlen);
                    if (o->dst) --o->dst;
                    if (o->to_ring && *kRingIdx > 0) --*kRingIdx;
                    flags |= 0x1000;
                    ++p;
                    goto output;
                }
                case 'z': goto fixed;
                default:
                    goto output;
            }
        strlen_prec : {
            const char* s = text;
            uint32_t left = prec < 0 ? 0x7FFFFFFFu : static_cast<uint32_t>(prec);
            for (;;) {
                const char ch = *s++;
                if (!ch) break;
                const uint32_t had = left--;
                if (!had) break;
            }
            if (s != text) textlen = static_cast<int32_t>(s - text) - 1;
            goto output;
        }
        integer : {
            char* e = buf + 0x3E;  // [esp+0x82], digits written backwards
            uint32_t v;
            if (flags & 0x20)
                v = (flags & 0x400) ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(va_arg(ap, int32_t))))
                                    : static_cast<uint32_t>(static_cast<uint16_t>(va_arg(ap, int32_t)));
            else
                v = va_arg(ap, uint32_t);
            if ((flags & 0x400) && static_cast<int32_t>(v) < 0) {
                v = 0u - v;
                flags |= 0x100;
            }
            if (prec < 0) prec = 1;
            else flags &= ~8u;
            if (v == 0) prefixlen = 0;
            const bool group = (flags & 0x2000) && static_cast<signed char>(lc_decimal()[2]) > 0;
            int32_t run = 0;
            for (;;) {
                const int32_t had = prec--;
                if (!(had > 0) && v == 0) break;
                if (group) {
                    ++run;
                    if (run > static_cast<signed char>(lc_decimal()[2])) {
                        *e-- = lc_decimal()[1];
                        run = 1;
                    }
                }
                char ch = static_cast<char>(v % base + '0');
                v /= base;
                if (static_cast<signed char>(ch) > '9') ch = static_cast<char>(ch + hexadd);
                *e-- = ch;
            }
            textlen = static_cast<int32_t>(buf + 0x3E - e);
            if (flags & 0x200)
                if (textlen == 0 || e[1] != '0') {
                    *e-- = '0';
                    ++textlen;
                }
            text = e + 1;
            goto output;
        }
        fixed : {  // %z: 16.16 fixed point
            flags |= 0x400;
            uint32_t v = va_arg(ap, uint32_t);
            char* e = buf + 0x3E;
            if (v >= 0x7FFFFFFFu) {
                v = ~v + 1;
                flags |= 0x100;
            }
            uint32_t ip = v >> 16, fr = v & 0xFFFF;
            if (prec < 0) prec = 2;
            const int32_t digits = prec;
            if (fr != 0 || prec != 0) {
                e = buf + 0x3F - prec;
                if (prec > 0) {
                    int32_t n = prec;
                    prec = 0;
                    do {
                        if (fr < 0x10000) fr *= 10;
                        uint32_t d = 0, acc = 0;
                        do {
                            acc += 0x10000;
                            ++d;
                        } while (acc <= fr);
                        --d;
                        *e++ = static_cast<char>(d + '0');
                        fr -= d << 16;
                    } while (--n);
                }
                *e = 0;
                e += -1 - digits;
                *e-- = '.';
                if (fr != 0) {
                    if (fr < 0x10000) fr *= 10;
                    uint32_t d = 0, acc = 0;
                    do {
                        acc += 0x10000;
                        ++d;
                    } while (acc <= fr);
                    --d;
                    if (d >= 5) {
                        char* q = buf + 0x3E;
                        bool carry_int = false;
                        while (*q == '9') {
                            *q = '0';
                            if (q[-1] == '.') {
                                carry_int = true;
                                break;
                            }
                            --q;
                        }
                        if (carry_int) ++ip;
                        else ++*q;
                    }
                }
            }
            if (ip) {
                do {
                    *e-- = static_cast<char>(ip % 10 + '0');
                    ip /= 10;
                } while (ip);
            } else {
                *e-- = '0';
            }
            textlen = static_cast<int32_t>(buf + 0x3E - e);
            text = e + 1;
            goto output;
        }
        flt : {
            const double dv = va_arg(ap, double);
            uint32_t hi;
            std::memcpy(&hi, reinterpret_cast<const uint8_t*>(&dv) + 4, 4);
            flags |= 0x400;
            if ((hi & 0x7FF00000u) == 0x7FF00000u) {
                const uintptr_t sp = hi == 0xFFF00000u ? 0x007EB3D8 : hi == 0x7FF00000u ? 0x007EB3DC : 0x007EB3E0;
                text = *reinterpret_cast<char**>(sp);
                textlen = 7;
                if (text) goto output;
            }
            X v = x_ld(dv), pw = x_ld(1.0);
            buf[0] = '0';
            text = buf + 1;
            char* out = buf + 1;
            if ((flags & 0x400) && only_c0(x_cmpd(v, 0.0))) {
                flags |= 0x100;
                v = x_neg(v);
            }
            if (prec == -1) prec = 3;
            int32_t ex = 0;
            const uint32_t efmt = flags & 0x800;
            if (efmt && parity_even(x_cmpd(v, 0.0) & 0x44)) {  // v != 0 (or unordered)
                if (!(x_cmpd(v, 1.0) & 0x01)) {
                    do {
                        ++ex;
                        v = x_mul(v, 0.1);
                    } while (!(x_cmpd(v, 1.0) & 0x01));
                }
                do {
                    --ex;
                    v = x_mul(v, 10.0);
                } while (only_c0(x_cmpd(v, 1.0)));
            } else {
                if (only_c0(x_cmp(x_ld(1.0), v))) {
                    do {
                        pw = x_mul(pw, 10.0);
                        ++ex;
                    } while (only_c0(x_cmp(pw, v)));
                }
                if (!(x_cmp(pw, v) & 0x41)) {
                    pw = x_mul(pw, 0.1);
                    --ex;
                }
            }
            const int32_t first = ex + 1;
            int32_t gpos = first;
            const bool group = (flags & 0x2000) && static_cast<signed char>(lc_decimal()[2]) > 0;
            const char dec = (flags & 0x2000) ? lc_decimal()[0] : '.';
            if (!(x_cmpd(pw, 1.0) & 0x01)) {
                do {
                    if (group) {
                        if (gpos % static_cast<signed char>(lc_decimal()[2]) == 0 && gpos != first) *out++ = lc_decimal()[1];
                        --gpos;
                    }
                    const int32_t d = x_ftol(x_div(v, pw));
                    *out++ = static_cast<char>(static_cast<signed char>(d) + '0');
                    v = x_sub(v, x_mulx(x_int(static_cast<signed char>(d)), pw));
                    pw = x_mul(pw, 0.1);
                } while (!(x_cmpd(pw, 1.0) & 0x01));
                if (out == buf + 1) *out++ = '0';
            } else {
                *out++ = '0';
            }
            int32_t fp = prec;
            if (fp != 0 || (flags & 0x80)) {
                *out++ = dec;
                if (fp != 0) {
                    int32_t n = fp;
                    fp = 0;
                    do {
                        v = x_mul(v, 10.0);
                        const int32_t d = x_ftol(v);
                        *out++ = static_cast<char>(static_cast<signed char>(d) + '0');
                        v = x_sub(v, x_int(static_cast<signed char>(d)));
                    } while (--n);
                }
            }
            prec = fp - 1;
            if (!(x_cmpd(x_mul(v, 10.0), 5.0) & 0x01)) {  // round up: 10*v >= 5
                char* q = out - 1;
                for (;;) {
                    while (*q == '9') *q-- = '0';
                    if (*q == dec || (group && *q == lc_decimal()[1])) {
                        --q;
                        continue;
                    }
                    break;
                }
                ++*q;
                if (q < buf + 1) text = buf;
            }
            if (efmt) {
                *out++ = prefix[0];
                if (ex < 0) {
                    ex = -ex;
                    *out = '-';
                } else {
                    *out = '+';
                }
                const signed char q1 = static_cast<signed char>(ex / 100);
                out[1] = static_cast<char>(q1 + '0');
                ++out;
                ex += q1 * -100;
                const signed char q2 = static_cast<signed char>(ex / 10);
                ++out;
                *out = static_cast<char>(q2 + '0');
                ++out;
                *out = static_cast<char>((ex - q2 * 10) % 10 + '0');
                ++out;
                prefixlen = 0;
            }
            textlen = static_cast<int32_t>(out - text);
            goto output;
        }
        }
    output:
        if (!(flags & 0x1000)) {
            if (flags & 0x400) {
                if (flags & 0x100) {
                    prefix[0] = '-';
                    prefixlen = 1;
                } else if (flags & 1) {
                    prefix[0] = '+';
                    prefixlen = 1;
                } else if (flags & 2) {
                    prefix[0] = ' ';
                    prefixlen = 1;
                }
            }
            int32_t pad = width - textlen - prefixlen;
            if (pad < 0) {
                pad = 0;
            } else if (pad != 0 && !(flags & 0xC)) {
                do {
                    put(o, ' ');
                    ++count;
                } while (--pad);
                pad = -1;
            }
            if (prefixlen) WriteChars(o, prefix, prefixlen, &count);
            if (pad > 0 && !(flags & 4)) {
                do {
                    put(o, '0');
                    ++count;
                } while (--pad);
                pad = -1;
            }
            WriteChars(o, text, textlen, &count);
            if (pad > 0)
                do {
                    put(o, ' ');
                    ++count;
                } while (--pad);
        }
    next:
        c = *p;
        if (c == 0) break;
    }
done:
    if (o->dst && count != -1) *o->dst = 0;
    return count;
}

// ---------------------------------------------------------------- 0x0043FBC0 bVSPrintf
using VSPrintf_t = int32_t(__cdecl*)(char*, const char*, va_list);
VSPrintf_t o_bVSPrintf = nullptr;
int32_t __cdecl bVSPrintf(char* dst, const char* fmt, va_list ap) {
    const uint32_t start = game_fn<uint32_t(__cdecl*)()>(0x0043BDF0)();  // bGetTicker
    OutputInfo info{dst, 0, {0, 0, 0}, 0};
    const int32_t n = bOutput(&info, fmt, ap);
    const float t = game_fn<float(__cdecl*)(uint32_t)>(0x0043E5C0)(start);  // bGetTickerDifference(start)
    *reinterpret_cast<volatile float*>(0x00828518) = t + *reinterpret_cast<volatile float*>(0x00828518);
    ++*reinterpret_cast<volatile int32_t*>(0x0082851C);
    return n;
}

int32_t __cdecl bSPrintf_impl(char* dst, const char* fmt, va_list ap) { return bVSPrintf(dst, fmt, ap); }

}  // namespace

// 0x004400D0 bSPrintf(dst, fmt, ...): varargs, so its own entry stub hands bVSPrintf a pointer to the caller's args
static void* const g_bSPrintf_impl = reinterpret_cast<void*>(&bSPrintf_impl);
static volatile uint32_t g_calls_bSPrintf = 0;
static int32_t (*o_bSPrintf)(char*, const char*, ...) = nullptr;
extern "C" __declspec(naked) void nfsu2_entry_bSPrintf() {
    __asm {
        lock inc dword ptr [g_calls_bSPrintf]
        lock inc dword ptr [nfsu2_inflight]
        push ecx
        push edx
        lea eax, [esp + 0x14]     // &first vararg (ret, dst, fmt after the two pushes)
        push eax
        push dword ptr [esp + 0x14]   // fmt
        push dword ptr [esp + 0x14]   // dst
        call dword ptr [g_bSPrintf_impl]
        add esp, 0xC
        pop edx
        pop ecx
        lock dec dword ptr [nfsu2_inflight]
        ret
    }
}
static ::game::Registrar g_reg_bSPrintf(0x004400D0, "bSPrintf", "int __cdecl bSPrintf(char* dst, const char* fmt, ...)",
                                        reinterpret_cast<void*>(&nfsu2_entry_bSPrintf), reinterpret_cast<void**>(&o_bSPrintf),
                                        __FILE__, &g_calls_bSPrintf);

NFSU2_REWRITE_N(0x0043E950, bOutput, 3, "int __cdecl _bOutput(bOutputInfo*, const char* fmt, va_list)", o_bOutput);
NFSU2_REWRITE_N(0x0043FBC0, bVSPrintf, 3, "int __cdecl bVSPrintf(char* dst, const char* fmt, va_list)", o_bVSPrintf);
NFSU2_REWRITE_N(0x0043CB40, WriteChars, 4, "void __cdecl bPrintf write_chars(info, const char*, int n, int* count)", o_WriteChars);
NFSU2_REWRITE_N(0x0043BD60, bIsValidPtr, 1, "int __cdecl bIsValidPointer(const void*) !(IsBadRead && IsBadWrite)", o_bIsValidPtr);

namespace {

// a random format with its arguments packed as the original reads them (4-byte slots, doubles 8)
struct Case {
    std::string fmt;
    uint32_t args[64];
    int n = 0;
    void push(uint32_t v) { if (n < 64) args[n++] = v; }
    void pushd(double d) {
        uint32_t w[2];
        std::memcpy(w, &d, 8);
        push(w[0]);
        push(w[1]);
    }
};
float g_vec[3];
const char* const kStrs[] = {"hello", "", "a longer string with spaces", "x"};

double rnd_double(game::Rng& r) {
    switch (r.below(8)) {
        case 0: return 0.0;
        case 1: return -0.0;
        case 2: return static_cast<double>(static_cast<int32_t>(r.next()) >> r.below(31));
        case 3: return (static_cast<double>(r.next()) / 4294967296.0) * (r.below(2) ? 1.0 : -1.0);
        case 4: return 0.5 + r.below(100);  // ties for rounding
        case 5: { const uint32_t k = r.below(3); return k == 0 ? INFINITY : k == 1 ? -INFINITY : NAN; }
        default: return (static_cast<double>(r.next()) / 4294967296.0 - 0.5) * (r.below(2) ? 2e6 : 2e-3);
    }
}

Case make_case(game::Rng& r) {
    Case c;
    for (int k = 0, m = 1 + r.below(4); k < m; ++k) {
        if (r.below(3) == 0) c.fmt += "txt ";
        if (r.below(15) == 0) { c.fmt += "%%"; continue; }
        c.fmt += '%';
        const char conv = "diuoxXpbcsfeEgGzvn"[r.below(18)];
        // %v formats into a ~48-byte buffer inside the original's own stack frame: long output overruns it and
        // crashes the original, so vectors get small widths/precisions (game formats are short)
        const bool tiny = conv == 'v';
        const char* fl = " #+-0$";
        for (int i = 0, n = r.below(3); i < n; ++i) c.fmt += fl[r.below(6)];
        if (r.below(3) == 0) {
            if (r.below(4) == 0) { c.fmt += '*'; c.push(static_cast<uint32_t>(static_cast<int32_t>(r.below(tiny ? 5 : 30)) - (tiny ? 0 : 10))); }
            else c.fmt += std::to_string(r.below(tiny ? 5 : 20));
        }
        if (r.below(3) == 0) {
            c.fmt += '.';
            if (r.below(4) == 0) { c.fmt += '*'; c.push(static_cast<uint32_t>(static_cast<int32_t>(r.below(tiny ? 3 : 12)) - (tiny ? 0 : 2))); }
            else c.fmt += std::to_string(r.below(tiny ? 3 : (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') ? 12 : 20));
        }
        if (r.below(5) == 0) c.fmt += r.below(2) ? 'h' : 'l';
        c.fmt += conv;
        switch (conv) {
            case 'f': case 'e': case 'E': case 'g': case 'G': c.pushd(rnd_double(r)); break;
            case 's': c.push(r.below(8) == 0 ? 0u : static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kStrs[r.below(4)]))); break;
            case 'c': c.push(32 + r.below(95)); break;
            case 'v':
                if (r.below(8) == 0) {
                    c.fmt += '7';  // invalid: the original consumes no argument
                } else {
                    c.fmt += r.below(2) ? '2' : '3';
                    c.push(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_vec)));
                }
                break;
            case 'n': c.push(0); break;  // patched with a real pointer by the test
            default: {
                const uint32_t v = r.below(4) == 0 ? 0u : r.next() >> r.below(32);
                c.push(r.below(5) == 0 ? (0u - v) : v);
            }
        }
    }
    if (r.below(6) == 0) c.fmt += " end";
    return c;
}

}  // namespace

NFSU2_DIFF_TEST(bPrintf) {
    return game::run_diff(ctx, "bPrintf", o_bOutput, [](game::Rng& r) -> game::Case {
        g_vec[0] = static_cast<float>(static_cast<int32_t>(r.below(2000)) - 1000) / static_cast<float>(1 + r.below(7));
        g_vec[1] = 1.5f;
        g_vec[2] = -0.25f;
        Case c = make_case(r);
        int32_t na = -7, nb = -7;
        // %n targets: point every zero arg of a %n at a per-run int (same slot in both runs)
        Case ca = c, cb = c;
        {
            size_t ai = 0;
            const std::string& f = c.fmt;
            for (size_t i = 0; i < f.size(); ++i) {
                if (f[i] != '%') continue;
                size_t j = i + 1;
                if (f[j] == '%') { i = j; continue; }
                while (j < f.size() && std::strchr(" #+-0$", f[j])) ++j;
                if (f[j] == '*') { ++ai; ++j; } else while (j < f.size() && f[j] >= '0' && f[j] <= '9') ++j;
                if (f[j] == '.') { ++j; if (f[j] == '*') { ++ai; ++j; } else while (j < f.size() && f[j] >= '0' && f[j] <= '9') ++j; }
                if (f[j] == 'h' || f[j] == 'l') ++j;
                const char conv = f[j];
                if (conv == 'n') {
                    ca.args[ai] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&na));
                    cb.args[ai] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb));
                }
                if (conv == 'v') ai += (f[j + 1] == '2' || f[j + 1] == '3') ? 1 : 0;
                else ai += (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') ? 2 : 1;
                i = j;
            }
        }
        static std::string step;
        step = c.fmt;
        game::g_test_step = step.c_str();
        char a[1024], b[1024];
        std::memset(a, 0x5A, sizeof a);
        std::memset(b, 0x5A, sizeof b);
        OutputInfo ia{a, 0, {0, 0, 0}, 0}, ib{b, 0, {0, 0, 0}, 0};
        const int32_t ra = o_bOutput(&ia, c.fmt.c_str(), reinterpret_cast<va_list>(ca.args));
        const int32_t rb = bOutput(&ib, c.fmt.c_str(), reinterpret_cast<va_list>(cb.args));
        if (ra != rb || std::memcmp(a, b, sizeof a) || na != nb || (ia.dst - a) != (ib.dst - b))
            return game::MismatchText("fmt \"" + c.fmt + "\": \"" + std::string(a, strnlen(a, 120)) + "\" vs \"" + std::string(b, strnlen(b, 120)) + "\"");
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(bPrintf_ring) {  // the ring-buffer target (index and bytes restored afterwards)
    return game::run_diff(ctx, "bPrintf_ring", o_bOutput, [](game::Rng& r) -> game::Case {
        Case c = make_case(r);
        if (c.fmt.find('n') != std::string::npos || c.fmt.find('v') != std::string::npos) return game::Ok();
        uint8_t saved[128];
        std::memcpy(saved, kRing, 128);
        const int32_t idx0 = *kRingIdx;
        const uint8_t b150 = *reinterpret_cast<volatile uint8_t*>(0x00828150);
        game::ScopedUndo undo([=] {
            std::memcpy(kRing, saved, 128);
            *kRingIdx = idx0;
            *reinterpret_cast<volatile uint8_t*>(0x00828150) = b150;
        });
        const uint8_t arg = static_cast<uint8_t>(r.next());
        OutputInfo ia{nullptr, 1, {0, 0, 0}, arg}, ib = ia;
        *kRingIdx = static_cast<int32_t>(r.below(128));
        const int32_t start = *kRingIdx;
        const int32_t ra = o_bOutput(&ia, c.fmt.c_str(), reinterpret_cast<va_list>(c.args));
        uint8_t ring_a[128];
        std::memcpy(ring_a, kRing, 128);
        const int32_t ea = *kRingIdx;
        std::memcpy(kRing, saved, 128);
        *kRingIdx = start;
        const int32_t rb = bOutput(&ib, c.fmt.c_str(), reinterpret_cast<va_list>(c.args));
        return ra == rb && ea == *kRingIdx && std::memcmp(ring_a, kRing, 128) == 0 ? game::Ok() : game::MismatchText("ring \"" + c.fmt + "\"");
    });
}

NFSU2_DIFF_TEST(regs_bPrintf) {
    using game::arg;
    return game::run_diff(ctx, "regs_bPrintf", o_bOutput, [](game::Rng& r) -> game::Case {
        Case c = make_case(r);
        if (c.fmt.find('n') != std::string::npos) return game::Ok();
        char a[1024], b[1024];
        OutputInfo ia{a, 0, {0, 0, 0}, 0}, ib{b, 0, {0, 0, 0}, 0};
        game::Case x = game::check_regs("_bOutput", (void*)o_bOutput, NFSU2_ENTRY(bOutput), {arg(&ia), arg(c.fmt.c_str()), arg(c.args)}, false);
        if (!x.ok) return x;
        // bVSPrintf / bSPrintf return the count; bSPrintf with up to 8 dwords of varargs
        x = game::check_regs("bVSPrintf", (void*)o_bVSPrintf, NFSU2_ENTRY(bVSPrintf), {arg(a), arg(c.fmt.c_str()), arg(c.args)}, false);
        if (!x.ok) return x;
        if (c.n > 6) return game::Ok();  // only 6 dwords of varargs fit the register test call
        return game::check_regs("bSPrintf", (void*)o_bSPrintf, reinterpret_cast<const void*>(&nfsu2_entry_bSPrintf),
                                {arg(b), arg(c.fmt.c_str()), c.args[0], c.args[1], c.args[2], c.args[3], c.args[4], c.args[5]}, false);
    });
}

NFSU2_DIFF_TEST(bPrintf_BROKEN) {  // harness self-check: the CRT's own printf disagrees with EA's (%z, $, rounding)
    return game::run_diff(ctx, "bPrintf_BROKEN", o_bOutput, [](game::Rng& r) -> game::Case {
        const double d = rnd_double(r);
        char a[256], b[256];
        OutputInfo ia{a, 0, {0, 0, 0}, 0};
        uint32_t w[2];
        std::memcpy(w, &d, 8);
        o_bOutput(&ia, "%.3f", reinterpret_cast<va_list>(w));
        snprintf(b, sizeof b, "%.3f", d);
        return std::strcmp(a, b) == 0 ? game::Ok() : game::MismatchText("crt printf");
    });
}
