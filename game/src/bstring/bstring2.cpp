// EA bString library, part 2 (NA v1.2). All __cdecl. Written from the disassembly, quirks kept:
//  - bounded compares (bStrNCmp/bStrNICmp) finish by looking at a[-1]/b[-1] (can be the byte BEFORE the string);
//  - char -> wide copies sign-extend (byte 0xE9 -> 0xFFE9);
//  - bStrToInt: "0x" (lowercase x) = hex, else optional sign + decimal; stops at the first invalid char; wraps;
//  - bStrUpr returns a pointer to the terminator.
// Not done: bStrToFloat 0x43DEF0 (x87 math; needs an exact-x87 approach, see DEVLOG).
#include <cstring>
#include <string>

#include "../diff.h"
#include "../rewrite.h"

namespace {

using u16 = uint16_t;
inline signed char sc(char c) { return static_cast<signed char>(c); }
inline signed char up(signed char c) { return (c >= 'a' && c <= 'z') ? static_cast<signed char>(c & 0x5F) : c; }

// ---------------------------------------------------------------- bStrNCpy 0x0043DC00 (126 callers)
using NCpy_t = char*(__cdecl*)(char*, const char*, int);
NCpy_t o_bStrNCpy = nullptr;
char* __cdecl bStrNCpy(char* dst, const char* src, int n) {
    if (n == 0) return dst;
    for (char* d = dst;; ++d, ++src) {
        const char c = *src;
        --n;
        *d = c;
        if (!c || n == 0) break;
    }
    return dst;
}

// ---------------------------------------------------------------- bStrNCmp 0x0043DC60 (7 callers)
using NCmp_t = int(__cdecl*)(const char*, const char*, int);
NCmp_t o_bStrNCmp = nullptr;
int __cdecl bStrNCmp(const char* a, const char* b, int n) {
    for (;;) {
        const int count = n--;
        if (count == 0) break;
        const signed char ca = sc(*a);
        if (!ca) break;
        const signed char cb = sc(*b);
        if (!cb) break;
        ++a;
        ++b;
        if (ca != cb) break;
    }
    if (n < 0) return 0;
    if (*a) return 1;
    if (*b) return -1;
    return sc(a[-1]) - sc(b[-1]);
}

// ---------------------------------------------------------------- bStrNICmp 0x0043DD10 (3 callers)
NCmp_t o_bStrNICmp = nullptr;
int __cdecl bStrNICmp(const char* a, const char* b, int n) {
    for (;;) {
        const int count = n--;
        if (count == 0) break;
        signed char ca = sc(*a);
        if (!ca) break;
        signed char cb = sc(*b);
        if (!cb) break;
        ca = up(ca);
        cb = up(cb);
        ++a;
        ++b;
        if (ca != cb) break;
    }
    if (n < 0) return 0;
    if (*a) return 1;
    if (*b) return -1;
    return up(sc(a[-1])) - up(sc(b[-1]));
}

// ---------------------------------------------------------------- bStrCat 0x0043DDA0 (73 callers): dst = a + b
using Cat_t = char*(__cdecl*)(char*, const char*, const char*);
Cat_t o_bStrCat = nullptr;
char* __cdecl bStrCat(char* dst, const char* a, const char* b) {
    int i = 0;
    for (; *a; ++a) dst[i++] = *a;
    for (; *b; ++b) dst[i++] = *b;
    dst[i] = 0;
    return dst;
}

// ---------------------------------------------------------------- bStrChr 0x0043DDF0 (6 callers)
using Chr_t = const char*(__cdecl*)(const char*, int);
Chr_t o_bStrChr = nullptr;
const char* __cdecl bStrChr(const char* s, int c) {
    if (!s) return nullptr;
    for (;;) {
        const signed char ch = sc(*s);
        if (ch == c) return s;
        ++s;
        if (!ch) return nullptr;
    }
}

// ---------------------------------------------------------------- bStrUpr 0x0043DE20 (1 caller), in place
using Upr_t = char*(__cdecl*)(char*);
Upr_t o_bStrUpr = nullptr;
char* __cdecl bStrUpr(char* s) {
    if (!*s) return s;
    for (;;) {
        *s = up(sc(*s));
        if (!s[1]) return s + 1;
        ++s;
    }
}

// ---------------------------------------------------------------- bStrToInt 0x0043DE50 (12 callers)
using ToInt_t = int(__cdecl*)(const char*);
ToInt_t o_bStrToInt = nullptr;
int __cdecl bStrToInt(const char* s) {
    if (s[0] == '0' && s[1] == 'x') {
        uint32_t v = 0;
        for (s += 2; *s; ++s) {
            signed char c = sc(*s);
            uint32_t d;
            if (c >= '0' && c <= '9') {
                d = static_cast<uint32_t>(c - '0');
            } else {
                c = up(c);
                if (c < 'A' || c > 'F') break;
                d = static_cast<uint32_t>(c - 0x37);
            }
            v = (v << 4) + d;
        }
        return static_cast<int>(v);
    }
    bool neg = false;
    if (*s == '-') {
        neg = true;
        ++s;
    } else if (*s == '+') {
        ++s;
    }
    uint32_t v = 0;
    for (; *s && sc(*s) >= '0' && sc(*s) <= '9'; ++s) v = v * 10 + static_cast<uint32_t>(sc(*s) - '0');
    return static_cast<int>(neg ? 0u - v : v);
}

// ---------------------------------------------------------------- wide strings
using WLen_t = int(__cdecl*)(const u16*);
WLen_t o_bWStrLen = nullptr;
int __cdecl bWStrLen(const u16* s) {  // 0x0043DFE0
    int n = 0;
    if (s)
        while (s[n]) ++n;
    return n;
}

using ToW_t = u16*(__cdecl*)(u16*, const char*);
ToW_t o_bStrToWStr = nullptr;
u16* __cdecl bStrToWStr(u16* dst, const char* src) {  // 0x0043E000, sign-extends
    u16* d = dst;
    for (;; ++d, ++src) {
        *d = static_cast<u16>(static_cast<int16_t>(sc(*src)));
        if (!*d) break;
    }
    return dst;
}

using NToW_t = u16*(__cdecl*)(u16*, const char*, int);
NToW_t o_bStrNToWStr = nullptr;
u16* __cdecl bStrNToWStr(u16* dst, const char* src, int n) {  // 0x0043E030
    if (n == 0) return dst;
    for (u16* d = dst;; ++d, ++src) {
        const u16 c = static_cast<u16>(static_cast<int16_t>(sc(*src)));
        --n;
        *d = c;
        if (!c || n == 0) break;
    }
    return dst;
}

// ---------------------------------------------------------------- test inputs
// A string placed at a random offset in an arena with random bytes before it (for the [-1] reads).
struct Arena {
    char buf[96];
    char* p;
    Arena(game::Rng& r, const std::string& s) {
        for (char& c : buf) c = static_cast<char>(r.next());
        p = buf + 1 + r.below(8);
        memcpy(p, s.c_str(), s.size() + 1);
    }
};

std::pair<std::string, std::string> pair_of(game::Rng& r) {
    std::string a = r.str(40), b = a;
    switch (r.below(4)) {
        case 0: break;
        case 1:
            for (char& c : b)
                if (r.below(2) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c ^= 0x20;
            break;
        case 2: b.resize(r.below(static_cast<uint32_t>(b.size()) + 1)); break;
        default: b = r.str(40);
    }
    if (r.below(2)) std::swap(a, b);
    return {a, b};
}

std::string number_str(game::Rng& r) {
    std::string s;
    switch (r.below(4)) {
        case 0: s = "0x"; for (uint32_t i = r.below(12); i; --i) s += "0123456789abcdefABCDEF"[r.below(22)]; break;
        case 1: if (r.below(3)) s += "+-"[r.below(2)]; for (uint32_t i = r.below(14); i; --i) s += char('0' + r.below(10)); break;
        case 2: s = r.str(16); break;
        default: s = std::to_string(static_cast<int>(r.next()));
    }
    if (r.below(3) == 0) s += r.str(4);  // trailing garbage
    return s;
}

int rand_n(game::Rng& r) { return r.below(10) == 0 ? -static_cast<int>(r.below(5)) : static_cast<int>(r.below(50)); }

}  // namespace

NFSU2_REWRITE(0x0043DC00, bStrNCpy, "char* __cdecl(char* dst, const char* src, int n)", o_bStrNCpy);
NFSU2_REWRITE(0x0043DC60, bStrNCmp, "int __cdecl(const char*, const char*, int n)", o_bStrNCmp);
NFSU2_REWRITE(0x0043DD10, bStrNICmp, "int __cdecl(const char*, const char*, int n)", o_bStrNICmp);
NFSU2_REWRITE(0x0043DDA0, bStrCat, "char* __cdecl(char* dst, const char* a, const char* b)", o_bStrCat);
NFSU2_REWRITE(0x0043DDF0, bStrChr, "const char* __cdecl(const char*, int c)", o_bStrChr);
NFSU2_REWRITE(0x0043DE20, bStrUpr, "char* __cdecl(char*) -> end", o_bStrUpr);
NFSU2_REWRITE(0x0043DE50, bStrToInt, "int __cdecl(const char*)", o_bStrToInt);
NFSU2_REWRITE(0x0043DFE0, bWStrLen, "int __cdecl(const uint16_t*)", o_bWStrLen);
NFSU2_REWRITE(0x0043E000, bStrToWStr, "uint16_t* __cdecl(uint16_t* dst, const char*)", o_bStrToWStr);
NFSU2_REWRITE(0x0043E030, bStrNToWStr, "uint16_t* __cdecl(uint16_t* dst, const char*, int n)", o_bStrNToWStr);

NFSU2_DIFF_TEST(bStrNCpy) {
    return game::run_diff(ctx, "bStrNCpy", o_bStrNCpy, [](game::Rng& r) {
        std::string s = r.str(40);
        // n bounded by the buffer: the original writes up to n bytes when the source is longer (if n < 0 it is a
        // runaway copy in the original too, so only test n >= 0)
        const int n = static_cast<int>(r.below(60));
        char da[80], db[80];
        const char fill = static_cast<char>(r.next());
        memset(da, fill, sizeof da);
        memset(db, fill, sizeof db);
        char* ra = o_bStrNCpy(da, s.c_str(), n);
        char* rb = bStrNCpy(db, s.c_str(), n);
        if (memcmp(da, db, sizeof da) || (ra - da) != (rb - db))
            return game::MismatchText("s=" + game::show(s) + " n=" + std::to_string(n) + " buffers/return differ");
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(bStrNCmp) {
    return game::run_diff(ctx, "bStrNCmp", o_bStrNCmp, [](game::Rng& r) {
        auto [x, y] = pair_of(r);
        Arena A(r, x), B(r, y);
        const int n = rand_n(r);
        int a = o_bStrNCmp(A.p, B.p, n), b = bStrNCmp(A.p, B.p, n);
        return a == b ? game::Ok()
                      : game::Mismatch("a=" + game::show(x) + " b=" + game::show(y) + " n=" + std::to_string(n), a, b);
    });
}

NFSU2_DIFF_TEST(bStrNICmp) {
    return game::run_diff(ctx, "bStrNICmp", o_bStrNICmp, [](game::Rng& r) {
        auto [x, y] = pair_of(r);
        Arena A(r, x), B(r, y);
        const int n = rand_n(r);
        int a = o_bStrNICmp(A.p, B.p, n), b = bStrNICmp(A.p, B.p, n);
        return a == b ? game::Ok()
                      : game::Mismatch("a=" + game::show(x) + " b=" + game::show(y) + " n=" + std::to_string(n), a, b);
    });
}

NFSU2_DIFF_TEST(bStrCat) {
    return game::run_diff(ctx, "bStrCat", o_bStrCat, [](game::Rng& r) {
        std::string x = r.str(30), y = r.str(30);
        char da[80], db[80];
        const char fill = static_cast<char>(r.next());
        memset(da, fill, sizeof da);
        memset(db, fill, sizeof db);
        char* ra = o_bStrCat(da, x.c_str(), y.c_str());
        char* rb = bStrCat(db, x.c_str(), y.c_str());
        if (memcmp(da, db, sizeof da) || (ra - da) != (rb - db))
            return game::MismatchText("a=" + game::show(x) + " b=" + game::show(y) + " buffers/return differ");
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(bStrChr) {
    return game::run_diff(ctx, "bStrChr", o_bStrChr, [](game::Rng& r) {
        std::string s = r.str(40);
        int c;
        switch (r.below(4)) {
            case 0: c = s.empty() ? 0 : sc(s[r.below(static_cast<uint32_t>(s.size()))]); break;  // present
            case 1: c = 0; break;                                                                  // terminator
            case 2: c = static_cast<int>(r.below(256)); break;                                     // unsigned byte value
            default: c = static_cast<int>(r.next());
        }
        const bool null_in = r.below(16) == 0;
        const char* p = null_in ? nullptr : s.c_str();
        const char* a = o_bStrChr(p, c);
        const char* b = bStrChr(p, c);
        if (a != b)
            return game::MismatchText("s=" + (null_in ? std::string("nullptr") : game::show(s)) + " c=" + std::to_string(c) +
                                      " offsets " + std::to_string(a ? a - p : -1) + " vs " + std::to_string(b ? b - p : -1));
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(bStrUpr) {
    return game::run_diff(ctx, "bStrUpr", o_bStrUpr, [](game::Rng& r) {
        std::string s = r.str(40);
        char da[64], db[64];
        memset(da, 0x5A, sizeof da);
        memset(db, 0x5A, sizeof db);
        memcpy(da, s.c_str(), s.size() + 1);
        memcpy(db, s.c_str(), s.size() + 1);
        char* ra = o_bStrUpr(da);
        char* rb = bStrUpr(db);
        if (memcmp(da, db, sizeof da) || (ra - da) != (rb - db))
            return game::MismatchText("s=" + game::show(s) + " buffers/return differ");
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(bStrToInt) {
    return game::run_diff(ctx, "bStrToInt", o_bStrToInt, [](game::Rng& r) {
        std::string s = number_str(r);
        int a = o_bStrToInt(s.c_str()), b = bStrToInt(s.c_str());
        return a == b ? game::Ok() : game::Mismatch("s=" + game::show(s), a, b);
    });
}

NFSU2_DIFF_TEST(bWStrLen) {
    return game::run_diff(ctx, "bWStrLen", o_bWStrLen, [](game::Rng& r) {
        if (r.below(16) == 0) {
            int a = o_bWStrLen(nullptr), b = bWStrLen(nullptr);
            return a == b ? game::Ok() : game::Mismatch("s=nullptr", a, b);
        }
        u16 w[64];
        const uint32_t n = r.below(60);
        for (uint32_t i = 0; i < n; ++i) w[i] = static_cast<u16>(1 + r.below(0xFFFF));
        w[n] = 0;
        int a = o_bWStrLen(w), b = bWStrLen(w);
        return a == b ? game::Ok() : game::Mismatch("len=" + std::to_string(n), a, b);
    });
}

NFSU2_DIFF_TEST(bStrToWStr) {
    return game::run_diff(ctx, "bStrToWStr", o_bStrToWStr, [](game::Rng& r) {
        std::string s = r.str(40);
        u16 da[64], db[64];
        memset(da, 0x5A, sizeof da);
        memset(db, 0x5A, sizeof db);
        u16* ra = o_bStrToWStr(da, s.c_str());
        u16* rb = bStrToWStr(db, s.c_str());
        if (memcmp(da, db, sizeof da) || (ra - da) != (rb - db))
            return game::MismatchText("s=" + game::show(s) + " buffers/return differ");
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(bStrNToWStr) {
    return game::run_diff(ctx, "bStrNToWStr", o_bStrNToWStr, [](game::Rng& r) {
        std::string s = r.str(40);
        const int n = static_cast<int>(r.below(60));
        u16 da[64], db[64];
        memset(da, 0x5A, sizeof da);
        memset(db, 0x5A, sizeof db);
        u16* ra = o_bStrNToWStr(da, s.c_str(), n);
        u16* rb = bStrNToWStr(db, s.c_str(), n);
        if (memcmp(da, db, sizeof da) || (ra - da) != (rb - db))
            return game::MismatchText("s=" + game::show(s) + " n=" + std::to_string(n) + " buffers/return differ");
        return game::Ok();
    });
}
