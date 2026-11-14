// DirectInput 8 hooks + keyboard injection.
// DirectInput8Create (MinHook; the game imports it from dinput8.dll = Ultimate ASI Loader, which forwards to the
// system DLL) -> IDirectInput8::CreateDevice (vtable slot 3) -> per-device vtable patches:
//   Acquire 7, GetDeviceState 9, GetDeviceData 10, SetDataFormat 11, SetCooperativeLevel 13
// (slots verified against SDK 10.0.26100 dinput.h).
// Injected keys are merged into GetDeviceState (256-byte DIK array) and delivered as synthesized events through
// GetDeviceData. When the real device can't deliver (window unfocused) and keep_active is on, the calls succeed
// with injected data only.
#define DIRECTINPUT_VERSION 0x0800
#include "probe.h"

#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <vector>
#include <mutex>
#include <stdexcept>

#include <intrin.h>

#include "MinHook.h"

namespace probe {
namespace {

constexpr int kSlotCreateDevice = 3;
constexpr int kSlotAcquire = 7, kSlotGetDeviceState = 9, kSlotGetDeviceData = 10, kSlotSetDataFormat = 11,
              kSlotSetCooperativeLevel = 13, kSlotBuildActionMap = 29, kSlotSetActionMap = 30;

using DI8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, LPUNKNOWN);
using Acquire_t = HRESULT(STDMETHODCALLTYPE*)(void*);
using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using SetDataFormat_t = HRESULT(STDMETHODCALLTYPE*)(void*, LPCDIDATAFORMAT);
using SetCoop_t = HRESULT(STDMETHODCALLTYPE*)(void*, HWND, DWORD);
using ActionMap_t = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIACTIONFORMATA, LPCSTR, DWORD);

DI8Create_t o_DI8Create = nullptr;

// Vtables are shared per class, but A/W interfaces (and different device classes) may have different vtables,
// so originals are stored per vtable.
struct DevVtbl {
    Acquire_t acquire = nullptr;
    GetDeviceState_t get_state = nullptr;
    GetDeviceData_t get_data = nullptr;
    SetDataFormat_t set_format = nullptr;
    SetCoop_t set_coop = nullptr;
    ActionMap_t build_map = nullptr;
    ActionMap_t set_map = nullptr;
};
std::mutex g_mu;  // guards everything below
std::map<void*, CreateDevice_t> g_di_create;  // IDirectInput8 vtable -> original CreateDevice
std::map<void*, DevVtbl> g_dev_vtbl;          // device vtable -> originals
enum class Kind { Keyboard, Mouse, Other };
struct DevInfo {
    Kind kind = Kind::Other;
    unsigned state_calls = 0, data_calls = 0;
    std::deque<DIDEVICEOBJECTDATA> pending;  // synthesized events not yet delivered
    DWORD seq = 0;
    // DirectInput 8 action mapping (SetActionMap): DIK -> game action ids (uAppData). The game reads only
    // dwData + uAppData from events (bit uAppData of a 64-bit "held" mask), see docs/structs/input.md.
    std::map<uint8_t, std::vector<UINT_PTR>> key_actions;
};
std::map<void*, DevInfo> g_devs;

struct KeyInject {
    bool down = false;
    ULONGLONG release_at = 0;  // 0 = hold until released
    unsigned polls = 0;        // keyboard reads since pressed
};
KeyInject g_keys[256];
std::atomic<unsigned> g_injected_down{0};

// Frame-scheduled input (drive scripts, replays). Applied at Present of frame base+f; the game reads the keyboard
// during the next frame. Guarded by g_mu.
struct SchedEvent {
    uint64_t f;
    bool is_action;  // false: DIK key, true: raw game action id (uAppData)
    uint32_t code;
    bool down;
};
std::vector<SchedEvent> g_sched;
size_t g_sched_next = 0;
uint64_t g_sched_base = 0;

// Recording of the real keyboard's action events (relative frame, uAppData, dwData).
struct RecEvent {
    uint64_t f;
    uint32_t action;
    uint32_t data;
};
bool g_rec_active = false;
uint64_t g_rec_base = 0;
std::vector<RecEvent> g_rec;

