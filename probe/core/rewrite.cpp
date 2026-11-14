// Rewrite manager: loads nfsu2_game.dll (from a temp copy), hooks every function it lists with MinHook in place of
// the original, and switches each between original and rewrite at runtime. Hot-reloads the game DLL when its file
// changes. All hook changes happen on the render thread (on_frame / run_on_main), per CLAUDE.md.
#include "probe.h"

#include <cstring>
#include <set>
#include <stdexcept>

#include "MinHook.h"
#include "game_api.h"

namespace probe {
namespace {

struct Entry {
    uintptr_t addr;
    std::string name, signature;
    void* replacement;
    void** original;
    std::string system;    // game/systems/<system>/ folder of the source file
    bool hooked = false;   // MH_CreateHook succeeded
    bool enabled = false;  // rewrite active
    std::string disk_bytes, mem_bytes;  // first 8 bytes in the exe file vs. in memory before we hooked
};

std::wstring g_path;  // build output we watch (env NFSU2_GAME)
HMODULE g_mod = nullptr;
std::wstring g_temp;
FILETIME g_mtime{}, g_pending{};
std::string g_build;
const GameApi* g_game = nullptr;
uint32_t g_seq = 0, g_reloads = 0;
std::vector<Entry> g_entries;

// Enabled set persisted across game restarts: <game dll dir>/rewrites_enabled.json (out/, gitignored).
std::wstring state_file() { return g_path.substr(0, g_path.find_last_of(L"\\/") + 1) + L"rewrites_enabled.json"; }

void save_enabled_file() {
    json names = json::array();
    for (const Entry& e : g_entries)
        if (e.enabled) names.push_back(e.name);
    if (FILE* f = _wfopen(state_file().c_str(), L"w")) {
        fputs(names.dump(1).c_str(), f);
        fclose(f);
    }
}

std::set<std::string> load_enabled_file() {
    std::set<std::string> out;
    if (FILE* f = _wfopen(state_file().c_str(), L"r")) {
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);
        try {
            for (const json& v : json::parse(text)) out.insert(v.get<std::string>());
        } catch (...) {
        }
    }
    return out;
}

bool file_mtime(const std::wstring& p, FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || a.nFileSizeLow == 0) return false;
    out = a.ftLastWriteTime;
    return true;
}
bool same(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

// bytes of SPEED2.EXE on disk at a virtual address (to see whether a mod already patched the function)
std::string exe_disk_bytes(uintptr_t va, size_t n) {
    static std::vector<uint8_t> file;
    if (file.empty()) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return "?";
        LARGE_INTEGER sz{};
        GetFileSizeEx(f, &sz);
        file.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        ReadFile(f, file.data(), static_cast<DWORD>(file.size()), &got, nullptr);
        CloseHandle(f);
    }
    const uintptr_t base = exe().base;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        const uintptr_t rva = va - base;
        if (rva >= sec->VirtualAddress && rva + n <= sec->VirtualAddress + sec->SizeOfRawData) {
            const size_t off = rva - sec->VirtualAddress + sec->PointerToRawData;
            if (off + n <= file.size()) return to_hex(file.data() + off, n);
        }
    }
    return "?";
}

void unload_locked() {
    for (Entry& e : g_entries) {
        if (e.hooked) {
            MH_DisableHook(reinterpret_cast<void*>(e.addr));
            MH_RemoveHook(reinterpret_cast<void*>(e.addr));
        }
    }
    g_entries.clear();
    g_game = nullptr;
    if (g_mod) {
        FreeLibrary(g_mod);
        g_mod = nullptr;
        DeleteFileW(g_temp.c_str());
    }
}

