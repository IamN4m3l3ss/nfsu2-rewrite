// bFile (NA v1.2): file objects on top of EA's asynchronous file library (0x6F7xxx, called by address), an open-file
// cache of up to 4 handles, and an archive index (empty on PC).
//
//   bFile (0xF0 bytes, slot pool at 0x865104: 0x40 slots, created on the first bOpen; lock 0x864EFC)
//     +00 mode  +04 size (-1 = not open)  +08 position  +0C handle  +10 OpenFileEntry* shared  +14 close-when-idle
//     +18 pending async ops  +1C char name[200]
//   OpenFileEntry (same slot pool), list 0x866584 (most recently used at the tail), count 0x865108 (max 4 kept)
//     +00 next  +04 prev  +08 refcount  +0C handle  +10 size  +14 char name[]
//   archive index: list 0x865218 of nodes {next, prev, ?, count, entries[count] of 0x14 bytes {hash, ?, ?, size, ?}}
//   file library: open 0x6F7C1B(name, mode, prio, 0) / stat 0x6F7C54(name, 1, &h, &size) / size 0x6F7861(h, prio, 0)
//     / close 0x6F7790(h, prio, 0) -> request; wait 0x6F75DD(req) -> status (1 = ok); result+release 0x6F76E9(req);
//     release with callback 0x6F76F2(req, cb); exists 0x6F8710(name, 1, prio) / close-sync 0x6F879A(h, prio)
//   modes seen: 1 read; 2 create (if it exists it is opened as mode 0); &6 = writing (flushes the cache first)
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

struct OpenFileEntry {
    OpenFileEntry* next;
    OpenFileEntry* prev;
    int32_t refcount;
    int32_t handle;
    int32_t size;
    char name[0xF0 - 0x14];
};
struct bFile {
    int32_t mode, size, pos, handle;
    OpenFileEntry* shared;
    int32_t close_when_idle, pending;
    char name[200];
    uint8_t pad[0xF0 - 0x1C - 200];
};
static_assert(sizeof(bFile) == 0xF0, "bFile size");
struct IndexEntry {
    uint32_t hash, unk4, unk8;
    int32_t size;
    uint32_t unk10;
};
struct IndexNode {
    IndexNode* next;
    IndexNode* prev;
    uint32_t unk8;
    int32_t count;
    IndexEntry entries[1];
};

constexpr uintptr_t kFilePool = 0x00865104, kCacheHead = 0x00866584, kCacheCount = 0x00865108, kIndexHead = 0x00865218;
constexpr int32_t kPrio = 0x64;
constexpr uintptr_t kCloseCallback = 0x005755D0;

inline OpenFileEntry* cache_head() { return reinterpret_cast<OpenFileEntry*>(kCacheHead); }
inline void* slot_alloc(uintptr_t pool) { return game_fn<void*(__cdecl*)(void*)>(0x00440E50)(game_var<void*>(pool)); }
inline void slot_free(uintptr_t pool, void* p) { game_fn<void(__cdecl*)(void*, void*)>(0x0043F920)(game_var<void*>(pool), p); }
inline void tick() { game_fn<void(__cdecl*)()>(0x0043BDF0)(); }
inline int bstrcmp(const char* a, const char* b) { return game_fn<int(__cdecl*)(const char*, const char*)>(0x0043DC30)(a, b); }
inline int bstrlen(const char* s) { return game_fn<int(__cdecl*)(const char*)>(0x0043DBB0)(s); }
inline void bstrcpy(char* d, const char* s) { game_fn<char*(__cdecl*)(char*, const char*)>(0x0043DBD0)(d, s); }

// file library requests
inline void* req_open(const char* name, int32_t mode) { return game_fn<void*(__cdecl*)(const char*, int32_t, int32_t, int32_t)>(0x006F7C1B)(name, mode, kPrio, 0); }
inline void* req_size(int32_t h) { return game_fn<void*(__cdecl*)(int32_t, int32_t, int32_t)>(0x006F7861)(h, kPrio, 0); }
inline void* req_close(int32_t h) { return game_fn<void*(__cdecl*)(int32_t, int32_t, int32_t)>(0x006F7790)(h, kPrio, 0); }
inline int32_t req_wait(void* r) { return game_fn<int32_t(__cdecl*)(void*)>(0x006F75DD)(r); }
inline int32_t req_result(void* r) { return game_fn<int32_t(__cdecl*)(void*)>(0x006F76E9)(r); }
inline void req_release_cb(void* r, uintptr_t cb) { game_fn<void(__cdecl*)(void*, uintptr_t)>(0x006F76F2)(r, cb); }
inline void close_handle_async(int32_t h) { req_release_cb(req_close(h), kCloseCallback); }

