// SpeedScript: EA's text-config parser (EA core library, zBWare.cpp), written from the PC machine code.
//
// PC layout (0x268 bytes):
//   +000 int   loaded texts (1 + INCLUDESCRIPT files)       +004 files[4] {char name[0x50]; char* text}
//   +154 int   entry count   +158 capacity   +15C Entry* entries   +160 cursor
//   +164 char  error message[0x100]                          +264 error callback (bBreak when "break on error")
//   Entry (12): u8 file, u8 is_command ("WORD:"), u16 entries until the next command, u32 offset into the file's
//               word buffer, u16 line
// Words are split at space, tab, '=', ',', CR, LF; "//" comments to the end of the line; "..." quotes ("" inside
// quotes = a literal quote). A word ending in ':' is a command and is upper-cased (a-z, signed char compare).
//
// Not rewritten here (they read files): the file constructor 0x440840 and HandleIncludeScript 0x4408A0 (called
// from Init when a command is "INCLUDESCRIPT:").
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

struct Entry {
    uint8_t file, cmd;
    uint16_t skip;
    uint32_t offset;
    uint16_t line, pad;
};
static_assert(sizeof(Entry) == 12, "Entry");
struct FileRec {
    char name[0x50];
    char* text;
};
struct Script {
    int32_t nfiles;
    FileRec files[4];
    int32_t count, cap;
    Entry* entries;
    int32_t cursor;
    char error[0x100];
    void* callback;
};
static_assert(sizeof(Script) == 0x268 && offsetof(Script, count) == 0x154 && offsetof(Script, error) == 0x164 &&
                  offsetof(Script, callback) == 0x264, "SpeedScript layout");

inline void* op_new(uint32_t n) { return game_fn<void*(__cdecl*)(uint32_t)>(0x00575620)(n); }
inline void op_delete(void* p) { game_fn<void(__cdecl*)(void*)>(0x00575630)(p); }
inline int32_t str_icmp(const char* a, const char* b) { return game_fn<int32_t(__cdecl*)(const char*, const char*)>(0x0043DCC0)(a, b); }
inline char* word(Script* s, const Entry* e) { return *reinterpret_cast<char**>(reinterpret_cast<uint8_t*>(s) + (e->file + 1) * 0x54) + e->offset; }
// entries + i*12 as the original computes it (its null checks test this sum)
inline Entry* entry_at(Script* s, int32_t i) {
    return reinterpret_cast<Entry*>(reinterpret_cast<uintptr_t>(s->entries) + static_cast<uintptr_t>(i) * 12);
}
char* const kPosBuf = reinterpret_cast<char*>(0x00828540);
using SPrintf_t = int(__cdecl*)(char*, const char*, ...);

// ---------------------------------------------------------------- 0x0043E060 ~SpeedScript
using Dtor_t = void(__thiscall*)(Script*);
Dtor_t o_ScriptDtor = nullptr;
void __cdecl ScriptDtor(Script* s) {
    for (int32_t i = 0; i < s->nfiles; ++i) op_delete(s->files[i].text);
    if (s->entries) op_delete(s->entries);
}

// ---------------------------------------------------------------- 0x0043E0A0 ResizeEntryTable(n)
using Resize_t = void(__thiscall*)(Script*, int32_t);
Resize_t o_ScriptResize = nullptr;
void __cdecl ScriptResize(Script* s, int32_t n) {
    Entry* t = static_cast<Entry*>(op_new(static_cast<uint32_t>(n * 12)));
    if (s->entries) {
        std::memcpy(t, s->entries, static_cast<uint32_t>(s->count * 12));
        op_delete(s->entries);
    }
    s->entries = t;
    s->cap = n;
}

