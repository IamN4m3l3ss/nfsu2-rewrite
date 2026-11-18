// Fingerprint of the menu engine (FEng) state, for A/B tests of menu rewrites: the same inputs on the same frames must
// produce the same fingerprint every frame with the original code and with the rewrites.
//
// Walks cFEng (*0x8384C4) -> FEngine (+0x08) -> active packages (+0xE4, FEMinNode-linked, count +0xE0) -> each
// package's top-level objects (FEMinList at +0x44) and, for groups (type 5), their children (FEMinList at +0x5C).
// Hashed: package name hash/priority/control masks/current button; object name hash, type, flags, its data block
// (+0x2C, size +0x30: position, colour, size, ... as the menu scripts animate them) and, for strings (type 2), the
// displayed text (FEWideString at +0x64). Layouts from nfsu2-gcpc's notes (CC0). Main thread only.
#include "probe.h"

#include "MinHook.h"

// ---- Fixed frame time for repeatable A/B runs. The game's per-frame real-time step RealTimeElapsed (float 0x865198) is
// stored once per frame by 0x57EAD0 (called only from MainLoop 0x5812ED, not patched by the mods): ticks * const.
// FEng advances menu scripts by it (cFEng::Service: * 1200), so with jittery Wine frame times two identical input
// walks drift apart (measured: pause menu highlight pulse, frame 778). While pinned, our wrapper calls the original and
// then overwrites the result with a fixed step. The wrapper is register-neutral (only flags change).
extern "C" {
void* g_fe_timer_orig = nullptr;
volatile uint32_t g_fe_fixed_on = 0;
volatile uint32_t g_fe_fixed_bits = 0;
__declspec(naked) void fe_timer_hook() {
    __asm {
        call dword ptr [g_fe_timer_orig]
        cmp dword ptr [g_fe_fixed_on], 0
        je done
        push eax
        mov eax, dword ptr [g_fe_fixed_bits]
        mov dword ptr ds:[0x00865198], eax
        pop eax
    done:
        ret
    }
}
}

namespace probe {

// Clock sync for in-game A/B drives (main thread, at the drive's first frame): the boot to free roam takes a
// different number of frames each time (its waits are wall-clock), so the game clocks start the drive at different
// values. Things like the camera shake (Camera::ApplyNoise at world time 0x86518C * 1/4000) then run at a different
// phase. This sets every clock to the same values: real ticks 0x8651AC (+ fraction 0x865144), the world timer
// 0x89CF48 (its last real tick +0x54, accumulated world ticks 0x869234), world time 0x86518C / seconds 0x7FB718,
// frame counters 0x7FB704 / 0x7FB708 / 0x86514C. Only for test drives: gameplay timers jump.
void time_sync_apply() {
    constexpr uint32_t kReal = 0x00100000, kWorld = 400000, kFrames = 6000;  // kFrames % 16 == 0 (ring index)
    *reinterpret_cast<volatile uint32_t*>(0x008651ACu) = kReal;
    *reinterpret_cast<volatile float*>(0x00865144u) = 0.0f;
    volatile uint32_t& last = *reinterpret_cast<volatile uint32_t*>(0x0089CF48u + 0x54);
    if (last != 0 && last != 0x7FFFFFFFu) last = kReal;
    *reinterpret_cast<volatile uint32_t*>(0x00869234u) = kWorld;
    *reinterpret_cast<volatile uint32_t*>(0x0086518Cu) = kWorld;
    *reinterpret_cast<volatile float*>(0x007FB718u) = 100.0f;  // kWorld / 4000
    *reinterpret_cast<volatile uint32_t*>(0x007FB704u) = kFrames;
    *reinterpret_cast<volatile uint32_t*>(0x007FB708u) = kFrames;
    *reinterpret_cast<volatile uint32_t*>(0x0086514Cu) = kFrames;
    logf("time_sync: game clocks set (real %u, world %u, frames %u)", kReal, kWorld, kFrames);
}

bool fe_fixed_dt(bool on, float dt) {  // main thread
    static bool hooked = false;
    if (on && !hooked) {
        MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(0x0057EAD0), reinterpret_cast<void*>(&fe_timer_hook),
                                    &g_fe_timer_orig);
        if (s == MH_OK) s = MH_EnableHook(reinterpret_cast<void*>(0x0057EAD0));
        if (s != MH_OK) {
            logf("fixed_dt: hook 0x57EAD0 failed: %s", MH_StatusToString(s));
            return false;
        }
        hooked = true;
    }
    uint32_t bits;
    memcpy(&bits, &dt, 4);
    g_fe_fixed_bits = bits;
    g_fe_fixed_on = on ? 1 : 0;
    logf("fixed_dt: %s (%.6f s)", on ? "on" : "off", dt);
    return true;
}