template <class T> inline void unlink(T* n) {
    n->prev->next = n->next;
    n->next->prev = n->prev;
}
inline void append(OpenFileEntry* e) {
    OpenFileEntry* const head = cache_head();
    OpenFileEntry* tail = head->prev;
    tail->next = e;
    head->prev = e;
    e->prev = tail;
    e->next = head;
}

// ---------------------------------------------------------------- bFileNameHash 0x00575330
// bStringHash of the name upper-cased (a-z only) with '/' -> '\'.
using Hash_t = uint32_t(__cdecl*)(const char*);
Hash_t o_bFileNameHash = nullptr;
uint32_t __cdecl bFileNameHash(const char* name) {
    char buf[0xA0 + 0x200];  // the original has 0xA0 bytes and no bound check; longer names overflow its stack
    int i = 0;
    for (; i < bstrlen(name); ++i) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c &= 0x5F;
        if (c == '/') c = '\\';
        buf[i] = c;
    }
    buf[i] = 0;
    return game_fn<Hash_t>(0x0043DB50)(buf);
}

// ---------------------------------------------------------------- FindFileIndexEntry 0x00579870
using FindIdx_t = IndexEntry*(__cdecl*)(const char*);
FindIdx_t o_FindFileIndexEntry = nullptr;
IndexEntry* __cdecl FindFileIndexEntry(const char* name) {
    const uint32_t h = game_fn<Hash_t>(0x00575330)(name);
    IndexNode* const head = reinterpret_cast<IndexNode*>(kIndexHead);
    for (IndexNode* n = head->next; n != head; n = n->next)
        for (int32_t i = 0; i < n->count; ++i)
            if (n->entries[i].hash == h) return &n->entries[i];
    return nullptr;
}

