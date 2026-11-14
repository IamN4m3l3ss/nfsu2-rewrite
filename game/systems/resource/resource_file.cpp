// Resource files (NA v1.2): one object per loaded bundle file, 0xF0 bytes from "ResourceFileSlotPool" (0x50 slots,
// pool pointer 0x864F78), kept in a list at 0x8651A4. Plus the compression front end used when a bundle is loaded.
//
//   ResourceFile  +00 next  +04 prev  +08 u8 owns_buffer  +0C type  +10 flags (2: alloc bottom, 4: free after load,
//                 8: may be compressed)  +14 ?  +18 size override  +1C char name[60]  +58 u8
//                 +94 char alloc_name[48]  +C4 alloc flags (bMalloc flags, default 0x2000)  +C8 pending  +CC loaded
//                 +D0 +D4  +D8 u8* chunks  +DC u32 size  +E0 +E4 +E8  +EC debug handle (passed to a no-op)
//   compressed block header: u32 magic, u8 version, ..., +08 u32 uncompressed size, +0C u32 compressed size,
//                 data at +10.  RAWW v1 = stored, HUFF v1, COMP v1 (LZ), JDLZ v2.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../bchunk/fake_loader.h"
#include "../memory/test_heap.h"
#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

constexpr uint32_t kRAWW = 0x57574152, kHUFF = 0x46465548, kCOMP = 0x504D4F43, kJDLZ = 0x5A4C444A;

struct ResourceFile {
    ResourceFile* next;
    ResourceFile* prev;
    uint8_t owns_buffer, pad09[3];
    int32_t type, flags, unk14, size_override;
    char name[60];
    uint8_t unk58, pad59[0x3B];
    char alloc_name[48];
    int32_t alloc_flags, pending, loaded, unkD0, unkD4;
    uint8_t* chunks;
    uint32_t size;
    int32_t unkE0, unkE4, unkE8, debug_handle;
};
static_assert(sizeof(ResourceFile) == 0xF0, "ResourceFile size");
static_assert(offsetof(ResourceFile, alloc_name) == 0x94 && offsetof(ResourceFile, chunks) == 0xD8, "layout");

using Void_t = void(__cdecl*)();
inline void* bmalloc2(uint32_t size, int32_t flags) { return game_fn<void*(__cdecl*)(uint32_t, int32_t)>(0x00440820)(size, flags); }
inline void bfree(void* p) { game_fn<void(__cdecl*)(void*)>(0x00440540)(p); }

// ---------------------------------------------------------------- IsCompressed 0x00574630
using IsComp_t = int(__cdecl*)(const uint8_t*);
IsComp_t o_IsCompressed = nullptr;
int __cdecl IsCompressed(const uint8_t* h) {
    const uint32_t magic = *reinterpret_cast<const uint32_t*>(h);
    if (magic == kRAWW || magic == kHUFF || magic == kCOMP) return h[4] == 1;
    if (magic == kJDLZ) return h[4] == 2;
    return 0;
}

// ---------------------------------------------------------------- RawwDecompress 0x005744F0
using Decomp_t = uint32_t(__cdecl*)(const uint8_t*, uint8_t*);
Decomp_t o_RawwDecompress = nullptr;
uint32_t __cdecl RawwDecompress(const uint8_t* src, uint8_t* dst) {
    if (*reinterpret_cast<const uint32_t*>(src) != kRAWW || src[4] != 1) return 0;
    const uint32_t n = *reinterpret_cast<const uint32_t*>(src + 8);
    const uint8_t* s = src + 0x10;
    __asm {  // rep movsd + rep movsb (forward copy, as the original)
        mov esi, s
        mov edi, dst
        mov ecx, n
        mov edx, ecx
        shr ecx, 2
        rep movsd
        mov ecx, edx
        and ecx, 3
        rep movsb
    }
    return n;
}

