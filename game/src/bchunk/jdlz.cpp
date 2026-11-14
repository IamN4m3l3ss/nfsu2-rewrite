// JDLZ decompression (NA v1.2) — 0x00573700, reached through a function pointer (no direct callers).
//
// Format: "JDLZ" | u8 version (must be 2) | 3 pad | u32 uncompressed size | u32 compressed size (incl. 0x12-byte
// header) | u8 flags1 | u8 flags2 | stream. flags1 bit = 1: back-reference (2 bytes), else literal. For a
// back-reference, flags2 bit = 1: length ((b0&0xF0)<<4 | b1) + 3, distance (b0&0x0F) + 1; else length (b0&0x1F) + 3,
// distance ((b0&0xE0)<<3 | b1) + 0x11. Flag bytes are refilled from the stream when their sentinel bit (0x100)
// shifts down to 1. Quirks kept: literals stop at the output end but back-reference copies don't; the loop runs
// until the remaining byte count is exactly 0. Returns the uncompressed size, or 0 if the header is wrong.
#include <cstdio>
#include <string>
#include <vector>

#include "../diff.h"
#include "../regs.h"
#include "../rewrite.h"

namespace {

using Jdlz_t = uint32_t(__cdecl*)(const uint8_t*, uint8_t*);
Jdlz_t o_JdlzDecompress = nullptr;

uint32_t __cdecl JdlzDecompress(const uint8_t* src, uint8_t* dst) {
    uint32_t magic;
    memcpy(&magic, src, 4);
    if (magic != 0x5A4C444Au || src[4] != 2) return 0;
    uint32_t unc, comp;
    memcpy(&unc, src + 8, 4);
    memcpy(&comp, src + 12, 4);
    uint32_t flags1 = src[0x10] | 0x100u, flags2 = src[0x11] | 0x100u;
    const uint8_t* p = src + 0x12;
    uint8_t* const end = dst + unc;
    int32_t count = static_cast<int32_t>(comp - 0x12);
    while (count != 0) {
        if (flags1 & 1) {
            const uint32_t b0 = p[0], b1 = p[1];
            uint32_t len, dist;
            if (flags2 & 1) {
                len = (((b0 & 0xF0u) << 4) | b1) + 3;
                dist = (b0 & 0x0Fu) + 1;
            } else {
                len = (b0 & 0x1Fu) + 3;
                dist = (((b0 & 0xE0u) << 3) | b1) + 0x11;
            }
            const uint8_t* from = dst - dist;
            for (uint32_t i = 0; static_cast<int32_t>(i) < static_cast<int32_t>(len); ++i) dst[i] = from[i];
            flags2 >>= 1;
            p += 2;
            count -= 2;
            dst += len;
        } else {
            if (dst < end) *dst++ = *p++;
            --count;
        }
        flags1 >>= 1;
        if (flags1 == 1) {
            flags1 = *p++ | 0x100u;
            --count;
        }
        if (flags2 == 1) {
            flags2 = *p++ | 0x100u;
            --count;
        }
    }
    return unc;
}

// Real JDLZ blocks from the install (game cwd = game folder).
struct Block {
    std::vector<uint8_t> data;
    uint32_t unc;
};
std::vector<Block>& blocks() {
    static std::vector<Block> b;
    if (!b.empty()) return b;
    for (const char* path : {"CARS\\TEXTURES.BIN", "GLOBAL\\DYNTEX.BIN", "CARS\\WHEELS\\TEXTURES.BIN"}) {
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        std::vector<uint8_t> d;
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
        fclose(f);
        for (size_t i = 0; i + 0x12 < d.size(); ++i) {
            if (memcmp(&d[i], "JDLZ\x02", 5) != 0) continue;
            uint32_t unc, comp;
            memcpy(&unc, &d[i + 8], 4);
            memcpy(&comp, &d[i + 12], 4);
            if (comp < 0x12 || i + comp > d.size() || unc > (16u << 20)) continue;
            b.push_back({std::vector<uint8_t>(d.begin() + i, d.begin() + i + comp), unc});
        }
    }
    return b;
}

}  // namespace

NFSU2_REWRITE_N(0x00573700, JdlzDecompress, 2, "uint32_t __cdecl(const uint8_t* src, uint8_t* dst) -> uncompressed size or 0", o_JdlzDecompress);

// iterations = number of real blocks to decompress (cycled); each decompressed by both into guarded buffers
NFSU2_DIFF_TEST(JdlzDecompress) {
    auto& bl = blocks();
    if (bl.empty()) {
        game::DiffResult r;
        r.name = "JdlzDecompress";
        r.error = "no JDLZ blocks found (cwd not the game folder?)";
        return r;
    }
    game::DiffCtx c{ctx.iterations < bl.size() ? ctx.iterations : static_cast<uint32_t>(bl.size()), ctx.seed};
    uint32_t i = 0;
    return game::run_diff(c, "JdlzDecompress", o_JdlzDecompress, [&bl, &i](game::Rng& r) {
        const Block& b = bl[(i++ + r.below(1)) % bl.size()];
        constexpr size_t kGuard = 8192;  // back-references read up to 2064 bytes before dst; copies may overrun
        std::vector<uint8_t> A(kGuard + b.unc + kGuard, 0xCD), B(A);
        const uint32_t ra = o_JdlzDecompress(b.data.data(), A.data() + kGuard);
        const uint32_t rb = JdlzDecompress(b.data.data(), B.data() + kGuard);
        if (ra != rb || A != B) {
            size_t k = 0;
            while (k < A.size() && A[k] == B[k]) ++k;
            return game::MismatchText("block of " + std::to_string(b.data.size()) + " bytes: return " + std::to_string(ra) +
                                      "/" + std::to_string(rb) + ", first differing byte at " + std::to_string(k));
        }
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(regs_jdlz) {
    using game::arg;
    auto& bl = blocks();
    game::DiffCtx c{bl.empty() ? 0u : 20u, ctx.seed};
    return game::run_diff(c, "regs_jdlz", o_JdlzDecompress, [&bl](game::Rng& r) {
        const Block& b = bl[r.below(static_cast<uint32_t>(bl.size()))];
        std::vector<uint8_t> out(8192 + b.unc + 8192);
        uint8_t bad[0x20] = {'J', 'D', 'L', 'X'};
        for (auto cc : {game::check_regs("JdlzDecompress", (void*)o_JdlzDecompress, NFSU2_ENTRY(JdlzDecompress),
                                         {arg(const_cast<uint8_t*>(b.data.data())), arg(out.data() + 8192)}, false),
                        game::check_regs("JdlzDecompress(bad)", (void*)o_JdlzDecompress, NFSU2_ENTRY(JdlzDecompress),
                                         {arg(bad), arg(out.data() + 8192)}, false)})
            if (!cc.ok) return cc;
        return game::Ok();
    });
}