// ---------------------------------------------------------------- 0x0043E120 ParseNextWord (stdcall, AL = found)
using Parse_t = uint8_t(__stdcall*)(char*, const char*, int32_t, int32_t*, int32_t*);
Parse_t o_ParseNextWord = nullptr;
inline bool sep(char c) { return c == ' ' || c == '\t' || c == '=' || c == ',' || c == '\r'; }
uint8_t __cdecl ParseNextWord(char* out, const char* text, int32_t size, int32_t* pos, int32_t* line) {
    int32_t i = *pos;
    bool comment = false;
    char c;
    for (;;) {  // skip separators, comments, line ends
        if (i >= size) break;
        c = text[i];
        if (c == '\r') {
            ++i;
            if (i >= size) break;
            c = text[i];
        }
        if (c == 0) break;
        if (c == '\n') {
            ++*line;
            comment = false;
            ++i;
            continue;
        }
        if (comment) {
            ++i;
            continue;
        }
        if (c == '/') {
            if (text[i + 1] == '/') {
                comment = true;
                ++i;
                continue;
            }
            goto word;
        }
        if (sep(c)) {
            ++i;
            continue;
        }
        goto word;
    }
    *pos = i;
    return 0;
word: {
    bool quoted = false;
    int32_t n = 0;
    for (;;) {
        c = text[i];
        if (c == 0) break;
        if (!quoted && (c == ' ' || c == '\n' || c == '\t' || c == '=' || c == ',' || c == '\r')) break;
        if (c == '"') {
            if (quoted && text[i + 1] == '"') {
                out[n++] = '"';
                ++i;
            } else {
                quoted = !quoted;
            }
        } else {
            out[n++] = c;
        }
        ++i;
        if (i >= size) break;
    }
    out[n] = 0;
    *pos = i;
    return 1;
}
}

// ---------------------------------------------------------------- 0x0043E200 Init(name, text, size)
using Init_t = void(__thiscall*)(Script*, const char*, const char*, int32_t);
Init_t o_ScriptInit = nullptr;
void __cdecl ScriptInit(Script* s, const char* name, const char* text, int32_t size) {
    s->error[0] = 0;
    s->nfiles = 1;
    {
        char* d = s->files[0].name;
        while ((*d++ = *name++) != 0) {
        }
    }
    s->files[0].text = static_cast<char*>(op_new(static_cast<uint32_t>(size)));
    s->count = s->cap = 0;
    s->entries = nullptr;
    ScriptResize(s, size / 16 + 0x20);
    s->cursor = 0;
    int32_t pos = 0, line = 1, offset = 0;
    if (ParseNextWord(s->files[0].text, text, size, &pos, &line)) {
        for (;;) {
            char* w = s->files[0].text + offset;
            int32_t len = 0;
            if (w && *w) do ++len; while (w[len]);  // strlen
            if (s->count == s->cap) ScriptResize(s, s->cap * 4 / 3 + 1);
            Entry* e = &s->entries[s->count++];
            std::memset(e, 0, sizeof *e);
            e->line = static_cast<uint16_t>(line);
            e->offset = static_cast<uint32_t>(offset);
            if (w[len - 1] == ':') {
                e->cmd = 1;
                for (int32_t k = 0; k < len; ++k) {
                    const signed char ch = static_cast<signed char>(w[k]);
                    w[k] = (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch & 0x5F) : static_cast<char>(ch);
                }
            }
            const int32_t next = offset + len + 1;
            if (s->count >= 2 && !e->cmd && e[-1].cmd) {
                const char* a = word(s, &e[-1]);
                const char* b = "INCLUDESCRIPT:";
                signed char ca, cb;
                do {
                    ca = static_cast<signed char>(*a++);
                    cb = static_cast<signed char>(*b++);
                } while (ca && cb && ca == cb);
                if (ca == cb) game_fn<void(__thiscall*)(Script*, char*)>(0x004408A0)(s, word(s, e));
            }
            if (!ParseNextWord(s->files[0].text + next, text, size, &pos, &line)) break;
            offset = next;
        }
    }
    int32_t upto = s->count;
    for (int32_t i = s->count - 1; i >= 0; --i) {
        s->entries[i].skip = static_cast<uint16_t>(upto - i);
        if (s->entries[i].cmd) upto = i;
    }
}

