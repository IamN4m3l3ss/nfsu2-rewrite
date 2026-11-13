#include "probe.h"

#include <algorithm>
#include <stdexcept>

namespace probe {

bool mem_read(uintptr_t addr, void* out, size_t size) {
    // ReadProcessMemory on ourselves fails cleanly on unmapped/protected pages instead of faulting
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, size, &got) && got == size;
}

bool mem_write(uintptr_t addr, const void* data, size_t size) {
    // make every page in the range writable, write, restore, flush icache (code may be patched)
    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t p = addr, end = addr + size;
    while (p < end) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
        p = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), size, PAGE_EXECUTE_READWRITE, &old)) return false;
    SIZE_T put = 0;
    BOOL ok = WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), data, size, &put);
    DWORD tmp;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), size, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), size);
    return ok && put == size;
}

namespace {
struct Pat { std::vector<uint8_t> bytes; std::vector<bool> mask; };

Pat parse_pattern(const std::string& s) {
    Pat p;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == ' ') { ++i; continue; }
        if (s[i] == '?') {
            p.bytes.push_back(0); p.mask.push_back(false);
            i += (i + 1 < s.size() && s[i + 1] == '?') ? 2 : 1;
            continue;
        }
        if (i + 1 >= s.size()) throw std::runtime_error("bad pattern");
        p.bytes.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
        p.mask.push_back(true);
        i += 2;
    }
    if (p.bytes.empty()) throw std::runtime_error("empty pattern");
    return p;
}

bool readable(DWORD prot) {
    if (prot & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                   PAGE_EXECUTE_WRITECOPY);
}
}  // namespace

std::vector<uintptr_t> aob_scan(const std::string& pattern, uintptr_t start, uintptr_t end, size_t max_hits) {
    Pat pat = parse_pattern(pattern);
    const size_t n = pat.bytes.size();
    std::vector<uintptr_t> hits;
    std::vector<uint8_t> buf;
    uintptr_t p = start;
    MEMORY_BASIC_INFORMATION mbi{};
    while (p < end && hits.size() < max_hits) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof mbi)) break;
        uintptr_t rbeg = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t rend = rbeg + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && readable(mbi.Protect)) {
            uintptr_t a = std::max(p, rbeg), b = std::min(end, rend);
            if (b > a && b - a >= n) {
                buf.resize(b - a);
                if (mem_read(a, buf.data(), buf.size())) {
                    for (size_t i = 0; i + n <= buf.size() && hits.size() < max_hits; ++i) {
                        size_t k = 0;
                        while (k < n && (!pat.mask[k] || buf[i + k] == pat.bytes[k])) ++k;
                        if (k == n) hits.push_back(a + i);
                    }
                }
            }
        }
        p = rend;
    }
    return hits;
}

}  // namespace probe