// ---------------------------------------------------------------- Decompress 0x00574670 (dispatch)
Decomp_t o_Decompress = nullptr;
uint32_t __cdecl Decompress(const uint8_t* src, uint8_t* dst) {
    if (!src || !dst) return 0;
    switch (*reinterpret_cast<const uint32_t*>(src)) {
        case kCOMP: return game_fn<Decomp_t>(0x00574530)(src, dst);
        case kHUFF: return game_fn<Decomp_t>(0x005744C0)(src, dst);
        case kRAWW: return game_fn<Decomp_t>(0x005744F0)(src, dst);
        case kJDLZ: return game_fn<Decomp_t>(0x00573700)(src, dst);
        default: return 0;
    }
}

// ---------------------------------------------------------------- ResourceFile small methods
using SetAlloc_t = void(__thiscall*)(ResourceFile*, int32_t, const char*);
SetAlloc_t o_SetAllocParams = nullptr;
void __cdecl SetAllocParams(ResourceFile* self, int32_t flags, const char* name) {  // 0x00570FA0
    self->alloc_flags = flags;
    std::memset(self->alloc_name, 0, sizeof self->alloc_name);
    game_fn<char*(__cdecl*)(char*, const char*, int)>(0x0043DC00)(self->alloc_name, name, 0x2F);  // bStrNCpy
}

using AllocBuf_t = void(__thiscall*)(ResourceFile*, int32_t);
AllocBuf_t o_AllocateBuffer = nullptr;
void __cdecl AllocateBuffer(ResourceFile* self, int32_t bottom) {  // 0x00570FD0 (arg: bool in the low byte)
    if (!self->size) return;
    int32_t f = self->alloc_flags;
    if (self->flags & 2) f |= 8;
    if (static_cast<uint8_t>(bottom)) f |= 8;
    self->chunks = static_cast<uint8_t*>(bmalloc2(self->size, f));
}

using SetOwns_t = void(__thiscall*)(ResourceFile*, int32_t);
SetOwns_t o_SetOwnsBuffer = nullptr;
void __cdecl SetOwnsBuffer(ResourceFile* self, int32_t owns) { self->owns_buffer = static_cast<uint8_t>(owns); }  // 0x00571010

using Method_t = void(__thiscall*)(ResourceFile*);
Method_t o_FreeBuffer = nullptr;
void __cdecl FreeBuffer(ResourceFile* self) {  // 0x00571020
    if (self->owns_buffer && self->chunks) {
        bfree(self->chunks);
        self->chunks = nullptr;
    }
}

using DecPending_t = void(__cdecl*)(ResourceFile*);
DecPending_t o_DecPending = nullptr;
void __cdecl DecPending(ResourceFile* rf) { --rf->pending; }  // 0x00571050 (callback)