json fixed_dt(const json& p) {
    const bool on = p.value("enabled", true);
    const float dt = p.value("dt", 1.0f / 60.0f);
    return run_on_main([on, dt] { return json{{"ok", fe_fixed_dt(on, dt)}, {"enabled", on}, {"dt", dt}}; }, 2000);
}

namespace {

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    }
    void u32(uint32_t v) { bytes(&v, 4); }
};

template <class T> T rd(const uint8_t* p, uint32_t off) { return *reinterpret_cast<const T*>(p + off); }

void hash_object(Fnv& f, const uint8_t* o, int depth, uint32_t& count) {
    if (!o || depth > 16 || count > 20000) return;
    ++count;
    f.u32(rd<uint32_t>(o, 0x10));  // name hash
    const uint32_t type = rd<uint32_t>(o, 0x18);
    f.u32(type);
    f.u32(rd<uint32_t>(o, 0x1C));  // flags
    const uint8_t* data = rd<const uint8_t*>(o, 0x2C);
    const uint32_t size = rd<uint32_t>(o, 0x30);
    if (data && size && size <= 4096) f.bytes(data, size);
    if (type == 2) {  // FEString: displayed text
        const uint16_t* s = rd<const uint16_t*>(o, 0x64);
        const uint32_t cap = rd<uint32_t>(o, 0x68);
        for (uint32_t i = 0; s && i < cap && i < 1024 && s[i]; ++i) f.bytes(&s[i], 2);
    }
    if (type == 5) {  // FEGroup: children (FEMinList at +0x5C: count +0x60, head +0x64)
        const uint32_t n = rd<uint32_t>(o, 0x60);
        const uint8_t* c = rd<const uint8_t*>(o, 0x64);
        for (uint32_t i = 0; c && i < n && i < 4096; ++i, c = rd<const uint8_t*>(c, 4)) hash_object(f, c, depth + 1, count);
    }
}

bool walk(uint64_t* out_hash, uint32_t* out_objects, uint32_t* out_packages, uint32_t* pk_names, uint64_t* pk_hashes,
          uint32_t pk_max) {
    Fnv f;
    uint32_t count = 0, npk = 0;
    const uint8_t* cfeng = *reinterpret_cast<const uint8_t* const*>(0x008384C4);
    if (!cfeng) return false;
    const uint8_t* eng = rd<const uint8_t*>(cfeng, 0x08);
    if (!eng) return false;
    const uint32_t n = rd<uint32_t>(eng, 0xE0);
    const uint8_t* pkg = rd<const uint8_t*>(eng, 0xE4);
    for (uint32_t k = 0; pkg && k < n && k < 64; ++k, pkg = rd<const uint8_t*>(pkg, 4)) {
        ++npk;
        Fnv pf;  // this package alone (A/B tests compare menus, not the live HUD)
        Fnv* const all = &f;
        Fnv& f = pf;
        f.u32(rd<uint32_t>(pkg, 0x10));  // name hash
        f.u32(rd<uint32_t>(pkg, 0x18));  // priority
        f.u32(rd<uint32_t>(pkg, 0x1C));  // control mask
        f.u32(rd<uint32_t>(pkg, 0x24));  // input control mask
        const uint8_t* btn = rd<const uint8_t*>(pkg, 0x8C);
        f.u32(btn ? rd<uint32_t>(btn, 0x10) : 0);
        const uint32_t nobj = rd<uint32_t>(pkg, 0x48);  // FEMinList at +0x44: count +4, head +8
        const uint8_t* o = rd<const uint8_t*>(pkg, 0x4C);
        for (uint32_t i = 0; o && i < nobj && i < 4096; ++i, o = rd<const uint8_t*>(o, 4)) hash_object(f, o, 0, count);
        all->bytes(&pf.h, 8);
        if (k < pk_max) {
            pk_names[k] = rd<uint32_t>(pkg, 0x10);
            pk_hashes[k] = pf.h;
        }
    }
    *out_hash = f.h;
    *out_objects = count;
    *out_packages = npk;
    return true;
}