// ---------------------------------------------------------------- cursor walkers
// 0x0043E430 GetNextCommand(): the next command, whatever its name
using Get0_t = char*(__thiscall*)(Script*);
Get0_t o_ScriptNextCommandAny = nullptr;
char* __cdecl ScriptNextCommandAny(Script* s) {
    const int32_t count = s->count;
    if (s->cursor >= count) return nullptr;
    Entry* e = entry_at(s, s->cursor);
    if (!e) return nullptr;
    if (!e->cmd) {
        s->cursor += e->skip;
        if (s->cursor >= count) return nullptr;
        e = entry_at(s, s->cursor);
        if (!e) return nullptr;
    }
    ++s->cursor;
    return word(s, e);
}
// 0x0043E4A0 GetNextCommand(name): the next command named `name` (bStrICmp)
using Get1_t = char*(__thiscall*)(Script*, const char*);
Get1_t o_ScriptNextCommand = nullptr;
char* __cdecl ScriptNextCommand(Script* s, const char* name) {
    const int32_t count = s->count;
    for (;;) {
        if (s->cursor >= count) return nullptr;
        Entry* e = entry_at(s, s->cursor);
        if (!e) return nullptr;
        if (!e->cmd) {
            s->cursor += e->skip;
            if (s->cursor >= count) return nullptr;
            e = entry_at(s, s->cursor);
            if (!e) return nullptr;
        }
        ++s->cursor;
        char* w = word(s, e);
        if (!w) return nullptr;
        if (str_icmp(w, name) == 0) return w;
    }
}
// 0x0043E530 IsAnotherArgument
using Is_t = int32_t(__thiscall*)(Script*);
Is_t o_ScriptIsAnotherArgument = nullptr;
int32_t __cdecl ScriptIsAnotherArgument(Script* s) {
    if (s->cursor >= s->count) return 0;
    Entry* e = entry_at(s, s->cursor);
    return (e && !e->cmd) ? 1 : 0;
}
// 0x0043E560 GetNextArgument
Get0_t o_ScriptNextArgument = nullptr;
char* __cdecl ScriptNextArgument(Script* s) {
    const int32_t cur = s->cursor;
    if (cur >= s->count) return nullptr;
    Entry* e = entry_at(s, cur);
    if (!e || e->cmd) return nullptr;
    s->cursor = cur + 1;
    return word(s, e);
}

// ---------------------------------------------------------------- 0x0043F940 SpeedScript(name, text, break_on_error)
using CtorText_t = Script*(__thiscall*)(Script*, const char*, const char*, int32_t);
CtorText_t o_ScriptCtorText = nullptr;
Script* __cdecl ScriptCtorText(Script* s, const char* name, const char* text, int32_t brk) {
    s->callback = brk ? reinterpret_cast<void*>(0x0043BD50) : nullptr;  // bBreak
    int32_t len = 0;
    if (text && *text)
        while (text[++len]) {
        }
    ScriptInit(s, name, text, len + 1);
    return s;
}

// ---------------------------------------------------------------- 0x0043FC40 Error(this, fmt, ...) (cdecl varargs)
// EAX: the callback pointer when there is none (0), else whatever the callback returns (bBreak: its own address).
using Error_t = uint32_t(__cdecl*)(Script*, const char*, ...);
Error_t o_ScriptError = nullptr;
uint32_t __cdecl ScriptError(Script* s, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    game_fn<void(__cdecl*)(char*, const char*, va_list)>(0x0043FBC0)(s->error, fmt, va);  // bVSPrintf
    va_end(va);
    const uintptr_t cb = reinterpret_cast<uintptr_t>(s->callback);
    if (!cb) return 0;
    return game_fn<uint32_t(__cdecl*)(char*)>(cb)(s->error);
}

// ---------------------------------------------------------------- 0x004400F0 GetPositionName -> static buffer
Get0_t o_ScriptPositionName = nullptr;
char* __cdecl ScriptPositionName(Script* s) {
    const int32_t cur = s->cursor;
    const SPrintf_t sp = game_fn<SPrintf_t>(0x004400D0);  // bSPrintf
    if (cur > 0 && cur <= s->count) {
        Entry* e = entry_at(s, cur - 1);
        if (e) {
            sp(kPosBuf, reinterpret_cast<const char*>(0x00786E74), static_cast<uint32_t>(e->line), s->files[e->file].name);
            return kPosBuf;
        }
    }
    sp(kPosBuf, reinterpret_cast<const char*>(0x00786E70), s->files[0].name);
    return kPosBuf;
}