// ---------------------------------------------------------------- LoadChunksAligned 0x00577C70
// If any container would need re-alignment, copy the bundle into a fresh aligned buffer (bMalloc2 with the given
// flags), blank the old one, load the copy, "unload" the blanked old one (all ids 0) and free it; the caller's
// pointer/size are replaced. Otherwise just LoadChunks in place. Returns whether it re-aligned (no caller reads it).
using LCA_t = int(__cdecl*)(uint8_t**, uint32_t*, int32_t, const char*);
LCA_t o_LoadChunksAligned = nullptr;
int __cdecl LoadChunksAligned(uint8_t** pchunks, uint32_t* psize, int32_t alloc_flags, const char* name) {
    using Copy_t = int(__cdecl*)(int, uint8_t*, uint32_t, uint8_t*, int, int);
    using Chunks3_t = void(__cdecl*)(uint8_t*, uint32_t, const char*);
    uint8_t* const src = *pchunks;
    const uint32_t size = *psize;
    if (!src || !size) return 0;
    const int aligned = game_fn<Copy_t>(0x00570DD0)(0, src, size, nullptr, 0, 0);
    int moved = 0;
    for (uint8_t* c = src; c < src + size; c += *reinterpret_cast<uint32_t*>(c + 4) + 8) {
        if (*reinterpret_cast<int32_t*>(c) < 0) {
            const int at = moved + 8;
            const int r = game_fn<Copy_t>(0x00570DD0)(1, c + 8, *reinterpret_cast<uint32_t*>(c + 4), nullptr, at, 1);
            if (r != at) moved = r;
        }
    }
    if (moved == 0) {
        game_fn<Chunks3_t>(0x00577AB0)(src, size, name);  // LoadChunks
        return 0;
    }
    uint8_t* dst = static_cast<uint8_t*>(bmalloc2(static_cast<uint32_t>(aligned), alloc_flags));
    game_fn<Copy_t>(0x00570DD0)(0, src, size, dst, 0, 0);
    game_fn<void(__cdecl*)(uint8_t*, uint32_t)>(0x00570F00)(src, size);  // ClearChunks
    game_fn<Chunks3_t>(0x00577AB0)(src, size, name);
    game_fn<Chunks3_t>(0x00577AB0)(dst, static_cast<uint32_t>(aligned), name);
    game_fn<Chunks3_t>(0x00577B10)(src, size, name);  // UnloadChunks
    bfree(src);
    *psize = static_cast<uint32_t>(aligned);
    *pchunks = dst;
    return 1;
}

// ---------------------------------------------------------------- ResourceFile::Unload 0x00577D80
Method_t o_Unload = nullptr;
void __cdecl Unload(ResourceFile* self) {
    if (self->debug_handle) game_fn<void(__cdecl*)(int32_t)>(0x004022C0)(self->debug_handle);  // empty stub
    if (self->chunks) {
        game_fn<void(__cdecl*)(uint8_t*, uint32_t, const char*)>(0x00577B10)(self->chunks, self->size, self->name);
        if (self->owns_buffer && self->chunks) {
            bfree(self->chunks);
            self->chunks = nullptr;
        }
    }
}

// ---------------------------------------------------------------- ResourceFile::Load 0x00577DE0
// After the file data is in memory: decompress if flagged and compressed, then load the chunks (re-aligned if
// needed); flag 4 frees the buffer afterwards. Does nothing if already loaded or loads are still pending.
Method_t o_Load = nullptr;
void __cdecl Load(ResourceFile* self) {
    if (self->loaded || self->pending) return;
    if ((self->flags & 8) && self->chunks) {
        uint8_t* packed = self->chunks;
        if (game_fn<IsComp_t>(0x00574630)(packed)) {
            const uint32_t n = *reinterpret_cast<uint32_t*>(packed + 8);
            self->size = n;
            self->chunks = nullptr;
            if (n) {
                game_fn<AllocBuf_t>(0x00570FD0)(self, 0);
                game_fn<Decomp_t>(0x00574670)(packed, self->chunks);
            }
            bfree(packed);
        }
    }
    game_fn<LCA_t>(0x00577C70)(&self->chunks, &self->size, self->alloc_flags, self->alloc_name);
    if (self->flags & 4) {
        if (self->chunks) game_fn<Method_t>(0x00571020)(self);
        self->size = 0;
    }
    self->loaded = 1;
}

// ---------------------------------------------------------------- FindResourceFile 0x00577EB0
// Walks the list at 0x8651A4 backwards (prev links) comparing names with bStrCmp.
using Find_t = ResourceFile*(__cdecl*)(const char*);
Find_t o_FindResourceFile = nullptr;
ResourceFile* __cdecl FindResourceFile(const char* name) {
    ResourceFile* const head = reinterpret_cast<ResourceFile*>(0x008651A4);
    for (ResourceFile* f = head->prev; f != head; f = f->prev)
        if (game_fn<int(__cdecl*)(const char*, const char*)>(0x0043DC30)(f->name, name) == 0) return f;
    return nullptr;
}

}  // namespace