// Telemetry trace: car samples every N frames (relative to trace start).
bool g_trace_active = false;
uint64_t g_trace_base = 0, g_trace_every = 30;
std::vector<CarSample> g_trace;
std::vector<uintptr_t> g_trace_watch;  // extra addresses sampled with every trace sample (max 8)

const char* kind_name(Kind k) { return k == Kind::Keyboard ? "keyboard" : k == Kind::Mouse ? "mouse" : "other"; }

bool keep_active_on() { return keep_active_enabled(); }

void* vtbl_of(void* obj) { return *reinterpret_cast<void**>(obj); }

// Device record; devices we didn't see being created (some reach us only through the shared vtable) are
// classified on first use via IDirectInputDevice8::GetCapabilities (slot 3). Called with g_mu held.
DevInfo& dev_info_locked(void* dev) {
    auto it = g_devs.find(dev);
    if (it != g_devs.end()) return it->second;
    DevInfo& info = g_devs[dev];
    using GetCaps_t = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIDEVCAPS);
    auto get_caps = reinterpret_cast<GetCaps_t>((*reinterpret_cast<void***>(dev))[3]);
    DIDEVCAPS caps{};
    caps.dwSize = sizeof caps;
    HRESULT hr = get_caps(dev, &caps);
    if (SUCCEEDED(hr)) {
        BYTE type = GET_DIDEVICE_TYPE(caps.dwDevType);
        info.kind = type == DI8DEVTYPE_KEYBOARD ? Kind::Keyboard : type == DI8DEVTYPE_MOUSE ? Kind::Mouse : Kind::Other;
    }
    logf("new device %p: GetCapabilities hr=%08lx devType=%lx -> %s", dev, hr, caps.dwDevType, kind_name(info.kind));
    return info;
}

template <class F> F orig(void* dev, F DevVtbl::*member) {
    std::lock_guard lk(g_mu);
    return g_dev_vtbl[vtbl_of(dev)].*member;
}

// push an event to every keyboard's pending queue (called with g_mu held)
void push_key_event_locked(uint8_t dik, bool down) {
    for (auto& [dev, info] : g_devs) {
        if (info.kind != Kind::Keyboard) continue;
        auto it = info.key_actions.find(dik);
        if (it == info.key_actions.end()) continue;  // action-mapped devices only report mapped keys
        for (UINT_PTR action : it->second) {
            DIDEVICEOBJECTDATA ev{};
            ev.dwOfs = dik;
            ev.dwData = down ? 0x80 : 0;
            ev.dwTimeStamp = GetTickCount();
            ev.dwSequence = ++info.seq;
            ev.uAppData = action;
            info.pending.push_back(ev);
        }
    }
}

// push a raw action event (uAppData) to every keyboard (called with g_mu held); dwOfs = a key bound to it, if any
void push_action_event_locked(uint32_t action, bool down) {
    for (auto& [dev, info] : g_devs) {
        if (info.kind != Kind::Keyboard) continue;
        DIDEVICEOBJECTDATA ev{};
        for (auto& [dik, acts] : info.key_actions)
            for (UINT_PTR a : acts)
                if (a == action && !ev.dwOfs) ev.dwOfs = dik;
        ev.dwData = down ? 0x80 : 0;
        ev.dwTimeStamp = GetTickCount();
        ev.dwSequence = ++info.seq;
        ev.uAppData = action;
        info.pending.push_back(ev);
    }
}

void set_key_locked(uint8_t dik, bool down) {
    KeyInject& k = g_keys[dik];
    if (down && !k.down) {
        k = {true, 0, 0};
        ++g_injected_down;
        push_key_event_locked(dik, true);
    } else if (!down && k.down) {
        k = {};
        --g_injected_down;
        push_key_event_locked(dik, false);
    }
}

