// EA RealFile "FILESYS" front end (NA v1.2, 0x6F7745-0x6F87EA): each asynchronous call allocates a 0x40-byte
// FILEOPERATION (0x6F70AC), constructs the operation type on it (thiscall constructor that also queues it) and returns
// the operation id at +0x0C; the ...sync versions start the operation, FILESYS_waitop it and FILESYS_completeop it.
// Names: EA's, from the GameCube build (Speed.elf) via the RE kit. File handles are pointers; +0x0C = device.
// Not here yet: FILESYS_write (0x6F782C), its sync version (0x6F876C) and 0x6F8815 - no safe write test yet.
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

constexpr int32_t kPrio = 0x64;
inline uint8_t* op_new() { return game_fn<uint8_t*(__cdecl*)(uint32_t)>(0x006F70AC)(0x40); }
inline int32_t op_id(const uint8_t* op) { return *reinterpret_cast<const int32_t*>(op + 0x0C); }  // (null: crash, like the original)
inline int32_t device_of(int32_t fh) { return *reinterpret_cast<int32_t*>(fh + 0x0C); }

using Ctor4_t = uint8_t*(__thiscall*)(uint8_t*, int32_t, int32_t, int32_t, int32_t);
using Ctor7_t = uint8_t*(__thiscall*)(uint8_t*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
using Ctor9_t = uint8_t*(__thiscall*)(uint8_t*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);

// ---------------------------------------------------------------- FILESYS_priorityop 0x006F7745
// Re-prioritise a still-queued operation: take it out of its device's queue, set the priority, queue it again.
using Prio_t = void(__cdecl*)(int32_t, int32_t);
Prio_t o_FILESYS_priorityop = nullptr;
void __cdecl FILESYS_priorityop(int32_t id, int32_t priority) {
    uint8_t* dev = game_fn<uint8_t*(__cdecl*)(int32_t)>(0x006F6C47)(id);  // FILEOPERATION::GetDevice
    if (!dev) return;
    game_fn<void(__thiscall*)(void*)>(0x006F55DD)(dev + 0x28);
    uint8_t* op = game_fn<uint8_t*(__thiscall*)(void*, int32_t, int32_t)>(0x006F734A)(dev + 0x10, id, 1);
    game_fn<void(__thiscall*)(void*)>(0x006F55E8)(dev + 0x28);
    if (!op) return;
    op[0x12] = static_cast<uint8_t>(priority);
    game_fn<void(__thiscall*)(uint8_t*)>(0x006F74BB)(op);  // FILEOPERATION::AddToQueue
}

// ---------------------------------------------------------------- asynchronous operations
using Close_t = int32_t(__cdecl*)(int32_t, int32_t, int32_t);
Close_t o_FILESYS_close = nullptr;
int32_t __cdecl FILESYS_close(int32_t fh, int32_t priority, int32_t user) {  // 0x006F7790
    uint8_t* op = op_new();
    if (op) op = game_fn<Ctor4_t>(0x006F82C4)(op, fh, priority, user, device_of(fh));
    return op_id(op);
}

using Read_t = int32_t(__cdecl*)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
Read_t o_FILESYS_read = nullptr;
int32_t __cdecl FILESYS_read(int32_t fh, int32_t offset, int32_t buffer, int32_t bytes, int32_t priority, int32_t user) {  // 0x006F77BC
    uint8_t* op = op_new();
    if (op) op = game_fn<Ctor7_t>(0x006F8309)(op, fh, bytes, offset, buffer, priority, user, device_of(fh));
    return op_id(op);
}

using ReadLarge_t = int32_t(__cdecl*)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
ReadLarge_t o_FILESYS_readlarge = nullptr;
// (fh, u64 offset, buffer, u64 bytes, priority, user), 64-bit values as lo/hi dwords
int32_t __cdecl FILESYS_readlarge(int32_t fh, int32_t off_lo, int32_t off_hi, int32_t buffer, int32_t bytes_lo,
                                  int32_t bytes_hi, int32_t priority, int32_t user) {  // 0x006F77F1
    uint8_t* op = op_new();
    if (op)
        op = game_fn<Ctor9_t>(0x006F8416)(op, fh, bytes_lo, bytes_hi, off_lo, off_hi, buffer, priority, user, device_of(fh));
    return op_id(op);
}

using Size_t = int32_t(__cdecl*)(int32_t, int32_t, int32_t);
Size_t o_FILESYS_size = nullptr;
int32_t __cdecl FILESYS_size(int32_t fh, int32_t priority, int32_t user) {  // 0x006F7861
    uint8_t* op = op_new();
    if (op) op = game_fn<Ctor4_t>(0x006F8600)(op, fh, priority, user, device_of(fh));
    return op_id(op);
}

using Exists_t = int32_t(__cdecl*)(const char*, int32_t, int32_t);
Exists_t o_FILESYS_exists = nullptr;
int32_t __cdecl FILESYS_exists(const char* name, int32_t priority, int32_t user) {  // 0x006F7B34
    uint8_t* op = op_new();
    if (op) {
        const int32_t dev = game_fn<int32_t(__cdecl*)(const char*)>(0x006F78DC)(name);  // FILE_nametodevice
        op = game_fn<uint8_t*(__thiscall*)(uint8_t*, const char*, int32_t, int32_t, int32_t)>(0x006F8188)(op, name, priority, user, dev);
    }
    return op_id(op);
}

// ---------------------------------------------------------------- FILESYS_completeop 0x006F76E9 (thunk)
using Complete_t = int32_t(__cdecl*)(int32_t);
Complete_t o_FILESYS_completeop = nullptr;
int32_t __cdecl FILESYS_completeop(int32_t id) { return game_fn<Complete_t>(0x006F767C)(id); }  // -> completeop64

// ---------------------------------------------------------------- synchronous versions: start, wait, complete
inline int32_t finish(int32_t id) {
    game_fn<int32_t(__cdecl*)(int32_t)>(0x006F75DD)(id);  // FILESYS_waitop
    return game_fn<Complete_t>(0x006F76E9)(id);
}

using OpenSync_t = int32_t(__cdecl*)(const char*, int32_t, int32_t);
OpenSync_t o_FILESYS_opensync = nullptr;
int32_t __cdecl FILESYS_opensync(const char* name, int32_t mode, int32_t priority) {  // 0x006F8710
    return finish(game_fn<int32_t(__cdecl*)(const char*, int32_t, int32_t, int32_t)>(0x006F7C1B)(name, mode, priority, 0));
}

using ReadSync_t = int32_t(__cdecl*)(int32_t, int32_t, int32_t, int32_t, int32_t);
ReadSync_t o_FILESYS_readsync = nullptr;
int32_t __cdecl FILESYS_readsync(int32_t fh, int32_t offset, int32_t buffer, int32_t bytes, int32_t priority) {  // 0x006F8738
    // through readlarge, offset and size sign-extended to 64 bits (cdq)
    return finish(game_fn<ReadLarge_t>(0x006F77F1)(fh, offset, offset >> 31, buffer, bytes, bytes >> 31, priority, 0));
}

using CloseSync_t = int32_t(__cdecl*)(int32_t, int32_t);
CloseSync_t o_FILESYS_closesync = nullptr;
int32_t __cdecl FILESYS_closesync(int32_t fh, int32_t priority) {  // 0x006F879A -> 0/1
    return finish(game_fn<Close_t>(0x006F7790)(fh, priority, 0)) != 0;
}

using SizeSync_t = int32_t(__cdecl*)(int32_t, int32_t);
SizeSync_t o_FILESYS_sizesync = nullptr;
int32_t __cdecl FILESYS_sizesync(int32_t fh, int32_t priority) {  // 0x006F87C5
    return finish(game_fn<Size_t>(0x006F7861)(fh, priority, 0));
}

using ExistsSync_t = int32_t(__cdecl*)(const char*, int32_t);
ExistsSync_t o_FILESYS_existssync = nullptr;
int32_t __cdecl FILESYS_existssync(const char* name, int32_t priority) {  // 0x006F87EA -> 0/1
    return finish(game_fn<Exists_t>(0x006F7B34)(name, priority, 0)) != 0;
}

}  // namespace