NFSU2_REWRITE_N(0x00574630, IsCompressed, 1, "int __cdecl(const u8* header) RAWW/HUFF/COMP v1, JDLZ v2", o_IsCompressed);
NFSU2_REWRITE_N(0x005744F0, RawwDecompress, 2, "u32 __cdecl(const u8* src, u8* dst) stored block", o_RawwDecompress);
NFSU2_REWRITE_N(0x00574670, Decompress, 2, "u32 __cdecl(const u8* src, u8* dst) dispatch by magic", o_Decompress);
NFSU2_REWRITE_THIS(0x00570FA0, SetAllocParams, 2, "void ResourceFile::SetAllocParams(int flags, const char* name)", o_SetAllocParams);
NFSU2_REWRITE_THIS(0x00570FD0, AllocateBuffer, 1, "void ResourceFile::AllocateBuffer(bool bottom)", o_AllocateBuffer);
NFSU2_REWRITE_THIS(0x00571010, SetOwnsBuffer, 1, "void ResourceFile::SetOwnsBuffer(bool)", o_SetOwnsBuffer);
NFSU2_REWRITE_THIS(0x00571020, FreeBuffer, 0, "void ResourceFile::FreeBuffer()", o_FreeBuffer);
NFSU2_REWRITE_N(0x00571050, DecPending, 1, "void __cdecl(ResourceFile*) --pending (callback)", o_DecPending);
NFSU2_REWRITE_N(0x00577C70, LoadChunksAligned, 4, "int __cdecl(u8** chunks, u32* size, int alloc_flags, const char* name)", o_LoadChunksAligned);
NFSU2_REWRITE_THIS(0x00577D80, Unload, 0, "void ResourceFile::Unload()", o_Unload);
NFSU2_REWRITE_THIS(0x00577DE0, Load, 0, "void ResourceFile::Load()", o_Load);
NFSU2_REWRITE_N(0x00577EB0, FindResourceFile, 1, "ResourceFile* __cdecl(const char* name)", o_FindResourceFile);

// ============================================================================================ diff tests
namespace {
using game::testing::FakeChunkLoader;
using game::testing::TestHeap;

std::vector<uint8_t> raww_block(game::Rng& r, const std::vector<uint8_t>& payload, bool valid) {
    std::vector<uint8_t> b(0x10 + payload.size());
    const uint32_t magic = valid ? kRAWW : (r.below(2) ? kRAWW : r.next());
    std::memcpy(&b[0], &magic, 4);
    b[4] = valid ? 1 : static_cast<uint8_t>(r.below(4));
    const uint32_t n = static_cast<uint32_t>(payload.size());
    std::memcpy(&b[8], &n, 4);
    std::memcpy(&b[12], &n, 4);
    if (n) std::memcpy(&b[0x10], payload.data(), n);
    return b;
}

// Bundle of fake chunks: leaves (fake leaf id), id-0 padding, and containers (fake container id) holding leaves.
std::vector<uint8_t> fake_bundle(game::Rng& r) {
    std::vector<uint8_t> b;
    auto put = [&b](uint32_t id, uint32_t len, game::Rng& rr) {
        const size_t at = b.size();
        b.resize(at + 8 + len);
        std::memcpy(&b[at], &id, 4);
        std::memcpy(&b[at + 4], &len, 4);
        for (uint32_t i = 0; i < len; ++i) b[at + 8 + i] = static_cast<uint8_t>(rr.next());
        return at;
    };
    const uint32_t n = 1 + r.below(r.below(4) == 0 ? 120 : 10);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t kind = r.below(6);
        if (kind == 0) {
            put(0, r.below(4) * 4, r);
        } else if (kind == 1) {
            const size_t at = put(game::testing::kFakeContainerId, 0, r);
            const uint32_t kids = r.below(4);
            for (uint32_t k = 0; k < kids; ++k) put(game::testing::kFakeLeafId, r.below(6) * 4, r);
            const uint32_t len = static_cast<uint32_t>(b.size() - at - 8);
            std::memcpy(&b[at + 4], &len, 4);
        } else {
            put(game::testing::kFakeLeafId, r.below(24) * 4 + (r.below(4) == 0 ? r.below(4) : 0), r);
        }
    }
    return b;
}

