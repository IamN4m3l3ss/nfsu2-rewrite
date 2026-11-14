// Render-thread work for the core: the task queue drained at Present (the loader owns the D3D hooks and calls
// core on_frame there) and back-buffer screenshots.
#include "probe.h"

#include <d3d9.h>

#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace probe {
namespace {

struct Task {
    std::function<json()> fn;
    std::promise<json> done;
};
std::mutex g_task_mu;
std::deque<std::shared_ptr<Task>> g_tasks;
bool g_tasks_closed = false;

}  // namespace

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

void tasks_close() {
    std::deque<std::shared_ptr<Task>> tasks;
    {
        std::lock_guard lk(g_task_mu);
        g_tasks_closed = true;
        tasks.swap(g_tasks);
    }
    for (auto& t : tasks) t->done.set_exception(std::make_exception_ptr(std::runtime_error("core reloading")));
}

uint64_t current_frame() { return api().frames(); }

json d3d_info() {
    const LoaderApi& a = api();
    return {{"device", hex_addr(reinterpret_cast<uintptr_t>(a.device()))},
            {"window", hex_addr(reinterpret_cast<uintptr_t>(a.window()))},
            {"frames", a.frames()},
            {"create_thread", a.create_thread()},
            {"present_thread", a.present_thread()}};
}

json run_on_main(std::function<json()> fn, unsigned timeout_ms) {
    if (!api().device()) throw std::runtime_error("no D3D device yet");
    auto t = std::make_shared<Task>();
    t->fn = std::move(fn);
    auto fut = t->done.get_future();
    {
        std::lock_guard lk(g_task_mu);
        if (g_tasks_closed) throw std::runtime_error("core reloading");
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
    IDirect3DDevice9* dev = api().device();
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
            {"frame", api().frames()}, {"format", "BGRX"}, {"data", base64(out.data(), out.size())}};
}

}  // namespace probe
