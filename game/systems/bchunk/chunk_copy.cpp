// Chunk re-alignment used by the bundle loader 0x577C70 before it loads a bundle:
//   CopyChunksAligned measures (dst == null) or writes a copy of a chunk tree where every leaf is preceded by a
//   padding chunk (id 0, bytes 0x11) so that dst offset == src address (mod 128); empty containers are dropped.
//   ClearChunks then blanks the source tree (ids -> 0, container sizes -> 0) after it has been unloaded.
#include <cstring>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

struct bChunk {
    uint32_t id, size;
};
inline bool is_container(const bChunk* c) { return static_cast<int32_t>(c->id) < 0; }
inline bChunk* next_chunk(bChunk* c) { return reinterpret_cast<bChunk*>(reinterpret_cast<uint8_t*>(c) + c->size + 8); }

// rep movsd + rep movsb, as the original copies (forward; same result as memcpy when not overlapping)
inline void rep_copy(uint8_t* d, const uint8_t* s, uint32_t n) {
    __asm {
        mov edi, d
        mov esi, s
        mov ecx, n
        mov edx, ecx
        shr ecx, 2
        rep movsd
        mov ecx, edx
        and ecx, 3
        rep movsb
    }
}

// ---------------------------------------------------------------- CopyChunksAligned 0x00570DD0 (4 callers + itself)
using Copy_t = int(__cdecl*)(int, bChunk*, uint32_t, uint8_t*, int, int);
Copy_t o_CopyChunksAligned = nullptr;
int __cdecl CopyChunksAligned(int mode, bChunk* src, uint32_t size, uint8_t* dst, int offset, int depth) {
    if (++depth > 10) game_fn<void(__cdecl*)()>(0x0043BD50)();  // bBreak: nesting too deep
    const uintptr_t end = reinterpret_cast<uintptr_t>(src) + size;
    for (bChunk* c = src; reinterpret_cast<uintptr_t>(c) < end; c = next_chunk(c)) {
        if (static_cast<uint8_t>(mode) == 0) {
            uint32_t pad = (reinterpret_cast<uintptr_t>(c) - offset) & 0x7F;
            if (pad != 0 && static_cast<int>(pad) <= 8) pad += 0x80;  // room for the padding chunk's header
            if (dst && pad) {
                bChunk* p = reinterpret_cast<bChunk*>(dst + offset);
                p->size = pad - 8;
                p->id = 0;
                std::memset(dst + offset + 8, 0x11, pad - 8);
            }
            offset += pad;
        }
        if (is_container(c)) {
            const int r = CopyChunksAligned(mode, c + 1, c->size, dst, offset + 8, depth);
            if (r != offset + 8) {  // non-empty container: emit its header with the new size
                if (dst) {
                    bChunk* h = reinterpret_cast<bChunk*>(dst + offset);
                    h->id = c->id;
                    h->size = r - offset - 8;
                }
                offset = r;
            }
        } else if (static_cast<uint8_t>(mode) == 0) {
            if (dst) rep_copy(dst + offset, reinterpret_cast<const uint8_t*>(c), c->size + 8);
            offset += c->size + 8;
        }
    }
    return offset;
}

// ---------------------------------------------------------------- ClearChunks 0x00570F00 (1 caller + itself)
using Clear_t = uint32_t(__cdecl*)(bChunk*, uint32_t);
Clear_t o_ClearChunks = nullptr;
uint32_t __cdecl ClearChunks(bChunk* src, uint32_t size) {
    uint32_t eax = size;  // EAX the original leaves (no caller reads it; kept for the register test)
    const uintptr_t end = reinterpret_cast<uintptr_t>(src) + size;
    for (bChunk* c = src; reinterpret_cast<uintptr_t>(c) < end;) {
        eax = c->size;
        bChunk* next = next_chunk(c);  // from the size before it is cleared
        if (is_container(c)) {
            eax = ClearChunks(c + 1, c->size);
            c->size = 0;
        }
        c->id = 0;
        c = next;
    }
    return eax;
}

}  // namespace

NFSU2_REWRITE_N(0x00570DD0, CopyChunksAligned, 6,
                "int __cdecl(int mode, bChunk* src, u32 size, u8* dst /*null=measure*/, int offset, int depth)",
                o_CopyChunksAligned);
NFSU2_REWRITE_N(0x00570F00, ClearChunks, 2, "void __cdecl(bChunk* src, u32 size) (ids/container sizes -> 0)",
                o_ClearChunks);