// heap-allocated copy of `bytes` (so the code under test may bFree it), in the given test heap's slot 7
uint8_t* heap_copy(const std::vector<uint8_t>& bytes) {
    uint8_t* p = static_cast<uint8_t*>(bmalloc2(static_cast<uint32_t>(bytes.size()), 7));
    if (p && !bytes.empty()) std::memcpy(p, bytes.data(), bytes.size());
    return p;
}

// object as dwords with the chunk pointer replaced by its heap offset
std::vector<uint64_t> rf_norm(const ResourceFile& f, const TestHeap& h) {
    std::vector<uint64_t> v;
    const uint32_t* d = reinterpret_cast<const uint32_t*>(&f);
    for (uint32_t i = 0; i < sizeof(ResourceFile) / 4; ++i) v.push_back(d[i]);
    v[offsetof(ResourceFile, chunks) / 4] = h.off(f.chunks);
    return v;
}
std::vector<uint64_t> calls_norm(const TestHeap& h) {
    std::vector<uint64_t> v;
    for (auto& c : FakeChunkLoader::calls) v.push_back((h.off(reinterpret_cast<void*>(c.chunk)) << 8) | uint8_t(c.kind));
    return v;
}
void random_rf(game::Rng& r, ResourceFile& f) {
    uint8_t* p = reinterpret_cast<uint8_t*>(&f);
    for (uint32_t i = 0; i < sizeof f; ++i) p[i] = static_cast<uint8_t>(r.next());
    f.name[r.below(59)] = 0;
    f.alloc_name[r.below(47)] = 0;
}
int32_t rand_alloc_flags(game::Rng& r) {
    static const int32_t al[] = {0, 0, 16, 64, 128};
    return 7 | (r.below(2) ? 0x10 : 0) | (r.below(3) == 0 ? 8 : 0) | (al[r.below(5)] << 6) | 0x2000;
}
std::string rand_name(game::Rng& r) {
    std::string s;
    const uint32_t n = r.below(4) == 0 ? 40 + r.below(30) : r.below(20);
    for (uint32_t i = 0; i < n; ++i) s += static_cast<char>('A' + r.below(26));
    return s;
}

struct Snap {
    std::vector<uint64_t> rf, calls, bytes;
    int32_t allocs, used;
    bool operator==(const Snap&) const = default;
};
// Runs `op` on a fresh object in heap h (installed in slot 7); `setup` builds the object (same RNG state for both).
template <class Setup, class Op> Snap run_rf(TestHeap& h, uint32_t seed, Setup setup, Op op) {
    game::Rng r(seed);
    h.install();
    ResourceFile f{};
    setup(r, f);
    FakeChunkLoader::calls.clear();
    op(&f);
    Snap s{rf_norm(f, h), calls_norm(h), {}, h.num_allocs(), h.used()};
    if (f.chunks && h.off(f.chunks) && !(h.off(f.chunks) >> 63))
        for (uint32_t i = 0; i < f.size && i < 65536; ++i) s.bytes.push_back(f.chunks[i]);
    h.uninstall();
    return s;
}
std::string snap_diff(const Snap& a, const Snap& b) {
    if (a.rf != b.rf) {
        size_t i = 0;
        while (a.rf[i] == b.rf[i]) ++i;
        return "object +0x" + [](size_t x) { char t[8]; snprintf(t, sizeof t, "%X", static_cast<unsigned>(x)); return std::string(t); }(i * 4);
    }
    if (a.calls != b.calls) return "loader calls " + std::to_string(a.calls.size()) + "/" + std::to_string(b.calls.size());
    if (a.allocs != b.allocs || a.used != b.used) return "heap allocs " + std::to_string(a.allocs) + "/" + std::to_string(b.allocs);
    return "buffer bytes";
}