NFSU2_REWRITE_N(0x006F7745, FILESYS_priorityop, 2, "void __cdecl(int ophandle, int priority)", o_FILESYS_priorityop);
NFSU2_REWRITE_N(0x006F7790, FILESYS_close, 3, "int __cdecl(int filehandle, int priority, void* userdata)", o_FILESYS_close);
NFSU2_REWRITE_N(0x006F77BC, FILESYS_read, 6, "int __cdecl(int filehandle, int offset, void* buffer, int bytes, int priority, void* userdata)", o_FILESYS_read);
NFSU2_REWRITE_N(0x006F77F1, FILESYS_readlarge, 8, "int __cdecl(int filehandle, u64 offset, void* buffer, u64 bytes, int priority, void* userdata)", o_FILESYS_readlarge);
NFSU2_REWRITE_N(0x006F7861, FILESYS_size, 3, "int __cdecl(int filehandle, int priority, void* userdata)", o_FILESYS_size);
NFSU2_REWRITE_N(0x006F7B34, FILESYS_exists, 3, "int __cdecl(const char* name, int priority, void* userdata)", o_FILESYS_exists);
NFSU2_REWRITE_N(0x006F76E9, FILESYS_completeop, 1, "int __cdecl(int ophandle) -> FILESYS_completeop64", o_FILESYS_completeop);
NFSU2_REWRITE_N(0x006F8710, FILESYS_opensync, 3, "int __cdecl(const char* name, unsigned modeflags, int priority)", o_FILESYS_opensync);
NFSU2_REWRITE_N(0x006F8738, FILESYS_readsync, 5, "int __cdecl(int filehandle, int offset, void* buffer, int bytes, int priority)", o_FILESYS_readsync);
NFSU2_REWRITE_N(0x006F879A, FILESYS_closesync, 2, "bool32 __cdecl(int filehandle, int priority)", o_FILESYS_closesync);
NFSU2_REWRITE_N(0x006F87C5, FILESYS_sizesync, 2, "int __cdecl(int filehandle, int priority)", o_FILESYS_sizesync);
NFSU2_REWRITE_N(0x006F87EA, FILESYS_existssync, 2, "bool32 __cdecl(const char* name, int priority)", o_FILESYS_existssync);