// ---------------------------------------------------------------- typed arguments
// 0x00440160 GetNextArgumentString: like GetNextArgument, but a missing one is an error ("Not enough arguments")
Get0_t o_ScriptArgString = nullptr;
char* __cdecl ScriptArgString(Script* s) {
    char* w = nullptr;
    const int32_t cur = s->cursor;
    if (cur < s->count) {
        Entry* e = entry_at(s, cur);
        if (e && !e->cmd) {
            s->cursor = cur + 1;
            w = word(s, e);
            if (w) return w;
        }
    }
    ScriptError(s, reinterpret_cast<const char*>(0x00786E84), ScriptPositionName(s));
    return w;
}
// 0x004401C0 GetNextArgumentInt: '0'..'9' (signed char) or '-' first -> bStrToLong, else an error and 0
Is_t o_ScriptArgInt = nullptr;
int32_t __cdecl ScriptArgInt(Script* s) {
    char* w = ScriptArgString(s);
    if (!w) return 0;
    const signed char c = static_cast<signed char>(w[0]);
    if ((c == '0' && w[1] == 'x') || (c >= '0' && c <= '9') || c == '-')
        return game_fn<int32_t(__cdecl*)(const char*)>(0x0043DE50)(w);  // bStrToLong
    ScriptError(s, reinterpret_cast<const char*>(0x00786EA0), w, ScriptPositionName(s));
    return 0;
}
// 0x00440210 GetNextArgumentShort: `mov ax, si` -> on the error path EAX's high half is what Error left
Is_t o_ScriptArgShort = nullptr;
int32_t __cdecl ScriptArgShort(Script* s) {
    const int32_t v = ScriptArgInt(s);
    if (v < -0x8000 || v > 0xFFFF) {
        const uint32_t eax = ScriptError(s, reinterpret_cast<const char*>(0x00786EC8), v, ScriptPositionName(s));
        return static_cast<int32_t>((eax & 0xFFFF0000u) | (static_cast<uint32_t>(v) & 0xFFFFu));
    }
    return v;
}

}  // namespace

// 0x00440250 GetNextArgumentFloat: bStrToFloat(GetNextArgumentString()); the result stays in st0 untouched
// (naked: no float spill could round an extended-range value)
static void* const g_ScriptArgString = reinterpret_cast<void*>(&ScriptArgString);
static void* const g_StrToFloat = reinterpret_cast<void*>(0x0043DEF0);
using GetF_t = float(__thiscall*)(Script*);
static GetF_t o_ScriptArgFloat = nullptr;
extern "C" __declspec(naked) void ScriptArgFloat() {  // cdecl (Script* s), result in st0
    __asm {
        push dword ptr [esp + 4]
        call dword ptr [g_ScriptArgString]
        add esp, 4
        push eax
        call dword ptr [g_StrToFloat]
        add esp, 4
        ret
    }
}

