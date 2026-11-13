// Checks the parts of the MSVC x86 ABI that our code relies on when it touches game objects.
// Built with our toolchain (clang-cl) every check must PASS. Built with MinGW g++ some must FAIL,
// which shows the test can tell the two ABIs apart. Exit code = number of failures.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <typeinfo>

// Hide a pointer's origin from the optimizer. Calling a known function through a different function type
// is UB that clang turns into a trap; real game addresses are opaque integers, which this models.
template <class T> static T opaque(T v) { volatile T x = v; return x; }

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// --- 1. member functions use __thiscall: `this` in ECX, args on stack, callee pops ---
struct Counter {
    int base;
    __declspec(noinline) int add(int a, int b) { return base + a * 10 + b; }
};
using AddThiscall = int(__thiscall*)(Counter*, int, int);
// The usual hook trick: a __fastcall function sees ECX as arg1 and EDX (garbage) as arg2.
using AddFastcallShim = int(__fastcall*)(Counter*, void* edx, int, int);

// --- 2. vtable: vfptr at offset 0, virtual destructor takes ONE slot (scalar deleting dtor) ---
struct Base {
    virtual ~Base() {}
    virtual int id() { return 1; }
    int field = 0x1234;
};
struct Derived : Base {
    int id() override { return 2; }
};

// --- 3. struct layout rules ---
struct AlignDouble { char c; double d; int i; };       // MSVC: d at 8, size 24
struct Bits { char a : 4; int b : 4; };                 // MSVC: types differ -> new unit, size 8
#pragma pack(push, 1)
struct Packed { char c; int i; short s; };              // size 7
#pragma pack(pop)
struct EmptyBaseHolder : Base { char tail; };           // vfptr(4) + field(4) + tail -> 12

int main() {
    // 1
    Counter c{100};
    auto mfp = &Counter::add;
    void* raw;
    std::memcpy(&raw, &mfp, sizeof raw);                // MSVC: plain code pointer for single inheritance
    raw = opaque(raw);
    check(sizeof(mfp) == 4, "pointer-to-member-function is 4 bytes (single inheritance)");
    check(reinterpret_cast<AddThiscall>(raw)(&c, 2, 3) == 123, "member function callable as __thiscall(this, a, b)");
    check(reinterpret_cast<AddFastcallShim>(raw)(&c, nullptr, 4, 5) == 145, "member function callable via __fastcall(this, edx, a, b) shim");

    // 2
    Base* obj = opaque<Base*>(new Derived);
    void** vtbl = opaque(*reinterpret_cast<void***>(obj));
    using IdFn = int(__thiscall*)(Base*);
    check(reinterpret_cast<IdFn>(vtbl[1])(obj) == 2, "virtual dtor occupies one slot: id() is vtable[1]");
    check(reinterpret_cast<int*>(obj)[1] == 0x1234, "vfptr at offset 0, first field at offset 4");
    // MSVC RTTI: the CompleteObjectLocator pointer sits at vtable[-1] and has signature 0 on x86
    unsigned* col = reinterpret_cast<unsigned*>(vtbl[-1]);
    check(col != nullptr && col[0] == 0, "vtable[-1] is an MSVC RTTICompleteObjectLocator (signature 0)");
    delete obj;

    // 3
    check(offsetof(AlignDouble, d) == 8 && sizeof(AlignDouble) == 24, "double aligned to 8 inside structs");
    check(sizeof(Bits) == 8, "bitfields of different types start a new unit (MSVC rule)");
    check(sizeof(Packed) == 7, "#pragma pack(1)");
    check(sizeof(EmptyBaseHolder) == 12, "derived layout appends after base");
    check(sizeof(long double) == 8, "long double is 8 bytes");
    check(sizeof(wchar_t) == 2, "wchar_t is 2 bytes");
    check(sizeof(long) == 4, "long is 4 bytes");

    // 4. name mangling (what the game's RTTI strings use)
#ifdef _MSC_VER
    check(std::strcmp(typeid(Derived).raw_name(), ".?AUDerived@@") == 0, "MSVC RTTI decorated name .?AUDerived@@");
#else
    check(false, "MSVC RTTI decorated name (type_info::raw_name not available)");
#endif

    std::printf("%d failure(s)\n", g_fail);
    return g_fail;
}
