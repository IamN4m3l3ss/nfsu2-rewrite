// Direct3D 9 hooks: Direct3DCreate9 (MinHook) -> IDirect3D9::CreateDevice (vtable slot 16)
// -> IDirect3DDevice9::Present (vtable slot 17). Present is the frame safe point: queued main-thread work runs
// there, before the original Present, while the back buffer still holds the finished frame.
// Slot numbers verified against the Windows SDK d3d9.h (10.0.26100).
#include "probe.h"

#include <d3d9.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "MinHook.h"

namespace probe {
namespace {

constexpr int kSlotCreateDevice = 16;  // IDirect3D9
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

struct Task {
    std::function<json()> fn;
    std::promise<json> done;
};
std::mutex g_task_mu;
std::deque<std::shared_ptr<Task>> g_tasks;

bool patch_slot(void* obj, int slot, void* detour, void** original) {
    void** vtbl = *reinterpret_cast<void***>(obj);
    void* cur = vtbl[slot];
    if (cur == detour) return true;  // already patched (shared vtable)
    *original = cur;
    return mem_write(reinterpret_cast<uintptr_t>(&vtbl[slot]), &detour, sizeof detour);
}

void run_tasks() {
    std::deque<std::shared_ptr<Task>> tasks;
    {
        std::lock_guard lk(g_task_mu);
        tasks.swap(g_tasks);
    }
    for (auto& t : tasks) {
        try {
            t->done.set_value(t->fn());
        } catch (...) {
            t->done.set_exception(std::current_exception());
        }
    }
}

HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd,
                                     const RGNDATA* dirty) {
    uint64_t f = ++g_frames;
    if (f == 1) {
        g_present_thread = GetCurrentThreadId();
        logf("first Present: device=%p thread=%lx (CreateDevice thread=%lx)", dev, GetCurrentThreadId(),
             g_create_thread.load());
    }
    if (dev == g_device.load()) {
        input_frame_tick(f);
        run_tasks();
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

}  // namespace

bool d3d_install_hooks() {
    MH_STATUS s = MH_CreateHookApi(L"d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&hk_Direct3DCreate9),
                                   reinterpret_cast<void**>(&o_Direct3DCreate9));
    if (s != MH_OK) {
        logf("ERROR: hook Direct3DCreate9: %s", MH_StatusToString(s));
        return false;
    }
    s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) {
        logf("ERROR: enable hooks: %s", MH_StatusToString(s));
        return false;
    }
    logf("Direct3DCreate9 hooked");
    return true;
}

uint64_t current_frame() { return g_frames.load(); }

json d3d_info() {
    return {{"device", hex_addr(reinterpret_cast<uintptr_t>(g_device.load()))},
            {"window", hex_addr(reinterpret_cast<uintptr_t>(g_window.load()))},
            {"frames", g_frames.load()},
            {"create_thread", g_create_thread.load()},
            {"present_thread", g_present_thread.load()}};
}

json run_on_main(std::function<json()> fn, unsigned timeout_ms) {
    if (!g_device.load()) throw std::runtime_error("no D3D device yet");
    auto t = std::make_shared<Task>();
    t->fn = std::move(fn);
    auto fut = t->done.get_future();
    {
        std::lock_guard lk(g_task_mu);
        g_tasks.push_back(t);
    }
    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
        // leave the task queued; it completes (and is dropped) whenever the next frame comes
        throw std::runtime_error("timeout: no Present within " + std::to_string(timeout_ms) +
                                 " ms (game not rendering?)");
    }
    return fut.get();
}

namespace {
template <class T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
};