namespace {

// Random chunk tree: leaves of random size (not only multiples of 4), containers nested up to `levels` deep,
// sometimes empty. Ids: containers have the high bit set, leaves sometimes 0.
void gen_tree(game::Rng& r, std::vector<uint8_t>& out, int levels, int count) {
    for (int i = 0; i < count; ++i) {
        const size_t at = out.size();
        out.resize(at + 8);
        const bool cont = levels > 0 && r.below(3) == 0;
        uint32_t id = cont ? (0x80000000u | r.next()) : (r.below(8) == 0 ? 0u : (r.next() & 0x7FFFFFFFu));
        if (cont) {
            gen_tree(r, out, levels - 1, static_cast<int>(r.below(5)));  // 0 children = empty container
        } else {
            const uint32_t n = r.below(4) == 0 ? r.below(400) : r.below(40);
            for (uint32_t k = 0; k < n; ++k) out.push_back(static_cast<uint8_t>(r.next()));
        }
        const uint32_t size = static_cast<uint32_t>(out.size() - at - 8);
        std::memcpy(&out[at], &id, 4);
        std::memcpy(&out[at + 4], &size, 4);
    }
}

// The tree placed at a random address mod 128 (the padding depends on the source address).
struct Placed {
    std::vector<uint8_t> mem;
    bChunk* p;
    uint32_t size;
    Placed(const std::vector<uint8_t>& tree, uint32_t mis) : mem(tree.size() + 256), size(static_cast<uint32_t>(tree.size())) {
        uintptr_t a = (reinterpret_cast<uintptr_t>(mem.data()) + 127) & ~uintptr_t(127);
        p = reinterpret_cast<bChunk*>(a + mis);
        if (!tree.empty()) std::memcpy(p, tree.data(), tree.size());
    }
};

}  // namespace

NFSU2_DIFF_TEST(CopyChunksAligned) {
    return game::run_diff(ctx, "CopyChunksAligned", o_CopyChunksAligned, [](game::Rng& r) {
        std::vector<uint8_t> tree;
        const int levels = static_cast<int>(r.below(4));
        gen_tree(r, tree, levels, static_cast<int>(r.below(8)));
        const uint32_t mis = r.below(32) * 4 + (r.below(8) == 0 ? r.below(4) : 0);
        Placed src(tree, mis);
        const int mode = r.below(4) == 0 ? static_cast<int>(r.below(3) == 0 ? r.next() : 1) : 0;
        const int offset = static_cast<int>(r.below(2) ? r.below(300) : 0);
        const int depth = static_cast<int>(r.below(9 - levels));
        const int need = o_CopyChunksAligned(mode, src.p, src.size, nullptr, offset, depth);
        const int need2 = CopyChunksAligned(mode, src.p, src.size, nullptr, offset, depth);
        const std::string what = "levels=" + std::to_string(levels) + " bytes=" + std::to_string(tree.size()) +
                                 " mis=" + std::to_string(mis) + " mode=" + std::to_string(mode) +
                                 " off=" + std::to_string(offset);
        if (need != need2) return game::MismatchText("measure " + what + ": " + std::to_string(need) + "/" + std::to_string(need2));
        const bool write = r.below(4) != 0;
        std::vector<uint8_t> da(need + 64), db;
        for (auto& b : da) b = static_cast<uint8_t>(r.next());
        db = da;
        const int ra = o_CopyChunksAligned(mode, src.p, src.size, write ? da.data() : nullptr, offset, depth);
        const int rb = CopyChunksAligned(mode, src.p, src.size, write ? db.data() : nullptr, offset, depth);
        if (ra != rb) return game::MismatchText("ret " + what + ": " + std::to_string(ra) + "/" + std::to_string(rb));
        if (da != db) {
            size_t k = 0;
            while (da[k] == db[k]) ++k;
            return game::MismatchText("dst " + what + " first diff at " + std::to_string(k));
        }
        if (std::memcmp(src.p, tree.data(), tree.size()) != 0) return game::MismatchText("src modified " + what);
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(ClearChunks) {
    return game::run_diff(ctx, "ClearChunks", o_ClearChunks, [](game::Rng& r) {
        std::vector<uint8_t> tree;
        gen_tree(r, tree, static_cast<int>(r.below(4)), static_cast<int>(r.below(8)));
        Placed a(tree, r.below(32) * 4), b(tree, 0);
        std::memcpy(b.p, tree.data(), tree.size());
        const uint32_t ea = o_ClearChunks(a.p, a.size), eb = ClearChunks(b.p, b.size);
        if (ea != eb) return game::MismatchText("eax " + std::to_string(ea) + "/" + std::to_string(eb));
        if (std::memcmp(a.p, b.p, tree.size()) != 0) return game::MismatchText("tree bytes=" + std::to_string(tree.size()));
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(regs_chunk_copy) {
    using game::arg;
    return game::run_diff(ctx, "regs_chunk_copy", o_ClearChunks, [](game::Rng& r) {
        std::vector<uint8_t> tree;
        gen_tree(r, tree, 2, 1 + static_cast<int>(r.below(5)));
        Placed s(tree, r.below(32) * 4);
        std::vector<uint8_t> dst(CopyChunksAligned(0, s.p, s.size, nullptr, 0, 0) + 16);
        std::vector<uint8_t> leaves;  // leaves only: clearing twice (orig, then ours) walks the same way
        gen_tree(r, leaves, 0, 1 + static_cast<int>(r.below(5)));
        Placed c1(leaves, 0);
        for (auto c : {game::check_regs("CopyChunksAligned", (void*)o_CopyChunksAligned, NFSU2_ENTRY(CopyChunksAligned),
                                        {0, arg(s.p), s.size, arg(dst.data()), 0, 0}, false),
                       game::check_regs("ClearChunks", (void*)o_ClearChunks, NFSU2_ENTRY(ClearChunks),
                                        {arg(c1.p), c1.size}, false)})
            if (!c.ok) return c;
        return game::Ok();
    });
}