// setup for Load/Unload/LoadChunksAligned: a bundle in the heap, sometimes RAWW-packed (Load flag 8 path)
void setup_loaded(game::Rng& r, ResourceFile& f, bool for_load) {
    random_rf(r, f);
    f.alloc_flags = rand_alloc_flags(r);
    f.flags = static_cast<int32_t>(r.below(16));
    f.owns_buffer = static_cast<uint8_t>(r.below(3) ? 1 : 0);
    f.debug_handle = r.below(2) ? 0 : static_cast<int32_t>(r.next());
    if (for_load) {
        f.loaded = r.below(8) == 0 ? 1 : 0;
        f.pending = r.below(8) == 0 ? static_cast<int32_t>(r.below(3)) : 0;
    }
    std::vector<uint8_t> bundle = r.below(10) == 0 ? std::vector<uint8_t>{} : fake_bundle(r);
    if (for_load && (f.flags & 8) && r.below(3)) {
        std::vector<uint8_t> packed = raww_block(r, bundle, true);  // invalid headers: IsCompressed/Decompress tests
        f.chunks = heap_copy(packed);
        f.size = static_cast<uint32_t>(packed.size());
    } else {
        f.chunks = bundle.empty() && r.below(2) ? nullptr : heap_copy(bundle);
        f.size = static_cast<uint32_t>(bundle.size());
    }
}

template <class Setup, class OpA, class OpB>
game::Case compare_rf(game::Rng& r, uint32_t heap_size, Setup setup, OpA orig, OpB mine) {
    if (!game::testing::slot7_free()) return game::MismatchText("slot 7 in use");
    const uint32_t seed = r.next(), mis = r.below(32) * 4;
    TestHeap ha(heap_size, mis), hb(heap_size, mis);
    FakeChunkLoader fake;
    const Snap a = run_rf(ha, seed, setup, orig);
    const Snap b = run_rf(hb, seed, setup, mine);
    if (a == b) return game::Ok();
    return game::MismatchText("seed " + std::to_string(seed) + ": " + snap_diff(a, b));
}

}  // namespace

NFSU2_DIFF_TEST(IsCompressed) {
    return game::run_diff(ctx, "IsCompressed", o_IsCompressed, [](game::Rng& r) {
        static const uint32_t m[] = {kRAWW, kHUFF, kCOMP, kJDLZ};
        uint8_t h[16];
        for (auto& b : h) b = static_cast<uint8_t>(r.next());
        const uint32_t magic = r.below(5) == 0 ? r.next() : m[r.below(4)];
        std::memcpy(h, &magic, 4);
        h[4] = static_cast<uint8_t>(r.below(4) == 0 ? r.next() : r.below(4));
        const int a = o_IsCompressed(h), b = IsCompressed(h);
        return a == b ? game::Ok() : game::MismatchText("magic " + std::to_string(magic) + " v" + std::to_string(h[4]));
    });
}

NFSU2_DIFF_TEST(Decompress) {  // RAWW (valid/invalid), HUFF/COMP with a version their wrappers reject, unknown, nulls
    return game::run_diff(ctx, "Decompress", o_Decompress, [](game::Rng& r) {
        std::vector<uint8_t> payload(r.below(3000));
        for (auto& b : payload) b = static_cast<uint8_t>(r.next());
        std::vector<uint8_t> src = raww_block(r, payload, r.below(3) != 0);
        const uint32_t kind = r.below(10);
        if (kind == 0) std::memcpy(src.data(), &kHUFF, 4), src[4] = 2;
        if (kind == 1) std::memcpy(src.data(), &kCOMP, 4), src[6] = 2, src[7] = 0;  // COMP wrapper checks u16 at +6
        std::vector<uint8_t> da(payload.size() + 64), db;
        for (auto& b : da) b = static_cast<uint8_t>(r.next());
        db = da;
        const bool null_src = r.below(20) == 0, null_dst = r.below(20) == 0;
        if (kind == 1) return game::Ok();  // (COMP: not exercised until its decoder is mapped)
        const uint32_t a = o_Decompress(null_src ? nullptr : src.data(), null_dst ? nullptr : da.data());
        const uint32_t b = Decompress(null_src ? nullptr : src.data(), null_dst ? nullptr : db.data());
        if (a != b) return game::MismatchText("ret " + std::to_string(a) + "/" + std::to_string(b));
        return da == db ? game::Ok() : game::MismatchText("dst bytes, size " + std::to_string(payload.size()));
    });
}