// Load the DLL (temp copy) and hook everything, enabling the names in `enable`. Render thread.
bool load(const std::set<std::string>& enable) {
    FILETIME mt;
    if (g_path.empty() || !file_mtime(g_path, mt)) return false;
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring temp = std::wstring(dir) + L"nfsu2_game_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                        std::to_wstring(++g_seq) + L".dll";
    if (!CopyFileW(g_path.c_str(), temp.c_str(), FALSE)) return false;
    HMODULE mod = LoadLibraryW(temp.c_str());
    auto get = mod ? reinterpret_cast<nfsu2_game_api_fn>(GetProcAddress(mod, "nfsu2_game_api")) : nullptr;
    const GameApi* api = get ? get() : nullptr;
    if (!api || api->version != kGameApiVersion) {
        logf("rewrite: cannot load game DLL (LoadLibrary %s, api %u, want %u) - retrying after the next rebuild",
             mod ? "ok" : "failed", api ? api->version : 0, kGameApiVersion);
        if (mod) FreeLibrary(mod);
        DeleteFileW(temp.c_str());
        g_mtime = g_pending = mt;  // don't retry this same file every 30 frames
        return false;
    }
    unload_locked();  // the new one loaded fine: drop the old one
    g_mod = mod;
    g_temp = temp;
    g_mtime = g_pending = mt;
    g_build = api->build;
    g_game = api;
    for (uint32_t i = 0; i < api->count; ++i) {
        const GameRewrite& r = api->rewrites[i];
        Entry e{r.addr, r.name, r.signature, r.replacement, r.original};
        {
            std::string f = r.file ? r.file : "";
            for (char& c : f)
                if (c == '\\') c = '/';
            const size_t at = f.find("systems/");
            e.system = at == std::string::npos ? "other" : f.substr(at + 8, f.find('/', at + 8) - (at + 8));
        }
        uint8_t mem[8]{};
        mem_read(r.addr, mem, sizeof mem);
        e.mem_bytes = to_hex(mem, sizeof mem);
        e.disk_bytes = exe_disk_bytes(r.addr, sizeof mem);
        MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(r.addr), r.replacement, r.original);
        e.hooked = s == MH_OK;
        if (!e.hooked) logf("rewrite: hook %s at 0x%08x failed: %s", r.name, unsigned(r.addr), MH_StatusToString(s));
        if (e.mem_bytes != e.disk_bytes)
            logf("rewrite: WARNING %s at 0x%08x already patched in memory (disk %s, mem %s) - another mod?", r.name,
                 unsigned(r.addr), e.disk_bytes.c_str(), e.mem_bytes.c_str());
        if (e.hooked && enable.count(e.name)) e.enabled = MH_EnableHook(reinterpret_cast<void*>(r.addr)) == MH_OK;
        g_entries.push_back(std::move(e));
    }
    logf("rewrite: game DLL build %s, %u rewrites, %zu enabled", g_build.c_str(), api->count, enable.size());
    return true;
}

std::set<std::string> enabled_names() {
    std::set<std::string> s;
    for (const Entry& e : g_entries)
        if (e.enabled) s.insert(e.name);
    return s;
}

Entry* find(const json& p) {
    for (Entry& e : g_entries) {
        if (p.contains("name") && p["name"].get<std::string>() == e.name) return &e;
        if (p.contains("addr")) {
            const json& a = p["addr"];
            uintptr_t v = a.is_string() ? std::stoul(a.get<std::string>(), nullptr, 0) : a.get<uintptr_t>();
            if (v == e.addr) return &e;
        }
    }
    throw std::runtime_error("no such rewrite");
}

json entry_json(const Entry& e) {
    return {{"name", e.name}, {"system", e.system}, {"addr", hex_addr(e.addr)}, {"signature", e.signature}, {"hooked", e.hooked},
            {"enabled", e.enabled}, {"disk_bytes", e.disk_bytes}, {"mem_bytes_before_hook", e.mem_bytes},
            {"patched_by_other_mod", e.mem_bytes != e.disk_bytes}};
}

}  // namespace

void rewrite_init(const json& state) {
    if (const wchar_t* env = _wgetenv(L"NFSU2_GAME"); env && *env) {
        g_path = env;
    } else if (const wchar_t* core = _wgetenv(L"NFSU2_CORE"); core && *core) {  // same folder as the core
        std::wstring c = core;
        g_path = c.substr(0, c.find_last_of(L"\\/") + 1) + L"nfsu2_game.dll";
    }
    if (g_path.empty()) {
        logf("rewrite: NFSU2_GAME not set, no game DLL");
        return;
    }
    std::set<std::string> enable;
    if (state.contains("rewrite")) {
        for (const json& n : state["rewrite"]["enabled"]) enable.insert(n.get<std::string>());
    } else {
        enable = load_enabled_file();  // fresh game start: bring back what was enabled last time
        if (!enable.empty()) logf("rewrite: re-enabling %zu rewrites from rewrites_enabled.json", enable.size());
    }
    load(enable);
}

