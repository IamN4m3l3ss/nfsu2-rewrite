// nfsu2_probe.asi entry point. Loaded by the Ultimate ASI Loader (dinput8.dll) before the game's WinMain.
#include "probe.h"

#include <bcrypt.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "MinHook.h"

namespace probe {
namespace {

constexpr const char* kExpectedSha256 = "f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7";
ExeInfo g_exe;

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
    if (st != 0) return {};
    return to_hex(digest, sizeof digest);
}

std::wstring module_dir(HMODULE m) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(m, buf, MAX_PATH);
    std::wstring s(buf, n);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

void init(HMODULE self) {
    log_init(module_dir(self) + L"nfsu2_probe.log");
    logf("nfsu2_probe %s loading", kVersion);

    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    g_exe.base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(g_exe.base + dos->e_lfanew);
    g_exe.image_size = nt->OptionalHeader.SizeOfImage;
    g_exe.init_thread = GetCurrentThreadId();
    g_exe.sha256 = sha256_file(exe_path);
    g_exe.hash_ok = g_exe.sha256 == kExpectedSha256;
    logf("exe base=%s size=%08x sha256=%s %s", hex_addr(g_exe.base).c_str(), g_exe.image_size, g_exe.sha256.c_str(),
         g_exe.hash_ok ? "OK" : "MISMATCH");

    uint16_t port = kDefaultPort;
    if (const char* env = std::getenv("NFSU2_PROBE_PORT")) port = static_cast<uint16_t>(std::atoi(env));

    if (!g_exe.hash_ok) {
        // wrong build: our addresses would be wrong. Serve read-only diagnostics, install no hooks.
        logf("ERROR: unsupported SPEED2.EXE build, hooks disabled");
        server_start(port);
        return;
    }
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK) {
        logf("ERROR: MH_Initialize: %s", MH_StatusToString(s));
    } else {
        d3d_install_hooks();
        patches_install();
    }
    server_start(port);
}

}  // namespace

const ExeInfo& exe() { return g_exe; }

std::string hex_addr(uintptr_t a) {
    char b[16];
    snprintf(b, sizeof b, "0x%08x", static_cast<unsigned>(a));
    return b;
}

std::string to_hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = d[p[i] >> 4];
        s[i * 2 + 1] = d[p[i] & 15];
    }
    return s;
}

std::vector<uint8_t> from_hex(const std::string& in) {
    std::string s;
    for (char c : in)
        if (!isspace(static_cast<unsigned char>(c))) s += c;
    if (s.size() % 2) throw std::runtime_error("odd number of hex digits");
    std::vector<uint8_t> out(s.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(std::stoul(s.substr(i * 2, 2), nullptr, 16));
    return out;
}

std::string base64(const uint8_t* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i < n) {
        uint32_t v = p[i] << 16;
        if (i + 1 < n) v |= p[i + 1] << 8;
        o += t[v >> 18]; o += t[(v >> 12) & 63];
        o += (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        o += '=';
    }
    return o;
}

}  // namespace probe

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // Hooks must be in place before the game calls Direct3DCreate9, so init runs synchronously here.
        probe::init(inst);
    }
    return TRUE;
}