NFSU2_DIFF_TEST(RawwDecompress) {
    return game::run_diff(ctx, "RawwDecompress", o_RawwDecompress, [](game::Rng& r) {
        std::vector<uint8_t> payload(r.below(5000));
        for (auto& b : payload) b = static_cast<uint8_t>(r.next());
        std::vector<uint8_t> src = raww_block(r, payload, r.below(4) != 0);
        std::vector<uint8_t> da(payload.size() + 64), db;
        for (auto& b : da) b = static_cast<uint8_t>(r.next());
        db = da;
        const uint32_t a = o_RawwDecompress(src.data(), da.data()), b = RawwDecompress(src.data(), db.data());
        if (a != b) return game::MismatchText("ret");
        return da == db ? game::Ok() : game::MismatchText("dst");
    });
}

NFSU2_DIFF_TEST(ResourceFileSmall) {  // SetAllocParams, SetOwnsBuffer, DecPending (plain object state)
    return game::run_diff(ctx, "ResourceFileSmall", o_SetAllocParams, [](game::Rng& r) {
        ResourceFile a, b;
        random_rf(r, a);
        b = a;
        const uint32_t which = r.below(3);
        if (which == 0) {
            const int32_t fl = static_cast<int32_t>(r.next());
            const std::string name = rand_name(r);
            o_SetAllocParams(&a, fl, name.c_str());
            SetAllocParams(&b, fl, name.c_str());
        } else if (which == 1) {
            const int32_t v = static_cast<int32_t>(r.next());
            o_SetOwnsBuffer(&a, v);
            SetOwnsBuffer(&b, v);
        } else {
            o_DecPending(&a);
            DecPending(&b);
        }
        return std::memcmp(&a, &b, sizeof a) == 0 ? game::Ok() : game::MismatchText("method " + std::to_string(which));
    });
}

NFSU2_DIFF_TEST(ResourceFileBuffer) {  // AllocateBuffer, FreeBuffer through the slot-7 test heap
    return game::run_diff(ctx, "ResourceFileBuffer", o_AllocateBuffer, [](game::Rng& r) {
        const bool alloc = r.below(2);
        const int32_t bottom = static_cast<int32_t>(r.below(2) ? r.next() : 0);
        auto setup = [](game::Rng& rr, ResourceFile& f) {
            random_rf(rr, f);
            f.alloc_flags = rand_alloc_flags(rr);
            f.flags = static_cast<int32_t>(rr.below(16));
            f.size = rr.below(5) == 0 ? 0 : rr.below(4000);
            f.owns_buffer = static_cast<uint8_t>(rr.below(2));
            f.chunks = rr.below(5) == 0 ? nullptr : static_cast<uint8_t*>(bmalloc2(1 + rr.below(500), 7));
        };
        if (alloc)
            return compare_rf(r, 64 * 1024, setup, [bottom](ResourceFile* f) { o_AllocateBuffer(f, bottom); },
                              [bottom](ResourceFile* f) { AllocateBuffer(f, bottom); });
        return compare_rf(r, 64 * 1024, setup, [](ResourceFile* f) { o_FreeBuffer(f); }, [](ResourceFile* f) { FreeBuffer(f); });
    });
}

