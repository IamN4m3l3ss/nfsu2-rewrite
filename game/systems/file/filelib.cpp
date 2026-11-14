// EA asynchronous file library inside SPEED2.EXE (0x6F5xxx-0x6F8xxx; EBP frames, some register arguments).
//
//   FileSys (global, *0x8AD610): +0C lock wrapper, +98 Channel* channels (0x84 bytes each)
//   request handle: low byte = channel index
//   Channel: +04 active  +0C Request* current  +10 RequestList queued  +1C RequestList done  +28 lock wrapper
//            +78 event (signalled when a request completes)
//   RequestList {Request* head; Request* tail; int count}; requests are linked through Request+08, which points at
//            the next request's +08 field (so next = link - 8)
//   Request: vtable, ..., +08 link, +0C handle, +10 u8 state (1 = still running), +11 s8 status, +12 u8 priority,
//            +18 user, +1C completion callback
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "diff.h"
#include "regs.h"
#include "rewrite.h"

namespace {

struct Request {
    void* vtable;
    uint32_t unk4;
    uint8_t* link;  // -> next request's link field
    int32_t handle;
    uint8_t state;
    int8_t status;
    uint8_t priority;
    uint8_t pad13;
    uint32_t unk14, user;
    void* callback;
    uint8_t rest[0x20];
};
struct RequestList {
    Request* head;
    Request* tail;
    int32_t count;
};
struct Channel {
    uint32_t unk0;
    int32_t active;
    uint32_t unk8;
    Request* current;
    RequestList queued;
    RequestList done;
    uint8_t lock[0x84 - 0x28];
};
static_assert(sizeof(Channel) == 0x84, "Channel size");

inline Request* next_of(const Request* r) { return r->link ? reinterpret_cast<Request*>(r->link - 8) : nullptr; }
inline void lock(void* wrapper) { game_fn<void(__thiscall*)(void*)>(0x006F55DD)(wrapper); }
inline void unlock(void* wrapper) { game_fn<void(__thiscall*)(void*)>(0x006F55E8)(wrapper); }

// ---------------------------------------------------------------- ChannelFromHandle 0x006F6C47
using Chan_t = Channel*(__cdecl*)(int32_t);
Chan_t o_ChannelFromHandle = nullptr;
Channel* __cdecl ChannelFromHandle(int32_t handle) {
    uint8_t* fs = game_var<uint8_t*>(0x008AD610);
    return reinterpret_cast<Channel*>(*reinterpret_cast<uint8_t**>(fs + 0x98) + (handle & 0xFF) * 0x84);
}

// ---------------------------------------------------------------- RequestList::Find 0x006F734A
using ListFind_t = Request*(__thiscall*)(RequestList*, int32_t, int32_t);
ListFind_t o_RequestListFind = nullptr;
Request* __cdecl RequestListFind(RequestList* self, int32_t handle, int32_t remove) {
    Request* prev = nullptr;
    for (Request* r = self->head; r; prev = r, r = next_of(r)) {
        if (r->handle == handle) {
            if (static_cast<uint8_t>(remove))
                game_fn<void(__thiscall*)(RequestList*, Request*, Request*)>(0x006F7FA4)(self, r, prev);
            return r;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- FindRequest 0x006F7473 (ESI = channel, EBX = &state)
// state: -1 = the channel's current request, 1 = queued, 0 = done list (or not found / channel inactive)
Request* __cdecl FindRequest(Channel* ch, int32_t* state, int32_t handle) {
    *state = 0;
    if (!ch->active) return nullptr;
    if (ch->current && ch->current->handle == handle) {
        *state = -1;
        return ch->current;
    }
    if (Request* r = game_fn<ListFind_t>(0x006F734A)(&ch->queued, handle, 0)) {
        *state = 1;
        return r;
    }
    return game_fn<ListFind_t>(0x006F734A)(&ch->done, handle, 0);
}
void* o_FindRequest = nullptr;
void* const g_impl_FindRequest = reinterpret_cast<void*>(&FindRequest);
volatile uint32_t g_calls_FindRequest = 0;
extern "C" __declspec(naked) void nfsu2_entry_FindRequest() {
    __asm {
        lock inc dword ptr [g_calls_FindRequest]
        lock inc dword ptr [nfsu2_inflight]
        push ecx
        push edx
        push dword ptr [esp + 12]  // handle
        push ebx                   // &state
        push esi                   // channel
        call dword ptr [g_impl_FindRequest]
        add esp, 12
        pop edx
        pop ecx
        lock dec dword ptr [nfsu2_inflight]
        ret
    }
}
// call a FindRequest-convention function: ESI = channel, EBX = &state, one stack arg (caller pops)
Request* call_find(const void* fn, Channel* chan, int32_t* state, int32_t handle) {
    Request* r = nullptr;
    __asm {
        mov esi, chan
        mov ebx, state
        push handle
        call fn
        add esp, 4
        mov r, eax
    }
    return r;
}

// ---------------------------------------------------------------- GetStatus 0x006F7542
// 0 = still pending (running or queued), -1 = done but flagged running, else the request's status; -3 = unknown
using Status_t = int32_t(__cdecl*)(int32_t);
Status_t o_GetStatus = nullptr;
int32_t __cdecl GetStatus(int32_t handle) {
    Channel* ch = game_fn<Chan_t>(0x006F6C47)(handle);
    lock(ch->lock);
    int32_t state = 0;
    Request* r = call_find(reinterpret_cast<const void*>(0x006F7473), ch, &state, handle);
    int32_t res = 0;
    if (state == 0) res = !r ? -3 : (r->state == 1 ? -1 : r->status);
    unlock(ch->lock);
    return res;
}

// ---------------------------------------------------------------- Wait 0x006F75DD
// Until the request has left current/queued: the library's own thread pumps its timers and sleeps 1 ms, any other
// thread waits on the channel's event. Then GetStatus. -3 for handle 0 or an inactive channel.
using Wait_t = int32_t(__cdecl*)(int32_t);
Wait_t o_Wait = nullptr;
int32_t __cdecl Wait(int32_t handle) {
    if (!handle) return -3;
    Channel* ch = game_fn<Chan_t>(0x006F6C47)(handle);
    if (!ch->active) return -3;
    for (;;) {
        lock(ch->lock);
        int32_t state = 0;
        Request* r = call_find(reinterpret_cast<const void*>(0x006F7473), ch, &state, handle);
        const int32_t pending = state & (r != nullptr ? 1 : 0);
        unlock(ch->lock);
        if (!pending) break;
        if (static_cast<uint8_t>(game_fn<int32_t(__cdecl*)(int32_t)>(0x006F58C1)(0))) {
            game_fn<void(__cdecl*)()>(0x006F56BD)();
            game_fn<void(__cdecl*)(int32_t)>(0x006F5889)(1);
        } else {
            HANDLE ev = *reinterpret_cast<HANDLE*>(reinterpret_cast<uint8_t*>(game_fn<Chan_t>(0x006F6C47)(handle)) + 0x78);
            WaitForSingleObject(ev, INFINITE);
            ResetEvent(ev);
        }
    }
    return game_fn<Status_t>(0x006F7542)(handle);
}

// ---------------------------------------------------------------- Result 0x006F767C (thunk 0x6F76E9)
// Removes the finished request from the done list, takes its result (virtual, 64-bit; callers read only EAX) and
// gives the request back to the library.
using Result_t = uint32_t(__cdecl*)(int32_t);
Result_t o_Result = nullptr;
uint32_t __cdecl Result(int32_t handle) {
    Channel* ch = game_fn<Chan_t>(0x006F6C47)(handle);
    lock(ch->lock);
    Request* r = game_fn<ListFind_t>(0x006F734A)(&ch->done, handle, 1);
    unlock(ch->lock);
    using Get_t = uint64_t(__thiscall*)(Request*);
    const uint64_t v = reinterpret_cast<const Get_t*>(r->vtable)[2](r);  // vtable slot 2 (+8)
    r->handle = 0;
    uint8_t* fs = game_var<uint8_t*>(0x008AD610);
    lock(fs + 0xC);
    game_fn<void(__thiscall*)(void*, Request*)>(0x006F7F14)(game_var<uint8_t*>(0x008AD610), r);
    unlock(fs + 0xC);
    return static_cast<uint32_t>(v);
}

// ---------------------------------------------------------------- ReleaseWithCallback 0x006F76F2
// Still pending: remember the callback (the library calls it on completion); done: call it now with
// (handle, status, user).
using Cb_t = void(__cdecl*)(int32_t, int32_t, uint32_t);
using Release_t = void(__cdecl*)(int32_t, Cb_t);
Release_t o_ReleaseWithCallback = nullptr;
void __cdecl ReleaseWithCallback(int32_t handle, Cb_t cb) {
    Channel* ch = game_fn<Chan_t>(0x006F6C47)(handle);
    lock(ch->lock);
    int32_t state = 0;
    Request* r = call_find(reinterpret_cast<const void*>(0x006F7473), ch, &state, handle);
    if (state)
        r->callback = reinterpret_cast<void*>(cb);
    else
        cb(r->handle, r->status, r->user);
    unlock(ch->lock);
}

}  // namespace

NFSU2_REWRITE_N(0x006F75DD, Wait, 1, "int __cdecl(int handle) block until done -> GetStatus", o_Wait);
NFSU2_REWRITE_N(0x006F767C, Result, 1, "u32 __cdecl(int handle) take result + free request (EAX of a 64-bit result)", o_Result);
NFSU2_REWRITE_N(0x006F76F2, ReleaseWithCallback, 2, "void __cdecl(int handle, cb(handle, status, user))", o_ReleaseWithCallback);

static ::game::Registrar g_reg_FindRequest(0x006F7473, "FindRequest",
                                           "Request* (ESI Channel*, EBX int* state, int handle) -1 running / 1 queued / 0 done",
                                           reinterpret_cast<void*>(&nfsu2_entry_FindRequest), &o_FindRequest, __FILE__,
                                           &g_calls_FindRequest);
NFSU2_REWRITE_N(0x006F6C47, ChannelFromHandle, 1, "Channel* __cdecl(int handle) channels[handle & 0xFF]", o_ChannelFromHandle);
NFSU2_REWRITE_THIS(0x006F734A, RequestListFind, 2, "Request* RequestList::Find(int handle, bool remove)", o_RequestListFind);
NFSU2_REWRITE_N(0x006F7542, GetStatus, 1, "int __cdecl(int handle) 0 pending / status / -3 unknown", o_GetStatus);

// ============================================================================================ diff tests
// Fake channels/lists/requests in our memory (the real channels are shared with the library's I/O thread);
// GetStatus only reads the real channels, under their own lock.
namespace {

struct FakeChannel {
    Channel ch{};
    std::vector<Request> reqs;
    // n requests, spread over current / queued / done; handles from a small set so lookups hit and miss
    FakeChannel(game::Rng& r, uint32_t n) : reqs(n) {
        ch.active = r.below(6) ? 1 : 0;
        RequestList* lists[2] = {&ch.queued, &ch.done};
        for (uint32_t i = 0; i < n; ++i) {
            Request& q = reqs[i];
            std::memset(&q, 0, sizeof q);
            q.handle = static_cast<int32_t>(r.below(12)) << 8;
            q.state = static_cast<uint8_t>(r.below(3));
            q.status = static_cast<int8_t>(r.below(5)) - 2;
            const uint32_t where = r.below(5);
            if (where == 0 && !ch.current) {
                ch.current = &q;
                continue;
            }
            RequestList* l = lists[where & 1];
            if (!l->head) l->head = &q;
            else l->tail->link = reinterpret_cast<uint8_t*>(&q) + 8;
            l->tail = &q;
            ++l->count;
        }
    }
    std::vector<uint64_t> shape() const {  // list contents as request indices
        std::vector<uint64_t> v;
        auto idx = [this](const Request* q) -> uint64_t { return q ? 1 + (q - reqs.data()) : 0; };
        for (const RequestList* l : {&ch.queued, &ch.done}) {
            for (const Request* q = l->head; q; q = next_of(q)) v.push_back(idx(q));
            v.push_back(0xFF00 | idx(l->tail));
            v.push_back(static_cast<uint32_t>(l->count));
        }
        return v;
    }
    uint64_t idx(const Request* q) const { return q ? 1 + (q - reqs.data()) : 0; }
};

}  // namespace

NFSU2_DIFF_TEST(RequestListFind) {
    return game::run_diff(ctx, "RequestListFind", o_RequestListFind, [](game::Rng& r) {
        const uint32_t seed = r.next(), n = r.below(12);
        const int32_t h = static_cast<int32_t>(r.below(14)) << 8, remove = r.below(2) ? 1 : 0;
        const bool which = r.below(2);
        game::Rng ra(seed), rb(seed);
        FakeChannel a(ra, n), b(rb, n);
        Request* x = o_RequestListFind(which ? &a.ch.done : &a.ch.queued, h, remove);
        Request* y = RequestListFind(which ? &b.ch.done : &b.ch.queued, h, remove);
        if (a.idx(x) != b.idx(y)) return game::MismatchText("result");
        return a.shape() == b.shape() ? game::Ok() : game::MismatchText("list after remove");
    });
}

NFSU2_DIFF_TEST(FindRequest) {  // through the original and our entry stub with the ESI/EBX convention
    return game::run_diff(ctx, "FindRequest", o_FindRequest, [](game::Rng& r) {
        const uint32_t seed = r.next(), n = r.below(12);
        const int32_t h = static_cast<int32_t>(r.below(14)) << 8;
        game::Rng ra(seed), rb(seed);
        FakeChannel a(ra, n), b(rb, n);
        int32_t sa = 0x55, sb = 0x55;
        Request* x = call_find(o_FindRequest, &a.ch, &sa, h);
        Request* y = call_find(reinterpret_cast<const void*>(&nfsu2_entry_FindRequest), &b.ch, &sb, h);
        if (a.idx(x) != b.idx(y) || sa != sb) return game::MismatchText("handle " + std::to_string(h));
        return game::Ok();
    });
}

NFSU2_DIFF_TEST(ChannelFromHandle) {
    return game::run_diff(ctx, "ChannelFromHandle", o_ChannelFromHandle, [](game::Rng& r) {
        const int32_t h = static_cast<int32_t>(r.next());
        return o_ChannelFromHandle(h) == ChannelFromHandle(h) ? game::Ok() : game::MismatchText(std::to_string(h));
    });
}

// Handles on ACTIVE channels only: an inactive channel's lock was never initialised (all zeros) and taking it blocks
// forever under Wine - a test using channel 7 hung the game's main thread. The game itself never holds such handles.
int32_t active_channel(game::Rng& r) {
    for (int tries = 0; tries < 64; ++tries) {
        const int32_t c = static_cast<int32_t>(r.below(8));
        if (ChannelFromHandle(c)->active) return c;
    }
    return 0;
}
NFSU2_DIFF_TEST(GetStatus) {  // real channels, random handles (read-only)
    return game::run_diff(ctx, "GetStatus", o_GetStatus, [](game::Rng& r) {
        const int32_t h = static_cast<int32_t>(r.below(64) << 8) | active_channel(r);
        const int32_t a = o_GetStatus(h), b = GetStatus(h);
        return a == b ? game::Ok() : game::MismatchText(std::to_string(h) + ": " + std::to_string(a) + "/" + std::to_string(b));
    });
}

NFSU2_DIFF_TEST(regs_filelib) {
    using game::arg;
    return game::run_diff(ctx, "regs_filelib", o_GetStatus, [](game::Rng& r) {
        const int32_t h = active_channel(r);
        game::Rng ra(r.next());
        FakeChannel a(ra, 4);
        game::Case cs[] = {
            game::check_regs("ChannelFromHandle", (void*)o_ChannelFromHandle, NFSU2_ENTRY(ChannelFromHandle), {static_cast<uint32_t>(h)}, false),
            game::check_regs("GetStatus", (void*)o_GetStatus, NFSU2_ENTRY(GetStatus), {static_cast<uint32_t>(h)}, false),
            game::check_regs_this("RequestList::Find", (void*)o_RequestListFind, NFSU2_ENTRY(RequestListFind), &a.ch.done, &a.ch.done,
                                  {static_cast<uint32_t>(h << 8), 0}),
        };
        for (auto& c : cs)
            if (!c.ok) return c;
        return game::Ok();
    });
}

// Real round trips through the library, read-only: open a game file (mode 1), wait, take the handle, ask for its
// size, wait, take it, close the handle (released with the game's own callback 0x5755D0). One sequence with the
// original Wait/Result/ReleaseWithCallback, one with ours; statuses, sizes and handle validity must match. Also
// ReleaseWithCallback on a finished size request with a recording callback (called immediately, on this thread).
namespace {
struct CbRecord {
    int32_t handle, status;
    uint32_t user;
    int calls;
};
CbRecord g_cb{};
void __cdecl record_cb(int32_t h, int32_t st, uint32_t user) { g_cb = {h, st, user, g_cb.calls + 1}; }

std::vector<int64_t> round_trip(const char* name, Wait_t wait, Result_t result, Release_t release, bool ours) {
    auto step = [ours](const char* a, const char* b) { game::g_test_step = ours ? b : a; };
    using Open_t = int32_t(__cdecl*)(const char*, int32_t, int32_t, int32_t);
    using Req3_t = int32_t(__cdecl*)(int32_t, int32_t, int32_t);
    std::vector<int64_t> v;
    step("orig: open", "ours: open");
    const int32_t ro = game_fn<Open_t>(0x006F7C1B)(name, 1, 0x64, 0);
    v.push_back(ro != 0);
    if (!ro) return v;  // no request (e.g. missing file): Result(0) would grab whatever request has handle 0
    step("orig: wait(open)", "ours: wait(open)");
    const int32_t st = wait(ro);
    v.push_back(st);
    step("orig: result(open)", "ours: result(open)");
    const int32_t h = static_cast<int32_t>(result(ro));  // (always taken: frees the request)
    if (st != 1) return v;  // like the game: a result is only meaningful with status 1 (a missing file's "handle"
                            // closed anyway raised STATUS_INVALID_HANDLE mid-library and took the game down)
    v.push_back(h != 0);
    step("orig: size req", "ours: size req");
    const int32_t rs = game_fn<Req3_t>(0x006F7861)(h, 0x64, 0);
    step("orig: wait(size)", "ours: wait(size)");
    const int32_t ss = wait(rs);
    v.push_back(ss);
    if (ss != 1) {
        result(rs);
        release(game_fn<Req3_t>(0x006F7790)(h, 0x64, 0), reinterpret_cast<Cb_t>(0x005755D0));
        return v;
    }
    g_cb = {};
    step("orig: release(size, record)", "ours: release(size, record)");
    release(rs, &record_cb);  // done -> called now; the callback args are compared, then the request is freed
    v.push_back(g_cb.calls);
    v.push_back(g_cb.status);
    v.push_back(g_cb.user);
    v.push_back(g_cb.handle == rs);
    step("orig: result(size)", "ours: result(size)");
    v.push_back(result(rs));  // the file size
    const int32_t rc = game_fn<Req3_t>(0x006F7790)(h, 0x64, 0);
    step("orig: release(close)", "ours: release(close)");
    release(rc, reinterpret_cast<Cb_t>(0x005755D0));
    return v;
}
}  // namespace

NFSU2_DIFF_TEST(FileLibRoundTrip) {
    static const char* const kNames[] = {"GLOBAL\\GLOBALB.BUN", "FRONTEND\\FrontB.lzc", "CARS\\TEXTURES.BIN", "SPEED2.EXE",
                                         "NOPE\\missing.bin", "TRACKS\\L4RA.BUN"};
    game::DiffCtx few = ctx;
    few.iterations = ctx.iterations > 200 ? 200 : ctx.iterations;  // real I/O: keep the frame short
    return game::run_diff(few, "FileLibRoundTrip", o_Wait, [](game::Rng& r) {
        const char* n = kNames[r.below(6)];
        const auto a = round_trip(n, o_Wait, o_Result, o_ReleaseWithCallback, false);
        const auto b = round_trip(n, &Wait, &Result, &ReleaseWithCallback, true);
        game::g_test_step = "";
        if (a == b) return game::Ok();
        std::string s = n;
        for (size_t i = 0; i < a.size() || i < b.size(); ++i)
            s += " " + std::to_string(i < a.size() ? a[i] : -99) + "/" + std::to_string(i < b.size() ? b[i] : -99);
        return game::MismatchText(s);
    });
}