bool walk_guarded(uint64_t* h, uint32_t* objs, uint32_t* pkgs, uint32_t* pn, uint64_t* ph, uint32_t pmax) {
    __try {
        return walk(h, objs, pkgs, pn, ph, pmax);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- per-object fingerprints (checkpoint A/B: objects that vary between two runs of the original get masked)
struct ObjEntry {
    uint32_t pkg;       // package name hash
    int32_t parent;     // entry index of the parent group, -1 = package top level
    uint32_t index;     // position in the parent's list
    uint32_t name;      // object name hash
    uint32_t type;
    uint64_t hash;      // this object's own state (not its children)
};
constexpr uint32_t kMaxEntries = 8192;
ObjEntry g_entries[kMaxEntries];
uint32_t g_nentries = 0;

void list_objects(uint32_t pkg, const uint8_t* o, uint32_t n, int32_t parent, int depth) {
    for (uint32_t i = 0; o && i < n && i < 4096 && g_nentries < kMaxEntries; ++i, o = rd<const uint8_t*>(o, 4)) {
        Fnv f;
        uint32_t dummy = 0;
        const uint32_t type = rd<uint32_t>(o, 0x18);
        // own fields only: hash_object without recursing (groups get their children as separate entries)
        f.u32(rd<uint32_t>(o, 0x10));
        f.u32(type);
        f.u32(rd<uint32_t>(o, 0x1C));
        const uint8_t* data = rd<const uint8_t*>(o, 0x2C);
        const uint32_t size = rd<uint32_t>(o, 0x30);
        if (data && size && size <= 4096) f.bytes(data, size);
        if (type == 2) {
            const uint16_t* s = rd<const uint16_t*>(o, 0x64);
            const uint32_t cap = rd<uint32_t>(o, 0x68);
            for (uint32_t c = 0; s && c < cap && c < 1024 && s[c]; ++c) f.bytes(&s[c], 2);
        }
        (void)dummy;
        const int32_t me = static_cast<int32_t>(g_nentries);
        g_entries[g_nentries++] = {pkg, parent, i, rd<uint32_t>(o, 0x10), type, f.h};
        if (type == 5 && depth < 16) list_objects(pkg, rd<const uint8_t*>(o, 0x64), rd<uint32_t>(o, 0x60), me, depth + 1);
    }
}

bool list_all() {
    g_nentries = 0;
    const uint8_t* cfeng = *reinterpret_cast<const uint8_t* const*>(0x008384C4);
    if (!cfeng) return false;
    const uint8_t* eng = rd<const uint8_t*>(cfeng, 0x08);
    if (!eng) return false;
    const uint32_t n = rd<uint32_t>(eng, 0xE0);
    const uint8_t* pkg = rd<const uint8_t*>(eng, 0xE4);
    for (uint32_t k = 0; pkg && k < n && k < 64 && g_nentries < kMaxEntries; ++k, pkg = rd<const uint8_t*>(pkg, 4)) {
        const uint32_t ph = rd<uint32_t>(pkg, 0x10);
        Fnv f;  // the package's own state as a pseudo-object (type 0xFFFFFFFF)
        f.u32(rd<uint32_t>(pkg, 0x18));
        f.u32(rd<uint32_t>(pkg, 0x1C));
        f.u32(rd<uint32_t>(pkg, 0x24));
        const uint8_t* btn = rd<const uint8_t*>(pkg, 0x8C);
        f.u32(btn ? rd<uint32_t>(btn, 0x10) : 0);
        g_entries[g_nentries++] = {ph, -1, 0xFFFFFFFFu, ph, 0xFFFFFFFFu, f.h};
        list_objects(ph, rd<const uint8_t*>(pkg, 0x4C), rd<uint32_t>(pkg, 0x48), -1, 0);
    }
    return true;
}

bool list_all_guarded() {
    __try {
        return list_all();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool fe_state_hash(uint64_t& hash, uint32_t& objects, uint32_t& packages, uint32_t* pk_names, uint64_t* pk_hashes,
                   uint32_t pk_max) {
    hash = 0;
    objects = packages = 0;
    uint32_t dn[1];
    uint64_t dh[1];
    return walk_guarded(&hash, &objects, &packages, pk_names ? pk_names : dn, pk_hashes ? pk_hashes : dh, pk_names ? pk_max : 0);
}

json fe_state(const json& p) {
    if (p.value("objects", false))
        return run_on_main(
            [] {
                const bool ok = list_all_guarded();
                json arr = json::array();
                for (uint32_t i = 0; i < g_nentries; ++i) {
                    // path: package/index/index/... (stable across runs as long as the object lists match)
                    std::string path;
                    for (int32_t e = static_cast<int32_t>(i); e >= 0; e = g_entries[e].parent)
                        path = "/" + std::to_string(static_cast<int32_t>(g_entries[e].index)) + path;
                    char pk[12], nm[12], h[24];
                    snprintf(pk, sizeof pk, "%08x", g_entries[i].pkg);
                    snprintf(nm, sizeof nm, "%08x", g_entries[i].name);
                    snprintf(h, sizeof h, "%016llx", static_cast<unsigned long long>(g_entries[i].hash));
                    arr.push_back(json::array({std::string(pk) + path, nm, g_entries[i].type, h}));
                }
                return json{{"ok", ok}, {"objects", arr}};
            },
            4000);
    return run_on_main(
        [] {
            uint64_t h;
            uint32_t o, p;
            const bool ok = fe_state_hash(h, o, p);
            char buf[24];
            snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
            return json{{"ok", ok}, {"hash", buf}, {"objects", o}, {"packages", p}};
        },
        2000);
}

}  // namespace probe