NFSU2_DIFF_TEST(ResourceFileUnload) {
    return game::run_diff(ctx, "ResourceFileUnload", o_Unload, [](game::Rng& r) {
        return compare_rf(r, 96 * 1024, [](game::Rng& rr, ResourceFile& f) { setup_loaded(rr, f, false); },
                          [](ResourceFile* f) { o_Unload(f); }, [](ResourceFile* f) { Unload(f); });
    });
}

NFSU2_DIFF_TEST(ResourceFileLoad) {
    return game::run_diff(ctx, "ResourceFileLoad", o_Load, [](game::Rng& r) {
        return compare_rf(r, 160 * 1024, [](game::Rng& rr, ResourceFile& f) { setup_loaded(rr, f, true); },
                          [](ResourceFile* f) { o_Load(f); }, [](ResourceFile* f) { Load(f); });
    });
}

NFSU2_DIFF_TEST(LoadChunksAligned) {
    return game::run_diff(ctx, "LoadChunksAligned", o_LoadChunksAligned, [](game::Rng& r) {
        return compare_rf(r, 96 * 1024, [](game::Rng& rr, ResourceFile& f) { setup_loaded(rr, f, false); },
                          [](ResourceFile* f) { o_LoadChunksAligned(&f->chunks, &f->size, f->alloc_flags, f->alloc_name); },
                          [](ResourceFile* f) { LoadChunksAligned(&f->chunks, &f->size, f->alloc_flags, f->alloc_name); });
    });
}

// every resource file currently in the game's list + names that don't exist
NFSU2_DIFF_TEST(FindResourceFile) {
    std::vector<std::string> names;
    ResourceFile* const head = reinterpret_cast<ResourceFile*>(0x008651A4);
    for (ResourceFile* f = head->prev; f != head && names.size() < 200; f = f->prev) names.emplace_back(f->name);
    uint32_t i = 0;
    return game::run_diff(ctx, "FindResourceFile", o_FindResourceFile, [&names, &i](game::Rng& r) {
        std::string n = i < names.size() ? names[i] : (names.empty() || r.below(2) ? rand_name(r) : names[r.below(static_cast<uint32_t>(names.size()))]);
        if (i >= names.size() && !n.empty() && r.below(3) == 0) n[r.below(static_cast<uint32_t>(n.size()))] ^= 0x20;
        ++i;
        return o_FindResourceFile(n.c_str()) == FindResourceFile(n.c_str()) ? game::Ok() : game::MismatchText(n);
    });
}

NFSU2_DIFF_TEST(regs_resource) {
    using game::arg;
    return game::run_diff(ctx, "regs_resource", o_IsCompressed, [](game::Rng& r) {
        uint8_t h[16]{};
        std::memcpy(h, &kJDLZ, 4);
        h[4] = static_cast<uint8_t>(r.below(3));
        ResourceFile a{}, b{};
        const std::string n = rand_name(r);
        game::Case cs[] = {
            game::check_regs("IsCompressed", (void*)o_IsCompressed, NFSU2_ENTRY(IsCompressed), {arg(h)}, false),
            game::check_regs("FindResourceFile", (void*)o_FindResourceFile, NFSU2_ENTRY(FindResourceFile), {arg(n.c_str())}, false),
            game::check_regs_this("SetAllocParams", (void*)o_SetAllocParams, NFSU2_ENTRY(SetAllocParams), &a, &b,
                                  {r.next(), arg(n.c_str())}, false),
            game::check_regs_this("SetOwnsBuffer", (void*)o_SetOwnsBuffer, NFSU2_ENTRY(SetOwnsBuffer), &a, &b, {1}, false),
            game::check_regs_this("FreeBuffer(none)", (void*)o_FreeBuffer, NFSU2_ENTRY(FreeBuffer), &a, &b, {}, false),
            game::check_regs_this("Load(loaded)", (void*)o_Load, NFSU2_ENTRY(Load), &a, &b, {}, false),
        };
        for (auto& c : cs)
            if (!c.ok) return c;
        return game::Ok();
    });
}
