// EA bString library (NA v1.2 addresses), rewritten from the disassembly. All __cdecl.
//
// Quirks kept on purpose (the game's behaviour, not C's):
//  - hashes treat bytes as unsigned (movzx);
//  - bStrCmp / bStrICmp compare *signed* chars (movsx), unlike strcmp;
//  - bStrICmp uppercases with `& 0x5F` only for 'a'..'z' under a signed compare (bytes >= 0x80 untouched);
//  - bStrLen(nullptr) == 0; bStrCpy returns dst and copies the terminator.
#include <string>

#include "../diff.h"
#include "../rewrite.h"

namespace {

// ---------------------------------------------------------------- bStringHash 0x0043DB50 (800 callers)
using Hash_t = uint32_t(__cdecl*)(const char*);
Hash_t o_bStringHash = nullptr;

uint32_t __cdecl bStringHash(const char* s) {
    uint32_t h = 0xFFFFFFFFu;
    for (; *s; ++s) h = h * 33 + static_cast<uint8_t>(*s);
    return h;
}

// ---------------------------------------------------------------- bStringHash(str, prefix) 0x0043DB80 (113 callers)
using Hash2_t = uint32_t(__cdecl*)(const char*, uint32_t);
Hash2_t o_bStringHash2 = nullptr;

uint32_t __cdecl bStringHash2(const char* s, uint32_t h) {
    for (; *s; ++s) h = h * 33 + static_cast<uint8_t>(*s);
    return h;
}

// ---------------------------------------------------------------- bStrLen 0x0043DBB0 (115 callers)
using Len_t = int(__cdecl*)(const char*);
Len_t o_bStrLen = nullptr;

int __cdecl bStrLen(const char* s) {
    int n = 0;
    if (s)
        while (s[n]) ++n;
    return n;
}

// ---------------------------------------------------------------- bStrCpy 0x0043DBD0 (195 callers)
using Cpy_t = char*(__cdecl*)(char*, const char*);
Cpy_t o_bStrCpy = nullptr;

char* __cdecl bStrCpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++) != 0) {
    }
    return dst;
}

// ---------------------------------------------------------------- bStrCmp 0x0043DC30 (167 callers)
using Cmp_t = int(__cdecl*)(const char*, const char*);
Cmp_t o_bStrCmp = nullptr;

int __cdecl bStrCmp(const char* a, const char* b) {
    signed char ca, cb;
    do {
        ca = static_cast<signed char>(*a++);
        cb = static_cast<signed char>(*b++);
    } while (ca && cb && ca == cb);
    return ca - cb;
}

// ---------------------------------------------------------------- bStrICmp 0x0043DCC0 (92 callers)
Cmp_t o_bStrICmp = nullptr;

signed char upper(signed char c) { return (c >= 'a' && c <= 'z') ? static_cast<signed char>(c & 0x5F) : c; }

int __cdecl bStrICmp(const char* a, const char* b) {
    signed char ca, cb;
    do {
        ca = upper(static_cast<signed char>(*a++));
        cb = upper(static_cast<signed char>(*b++));
    } while (ca && cb && ca == cb);
    return ca - cb;
}

// Deliberately broken: hashes bytes as signed. Only differs for bytes >= 0x80 — the harness must catch it.
uint32_t __cdecl bStringHash_BROKEN(const char* s) {
    uint32_t h = 0xFFFFFFFFu;
    for (; *s; ++s) h = h * 33 + static_cast<uint32_t>(static_cast<int>(static_cast<signed char>(*s)));
    return h;
}

// a pair of strings that is often equal, often differs only in case, sometimes differs elsewhere
std::pair<std::string, std::string> str_pair(game::Rng& r) {
    std::string a = r.str(), b = a;
    switch (r.below(4)) {
        case 0: break;  // equal
        case 1:         // flip case of some letters
            for (char& c : b)
                if (r.below(2) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c ^= 0x20;
            break;
        case 2:  // truncate
            b.resize(r.below(static_cast<uint32_t>(b.size()) + 1));
            break;
        default:
            b = r.str();
    }
    return {a, b};
}

}  // namespace