// release timed keys whose time is up and that were seen by at least 2 reads (called with g_mu held)
void tick_releases_locked() {
    ULONGLONG now = GetTickCount64();
    for (int k = 0; k < 256; ++k) {
        KeyInject& ki = g_keys[k];
        if (ki.down && ki.release_at && now >= ki.release_at && ki.polls >= 2) {
            ki = {};
            --g_injected_down;
            push_key_event_locked(static_cast<uint8_t>(k), false);
        }
    }
}

HRESULT STDMETHODCALLTYPE hk_GetDeviceState(void* dev, DWORD cb, LPVOID data) {
    HRESULT hr = orig(dev, &DevVtbl::get_state)(dev, cb, data);
    std::lock_guard lk(g_mu);
    DevInfo& info = dev_info_locked(dev);
    if (++info.state_calls <= 3)
        logf("GetDeviceState dev=%p (%s) cb=%lu hr=%08lx", dev, kind_name(info.kind), cb, hr);
    if (info.kind != Kind::Keyboard || cb != 256 || !data) return hr;
    if (FAILED(hr)) {
        if (!keep_active_on()) return hr;
        memset(data, 0, cb);  // real device unavailable (unfocused): report injected keys only
        hr = DI_OK;
    }
    auto* keys = static_cast<uint8_t*>(data);
    for (int k = 0; k < 256; ++k)
        if (g_keys[k].down) {
            keys[k] = 0x80;
            ++g_keys[k].polls;
        }
    tick_releases_locked();
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_GetDeviceData(void* dev, DWORD cbObj, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inout,
                                           DWORD flags) {
    DWORD cap = inout ? *inout : 0;
    auto get_data = orig(dev, &DevVtbl::get_data);
    HRESULT hr = get_data(dev, cbObj, rgdod, inout, flags);
    if ((hr == DIERR_NOTACQUIRED || hr == DIERR_INPUTLOST) && inout && keep_active_on()) {
        // The game answers these errors with Acquire + skip. Because we report success below (so injection works
        // while unfocused), it would never re-acquire, and the real keyboard would stay dead once the window has
        // focus again. So acquire here and retry: real keys pass through when focused, injected keys always.
        HRESULT ha = orig(dev, &DevVtbl::acquire)(dev);
        if (SUCCEEDED(ha)) {
            *inout = cap;
            hr = get_data(dev, cbObj, rgdod, inout, flags);
        }
    }
    std::lock_guard lk(g_mu);
    DevInfo& info = dev_info_locked(dev);
    if (++info.data_calls <= 3)
        logf("GetDeviceData dev=%p (%s) cbObj=%lu cap=%lu got=%lu flags=%lx hr=%08lx caller=%p", dev,
             kind_name(info.kind), cbObj, cap, inout ? *inout : 0, flags, hr, _ReturnAddress());
    if (info.kind != Kind::Keyboard || !inout) return hr;
    if (SUCCEEDED(hr) && g_rec_active && rgdod && !(flags & DIGDD_PEEK)) {
        const uint64_t f = current_frame() - g_rec_base;
        for (DWORD i = 0; i < *inout; ++i) {
            auto* e = reinterpret_cast<const DIDEVICEOBJECTDATA*>(reinterpret_cast<const uint8_t*>(rgdod) +
                                                                  static_cast<size_t>(i) * cbObj);
            g_rec.push_back({f, static_cast<uint32_t>(e->uAppData), e->dwData});
        }
    }
    if (FAILED(hr)) {
        if (!keep_active_on()) return hr;
        *inout = 0;
        hr = DI_OK;
    }
    // the game read the keyboard: count it as a poll for timed presses
    for (auto& k : g_keys)
        if (k.down) ++k.polls;
    tick_releases_locked();
    if (info.pending.empty()) return hr;

    const bool peek = (flags & DIGDD_PEEK) != 0;
    DWORD n = *inout;
    if (!rgdod) {  // caller asks how many / flushes
        *inout = n + static_cast<DWORD>(info.pending.size());
        if (!peek) info.pending.clear();
        return hr;
    }
    size_t take = 0;
    while (n < cap && take < info.pending.size() && cbObj >= sizeof(DIDEVICEOBJECTDATA_DX3)) {
        auto* dst = reinterpret_cast<uint8_t*>(rgdod) + static_cast<size_t>(n) * cbObj;
        memset(dst, 0, cbObj);
        memcpy(dst, &info.pending[take], std::min<size_t>(cbObj, sizeof(DIDEVICEOBJECTDATA)));
        if (g_rec_active && !peek)  // the recording holds everything the game read: real and injected events
            g_rec.push_back({current_frame() - g_rec_base, static_cast<uint32_t>(info.pending[take].uAppData),
                             info.pending[take].dwData});
        ++n;
        ++take;
    }
    if (!peek) info.pending.erase(info.pending.begin(), info.pending.begin() + static_cast<ptrdiff_t>(take));
    *inout = n;
    static unsigned delivered_logs = 0;
    if (take && ++delivered_logs <= 40) logf("GetDeviceData delivered %zu injected event(s) (first ofs=%lx data=%lx) hr=%08lx peek=%d caller=%p",
                   take, rgdod[0].dwOfs, rgdod[0].dwData, hr, peek, _ReturnAddress());
    return hr;
}

std::atomic<unsigned> g_acquire_logs{0};

HRESULT STDMETHODCALLTYPE hk_Acquire(void* dev) {
    HRESULT hr = orig(dev, &DevVtbl::acquire)(dev);
    if (++g_acquire_logs <= 20) logf("Acquire dev=%p hr=%08lx caller=%p", dev, hr, _ReturnAddress());
    // DIERR_OTHERAPPHASPRIO etc. when unfocused: harmless for injection, so report success under keep_active
    if (FAILED(hr) && keep_active_on()) {
        std::lock_guard lk(g_mu);
        if (dev_info_locked(dev).kind == Kind::Keyboard) return DI_OK;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_SetDataFormat(void* dev, LPCDIDATAFORMAT fmt) {
    HRESULT hr = orig(dev, &DevVtbl::set_format)(dev, fmt);
    logf("SetDataFormat dev=%p dataSize=%lu objs=%lu flags=%lx hr=%08lx", dev, fmt ? fmt->dwDataSize : 0,
         fmt ? fmt->dwNumObjs : 0, fmt ? fmt->dwFlags : 0, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_SetCoop(void* dev, HWND wnd, DWORD flags) {
    HRESULT hr = orig(dev, &DevVtbl::set_coop)(dev, wnd, flags);
    logf("SetCooperativeLevel dev=%p hwnd=%p flags=%lx (%s%s%s) hr=%08lx", dev, wnd, flags,
         (flags & DISCL_FOREGROUND) ? "FOREGROUND " : "", (flags & DISCL_BACKGROUND) ? "BACKGROUND " : "",
         (flags & DISCL_EXCLUSIVE) ? "EXCLUSIVE" : "NONEXCLUSIVE", hr);
    return hr;
}

void log_action_format(void* dev, const char* what, LPDIACTIONFORMATA f, DWORD flags, HRESULT hr) {
    if (!f) return;
    logf("%s dev=%p hr=%08lx flags=%lx actions=%lu actionSize=%lu genre=%08lx bufSize=%lu map='%s'", what, dev, hr,
         flags, f->dwNumActions, f->dwActionSize, f->dwGenre, f->dwBufferSize, f->tszActionMap);
}

HRESULT STDMETHODCALLTYPE hk_BuildActionMap(void* dev, LPDIACTIONFORMATA f, LPCSTR user, DWORD flags) {
    HRESULT hr = orig(dev, &DevVtbl::build_map)(dev, f, user, flags);
    log_action_format(dev, "BuildActionMap", f, flags, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_SetActionMap(void* dev, LPDIACTIONFORMATA f, LPCSTR user, DWORD flags) {
    HRESULT hr = orig(dev, &DevVtbl::set_map)(dev, f, user, flags);
    log_action_format(dev, "SetActionMap", f, flags, hr);
    if (FAILED(hr) || !f || !f->rgoAction || f->dwActionSize < sizeof(DIACTIONA)) return hr;
    std::lock_guard lk(g_mu);
    DevInfo& info = dev_info_locked(dev);
    if (info.kind != Kind::Keyboard) return hr;
    info.key_actions.clear();
    int mapped = 0;
    for (DWORD i = 0; i < f->dwNumActions; ++i) {
        auto* a = reinterpret_cast<const DIACTIONA*>(reinterpret_cast<const uint8_t*>(f->rgoAction) +
                                                     static_cast<size_t>(i) * f->dwActionSize);
        if (a->dwHow == DIAH_UNMAPPED || !IsEqualGUID(a->guidInstance, GUID_SysKeyboard)) continue;
        unsigned dik = DIDFT_GETINSTANCE(a->dwObjID);
        if (dik == 0 || dik > 255) continue;
        info.key_actions[static_cast<uint8_t>(dik)].push_back(a->uAppData);
        ++mapped;
        logf("  action uAppData=%u semantic=%08lx how=%lx key=0x%02x", static_cast<unsigned>(a->uAppData),
             a->dwSemantic, a->dwHow, dik);
    }
    logf("keyboard %p: %d key->action mappings", dev, mapped);
    return hr;
}

bool patch(void* obj, int slot, void* detour, void** original) {
    void** vt = *reinterpret_cast<void***>(obj);
    if (vt[slot] == detour) return true;
    *original = vt[slot];
    return mem_write(reinterpret_cast<uintptr_t>(&vt[slot]), &detour, sizeof detour);
}

HRESULT STDMETHODCALLTYPE hk_CreateDevice(void* di, REFGUID guid, void** out, LPUNKNOWN outer) {
    CreateDevice_t o;
    {
        std::lock_guard lk(g_mu);
        o = g_di_create[vtbl_of(di)];
    }
    HRESULT hr = o(di, guid, out, outer);
    Kind kind = IsEqualGUID(guid, GUID_SysKeyboard) ? Kind::Keyboard
                : IsEqualGUID(guid, GUID_SysMouse)  ? Kind::Mouse
                                                    : Kind::Other;
    logf("IDirectInput8::CreateDevice guid=%08lx-... kind=%s hr=%08lx dev=%p", guid.Data1, kind_name(kind), hr,
         (SUCCEEDED(hr) && out) ? *out : nullptr);
    if (FAILED(hr) || !out || !*out) return hr;
    void* dev = *out;
    std::lock_guard lk(g_mu);
    g_devs[dev].kind = kind;
    void* vt = vtbl_of(dev);
    if (!g_dev_vtbl.count(vt)) {
        DevVtbl& v = g_dev_vtbl[vt];
        bool ok = patch(dev, kSlotAcquire, reinterpret_cast<void*>(&hk_Acquire), reinterpret_cast<void**>(&v.acquire)) &&
                  patch(dev, kSlotGetDeviceState, reinterpret_cast<void*>(&hk_GetDeviceState),
                        reinterpret_cast<void**>(&v.get_state)) &&
                  patch(dev, kSlotGetDeviceData, reinterpret_cast<void*>(&hk_GetDeviceData),
                        reinterpret_cast<void**>(&v.get_data)) &&
                  patch(dev, kSlotSetDataFormat, reinterpret_cast<void*>(&hk_SetDataFormat),
                        reinterpret_cast<void**>(&v.set_format)) &&
                  patch(dev, kSlotSetCooperativeLevel, reinterpret_cast<void*>(&hk_SetCoop),
                        reinterpret_cast<void**>(&v.set_coop)) &&
                  patch(dev, kSlotBuildActionMap, reinterpret_cast<void*>(&hk_BuildActionMap),
                        reinterpret_cast<void**>(&v.build_map)) &&
                  patch(dev, kSlotSetActionMap, reinterpret_cast<void*>(&hk_SetActionMap),
                        reinterpret_cast<void**>(&v.set_map));
        logf("device vtable %p patched: %s", vt, ok ? "ok" : "FAILED");
    }
    return hr;
}

HRESULT WINAPI hk_DI8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    HRESULT hr = o_DI8Create(inst, ver, riid, out, outer);
    logf("DirectInput8Create ver=%lx iid=%s hr=%08lx obj=%p", ver,
         IsEqualIID(riid, IID_IDirectInput8A) ? "A" : IsEqualIID(riid, IID_IDirectInput8W) ? "W" : "?", hr,
         (SUCCEEDED(hr) && out) ? *out : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        std::lock_guard lk(g_mu);
        void* vt = vtbl_of(*out);
        if (!g_di_create.count(vt)) {
            CreateDevice_t o = nullptr;
            if (patch(*out, kSlotCreateDevice, reinterpret_cast<void*>(&hk_CreateDevice), reinterpret_cast<void**>(&o)))
                g_di_create[vt] = o;
            else
                logf("ERROR: patching IDirectInput8::CreateDevice failed");
        }
    }
    return hr;
}

}  // namespace

bool input_install_hooks() {
    MH_STATUS s = MH_CreateHookApi(L"dinput8.dll", "DirectInput8Create", reinterpret_cast<void*>(&hk_DI8Create),
                                   reinterpret_cast<void**>(&o_DI8Create));
    if (s == MH_OK) s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) {
        logf("ERROR: hook DirectInput8Create: %s", MH_StatusToString(s));
        return false;
    }
    logf("DirectInput8Create hooked");
    return true;
}

json input_key(const json& p) {
    // {"key": dik, "action": "press"|"hold"|"release", "ms": duration for press}
    int dik = p.at("key").get<int>();
    if (dik <= 0 || dik > 255) throw std::runtime_error("key must be a DIK code 1..255");
    std::string action = p.value("action", "press");
    std::lock_guard lk(g_mu);
    KeyInject& k = g_keys[dik];
    if (action == "release") {
        if (k.down) {
            k = {};
            --g_injected_down;
            push_key_event_locked(static_cast<uint8_t>(dik), false);
        }
    } else if (action == "press" || action == "hold") {
        if (!k.down) {
            ++g_injected_down;
            push_key_event_locked(static_cast<uint8_t>(dik), true);
        } else if (action == "press") {
            // A press while the previous press is still held must still be a new key-down for the game,
            // otherwise two quick Enters merge into one: release, then press again.
            push_key_event_locked(static_cast<uint8_t>(dik), false);
            push_key_event_locked(static_cast<uint8_t>(dik), true);
        }
        k.down = true;
        k.polls = 0;
        k.release_at = action == "press" ? GetTickCount64() + p.value("ms", 100u) : 0;
    } else {
        throw std::runtime_error("action must be press, hold or release");
    }
    return {{"key", dik}, {"action", action}};
}

json input_release_all() {
    std::lock_guard lk(g_mu);
    int n = 0;
    for (int k = 0; k < 256; ++k)
        if (g_keys[k].down) {
            g_keys[k] = {};
            push_key_event_locked(static_cast<uint8_t>(k), false);
            ++n;
        }
    g_injected_down = 0;
    return {{"released", n}};
}

json input_state() {
    std::lock_guard lk(g_mu);
    json devs = json::array();
    for (auto& [dev, info] : g_devs)
        devs.push_back({{"dev", hex_addr(reinterpret_cast<uintptr_t>(dev))}, {"kind", kind_name(info.kind)},
                        {"state_calls", info.state_calls}, {"data_calls", info.data_calls},
                        {"pending", info.pending.size()}, {"mapped_keys", info.key_actions.size()}});
    json down = json::array();
    for (int k = 0; k < 256; ++k)
        if (g_keys[k].down) down.push_back(k);
    return {{"devices", devs}, {"keys_down", down}};
}

void input_frame_tick(uint64_t frame) {
    std::lock_guard lk(g_mu);
    while (g_sched_next < g_sched.size() && g_sched_base + g_sched[g_sched_next].f <= frame) {
        const SchedEvent& e = g_sched[g_sched_next++];
        if (e.is_action)
            push_action_event_locked(e.code, e.down);
        else
            set_key_locked(static_cast<uint8_t>(e.code), e.down);
    }
    if (g_trace_active && frame >= g_trace_base && (frame - g_trace_base) % g_trace_every == 0) {
        CarSample cs;
        cs.frame = frame - g_trace_base;
        const bool ok = car_read(cs);
        cs.nwatch = static_cast<uint8_t>(std::min<size_t>(8, g_trace_watch.size()));
        for (uint8_t i = 0; i < cs.nwatch; ++i) mem_read(g_trace_watch[i], &cs.watch[i], 4);
        if (ok || cs.nwatch) g_trace.push_back(cs);
    }
}

json input_schedule(const json& p) {
    std::vector<SchedEvent> evs;
    for (const json& e : p.at("events")) {
        SchedEvent s{e.at("f").get<uint64_t>(), e.contains("action"), 0, e.at("down").get<bool>()};
        s.code = s.is_action ? e["action"].get<uint32_t>() : e.at("key").get<uint32_t>();
        if (!s.is_action && (s.code == 0 || s.code > 255)) throw std::runtime_error("key must be a DIK code 1..255");
        evs.push_back(s);
    }
    std::stable_sort(evs.begin(), evs.end(), [](const SchedEvent& a, const SchedEvent& b) { return a.f < b.f; });
    std::lock_guard lk(g_mu);
    g_sched = std::move(evs);
    g_sched_next = 0;
    g_sched_base = current_frame() + 1;
    if (p.contains("trace_every")) {  // start a trace on the same base frame, so samples line up with the script
        g_trace.clear();
        g_trace_every = std::max<uint64_t>(1, p["trace_every"].get<uint64_t>());
        g_trace_base = g_sched_base;
        g_trace_active = true;
    }
    logf("schedule: %zu events from frame %llu", g_sched.size(), static_cast<unsigned long long>(g_sched_base));
    return {{"events", g_sched.size()}, {"base_frame", g_sched_base}};
}

json input_schedule_status() {
    std::lock_guard lk(g_mu);
    const uint64_t now = current_frame();
    return {{"events", g_sched.size()}, {"applied", g_sched_next}, {"done", g_sched_next >= g_sched.size()},
            {"base_frame", g_sched_base}, {"frame", now}, {"rel_frame", now >= g_sched_base ? now - g_sched_base : 0},
            {"last_f", g_sched.empty() ? 0 : g_sched.back().f}};
}

json trace_start(const json& p) {
    std::lock_guard lk(g_mu);
    g_trace.clear();
    g_trace_every = std::max<uint64_t>(1, p.value("every", uint64_t{30}));
    g_trace_watch.clear();
    if (p.contains("watch"))
        for (const json& a : p["watch"]) g_trace_watch.push_back(static_cast<uintptr_t>(a.get<uint64_t>()));
    g_trace_base = current_frame() + 1;
    g_trace_active = true;
    return {{"base_frame", g_trace_base}, {"every", g_trace_every}};
}

json trace_stop() {
    std::lock_guard lk(g_mu);
    g_trace_active = false;
    json arr = json::array();
    for (const CarSample& cs : g_trace) {
        json j = car_sample_json(cs);
        if (cs.nwatch) {
            json w = json::array();
            for (uint8_t i = 0; i < cs.nwatch; ++i) w.push_back(cs.watch[i]);
            j["watch"] = w;
        }
        arr.push_back(j);
    }
    return {{"samples", arr}, {"every", g_trace_every}};
}

json record_start(const json& p) {
    json t = trace_start(p);
    std::lock_guard lk(g_mu);
    g_rec.clear();
    g_rec_base = g_trace_base;
    g_rec_active = true;
    logf("recording real keyboard from frame %llu", static_cast<unsigned long long>(g_rec_base));
    return t;
}

json record_stop() {
    json events = json::array();
    uint64_t frames = 0;
    {
        std::lock_guard lk(g_mu);
        g_rec_active = false;
        for (const RecEvent& e : g_rec) events.push_back({{"f", e.f}, {"action", e.action}, {"down", e.data != 0}});
        frames = current_frame() - g_rec_base;
    }
    json t = trace_stop();
    t["events"] = events;
    t["frames"] = frames;
    return t;
}

}  // namespace probe