NFSU2_REWRITE_THIS(0x0043E060, ScriptDtor, 0, "SpeedScript::~SpeedScript()", o_ScriptDtor);
NFSU2_REWRITE_THIS(0x0043E0A0, ScriptResize, 1, "void SpeedScript::ResizeEntryTable(int n)", o_ScriptResize);
NFSU2_REWRITE_STD(0x0043E120, ParseNextWord, 5, "bool __stdcall SpeedScript::ParseNextWord(char* out, const char* text, int size, int* pos, int* line)", o_ParseNextWord);
NFSU2_REWRITE_THIS(0x0043E200, ScriptInit, 3, "void SpeedScript::Init(const char* name, const char* text, int size)", o_ScriptInit);
NFSU2_REWRITE_THIS(0x0043E430, ScriptNextCommandAny, 0, "char* SpeedScript::GetNextCommand()", o_ScriptNextCommandAny);
NFSU2_REWRITE_THIS(0x0043E4A0, ScriptNextCommand, 1, "char* SpeedScript::GetNextCommand(const char* name)", o_ScriptNextCommand);
NFSU2_REWRITE_THIS(0x0043E530, ScriptIsAnotherArgument, 0, "int SpeedScript::IsAnotherArgument()", o_ScriptIsAnotherArgument);
NFSU2_REWRITE_THIS(0x0043E560, ScriptNextArgument, 0, "char* SpeedScript::GetNextArgument()", o_ScriptNextArgument);
NFSU2_REWRITE_THIS(0x0043F940, ScriptCtorText, 3, "SpeedScript::SpeedScript(const char* name, const char* text, int break_on_error)", o_ScriptCtorText);
NFSU2_REWRITE_N(0x0043FC40, ScriptError, 8, "void __cdecl SpeedScript::Error(this, const char* fmt, ...)", o_ScriptError);
NFSU2_REWRITE_THIS(0x004400F0, ScriptPositionName, 0, "char* SpeedScript::GetPositionName() (static buffer 0x828540)", o_ScriptPositionName);
NFSU2_REWRITE_THIS(0x00440160, ScriptArgString, 0, "char* SpeedScript::GetNextArgumentString()", o_ScriptArgString);
NFSU2_REWRITE_THIS(0x004401C0, ScriptArgInt, 0, "int SpeedScript::GetNextArgumentInt()", o_ScriptArgInt);
NFSU2_REWRITE_THIS(0x00440210, ScriptArgShort, 0, "short SpeedScript::GetNextArgumentShort()", o_ScriptArgShort);
NFSU2_REWRITE_THIS(0x00440250, ScriptArgFloat, 0, "float SpeedScript::GetNextArgumentFloat()", o_ScriptArgFloat);

namespace {

const char* const kCommands[] = {"CAR:", "Engine:", "tires:", "GEAR:", "name:", "Ratio:", "x:", "TRACK:", "ab_c9:", "UNKNOWN:"};
const char* const kWords[] = {"12", "-7", "0x1F", "0", "3.25", "-0.5", "1e3", "abc", "Hello", "\"quoted text\"",
                              "\"a \"\"b\"\" c\"", "\"\"", "0xZ", "+5", "99999", "-40000", "65535", "70000",
                              "w\xE9ird", "x=y", "a,b", "lower:", "\"x:\""};
const char* const kSeps[] = {" ", "  ", "\t", "=", ",", " = ", "\n", "\r\n", "\n\n", " // comment\n", "//c\r\n", "\r"};

std::string make_script(game::Rng& r) {
    std::string t;
    for (int i = 0, n = 1 + r.below(30); i < n; ++i) {
        t += kCommands[r.below(10)];
        for (int k = 0, m = r.below(5); k < m; ++k) {
            t += kSeps[r.below(12)];
            t += kWords[r.below(23)];
        }
        t += kSeps[r.below(12)];
        if (r.below(20) == 0) t += static_cast<char>(1 + r.below(255));  // stray bytes
    }
    if (r.below(10) == 0) t.insert(r.below(static_cast<uint32_t>(t.size()) + 1), 1, '\0');  // NUL inside
    if (r.below(10) == 0) t += "\"unterminated";
    return t;
}

struct Pair {  // one object built by the original code, one by ours
    alignas(16) uint8_t a[sizeof(Script)];
    alignas(16) uint8_t b[sizeof(Script)];
    Script* A() { return reinterpret_cast<Script*>(a); }
    Script* B() { return reinterpret_cast<Script*>(b); }
};

std::string state_diff(Script* x, Script* y) {
    if (x->nfiles != y->nfiles || x->count != y->count || x->cap != y->cap || x->cursor != y->cursor)
        return "counters";
    if (std::strcmp(x->files[0].name, y->files[0].name)) return "name";
    if (std::strcmp(x->error, y->error)) return "error text";
    if (x->callback != y->callback) return "callback";
    for (int32_t i = 0; i < x->count; ++i) {
        if (std::memcmp(&x->entries[i], &y->entries[i], 10)) return "entry " + std::to_string(i);
        if (std::strcmp(word(x, &x->entries[i]), word(y, &y->entries[i]))) return "word " + std::to_string(i);
    }
    return "";
}
// a result pointer as an offset into its object's word buffer (null -> -1, static buffer -> -2)
int64_t rel(Script* s, const char* p) {
    if (!p) return -1;
    if (p == kPosBuf) return -2;
    return p - s->files[0].text;
}

}  // namespace