NFSU2_REWRITE(0x0043DB50, bStringHash, "uint32_t __cdecl(const char*)", o_bStringHash);
NFSU2_REWRITE(0x0043DB80, bStringHash2, "uint32_t __cdecl(const char*, uint32_t prefix)", o_bStringHash2);
NFSU2_REWRITE(0x0043DBB0, bStrLen, "int __cdecl(const char*)", o_bStrLen);
NFSU2_REWRITE(0x0043DBD0, bStrCpy, "char* __cdecl(char* dst, const char* src)", o_bStrCpy);
NFSU2_REWRITE(0x0043DC30, bStrCmp, "int __cdecl(const char*, const char*)", o_bStrCmp);
NFSU2_REWRITE(0x0043DCC0, bStrICmp, "int __cdecl(const char*, const char*)", o_bStrICmp);

NFSU2_DIFF_TEST(bStringHash) {
    return game::run_diff(ctx, "bStringHash", o_bStringHash, [](game::Rng& r) {
        std::string s = r.str();
        uint32_t a = o_bStringHash(s.c_str()), b = bStringHash(s.c_str());
        return a == b ? game::Ok() : game::Mismatch("s=" + game::show(s), a, b);
    });
}

NFSU2_DIFF_TEST(bStringHash2) {
    return game::run_diff(ctx, "bStringHash2", o_bStringHash2, [](game::Rng& r) {
        std::string s = r.str();
        uint32_t prefix = r.next();
        uint32_t a = o_bStringHash2(s.c_str(), prefix), b = bStringHash2(s.c_str(), prefix);
        return a == b ? game::Ok() : game::Mismatch("s=" + game::show(s) + " prefix=" + std::to_string(prefix), a, b);
    });
}

NFSU2_DIFF_TEST(bStrLen) {
    return game::run_diff(ctx, "bStrLen", o_bStrLen, [](game::Rng& r) {
        if (r.below(16) == 0) {  // null pointer case
            int a = o_bStrLen(nullptr), b = bStrLen(nullptr);
            return a == b ? game::Ok() : game::Mismatch("s=nullptr", a, b);
        }
        std::string s = r.str();
        int a = o_bStrLen(s.c_str()), b = bStrLen(s.c_str());
        return a == b ? game::Ok() : game::Mismatch("s=" + game::show(s), a, b);
    });
}

NFSU2_DIFF_TEST(bStrCpy) {
    return game::run_diff(ctx, "bStrCpy", o_bStrCpy, [](game::Rng& r) {
        std::string s = r.str();
        // each run writes into its own buffer pre-filled with the same garbage; compare buffers and return values
        char da[80], db[80];
        const uint8_t fill = static_cast<uint8_t>(r.next());
        memset(da, fill, sizeof da);
        memset(db, fill, sizeof db);
        char* ra = o_bStrCpy(da, s.c_str());
        char* rb = bStrCpy(db, s.c_str());
        if (memcmp(da, db, sizeof da) != 0) return game::MismatchText("s=" + game::show(s) + " output buffers differ");
        if ((ra - da) != (rb - db)) return game::MismatchText("s=" + game::show(s) + " return values differ");
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(bStrCmp) {
    return game::run_diff(ctx, "bStrCmp", o_bStrCmp, [](game::Rng& r) {
        auto [x, y] = str_pair(r);
        int a = o_bStrCmp(x.c_str(), y.c_str()), b = bStrCmp(x.c_str(), y.c_str());
        return a == b ? game::Ok() : game::Mismatch("a=" + game::show(x) + " b=" + game::show(y), a, b);
    });
}

NFSU2_DIFF_TEST(bStrICmp) {
    return game::run_diff(ctx, "bStrICmp", o_bStrICmp, [](game::Rng& r) {
        auto [x, y] = str_pair(r);
        int a = o_bStrICmp(x.c_str(), y.c_str()), b = bStrICmp(x.c_str(), y.c_str());
        return a == b ? game::Ok() : game::Mismatch("a=" + game::show(x) + " b=" + game::show(y), a, b);
    });
}

// The harness's own check: this one MUST report mismatches.
NFSU2_DIFF_TEST(bStringHash_BROKEN) {
    return game::run_diff(ctx, "bStringHash_BROKEN", o_bStringHash, [](game::Rng& r) {
        std::string s = r.str();
        uint32_t a = o_bStringHash(s.c_str()), b = bStringHash_BROKEN(s.c_str());
        return a == b ? game::Ok() : game::Mismatch("s=" + game::show(s), a, b);
    });
}