// ============================================================================================ diff tests
// Real reads of the game's own files (read-only), once through the original front end and once through ours:
// opensync, sizesync, readsync / read+wait+complete / readlarge at random offsets, existssync (present + missing),
// priorityop on a finished op (no-op), closesync. Results and every byte read must match. Never opens for writing.
namespace {

struct FsApi {
    OpenSync_t opensync;
    SizeSync_t sizesync;
    ReadSync_t readsync;
    Read_t read;
    ReadLarge_t readlarge;
    Complete_t completeop;
    ExistsSync_t existssync;
    Prio_t priorityop;
    CloseSync_t closesync;
};
const FsApi kOrigFs() {
    return {o_FILESYS_opensync, o_FILESYS_sizesync, o_FILESYS_readsync, o_FILESYS_read, o_FILESYS_readlarge,
            o_FILESYS_completeop, o_FILESYS_existssync, o_FILESYS_priorityop, o_FILESYS_closesync};
}
const FsApi kOurFs() {
    return {&FILESYS_opensync, &FILESYS_sizesync, &FILESYS_readsync, &FILESYS_read, &FILESYS_readlarge,
            &FILESYS_completeop, &FILESYS_existssync, &FILESYS_priorityop, &FILESYS_closesync};
}

std::vector<int64_t> fs_run(const FsApi& f, const char* name, uint32_t seed, bool ours) {
    auto step = [ours](const char* a, const char* b) { game::g_test_step = ours ? b : a; };
    game::Rng r(seed);
    std::vector<int64_t> v;
    auto wait = game_fn<int32_t(__cdecl*)(int32_t)>(0x006F75DD);
    step("orig: existssync", "ours: existssync");
    v.push_back(f.existssync(name, kPrio));
    v.push_back(f.existssync("NOPE\\missing.bin", kPrio));
    step("orig: opensync", "ours: opensync");
    const int32_t h = f.opensync(name, 1, kPrio);
    v.push_back(h != 0);
    if (!h) return v;
    step("orig: sizesync", "ours: sizesync");
    const int32_t size = f.sizesync(h, kPrio);
    v.push_back(size);
    std::vector<uint8_t> buf(4096);
    for (int i = 0; i < 4; ++i) {
        const int32_t n = static_cast<int32_t>(r.below(4096));
        const int32_t off = size > 0 ? static_cast<int32_t>(r.below(static_cast<uint32_t>(size))) : 0;
        std::memset(buf.data(), 0xCD, buf.size());
        const uint32_t kind = r.below(3);
        int32_t got = 0;
        if (kind == 0) {
            step("orig: readsync", "ours: readsync");
            got = f.readsync(h, off, reinterpret_cast<int32_t>(buf.data()), n, kPrio);
        } else if (kind == 1) {
            step("orig: read", "ours: read");
            const int32_t id = f.read(h, off, reinterpret_cast<int32_t>(buf.data()), n, kPrio, 0);
            v.push_back(wait(id));
            f.priorityop(id, 0x50);  // already done: not in the queue, no effect
            got = f.completeop(id);
        } else {
            step("orig: readlarge", "ours: readlarge");
            const int32_t id = f.readlarge(h, off, 0, reinterpret_cast<int32_t>(buf.data()), n, 0, kPrio, 0);
            v.push_back(wait(id));
            got = f.completeop(id);
        }
        v.push_back(got);
        uint64_t sum = 0;
        for (size_t k = 0; k < buf.size(); ++k) sum = sum * 31 + buf[k];
        v.push_back(static_cast<int64_t>(sum));
    }
    step("orig: closesync", "ours: closesync");
    v.push_back(f.closesync(h, kPrio));
    return v;
}
}  // namespace