NFSU2_DIFF_TEST(SpeedScript) {
    return game::run_diff(ctx, "SpeedScript", o_ScriptInit, [](game::Rng& r) -> game::Case {
        const std::string text = make_script(r);
        Pair p;
        std::memset(p.a, 0x5A, sizeof p.a);
        std::memset(p.b, 0x5A, sizeof p.b);
        const char* name = r.below(2) ? "test.txt" : "data\\scripts\\x.spd";
        if (r.below(4) == 0) {  // Init directly with an explicit size (may cut the text short)
            const int32_t size = 1 + static_cast<int32_t>(r.below(static_cast<uint32_t>(text.size()) + 1));
            p.A()->callback = p.B()->callback = nullptr;
            o_ScriptInit(p.A(), name, text.c_str(), size);
            ScriptInit(p.B(), name, text.c_str(), size);
        } else {
            o_ScriptCtorText(p.A(), name, text.c_str(), 0);
            ScriptCtorText(p.B(), name, text.c_str(), 0);
        }
        std::string d = state_diff(p.A(), p.B());
        game::Case result = d.empty() ? game::Ok() : game::MismatchText("after Init: " + d);
        // the same random queries on both
        for (int q = 0, n = result.ok ? 1 + r.below(40) : 0; q < n && result.ok; ++q) {
            const uint32_t op = r.below(9);
            int64_t x = 0, y = 0;
            std::string sx, sy;
            switch (op) {
                case 0: { const char* nm = kCommands[r.below(10)]; x = rel(p.A(), o_ScriptNextCommand(p.A(), nm)); y = rel(p.B(), ScriptNextCommand(p.B(), nm)); break; }
                case 1: x = rel(p.A(), o_ScriptNextCommandAny(p.A())); y = rel(p.B(), ScriptNextCommandAny(p.B())); break;
                case 2: x = o_ScriptIsAnotherArgument(p.A()); y = ScriptIsAnotherArgument(p.B()); break;
                case 3: x = rel(p.A(), o_ScriptNextArgument(p.A())); y = rel(p.B(), ScriptNextArgument(p.B())); break;
                case 4: x = rel(p.A(), o_ScriptArgString(p.A())); y = rel(p.B(), ScriptArgString(p.B())); break;
                case 5: x = o_ScriptArgInt(p.A()); y = ScriptArgInt(p.B()); break;
                case 6: x = o_ScriptArgShort(p.A()); y = ScriptArgShort(p.B()); break;
                case 7: {
                    // the original crashes on a missing argument (bStrToFloat(NULL)): only ask when there is one
                    if (!o_ScriptIsAnotherArgument(p.A()) || !ScriptIsAnotherArgument(p.B())) break;
                    const float fa = o_ScriptArgFloat(p.A());
                    const float fb = game_fn<float(__cdecl*)(Script*)>(reinterpret_cast<uintptr_t>(&ScriptArgFloat))(p.B());
                    x = game::same_float(fa, fb);
                    y = 1;
                    break;
                }
                default:
                    sx = o_ScriptPositionName(p.A());
                    sy = ScriptPositionName(p.B());
                    break;
            }
            d = state_diff(p.A(), p.B());
            if (x != y || sx != sy || !d.empty())
                result = game::MismatchText("query " + std::to_string(op) + " #" + std::to_string(q) + " " + d);
        }
        o_ScriptDtor(p.A());
        ScriptDtor(p.B());
        return result;
    });
}

NFSU2_DIFF_TEST(SpeedScript_parse) {  // ParseNextWord on its own: word, position, line counter, AL
    return game::run_diff(ctx, "SpeedScript_parse", o_ParseNextWord, [](game::Rng& r) -> game::Case {
        std::string text = make_script(r);
        text += std::string(4, '\0');  // the original may read one byte past `size`
        const int32_t size = static_cast<int32_t>(r.below(static_cast<uint32_t>(text.size()) - 3) + 1);
        int32_t pa = static_cast<int32_t>(r.below(static_cast<uint32_t>(size) + 1)), pb = pa, la = static_cast<int32_t>(r.below(100)), lb = la;
        std::vector<char> oa(text.size() + 8, 'q'), ob(text.size() + 8, 'q');
        const uint8_t ra = o_ParseNextWord(oa.data(), text.data(), size, &pa, &la);
        const uint8_t rb = ParseNextWord(ob.data(), text.data(), size, &pb, &lb);
        return ra == rb && pa == pb && la == lb && oa == ob ? game::Ok() : game::MismatchText("ParseNextWord");
    });
}