// convert one pixel row to BGRX (4 bytes/pixel)
bool row_to_bgrx(D3DFORMAT fmt, const uint8_t* src, uint8_t* dst, UINT w) {
    switch (fmt) {
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8R8G8B8:
            memcpy(dst, src, w * 4);
            return true;
        case D3DFMT_R5G6B5:
            for (UINT x = 0; x < w; ++x) {
                uint16_t v = reinterpret_cast<const uint16_t*>(src)[x];
                dst[x * 4 + 0] = static_cast<uint8_t>((v & 0x1f) * 255 / 31);
                dst[x * 4 + 1] = static_cast<uint8_t>(((v >> 5) & 0x3f) * 255 / 63);
                dst[x * 4 + 2] = static_cast<uint8_t>((v >> 11) * 255 / 31);
                dst[x * 4 + 3] = 255;
            }
            return true;
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
            for (UINT x = 0; x < w; ++x) {
                uint16_t v = reinterpret_cast<const uint16_t*>(src)[x];
                dst[x * 4 + 0] = static_cast<uint8_t>((v & 0x1f) * 255 / 31);
                dst[x * 4 + 1] = static_cast<uint8_t>(((v >> 5) & 0x1f) * 255 / 31);
                dst[x * 4 + 2] = static_cast<uint8_t>(((v >> 10) & 0x1f) * 255 / 31);
                dst[x * 4 + 3] = 255;
            }
            return true;
        default:
            return false;
    }
}
}  // namespace

json screenshot(int scale) {
    IDirect3DDevice9* dev = g_device.load();
    if (!dev) throw std::runtime_error("no device");
    if (scale < 1) scale = 1;

    ComPtr<IDirect3DSurface9> bb;
    HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr)) throw std::runtime_error("GetBackBuffer failed " + std::to_string(hr));
    D3DSURFACE_DESC desc{};
    bb->GetDesc(&desc);

    // GetRenderTargetData can't read a multisampled surface: resolve into a plain render target first
    ComPtr<IDirect3DSurface9> resolved;
    IDirect3DSurface9* src = bb.p;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        hr = dev->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved,
                                     nullptr);
        if (FAILED(hr)) throw std::runtime_error("CreateRenderTarget failed");
        hr = dev->StretchRect(bb.p, nullptr, resolved.p, nullptr, D3DTEXF_NONE);
        if (FAILED(hr)) throw std::runtime_error("StretchRect failed");
        src = resolved.p;
    }

    ComPtr<IDirect3DSurface9> sys;
    hr = dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (FAILED(hr)) throw std::runtime_error("CreateOffscreenPlainSurface failed");
    hr = dev->GetRenderTargetData(src, sys.p);
    if (FAILED(hr)) throw std::runtime_error("GetRenderTargetData failed " + std::to_string(hr));

    D3DLOCKED_RECT lr{};
    hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) throw std::runtime_error("LockRect failed");

    const UINT w = desc.Width, h = desc.Height;
    const UINT ow = w / scale, oh = h / scale;
    std::vector<uint8_t> row(w * 4), out(static_cast<size_t>(ow) * oh * 4);
    std::vector<uint32_t> acc(static_cast<size_t>(ow) * 3);
    bool ok = true;
    for (UINT oy = 0; oy < oh && ok; ++oy) {
        std::fill(acc.begin(), acc.end(), 0);
        for (int sy = 0; sy < scale; ++sy) {
            const uint8_t* line = static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(oy * scale + sy) * lr.Pitch;
            if (!row_to_bgrx(desc.Format, line, row.data(), w)) { ok = false; break; }
            for (UINT ox = 0; ox < ow; ++ox)
                for (int sx = 0; sx < scale; ++sx)
                    for (int c = 0; c < 3; ++c) acc[ox * 3 + c] += row[(ox * scale + sx) * 4 + c];
        }
        const uint32_t div = static_cast<uint32_t>(scale * scale);
        for (UINT ox = 0; ox < ow && ok; ++ox) {
            uint8_t* px = &out[(static_cast<size_t>(oy) * ow + ox) * 4];
            for (int c = 0; c < 3; ++c) px[c] = static_cast<uint8_t>(acc[ox * 3 + c] / div);
            px[3] = 255;
        }
    }
    sys->UnlockRect();
    if (!ok) throw std::runtime_error("unsupported back buffer format " + std::to_string(desc.Format));

    return {{"width", ow}, {"height", oh}, {"src_width", w}, {"src_height", h}, {"src_format", desc.Format},
            {"frame", g_frames.load()}, {"format", "BGRX"}, {"data", base64(out.data(), out.size())}};
}

}  // namespace probe