// ---------------------------------------------------------------- FindOpenFile 0x005798F0 (moves the hit to the tail)
using FindOpen_t = OpenFileEntry*(__cdecl*)(const char*);
FindOpen_t o_FindOpenFile = nullptr;
OpenFileEntry* __cdecl FindOpenFile(const char* name) {
    OpenFileEntry* const head = cache_head();
    for (OpenFileEntry* e = head->next; e != head; e = e->next) {
        if (bstrcmp(e->name, name) == 0) {
            unlink(e);
            append(e);
            return e;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- OpenFileEntry::Close 0x005798C0
using EntryClose_t = void(__thiscall*)(OpenFileEntry*);
EntryClose_t o_OpenFileEntryClose = nullptr;
void __cdecl OpenFileEntryClose(OpenFileEntry* self) {
    --game_var<int32_t>(kCacheCount);
    close_handle_async(self->handle);
}

// ---------------------------------------------------------------- AddOpenFile 0x0057C4A0
// Keeps at most 4 entries: while full, the first idle (refcount 0) entry is closed and freed.
using AddOpen_t = OpenFileEntry*(__cdecl*)(const char*, int32_t, int32_t);
AddOpen_t o_AddOpenFile = nullptr;
OpenFileEntry* __cdecl AddOpenFile(const char* name, int32_t handle, int32_t size) {
    OpenFileEntry* const head = cache_head();
    for (bool again = true; again;) {
        again = false;
        if (game_var<int32_t>(kCacheCount) < 4) break;
        for (OpenFileEntry* e = head->next; e != head; e = e->next) {
            if (e->refcount == 0) {
                unlink(e);
                --game_var<int32_t>(kCacheCount);
                close_handle_async(e->handle);
                slot_free(kFilePool, e);
                again = true;
                break;
            }
        }
    }
    OpenFileEntry* e = static_cast<OpenFileEntry*>(slot_alloc(kFilePool));
    if (e) {
        ++game_var<int32_t>(kCacheCount);
        e->handle = handle;
        e->refcount = 0;
        e->size = size;
        bstrcpy(e->name, name);
    }
    append(e);  // (the original also links a failed allocation: null)
    return e;
}

// ---------------------------------------------------------------- FlushOpenFileCache 0x0057B5D0
using Flush_t = void(__cdecl*)(int32_t);
Flush_t o_FlushOpenFileCache = nullptr;
void __cdecl FlushOpenFileCache(int32_t) {
    OpenFileEntry* const head = cache_head();
    for (bool again = true; again;) {
        again = false;
        for (OpenFileEntry* e = head->next; e != head; e = e->next) {
            if (e->refcount == 0) {
                unlink(e);
                game_fn<EntryClose_t>(0x005798C0)(e);
                slot_free(kFilePool, e);
                again = true;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------- bFile::bFile 0x0057C5B0
using Ctor_t = bFile*(__thiscall*)(bFile*, const char*, int32_t);
Ctor_t o_bFileCtor = nullptr;
bFile* __cdecl bFileCtor(bFile* self, const char* name, int32_t mode) {
    bstrcpy(self->name, name);
    self->mode = mode;
    self->size = -1;
    self->pos = 0;
    self->handle = 0;
    self->close_when_idle = 0;
    self->pending = 0;
    if (mode & 6) game_fn<Flush_t>(0x0057B5D0)(0);
    if (mode == 2) {  // create: if it already exists, open it as mode 0
        if (int32_t h = game_fn<int32_t(__cdecl*)(const char*, int32_t, int32_t)>(0x006F8710)(name, 1, kPrio)) {
            mode = 0;
            game_fn<void(__cdecl*)(int32_t, int32_t)>(0x006F879A)(h, kPrio);
        }
    }
    OpenFileEntry* e = game_fn<FindOpen_t>(0x005798F0)(name);
    if (e && mode == 1) {
        ++e->refcount;
        self->handle = e->handle;
        self->size = e->size;
        self->shared = e;
    } else if (IndexEntry* ie = game_fn<FindIdx_t>(0x00579870)(self->name)) {
        self->size = ie->size;
    } else {
        int32_t status = 0, result = 0;
        bool opened = false;
        if (mode == 1) {
            int32_t h_out = 0, size_out = 0;
            if (static_cast<uint8_t>(game_fn<int32_t(__cdecl*)(const char*, int32_t, int32_t*, int32_t*)>(0x006F7C54)(name, 1, &h_out, &size_out)))
                self->size = size_out;
            const char drive = game_var<char>(0x0080057F);
            if (self->size < 0 && drive) {  // not found: try the CD drive
                char prefix[4] = {drive, ':', '\\', 0};
                char path[0xD8];
                game_fn<char*(__cdecl*)(char*, const char*, const char*)>(0x0043DDA0)(path, prefix, name);  // bStrCat
                void* r = game_fn<void*(__cdecl*)(const char*, int32_t, int32_t, int32_t)>(0x006F7C1B)(path, 1, kPrio, 0);
                status = req_wait(r);
                result = req_result(r);
                opened = true;
            }
        } else {
            tick();
            void* r = req_open(name, mode);
            status = req_wait(r);
            result = req_result(r);
            opened = true;
        }
        if (opened && status == 1) self->handle = result;
        if (self->handle) {
            tick();
            void* r = req_size(self->handle);
            status = req_wait(r);
            result = req_result(r);
            if (status == 1) {
                self->size = result;
                if (mode == 1) {
                    OpenFileEntry* ne = game_fn<AddOpen_t>(0x0057C4A0)(name, self->handle, result);
                    ++ne->refcount;
                    self->shared = ne;
                }
            }
        }
    }
    if (self->size >= 0 && !(mode & 5)) self->pos = self->size;  // write modes start at the end
    return self;
}

// ---------------------------------------------------------------- bFile::Close 0x00579B90
using Close_t = bFile*(__thiscall*)(bFile*, int32_t);
Close_t o_bFileClose = nullptr;
bFile* __cdecl bFileClose(bFile* self, int32_t flags) {
    if (self->size >= 0) {
        if (self->shared)
            --self->shared->refcount;
        else if (self->handle)
            close_handle_async(self->handle);
        self->size = -1;
    }
    if (flags & 1) slot_free(kFilePool, self);
    return self;
}

// ---------------------------------------------------------------- bOpen 0x0057CA10
using Open_t = bFile*(__cdecl*)(const char*, int32_t, int32_t);
Open_t o_bOpen = nullptr;
bFile* __cdecl bOpen(const char* name, int32_t mode, int32_t) {
    if (bstrlen(name) >= 200) return nullptr;
    if (!game_var<void*>(kFilePool)) {
        game_fn<void(__thiscall*)(void*)>(0x006F55C7)(reinterpret_cast<void*>(0x00864EFC));  // InitializeCriticalSection
        game_var<void*>(kFilePool) = game_fn<void*(__cdecl*)(int32_t, int32_t, const char*, int32_t)>(0x00440DF0)(
            0xF0, 0x40, reinterpret_cast<const char*>(0x007A0468), 0);
    }
    bFile* f = static_cast<bFile*>(slot_alloc(kFilePool));
    if (f) f = game_fn<Ctor_t>(0x0057C5B0)(f, name, mode);
    if (f->size < 0) {  // (the original also dereferences a failed slot allocation)
        game_fn<Close_t>(0x00579B90)(f, 1);
        return nullptr;
    }
    return f;
}

// ---------------------------------------------------------------- bFileExists 0x0057CAC0 -> size + 1 (0 = missing)
using Exists_t = int32_t(__cdecl*)(const char*);
Exists_t o_bFileExists = nullptr;
int32_t __cdecl bFileExists(const char* name) {
    bFile* f = game_fn<Open_t>(0x0057CA10)(name, 1, 0);
    if (!f) return 0;
    const int32_t size = f->size;
    if (f->pending > 0)
        f->close_when_idle = 1;
    else
        game_fn<Close_t>(0x00579B90)(f, 1);
    return size + 1;
}

}  // namespace

NFSU2_REWRITE_N(0x00575330, bFileNameHash, 1, "u32 __cdecl(const char* name) upper-case, '/'->'\\', bStringHash", o_bFileNameHash);
NFSU2_REWRITE_N(0x00579870, FindFileIndexEntry, 1, "IndexEntry* __cdecl(const char* name)", o_FindFileIndexEntry);
NFSU2_REWRITE_N(0x005798F0, FindOpenFile, 1, "OpenFileEntry* __cdecl(const char* name) (moves hit to MRU)", o_FindOpenFile);
NFSU2_REWRITE_THIS(0x005798C0, OpenFileEntryClose, 0, "void OpenFileEntry::Close()", o_OpenFileEntryClose);
NFSU2_REWRITE_N(0x0057C4A0, AddOpenFile, 3, "OpenFileEntry* __cdecl(const char* name, int handle, int size)", o_AddOpenFile);
NFSU2_REWRITE_N(0x0057B5D0, FlushOpenFileCache, 1, "void __cdecl(int unused)", o_FlushOpenFileCache);
NFSU2_REWRITE_THIS(0x0057C5B0, bFileCtor, 2, "bFile* bFile::bFile(const char* name, int mode)", o_bFileCtor);
NFSU2_REWRITE_THIS(0x00579B90, bFileClose, 1, "bFile* bFile::Close(int flags) (&1: free)", o_bFileClose);
NFSU2_REWRITE_N(0x0057CA10, bOpen, 3, "bFile* __cdecl(const char* name, int mode, int)", o_bOpen);
NFSU2_REWRITE_N(0x0057CAC0, bFileExists, 1, "int __cdecl(const char* name) -> size+1, 0 if missing", o_bFileExists);

// ============================================================================================ diff tests
// No test opens a real OS handle or writes: read-mode opens only stat the file (no handle, no cache entry), cache
// hits use fake entries spliced into the cache list (refcount >= 1, never closed), the index path uses a fake index
// node. Everything spliced in is removed again before the case returns.
namespace {

std::vector<std::string> real_names() {
    std::vector<std::string> v = {"GLOBAL\\GLOBALB.BUN", "global/globalb.bun", "Global\\GlobalB.lzc", "FRONTEND\\FrontB.lzc",
                                  "TRACKS\\L4RA.BUN", "CARS\\TEXTURES.BIN", "SPEED2.EXE", "NOPE\\missing.bin", "", "\\"};
    struct RF {
        RF* next;
        RF* prev;
        uint8_t pad[0x14];
        char name[60];
    };
    RF* const head = reinterpret_cast<RF*>(0x008651A4);
    for (RF* f = head->prev; f != head && v.size() < 60; f = f->prev) v.emplace_back(f->name);
    return v;
}
std::string mutate_name(game::Rng& r, std::string s) {
    if (s.empty()) return s;
    const uint32_t k = r.below(4);
    const uint32_t i = r.below(static_cast<uint32_t>(s.size()));
    if (k == 0 && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) s[i] ^= 0x20;
    if (k == 1 && s[i] == '\\') s[i] = '/';
    if (k == 2) s += static_cast<char>('A' + r.below(26));
    return s;
}

struct FakeCache {  // fake OpenFileEntries appended to the cache list (fake handles: never closed)
    std::vector<OpenFileEntry> entries;
    game::ScopedUndo undo;  // restores the list as it was (normally empty)
    explicit FakeCache(const std::vector<std::string>& names)
        : entries(names.size()),
          undo([head = cache_head(), n = cache_head()->next, p = cache_head()->prev] {
              head->next = n;
              head->prev = p;
              if (p != head) p->next = head;
              if (n != head) n->prev = head;
          }) {
        for (size_t i = 0; i < names.size(); ++i) {
            OpenFileEntry& e = entries[i];
            std::memset(&e, 0, sizeof e);
            e.refcount = 1 + static_cast<int32_t>(i % 3);
            e.handle = 0x7EAD0000 + static_cast<int32_t>(i);
            e.size = 1000 + static_cast<int32_t>(i) * 77;
            std::strncpy(e.name, names[i].c_str(), sizeof e.name - 1);
            append(&e);
        }
    }

    // order of the fake entries in the list (they move to the tail on a hit)
    std::vector<int> order() const {
        std::vector<int> v;
        for (OpenFileEntry* e = cache_head()->next; e != cache_head(); e = e->next)
            v.push_back(e >= entries.data() && e < entries.data() + entries.size() ? static_cast<int>(e - entries.data()) : -1);
        return v;
    }
};

struct FakeIndex {  // one fake index node with entries for the given names
    std::vector<uint8_t> mem;
    IndexNode* node;
    game::ScopedUndo undo;
    explicit FakeIndex(const std::vector<std::string>& names)
        : mem(0x10 + 0x14 * (names.size() + 1)),
          undo([head = reinterpret_cast<IndexNode*>(kIndexHead), n = reinterpret_cast<IndexNode*>(kIndexHead)->next] {
              head->next = n;
              n->prev = head;
          }) {
        node = reinterpret_cast<IndexNode*>(mem.data());
        node->count = static_cast<int32_t>(names.size());
        for (size_t i = 0; i < names.size(); ++i) {
            node->entries[i] = {o_bFileNameHash(names[i].c_str()), 0, 0, 5000 + static_cast<int32_t>(i) * 13, 0};
        }
        IndexNode* const head = reinterpret_cast<IndexNode*>(kIndexHead);
        node->next = head->next;
        node->prev = head;
        head->next->prev = node;
        head->next = node;
    }

};

std::vector<uint64_t> file_state(const bFile* f, const FakeCache& fc) {
    if (!f) return {0xDEAD};
    auto idx = [&fc](const OpenFileEntry* e) -> uint64_t {
        if (e >= fc.entries.data() && e < fc.entries.data() + fc.entries.size()) return 1 + (e - fc.entries.data());
        return e ? 0xFFFF : 0;
    };
    std::vector<uint64_t> v = {static_cast<uint32_t>(f->mode), static_cast<uint32_t>(f->size), static_cast<uint32_t>(f->pos),
                               static_cast<uint32_t>(f->handle), idx(f->shared), static_cast<uint32_t>(f->close_when_idle),
                               static_cast<uint32_t>(f->pending)};
    for (const char* p = f->name; *p; ++p) v.push_back(static_cast<uint8_t>(*p));
    return v;
}
std::vector<uint64_t> cache_state(const FakeCache& fc) {
    std::vector<uint64_t> v;
    for (auto& e : fc.entries) v.push_back(static_cast<uint32_t>(e.refcount));
    for (int i : fc.order()) v.push_back(static_cast<uint32_t>(i));
    v.push_back(static_cast<uint32_t>(game_var<int32_t>(kCacheCount)));
    return v;
}

}  // namespace

NFSU2_DIFF_TEST(bFileNameHash) {
    const auto names = real_names();
    return game::run_diff(ctx, "bFileNameHash", o_bFileNameHash, [&names](game::Rng& r) {
        std::string s;
        if (r.below(2)) {
            s = mutate_name(r, names[r.below(static_cast<uint32_t>(names.size()))]);
        } else {
            const uint32_t n = r.below(150);
            for (uint32_t i = 0; i < n; ++i) s += static_cast<char>(1 + r.below(255));
        }
        const uint32_t a = o_bFileNameHash(s.c_str()), b = bFileNameHash(s.c_str());
        return a == b ? game::Ok() : game::MismatchText("name of length " + std::to_string(s.size()));
    });
}

NFSU2_DIFF_TEST(FindFileIndexEntry) {
    const auto names = real_names();
    return game::run_diff(ctx, "FindFileIndexEntry", o_FindFileIndexEntry, [&names](game::Rng& r) {
        std::vector<std::string> in;
        for (uint32_t i = 0, n = r.below(6); i < n; ++i) in.push_back(mutate_name(r, names[r.below(static_cast<uint32_t>(names.size()))]));
        FakeIndex fi(in);
        const std::string q = r.below(2) && !in.empty() ? mutate_name(r, in[r.below(static_cast<uint32_t>(in.size()))])
                                                        : names[r.below(static_cast<uint32_t>(names.size()))];
        return o_FindFileIndexEntry(q.c_str()) == FindFileIndexEntry(q.c_str()) ? game::Ok() : game::MismatchText(q);
    });
}

NFSU2_DIFF_TEST(FindOpenFile) {  // return value + list order afterwards (hit moves to the tail)
    const auto names = real_names();
    return game::run_diff(ctx, "FindOpenFile", o_FindOpenFile, [&names](game::Rng& r) {
        std::vector<std::string> in;
        for (uint32_t i = 0, n = r.below(6); i < n; ++i) in.push_back(names[r.below(static_cast<uint32_t>(names.size()))]);
        const std::string q = r.below(3) && !in.empty() ? in[r.below(static_cast<uint32_t>(in.size()))] : mutate_name(r, names[r.below(static_cast<uint32_t>(names.size()))]);
        std::vector<uint64_t> sa, sb;
        {
            FakeCache fc(in);
            OpenFileEntry* e = o_FindOpenFile(q.c_str());
            sa = cache_state(fc);
            sa.push_back(e ? (e >= fc.entries.data() && e < fc.entries.data() + fc.entries.size() ? 1 + (e - fc.entries.data()) : 0xFFFF) : 0);
        }
        {
            FakeCache fc(in);
            OpenFileEntry* e = FindOpenFile(q.c_str());
            sb = cache_state(fc);
            sb.push_back(e ? (e >= fc.entries.data() && e < fc.entries.data() + fc.entries.size() ? 1 + (e - fc.entries.data()) : 0xFFFF) : 0);
        }
        return sa == sb ? game::Ok() : game::MismatchText(q);
    });
}

// AddOpenFile while the cache has room (no eviction: that would close a real handle); the new entry is removed again
NFSU2_DIFF_TEST(AddOpenFile) {
    const auto names = real_names();
    return game::run_diff(ctx, "AddOpenFile", o_AddOpenFile, [&names](game::Rng& r) -> game::Case {
        if (game_var<int32_t>(kCacheCount) != 0) return game::Ok();  // only from the normal (empty) state
        const std::string n = names[r.below(static_cast<uint32_t>(names.size()))];
        const int32_t h = 0x7EAD1000 + static_cast<int32_t>(r.below(100)), sz = static_cast<int32_t>(r.next() >> 1);
        auto run = [&](AddOpen_t fn) {
            std::vector<uint64_t> v;
            OpenFileEntry* e = fn(n.c_str(), h, sz);
            v = {static_cast<uint32_t>(game_var<int32_t>(kCacheCount)), static_cast<uint32_t>(e->refcount),
                 static_cast<uint32_t>(e->handle), static_cast<uint32_t>(e->size), e->next == cache_head(),
                 cache_head()->prev == e, static_cast<uint64_t>(std::strcmp(e->name, n.c_str()) == 0)};
            unlink(e);  // undo without closing the fake handle
            --game_var<int32_t>(kCacheCount);
            slot_free(kFilePool, e);
            return v;
        };
        return run(o_AddOpenFile) == run(&AddOpenFile) ? game::Ok() : game::MismatchText(n);
    });
}

// bOpen / bFile::bFile / bFile::Close / bFileExists, read mode: real files (stat only), missing files, long names,
// fake cache hits (shared path) and fake index entries
NFSU2_DIFF_TEST(bOpen) {
    const auto names = real_names();
    return game::run_diff(ctx, "bOpen", o_bOpen, [&names](game::Rng& r) -> game::Case {
        std::vector<std::string> cached, indexed;
        for (uint32_t i = 0, n = r.below(4); i < n; ++i) cached.push_back(names[r.below(static_cast<uint32_t>(names.size()))]);
        for (uint32_t i = 0, n = r.below(3); i < n; ++i) indexed.push_back(names[r.below(static_cast<uint32_t>(names.size()))]);
        std::string q = mutate_name(r, names[r.below(static_cast<uint32_t>(names.size()))]);
        // >= 200 chars: rejected by bOpen. (160..199 would overflow the ORIGINAL's 0xA0-byte stack buffer in
        // bFileNameHash via the index lookup - a real bug in the game, never triggered by its own file names.)
        if (r.below(15) == 0) q = std::string(200 + r.below(20), 'A');
        const uint32_t api = r.below(3);  // 0 bOpen+Close, 1 bFileExists, 2 ctor on a slot + Close
        auto run = [&](Open_t open, Exists_t exists, Ctor_t ctor, Close_t close) {
            FakeIndex fi(indexed);
            FakeCache fc(cached);
            std::vector<uint64_t> v;
            if (api == 1) {
                v.push_back(static_cast<uint32_t>(exists(q.c_str())));
            } else if (api == 0) {
                bFile* f = open(q.c_str(), 1, 0);
                v = file_state(f, fc);
                if (f) close(f, 1);
            } else {
                if (q.size() >= 200) q.resize(150);
                bFile* f = static_cast<bFile*>(slot_alloc(kFilePool));
                f->shared = nullptr;  // (the constructor leaves +0x10 alone on paths that don't share)
                ctor(f, q.c_str(), 1);
                v = file_state(f, fc);
                close(f, 1);
            }
            auto c = cache_state(fc);
            v.insert(v.end(), c.begin(), c.end());
            return v;
        };
        const auto a = run(o_bOpen, o_bFileExists, o_bFileCtor, o_bFileClose);
        const auto b = run(&bOpen, &bFileExists, [](bFile* f, const char* n, int32_t m) { return bFileCtor(f, n, m); },
                           [](bFile* f, int32_t fl) { return bFileClose(f, fl); });
        return a == b ? game::Ok() : game::MismatchText("api " + std::to_string(api) + " " + q);
    });
}

NFSU2_DIFF_TEST(regs_bfile) {
    using game::arg;
    return game::run_diff(ctx, "regs_bfile", o_bFileNameHash, [](game::Rng& r) -> game::Case {
        const char* n = r.below(2) ? "GLOBAL\\GLOBALB.BUN" : "NOPE\\missing.bin";
        bFile* a = static_cast<bFile*>(slot_alloc(kFilePool));
        bFile* b = static_cast<bFile*>(slot_alloc(kFilePool));
        a->shared = b->shared = nullptr;
        game::Case cs[] = {
            game::check_regs("bFileNameHash", (void*)o_bFileNameHash, NFSU2_ENTRY(bFileNameHash), {arg(n)}, false),
            game::check_regs("FindFileIndexEntry", (void*)o_FindFileIndexEntry, NFSU2_ENTRY(FindFileIndexEntry), {arg(n)}, false),
            game::check_regs("FindOpenFile", (void*)o_FindOpenFile, NFSU2_ENTRY(FindOpenFile), {arg(n)}, false),
            game::check_regs("bFileExists", (void*)o_bFileExists, NFSU2_ENTRY(bFileExists), {arg(n)}, false),
            game::check_regs_this("bFile::bFile", (void*)o_bFileCtor, NFSU2_ENTRY(bFileCtor), a, b, {arg(n), 1}, false),  // EAX = this (differs)
            game::check_regs_this("bFile::Close", (void*)o_bFileClose, NFSU2_ENTRY(bFileClose), a, b, {0}, false),
        };
        slot_free(kFilePool, a);
        slot_free(kFilePool, b);
        for (auto& c : cs)
            if (!c.ok) return c;
        return game::Ok();
    });
}