NFSU2_DIFF_TEST(FileSysFrontEnd) {
    static const char* const kNames[] = {"GLOBAL\\GLOBALB.BUN", "FRONTEND\\FrontB.lzc", "CARS\\TEXTURES.BIN",
                                         "SPEED2.EXE", "NOPE\\missing.bin", "TRACKS\\L4RA.BUN"};
    game::DiffCtx few = ctx;
    few.iterations = ctx.iterations > 150 ? 150 : ctx.iterations;  // real I/O: keep the frame short
    return game::run_diff(few, "FileSysFrontEnd", o_FILESYS_opensync, [](game::Rng& r) {
        const char* n = kNames[r.below(6)];
        const uint32_t seed = r.next();
        const auto a = fs_run(kOrigFs(), n, seed, false);
        const auto b = fs_run(kOurFs(), n, seed, true);
        game::g_test_step = "";
        if (a == b) return game::Ok();
        std::string s = n;
        for (size_t i = 0; i < a.size() || i < b.size(); ++i)
            if (i >= a.size() || i >= b.size() || a[i] != b[i]) {
                s += " first difference at value " + std::to_string(i) + ": " + std::to_string(i < a.size() ? a[i] : -99) +
                     "/" + std::to_string(i < b.size() ? b[i] : -99);
                break;
            }
        return game::MismatchText(s);
    });
}

NFSU2_DIFF_TEST(regs_filesys) {
    using game::arg;
    return game::run_diff(ctx, "regs_filesys", o_FILESYS_existssync, [](game::Rng& r) {
        const char* n = r.below(2) ? "GLOBAL\\GLOBALB.BUN" : "NOPE\\missing.bin";
        for (auto c : {game::check_regs("FILESYS_existssync", (void*)o_FILESYS_existssync, NFSU2_ENTRY(FILESYS_existssync),
                                        {arg(n), static_cast<uint32_t>(kPrio)}, false)})
            if (!c.ok) return c;
        return game::Ok();
    });
}