void rewrite_on_frame(uint64_t frame) {
    if (g_path.empty() || frame % 30 != 0) return;
    FILETIME mt;
    if (!file_mtime(g_path, mt) || same(mt, g_mtime)) return;
    if (!same(mt, g_pending)) {  // wait until the file stopped changing
        g_pending = mt;
        return;
    }
    if (load(enabled_names())) {
        ++g_reloads;
        logf("rewrite: hot-reloaded game DLL (#%u)", g_reloads);
    }
}

json rewrite_save_state() {
    json names = json::array();
    for (const std::string& n : enabled_names()) names.push_back(n);
    return {{"enabled", names}};
}

void rewrite_shutdown() { unload_locked(); }

json rewrite_list() {
    return run_on_main(
        [] {
            json arr = json::array();
            for (const Entry& e : g_entries) arr.push_back(entry_json(e));
            return json{{"build", g_build}, {"reloads", g_reloads}, {"rewrites", arr}};
        },
        5000);
}

namespace {
void set_enabled(Entry* e, bool want) {  // render thread
    if (!e->hooked) throw std::runtime_error("not hooked");
    MH_STATUS s = want ? MH_EnableHook(reinterpret_cast<void*>(e->addr)) : MH_DisableHook(reinterpret_cast<void*>(e->addr));
    if (s != MH_OK && s != MH_ERROR_ENABLED && s != MH_ERROR_DISABLED) throw std::runtime_error(MH_StatusToString(s));
    e->enabled = want;
    save_enabled_file();
    logf("rewrite: %s %s", e->name.c_str(), want ? "ENABLED (rewrite)" : "disabled (original)");
}
}  // namespace

// ---- direct access for the overlay (render thread only) ----
std::vector<RewriteInfo> rewrite_snapshot() {
    std::vector<RewriteInfo> v;
    for (const Entry& e : g_entries) v.push_back({e.name, e.system, e.signature, e.addr, e.enabled, e.hooked});
    return v;
}
bool rewrite_set(const std::string& name, bool on) {
    for (Entry& e : g_entries)
        if (e.name == name) {
            try {
                set_enabled(&e, on);
                return true;
            } catch (const std::exception& ex) {
                logf("rewrite: %s: %s", name.c_str(), ex.what());
                return false;
            }
        }
    return false;
}
std::string rewrite_diff_now(const std::string& name, uint32_t iterations, uint32_t seed) {
    if (!g_game) return "{\"error\":\"no game DLL\"}";
    return g_game->diff(name.c_str(), iterations, seed);
}
std::string rewrite_build() { return g_build; }

json rewrite_enable(const json& p) {
    const bool want = p.value("enabled", true);
    return run_on_main(
        [p, want] {
            Entry* e = find(p);
            set_enabled(e, want);
            return entry_json(*e);
        },
        5000);
}

json rewrite_reload() {
    return run_on_main(
        [] {
            if (!load(enabled_names())) throw std::runtime_error("game DLL reload failed (see log)");
            ++g_reloads;
            return json{{"build", g_build}, {"reloads", g_reloads}};
        },
        10000);
}

json diff_test(const json& p) {
    const std::string name = p.value("name", std::string("*"));
    const uint32_t iterations = p.value("iterations", 10000u);
    const uint32_t seed = p.value("seed", 12345u);
    return run_on_main(
        [name, iterations, seed] {
            if (!g_game) throw std::runtime_error("no game DLL loaded");
            if (name == "list") return json::parse(g_game->diff_list());
            return json::parse(g_game->diff(name.c_str(), iterations, seed));
        },
        30000);
}

}  // namespace probe
