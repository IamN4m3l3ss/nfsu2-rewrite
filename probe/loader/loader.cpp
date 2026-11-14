// nfsu2_probe.asi — the stable loader. Loaded by the Ultimate ASI Loader (dinput8.dll) before the game's WinMain.
//
// Owns what must survive a core reload: exe hash check, the log, the D3D9 hooks (Direct3DCreate9 -> CreateDevice
// slot 16 -> Present slot 17; Present is the frame safe point), the keep_active WndProc hook, and the core itself:
// nfsu2_core.dll is loaded from a temp copy (Windows locks loaded DLLs) and reloaded at Present when its file
// changes or the core asks for it. A crash inside the core's per-frame code is caught and logged; the core is then
// left disabled until the next reload.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bcrypt.h>
#include <d3d9.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "MinHook.h"
#include "api.h"

namespace {

using json = nlohmann::json;
constexpr const char* kLoaderVersion = "0.2.0";
constexpr const char* kExpectedSha256 = "f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7";

// ---------------------------------------------------------------- log (file + ring, survives reloads)
std::mutex g_log_mu;
FILE* g_log_file = nullptr;
struct LogLine { uint64_t seq; std::string text; };
std::deque<LogLine> g_log_ring;
uint64_t g_log_seq = 0;

void log_line(const char* msg) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [%04lx] %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
             GetCurrentThreadId(), msg);
    std::lock_guard lk(g_log_mu);
    if (g_log_file) {
        fputs(line, g_log_file);
        fputc('\n', g_log_file);
        fflush(g_log_file);
    }
    g_log_ring.push_back({++g_log_seq, line});
    if (g_log_ring.size() > 4000) g_log_ring.pop_front();
}

void logf(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_line(msg);
}

const char* log_tail_json(uint32_t n, uint64_t since) {
    thread_local std::string out;
    json lines = json::array();
    uint64_t last;
    {
        std::lock_guard lk(g_log_mu);
        size_t start = g_log_ring.size() > n ? g_log_ring.size() - n : 0;
        for (size_t i = start; i < g_log_ring.size(); ++i)
            if (g_log_ring[i].seq > since) lines.push_back({{"seq", g_log_ring[i].seq}, {"text", g_log_ring[i].text}});
        last = g_log_seq;
    }
    out = json{{"last_seq", last}, {"lines", lines}}.dump(-1, ' ', false, json::error_handler_t::replace);
    return out.c_str();
}

// ---------------------------------------------------------------- helpers
std::string to_hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = d[p[i] >> 4];
        s[i * 2 + 1] = d[p[i] & 15];
    }
    return s;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128) ? static_cast<char>(c) : '?';
    return s;
}

std::string sha256_file(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    BOOL ok = ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
    CloseHandle(f);
    if (!ok || got != data.size()) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    uint8_t digest[32];
    NTSTATUS st = BCryptHash(alg, nullptr, 0, data.data(), static_cast<ULONG>(data.size()), digest, sizeof digest);
    BCryptCloseAlgorithmProvider(alg, 0);
    return st == 0 ? to_hex(digest, sizeof digest) : std::string{};
}

bool write_ptr(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof value, PAGE_EXECUTE_READWRITE, &old)) return false;
    *slot = value;
    DWORD tmp;
    VirtualProtect(slot, sizeof value, old, &tmp);
    return true;
}

bool patch_slot(void* obj, int slot, void* detour, void** original) {
    void** vtbl = *reinterpret_cast<void***>(obj);
    if (vtbl[slot] == detour) return true;  // shared vtable already patched
    *original = vtbl[slot];
    return write_ptr(&vtbl[slot], detour);
}

// ---------------------------------------------------------------- exe identity
std::string g_sha256;
bool g_hash_ok = false;
uintptr_t g_exe_base = 0;
uint32_t g_exe_size = 0;
uint32_t g_init_thread = 0;

// ---------------------------------------------------------------- core management
std::wstring g_core_path;  // the build output we watch
HMODULE g_core_mod = nullptr;
const probe::CoreApi* g_core = nullptr;
std::wstring g_core_temp;
FILETIME g_core_mtime{};
FILETIME g_pending_mtime{};
std::atomic<bool> g_reload_requested{false};
std::atomic<uint32_t> g_reload_count{0};
bool g_core_faulted = false;
uint32_t g_temp_seq = 0;
probe::LoaderApi g_api{};

bool file_mtime(const std::wstring& path, FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || a.nFileSizeLow == 0) return false;
    out = a.ftLastWriteTime;
    return true;
}