NFSU2_DIFF_TEST(regs_SpeedScript) {
    using game::arg;
    return game::run_diff(ctx, "regs_SpeedScript", o_ScriptArgInt, [](game::Rng& r) -> game::Case {
        const std::string text = make_script(r) + std::string(4, '\0');
        Pair p;
        o_ScriptCtorText(p.A(), "t", text.c_str(), 0);
        o_ScriptCtorText(p.B(), "t", text.c_str(), 0);
        game::Case c = game::Ok();
        // getters: EAX compared (both objects advance the same way); thiscall: pops checked
        for (int i = 0; i < 8 && c.ok; ++i) {
            const uint32_t op = r.below(5);
            const void* fo = op == 0 ? (void*)o_ScriptArgInt : op == 1 ? (void*)o_ScriptArgShort : op == 2 ? (void*)o_ScriptIsAnotherArgument : op == 3 ? (void*)o_ScriptPositionName : (void*)o_ScriptNextCommandAny;
            const void* fm = op == 0 ? NFSU2_ENTRY(ScriptArgInt) : op == 1 ? NFSU2_ENTRY(ScriptArgShort) : op == 2 ? NFSU2_ENTRY(ScriptIsAnotherArgument) : op == 3 ? NFSU2_ENTRY(ScriptPositionName) : NFSU2_ENTRY(ScriptNextCommandAny);
            const game::RegOutThis a = game::call_regs_this(fo, p.A(), nullptr, 0);
            const game::RegOutThis b = game::call_regs_this(fm, p.B(), nullptr, 0);
            const bool ptr = op == 4;  // pointers into each object's own buffer
            const bool same_eax = ptr ? rel(p.A(), reinterpret_cast<const char*>(a.eax)) == rel(p.B(), reinterpret_cast<const char*>(b.eax)) : a.eax == b.eax;
            if (!same_eax || a.popped != b.popped || (a.edx == game::kSentEDX && b.edx != game::kSentEDX))
                c = game::MismatchText("register contract op " + std::to_string(op));
        }
        // ParseNextWord: stdcall, AL only
        if (c.ok) {
            int32_t pa = 0, pb = 0, la = 1, lb = 1;
            std::vector<char> oa(text.size() + 8), ob(text.size() + 8);
            const uint32_t argsA[] = {arg(oa.data()), arg(text.c_str()), static_cast<uint32_t>(text.size() - 4), arg(&pa), arg(&la)};
            const uint32_t argsB[] = {arg(ob.data()), arg(text.c_str()), static_cast<uint32_t>(text.size() - 4), arg(&pb), arg(&lb)};
            const game::RegOutThis a = game::call_regs_this((void*)o_ParseNextWord, nullptr, argsA, 5);
            const game::RegOutThis b = game::call_regs_this(NFSU2_ENTRY(ParseNextWord), nullptr, argsB, 5);
            if ((a.eax & 0xFF) != (b.eax & 0xFF) || a.popped != b.popped) c = game::MismatchText("ParseNextWord AL/pops");
        }
        o_ScriptDtor(p.A());
        o_ScriptDtor(p.B());
        return c;
    });
}

NFSU2_DIFF_TEST(SpeedScript_BROKEN) {  // harness self-check: '+' accepted as a number
    return game::run_diff(ctx, "SpeedScript_BROKEN", o_ScriptArgInt, [](game::Rng& r) -> game::Case {
        Pair p;
        const std::string text = std::string("N: +") + std::to_string(r.below(1000));
        o_ScriptCtorText(p.A(), "t", text.c_str(), 0);
        o_ScriptNextCommandAny(p.A());
        const int32_t v = o_ScriptArgInt(p.A());
        const int32_t bad = std::atoi(text.c_str() + 3);
        o_ScriptDtor(p.A());
        return v == bad ? game::Ok() : game::MismatchText("'+' number");
    });
}