bool same_time(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

// Load a fresh temp copy of the core. Does not init it.
bool load_core_module(HMODULE& mod, const probe::CoreApi*& api, std::wstring& temp) {
    FILETIME mt;
    if (!file_mtime(g_core_path, mt)) {
        logf("core: %s not found", narrow(g_core_path).c_str());
        return false;
    }
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    temp = std::wstring(dir) + L"nfsu2_core_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
           std::to_wstring(++g_temp_seq) + L".dll";
    if (!CopyFileW(g_core_path.c_str(), temp.c_str(), FALSE)) {
        logf("core: copy to temp failed (%lu)", GetLastError());
        return false;
    }
    mod = LoadLibraryW(temp.c_str());
    if (!mod) {
        logf("core: LoadLibrary failed (%lu)", GetLastError());
        DeleteFileW(temp.c_str());
        return false;
    }
    auto get = reinterpret_cast<nfsu2_core_api_fn>(GetProcAddress(mod, "nfsu2_core_api"));
    api = get ? get() : nullptr;
    if (!api || api->version != probe::kApiVersion) {
        logf("core: missing export or API version mismatch (%u, want %u)", api ? api->version : 0,
             probe::kApiVersion);
        FreeLibrary(mod);
        DeleteFileW(temp.c_str());
        mod = nullptr;
        api = nullptr;
        return false;
    }
    g_core_mtime = mt;
    g_pending_mtime = mt;
    return true;
}

void first_load() {
    HMODULE mod;
    const probe::CoreApi* api;
    std::wstring temp;
    if (!load_core_module(mod, api, temp)) return;
    g_core_mod = mod;
    g_core = api;
    g_core_temp = temp;
    logf("core: loaded build %s", api->build);
    if (!g_core->init(&g_api, "")) logf("core: init failed");
}

// Render thread only.
void reload_core() {
    HMODULE mod;
    const probe::CoreApi* api;
    std::wstring temp;
    if (!load_core_module(mod, api, temp)) return;  // keep the running core if the new one can't load
    std::string state;
    if (g_core) {
        if (const char* s = g_core->shutdown()) state = s;  // copy before the old DLL goes away
        FreeLibrary(g_core_mod);
        DeleteFileW(g_core_temp.c_str());
    }
    g_core_mod = mod;
    g_core = api;
    g_core_temp = temp;
    g_core_faulted = false;
    ++g_reload_count;
    logf("core: reloaded (#%u) build %s, state %zu bytes", g_reload_count.load(), api->build, state.size());
    if (!g_core->init(&g_api, state.c_str())) logf("core: init failed after reload");
}

void maybe_reload(uint64_t frame) {
    bool want = g_reload_requested.exchange(false);
    if (!want && frame % 30 == 0) {  // poll the build output twice a second; reload once it stopped changing
        FILETIME mt;
        if (file_mtime(g_core_path, mt) && !same_time(mt, g_core_mtime)) {
            if (same_time(mt, g_pending_mtime))
                want = true;
            else
                g_pending_mtime = mt;
        }
    }
    if (want) reload_core();
}

// SEH guard: no C++ objects with destructors in here (clang-cl/MSVC rule for __try).
int call_on_frame_seh(const probe::CoreApi* core, uint64_t frame, DWORD* code, void** addr) {
    __try {
        core->on_frame(frame);
        return 0;
    } __except (*code = GetExceptionCode(),
                *addr = (GetExceptionInformation())->ExceptionRecord->ExceptionAddress, EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// ---------------------------------------------------------------- D3D9 hooks
constexpr int kSlotCreateDevice = 16;  // IDirect3D9 (SDK 10.0.26100 d3d9.h)
constexpr int kSlotPresent = 17;       // IDirect3DDevice9

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
Direct3DCreate9_t o_Direct3DCreate9 = nullptr;
CreateDevice_t o_CreateDevice = nullptr;
Present_t o_Present = nullptr;

std::atomic<IDirect3DDevice9*> g_device{nullptr};
std::atomic<HWND> g_window{nullptr};
std::atomic<uint64_t> g_frames{0};
std::atomic<DWORD> g_create_thread{0};
std::atomic<DWORD> g_present_thread{0};

HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd,
                                     const RGNDATA* dirty) {
    uint64_t f = ++g_frames;
    if (f == 1) {
        g_present_thread = GetCurrentThreadId();
        logf("first Present: device=%p thread=%lx (CreateDevice thread=%lx)", dev, GetCurrentThreadId(),
             g_create_thread.load());
    }
    if (dev == g_device.load()) {
        maybe_reload(f);
        if (g_core && !g_core_faulted) {
            DWORD code = 0;
            void* addr = nullptr;
            if (call_on_frame_seh(g_core, f, &code, &addr)) {
                g_core_faulted = true;
                logf("ERROR: exception %08lx at %p in core on_frame; core disabled until the next reload", code, addr);
            }
        }
    }
    return o_Present(dev, src, dst, wnd, dirty);
}

HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                          D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    HRESULT hr = o_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    logf("CreateDevice hr=%08lx focus=%p %ux%u fmt=%d windowed=%d msaa=%d swap=%d interval=%x", hr, focus,
         pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->BackBufferFormat : 0,
         pp ? pp->Windowed : -1, pp ? pp->MultiSampleType : -1, pp ? pp->SwapEffect : -1,
         pp ? pp->PresentationInterval : 0);
    if (SUCCEEDED(hr) && out && *out) {
        g_device = *out;
        g_window = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : focus;
        g_create_thread = GetCurrentThreadId();
        if (!patch_slot(*out, kSlotPresent, reinterpret_cast<void*>(&hk_Present), reinterpret_cast<void**>(&o_Present)))
            logf("ERROR: patching Present failed");
        else
            logf("Present hooked (orig=%p)", o_Present);
    }
    return hr;
}

IDirect3D9* WINAPI hk_Direct3DCreate9(UINT sdk) {
    IDirect3D9* d3d = o_Direct3DCreate9(sdk);
    logf("Direct3DCreate9(%u) -> %p", sdk, d3d);
    if (d3d && !patch_slot(d3d, kSlotCreateDevice, reinterpret_cast<void*>(&hk_CreateDevice),
                           reinterpret_cast<void**>(&o_CreateDevice)))
        logf("ERROR: patching CreateDevice failed");
    return d3d;
}

// ---------------------------------------------------------------- keep_active (see docs/DEVLOG.md, M1)
// MainWndProc 0x005CCD60: WM_ACTIVATEAPP/WM_SIZE drive g_AppActive (0x00870988); while it is 0 the message pump
// blocks in GetMessageA and no frames are drawn. Rewrite focus-loss messages and pin the flag.
constexpr uintptr_t kWndProc = 0x005CCD60;
constexpr uintptr_t kAppActive = 0x00870988;
using WndProc_t = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
WndProc_t o_WndProc = nullptr;
std::atomic<bool> g_keep_active{true};

LRESULT CALLBACK hk_WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool keep = g_keep_active.load(std::memory_order_relaxed);
    if (keep) {
        if (msg == WM_ACTIVATEAPP && !wp) wp = TRUE;
        if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) wp = MAKEWPARAM(WA_ACTIVE, HIWORD(wp));
    }
    LRESULT r = o_WndProc(wnd, msg, wp, lp);
    if (keep) *reinterpret_cast<volatile int*>(kAppActive) = 1;
    return r;
}

// ---------------------------------------------------------------- init
std::wstring module_dir(HMODULE m) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(m, buf, MAX_PATH);
    std::wstring s(buf, n);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

void init(HMODULE self) {
    {
        std::lock_guard lk(g_log_mu);
        g_log_file = _wfopen((module_dir(self) + L"nfsu2_probe.log").c_str(), L"w");
    }
    logf("nfsu2_probe loader %s", kLoaderVersion);

    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    g_exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe_base);
    g_exe_size = reinterpret_cast<IMAGE_NT_HEADERS32*>(g_exe_base + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    g_init_thread = GetCurrentThreadId();
    g_sha256 = sha256_file(exe_path);
    g_hash_ok = g_sha256 == kExpectedSha256;
    logf("exe base=0x%08x size=%08x sha256=%s %s", static_cast<unsigned>(g_exe_base), g_exe_size, g_sha256.c_str(),
         g_hash_ok ? "OK" : "MISMATCH");

    g_api = probe::LoaderApi{
        probe::kApiVersion, kLoaderVersion, g_exe_base, g_exe_size, g_sha256.c_str(), g_hash_ok, g_init_thread,
        [](const char* m) { log_line(m); }, &log_tail_json,
        [] { return g_frames.load(); }, [] { return g_device.load(); },
        [] { return static_cast<void*>(g_window.load()); },
        [] { return static_cast<uint32_t>(g_create_thread.load()); },
        [] { return static_cast<uint32_t>(g_present_thread.load()); },
        [] { return g_keep_active.load(); },
        [](bool e) {
            g_keep_active = e;
            if (e) *reinterpret_cast<volatile int*>(kAppActive) = 1;
        },
        [] { return *reinterpret_cast<volatile int*>(kAppActive); },
        [] { g_reload_requested = true; }, [] { return g_reload_count.load(); }};

    if (const char* env = std::getenv("NFSU2_KEEP_ACTIVE")) g_keep_active = std::atoi(env) != 0;
    if (const wchar_t* env = _wgetenv(L"NFSU2_CORE"); env && *env)
        g_core_path = env;
    else
        g_core_path = module_dir(self) + L"nfsu2_core.dll";
    logf("core path: %s", narrow(g_core_path).c_str());

    if (g_hash_ok) {
        MH_STATUS s = MH_Initialize();
        if (s == MH_OK)
            s = MH_CreateHookApi(L"d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&hk_Direct3DCreate9),
                                 reinterpret_cast<void**>(&o_Direct3DCreate9));
        if (s == MH_OK)
            s = MH_CreateHook(reinterpret_cast<void*>(kWndProc), reinterpret_cast<void*>(&hk_WndProc),
                              reinterpret_cast<void**>(&o_WndProc));
        if (s == MH_OK) s = MH_EnableHook(MH_ALL_HOOKS);
        logf("loader hooks (Direct3DCreate9, WndProc keep_active=%d): %s", g_keep_active.load(), MH_StatusToString(s));
    } else {
        logf("ERROR: unsupported SPEED2.EXE build: no hooks; the core still loads for read-only diagnostics");
    }
    // The core must be in place before the game creates its DirectInput devices (it hooks DirectInput8Create).
    first_load();
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        init(inst);
    }
    return TRUE;
}
