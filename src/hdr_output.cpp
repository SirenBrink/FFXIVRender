#include "xivrender/hdr.hpp"
#include "xivrender/frame_generation.hpp"
#include "xivrender/fg_diagnostics.hpp"
#include "xivrender/d3d11_work.hpp"
#include "xivrender/d3d12_identity.hpp"
#include "xivrender/d3d11_identity.hpp"
#include <dxgi1_6.h>
#include <MinHook.h>
#include <atomic>
#include <algorithm>
#include <vector>
#include <sstream>
#include <iomanip>
#include <mutex>
namespace xivrender::hdr {
using Microsoft::WRL::ComPtr;
namespace {
thread_local std::string setup_failure;
HRESULT fail(HRESULT hr, const char *where) {
    std::ostringstream line;
    line << where << " (HRESULT 0x" << std::hex << std::setw(8) << std::setfill('0') << unsigned(hr) << ')';
    setup_failure = line.str();
    return hr;
}
void disable_output(HRESULT hr) {
    if (setup_failure.empty())
        fail(hr, "native HDR swapchain creation");
    disable(("HDR setup failed: " + setup_failure + "; original SDR presentation retained").c_str());
}
struct CarrierWindow {
    HWND window = nullptr;
    UINT width = 0, height = 0;
    ~CarrierWindow() {
        if (!window)
            return;
        if (GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId())
            DestroyWindow(window);
        else
            PostMessageW(window, WM_CLOSE, 0, 0);
    }
    HRESULT create(const DXGI_SWAP_CHAIN_DESC &logical) {
        WNDCLASSW wc{};
        // Use an OS window procedure, so asynchronous closure cannot call
        // addon code during unload. No visible window or message worker.
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"XIVRender private SDR transport";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return fail(HRESULT_FROM_WIN32(GetLastError()), "SDR transport window class");
        RECT client{};
        GetClientRect(logical.OutputWindow, &client);
        width = logical.BufferDesc.Width ? logical.BufferDesc.Width : UINT(std::max(1L, client.right));
        height = logical.BufferDesc.Height ? logical.BufferDesc.Height : UINT(std::max(1L, client.bottom));
        window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, -32000,
                                 -32000, int(width), int(height), nullptr, nullptr, wc.hInstance, nullptr);
        return window ? S_OK : fail(HRESULT_FROM_WIN32(GetLastError()), "SDR transport hidden window");
    }
};
class Output final : public IDXGISwapChain4 {
    std::recursive_mutex output_mutex;
    std::atomic<ULONG> references{1};
    ComPtr<IDXGISwapChain4> ordinary_chain;
    IDXGISwapChain4 *chain = nullptr; // Borrowed while the FG Presenter owns it.
    std::unique_ptr<CarrierWindow> carrier_window;
    ComPtr<IDXGISwapChain> carrier;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    DXGI_SWAP_CHAIN_DESC logical{};
    bool pq = true;
    bool is_hdr = true;
    DXGI_FORMAT output_format = DXGI_FORMAT_UNKNOWN;
    std::unique_ptr<fg::Presenter> generator;
    std::uint64_t generator_generation = 0;
    std::vector<ComPtr<ID3D11Texture2D>> buffers;
    std::vector<ComPtr<ID3D11RenderTargetView>> targets;
    HRESULT allocate() {
        DXGI_SWAP_CHAIN_DESC actual{};
        HRESULT hr = chain->GetDesc(&actual);
        if (FAILED(hr))
            return fail(hr, "HDR swapchain description");
        output_format = actual.BufferDesc.Format;
        if (generator) {
            is_hdr = output_format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                     output_format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            pq = output_format != DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
        logical.BufferDesc.Width = actual.BufferDesc.Width;
        logical.BufferDesc.Height = actual.BufferDesc.Height;
        buffers.clear();
        targets.clear();
        // A real, never-presented SDR chain preserves DXGI's special
        // UNORM/sRGB view rules. Plain UNORM textures cannot provide both views
        // and typeless stand-ins change the game's GetDesc contract.
        // DXGI blt-discard exposes only buffer 0, even if BufferCount > 1.
        // The native descriptor count is a presentation allocation policy,
        // not the number of GetBuffer indices that the application can access.
        const UINT accessible = logical.SwapEffect == DXGI_SWAP_EFFECT_DISCARD ? 1u : logical.BufferCount;
        for (unsigned i = 0; i < accessible; ++i) {
            ComPtr<ID3D11Texture2D> t;
            if (FAILED(hr = carrier->GetBuffer(i, IID_PPV_ARGS(&t))))
                return fail(hr, "native SDR backbuffer acquisition");
            buffers.push_back(std::move(t));
        }
        return S_OK;
    }
    HRESULT before(UINT flags) {
        if (flags & DXGI_PRESENT_TEST)
            return S_OK;
        // A failed resize must never turn a recoverable DXGI error into an
        // out-of-bounds access on the next Present. Another resize can retry.
        if (buffers.empty())
            return DXGI_ERROR_INVALID_CALL;
        if (generator) {
            std::string error;
            if (!generator->synchronize(error)) {
                fg::report(error);
                chain = generator->chain();
                return E_FAIL;
            }
            chain = generator->chain();
            if (generator_generation != generator->generation()) {
                const auto ready = initialize();
                if (FAILED(ready))
                    return ready;
            }
            if (!generator->prepare_colour(buffers[0].Get(), error)) {
                fg::report(error);
                return E_FAIL;
            }
            if (is_hdr)
                return hdr::present(context.Get(), buffers[0].Get(), generator->target(),
                                    logical.BufferDesc.Width, logical.BufferDesc.Height, pq)
                           ? S_OK
                           : E_FAIL;
            return S_OK;
        }
        const auto index = chain->GetCurrentBackBufferIndex();
        if (targets.size() <= index)
            targets.resize(index + 1);
        if (!targets[index]) {
            ComPtr<ID3D11Texture2D> b;
            HRESULT hr = chain->GetBuffer(index, IID_PPV_ARGS(&b));
            if (FAILED(hr))
                return hr;
            if (FAILED(hr = device->CreateRenderTargetView(b.Get(), nullptr, &targets[index])))
                return hr;
        }
        const auto source = logical.SwapEffect == DXGI_SWAP_EFFECT_DISCARD ||
                                    logical.SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL
                                ? 0
                                : index % unsigned(buffers.size());
        if (!hdr::present(context.Get(), buffers[source].Get(), targets[index].Get(),
                          logical.BufferDesc.Width, logical.BufferDesc.Height, pq))
            return E_FAIL;
        return S_OK;
    }

  public:
    Output(IDXGISwapChain4 *c, IDXGISwapChain *sdr, std::unique_ptr<CarrierWindow> host, ID3D11Device *d,
           const DXGI_SWAP_CHAIN_DESC &desc, bool is_pq, std::unique_ptr<fg::Presenter> fg_owner)
        : ordinary_chain(fg_owner ? nullptr : c), chain(c), carrier_window(std::move(host)), carrier(sdr),
          device(d), logical(desc), pq(is_pq), is_hdr(active()), generator(std::move(fg_owner)) {
        d->GetImmediateContext(&context);
    }
    ~Output() {
        fg::diagnostics::Scope phase("shutdown.output");
        // The provider context owns the swapchain proxy. Release our extra
        // proxy reference before destroying that context, never afterward.
        // Presenter retains its own chain until it has drained the queue.
        if (generator) {
            chain = nullptr;
            generator.reset();
        }
    }
    HRESULT initialize() {
        HRESULT hr = allocate();
        if (FAILED(hr)) {
            return hr;
        }
        UINT support = 0;
        const auto space = is_hdr ? (pq ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                        : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
                                  : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        hr = chain->CheckColorSpaceSupport(space, &support);
        if (FAILED(hr) || !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
            return fail(FAILED(hr) ? hr : DXGI_ERROR_UNSUPPORTED, "HDR color-space support");
        hr = chain->SetColorSpace1(space);
        if (FAILED(hr))
            return fail(hr, "HDR color-space activation");
        if (is_hdr && !warmup(context.Get(), logical.BufferDesc.Width, logical.BufferDesc.Height))
            return fail(E_OUTOFMEMORY, "HDR working image initialization");
        DXGI_SWAP_CHAIN_DESC observed{};
        hr = chain->GetDesc(&observed);
        if (FAILED(hr))
            return hr;
        const UINT info[]{UINT(observed.BufferDesc.Format), UINT(space)};
        chain->SetPrivateData(output_info, sizeof(info), info);
        DXGI_SWAP_CHAIN_DESC transport{};
        if (SUCCEEDED(carrier->GetDesc(&transport))) {
            const TransportInfo data{reinterpret_cast<std::uint64_t>(transport.OutputWindow),
                                     UINT(transport.SwapEffect), transport.BufferCount};
            chain->SetPrivateData(transport_info, sizeof(data), &data);
        }
        if (generator) {
            generator_generation = generator->generation();
            commit_output(is_hdr);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        std::lock_guard lock(output_mutex);
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IDXGIObject) || id == __uuidof(IDXGIDeviceSubObject) ||
            id == __uuidof(IDXGISwapChain) || id == __uuidof(IDXGISwapChain1) ||
            id == __uuidof(IDXGISwapChain2) || id == __uuidof(IDXGISwapChain3) ||
            id == __uuidof(IDXGISwapChain4)) {
            *out = static_cast<IDXGISwapChain4 *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto n = --references;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID id, void **out) override {
        std::lock_guard lock(output_mutex);
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (index >= buffers.size())
            return DXGI_ERROR_INVALID_CALL;
        return buffers[index]->QueryInterface(id, out);
    }
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override {
        fg::diagnostics::Scope phase("output.present");
        const auto preparation_started = fg::TimingClock::now();
        auto presentation =
            generator ? generator->lock_presentation() : std::unique_lock<std::recursive_mutex>{};
        d3d11_work::Guard context_owner;
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        const auto ready = before(flags);
        if (generator && !(flags & DXGI_PRESENT_TEST))
            fg::note_timing(fg::TimingPhase::OutputPreparation, fg::elapsed_ms(preparation_started));
        if (FAILED(ready))
            return ready;
        bool sealed = false;
        const auto release_render_context = [&] {
            // Seal only this composed frame while its metadata is still ours.
            // The native render worker may start the next HDR frame as soon
            // as the provider enters its presentation/pacing wait.
            finish_frame();
            sealed = true;
            context_owner.lock.unlock();
        };
        const auto hr = generator
                            ? generator->present(sync, flags,
                                                 GetForegroundWindow() == logical.OutputWindow ||
                                                     IsChild(logical.OutputWindow, GetForegroundWindow()),
                                                 release_render_context)
                            : chain->Present(sync, flags);
        if (!sealed && !(flags & DXGI_PRESENT_TEST) && SUCCEEDED(hr))
            finish_frame();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *) override {
        fg::diagnostics::Scope phase("output.present1");
        const auto preparation_started = fg::TimingClock::now();
        auto presentation =
            generator ? generator->lock_presentation() : std::unique_lock<std::recursive_mutex>{};
        d3d11_work::Guard context_owner;
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        const auto ready = before(flags);
        if (generator && !(flags & DXGI_PRESENT_TEST))
            fg::note_timing(fg::TimingPhase::OutputPreparation, fg::elapsed_ms(preparation_started));
        if (FAILED(ready))
            return ready;
        DXGI_PRESENT_PARAMETERS full{};
        bool sealed = false;
        const auto release_render_context = [&] {
            finish_frame();
            sealed = true;
            context_owner.lock.unlock();
        };
        const auto hr = generator
                            ? generator->present(sync, flags,
                                                 GetForegroundWindow() == logical.OutputWindow ||
                                                     IsChild(logical.OutputWindow, GetForegroundWindow()),
                                                 release_render_context)
                            : chain->Present1(sync, flags, &full);
        if (!sealed && !(flags & DXGI_PRESENT_TEST) && SUCCEEDED(hr))
            finish_frame();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *out) override {
        std::lock_guard lock(output_mutex);
        if (!out)
            return E_POINTER;
        *out = logical;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *out) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        if (!out)
            return E_POINTER;
        HRESULT hr = chain->GetDesc1(out);
        if (SUCCEEDED(hr)) {
            out->Width = logical.BufferDesc.Width;
            out->Height = logical.BufferDesc.Height;
            out->Format = logical.BufferDesc.Format;
            out->BufferCount = logical.BufferCount;
            out->BufferUsage = logical.BufferUsage;
            out->SwapEffect = logical.SwapEffect;
            out->Flags = logical.Flags;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT w, UINT h, DXGI_FORMAT format,
                                            UINT flags) override {
        auto presentation =
            generator ? generator->lock_presentation() : std::unique_lock<std::recursive_mutex>{};
        d3d11_work::Guard context_owner;
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        if (flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
            return DXGI_ERROR_INVALID_CALL;
        if (generator) {
            std::string error;
            if (!generator->drain(1000, error)) {
                fg::report(error);
                return DXGI_ERROR_WAS_STILL_DRAWING;
            }
        }
        targets.clear();
        buffers.clear();
        auto next = logical;
        if (count)
            next.BufferCount = count;
        if (format != DXGI_FORMAT_UNKNOWN)
            next.BufferDesc.Format = format;
        next.Flags = flags;
        // Zero dimensions refer to the game client, not the private carrier HWND.
        RECT client{};
        if ((!w || !h) && !GetClientRect(logical.OutputWindow, &client))
            return E_INVALIDARG;
        if (!w)
            w = UINT(std::max(1L, client.right));
        if (!h)
            h = UINT(std::max(1L, client.bottom));
        auto hr = carrier->ResizeBuffers(std::max(2u, next.BufferCount), w, h, next.BufferDesc.Format, 0);
        if (FAILED(hr)) {
            allocate();
            return hr;
        }
        hr = chain->ResizeBuffers(generator ? 3u : std::max(2u, next.BufferCount), w, h, output_format,
                                  generator ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : flags);
        if (FAILED(hr)) {
            // The actual chain kept its old dimensions. Restore its carrier
            // too, allowing the caller to continue or retry after DXGI fails.
            if (SUCCEEDED(carrier->ResizeBuffers(std::max(2u, logical.BufferCount), logical.BufferDesc.Width,
                                                 logical.BufferDesc.Height, logical.BufferDesc.Format, 0)))
                allocate();
            return hr;
        }
        if (generator) {
            std::string error;
            if (!generator->resize(w, h, error)) {
                fg::report(error);
                return E_FAIL;
            }
        }
        logical = next;
        finish_frame();
        return initialize();
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags,
                                             const UINT *, IUnknown *const *) override {
        return ResizeBuffers(count, w, h, format, flags);
    }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return 0;
        return logical.SwapEffect == DXGI_SWAP_EFFECT_DISCARD ||
                       logical.SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL
                   ? 0
                   : chain->GetCurrentBackBufferIndex() % unsigned(buffers.size());
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->SetColorSpace1(!is_hdr ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                                             : (pq ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                                   : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID a, UINT b, const void *c) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->SetPrivateData(a, b, c);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID a, const IUnknown *b) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->SetPrivateDataInterface(a, b);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID a, UINT *b, void *c) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetPrivateData(a, b, c);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID a, void **b) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetParent(a, b);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID a, void **b) override {
        std::lock_guard lock(output_mutex);
        return device->QueryInterface(a, b);
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL a, IDXGIOutput *b) override {
        auto presentation =
            generator ? generator->lock_presentation() : std::unique_lock<std::recursive_mutex>{};
        d3d11_work::Guard context_owner;
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        if (generator) {
            std::string error;
            if (!generator->drain(1000, error)) {
                fg::report(error);
                return DXGI_ERROR_WAS_STILL_DRAWING;
            }
        }
        const auto hr = chain->SetFullscreenState(a, b);
        if (SUCCEEDED(hr))
            logical.Windowed = !a;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *a, IDXGIOutput **b) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetFullscreenState(a, b);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->ResizeTarget(a);
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetContainingOutput(a);
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *a) override {
        if (generator)
            fg::note_presentation_query(3);
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetFrameStatistics(a);
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *a) override {
        if (generator)
            fg::note_presentation_query(4);
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetLastPresentCount(a);
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetFullscreenDesc(a);
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND *a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetHwnd(a);
    }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID a, void **b) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetCoreWindow(a, b);
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return 0;
        return chain->IsTemporaryMonoSupported();
    }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetRestrictToOutput(a);
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->SetBackgroundColor(a);
    }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetBackgroundColor(a);
    }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->SetRotation(a);
    }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetRotation(a);
    }
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT a, UINT b) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->SetSourceSize(a, b);
    }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *a, UINT *b) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetSourceSize(a, b);
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        if (generator)
            fg::note_presentation_query(0);
        // The private provider owns its pacing policy. Preserve the native
        // SDR contract instead of changing the SDK's internal waitable queue.
        ComPtr<IDXGISwapChain2> native;
        const auto hr = carrier.As(&native);
        return SUCCEEDED(hr) ? native->SetMaximumFrameLatency(a) : hr;
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        if (generator)
            fg::note_presentation_query(1);
        ComPtr<IDXGISwapChain2> native;
        const auto hr = carrier.As(&native);
        return SUCCEEDED(hr) ? native->GetMaximumFrameLatency(a) : hr;
    }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return nullptr;
        if (generator)
            fg::note_presentation_query(2);
        // SL always creates an internal waitable object even when the game
        // requested none. Exposing it lets two independent waiters consume
        // its signals, starving the SDK pacer. The carrier is never presented.
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->SetMatrixTransform(a);
    }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *a) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->GetMatrixTransform(a);
    }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE a, UINT *b) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->CheckColorSpaceSupport(a, b);
    }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE a, UINT b, void *c) override {
        std::lock_guard lock(output_mutex);
        if (!chain)
            return DXGI_ERROR_DEVICE_REMOVED;
        return chain->SetHDRMetaData(a, b, c);
    }
};
using Create = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *,
                                            IDXGISwapChain **);
using CreateHwnd = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, HWND,
                                                const DXGI_SWAP_CHAIN_DESC1 *,
                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *,
                                                IDXGISwapChain1 **);
Create original = nullptr;
CreateHwnd original_hwnd = nullptr;
std::vector<void *> hooks;
thread_local bool creating = false;
bool eligible(IUnknown *d, HWND window, const DXGI_SWAP_CHAIN_DESC &desc, ComPtr<ID3D11Device> &device) {
    if (creating || !window || desc.SampleDesc.Count != 1 ||
        (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ||
        (desc.SwapEffect != DXGI_SWAP_EFFECT_DISCARD && desc.SwapEffect != DXGI_SWAP_EFFECT_SEQUENTIAL) ||
        (desc.BufferDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
         desc.BufferDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM))
        return false;
    DWORD pid = 0;
    RECT r{};
    if (!GetWindowThreadProcessId(window, &pid) || pid != GetCurrentProcessId() ||
        !GetClientRect(window, &r) || r.right - r.left < 640 || r.bottom - r.top < 360)
        return false;
    if (FAILED(d->QueryInterface(IID_PPV_ARGS(&device))))
        return false;
    device = d3d11_identity::native_device(device.Get());
    return bool(device);
}
HRESULT wrap(IDXGISwapChain *raw, IDXGISwapChain *sdr, std::unique_ptr<CarrierWindow> &host, ID3D11Device *d,
             const DXGI_SWAP_CHAIN_DESC &desc, IDXGISwapChain **out,
             std::unique_ptr<fg::Presenter> generator = {}) {
    ComPtr<IDXGISwapChain4> chain;
    if (FAILED(raw->QueryInterface(IID_PPV_ARGS(&chain))))
        return E_NOINTERFACE;
    auto *proxy =
        new (std::nothrow) Output(chain.Get(), sdr, std::move(host), d, desc,
                                  options().output != HdrOutput::ScRgb16Float, std::move(generator));
    if (!proxy)
        return E_OUTOFMEMORY;
    const auto hr = proxy->initialize();
    if (FAILED(hr)) {
        proxy->Release();
        return hr;
    }
    raw->Release();
    *out = proxy;
    if (active())
        report("HDR presentation active; native and ReShade buffers remain SDR");
    return S_OK;
}
HRESULT STDMETHODCALLTYPE create(IDXGIFactory *f, IUnknown *d, DXGI_SWAP_CHAIN_DESC *desc,
                                 IDXGISwapChain **out) {
    ComPtr<ID3D11Device> device;
    if (!desc || !out || !eligible(d, desc->OutputWindow, *desc, device))
        return original(f, d, desc, out);
    setup_failure.clear();
    auto hdr = *desc;
    hdr.BufferDesc.Format = !active()                                     ? desc->BufferDesc.Format
                            : options().output == HdrOutput::ScRgb16Float ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                                          : DXGI_FORMAT_R10G10B10A2_UNORM;
    hdr.BufferCount = std::max(2u, hdr.BufferCount);
    hdr.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    creating = true;
    ComPtr<IDXGISwapChain> sdr;
    auto native = *desc;
    auto host = std::make_unique<CarrierWindow>();
    auto hr = host->create(*desc);
    native.OutputWindow = host->window;
    native.Windowed = TRUE;
    native.BufferDesc.Width = host->width;
    native.BufferDesc.Height = host->height;
    native.BufferCount = std::max(2u, native.BufferCount);
    native.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    native.Flags = 0;
    native.BufferUsage |= DXGI_USAGE_SHADER_INPUT;
    if (SUCCEEDED(hr))
        hr = original(f, d, &native, &sdr);
    if (FAILED(hr))
        fail(hr, "SDR carrier swapchain creation");
    std::unique_ptr<fg::Presenter> generator;
    if (SUCCEEDED(hr)) {
        auto candidate = std::make_unique<fg::Presenter>();
        auto desired = hdr;
        if (!active())
            hdr.BufferDesc.Format = desired.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desired.BufferDesc.Width = host->width;
        desired.BufferDesc.Height = host->height;
        std::string error;
        if (candidate->initialize(device.Get(), f, desc->OutputWindow, desired, hdr.BufferDesc.Format,
                                  error)) {
            *out = candidate->chain();
            (*out)->AddRef();
            generator = std::move(candidate);
        } else
            fg::report("FG unavailable: " + error);
    }
    if (SUCCEEDED(hr) && !generator && !active()) {
        creating = false;
        sdr.Reset();
        return original(f, d, desc, out);
    }
    if (SUCCEEDED(hr) && !generator) {
        hr = original(f, d, &hdr, out);
        if (FAILED(hr))
            fail(hr, "HDR display swapchain creation");
    }
    creating = false;
    if (SUCCEEDED(hr)) {
        hr = wrap(*out, sdr.Get(), host, device.Get(), *desc, out, std::move(generator));
        if (SUCCEEDED(hr))
            return hr;
        (*out)->Release();
        *out = nullptr;
    }
    sdr.Reset();
    disable_output(hr);
    return original(f, d, desc, out);
}
HRESULT STDMETHODCALLTYPE create_hwnd(IDXGIFactory2 *f, IUnknown *d, HWND window,
                                      const DXGI_SWAP_CHAIN_DESC1 *desc,
                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *full, IDXGIOutput *restrict_to,
                                      IDXGISwapChain1 **out) {
    DXGI_SWAP_CHAIN_DESC logical{};
    if (desc) {
        logical.BufferDesc.Width = desc->Width;
        logical.BufferDesc.Height = desc->Height;
        logical.BufferDesc.Format = desc->Format;
        logical.SampleDesc = desc->SampleDesc;
        logical.BufferCount = desc->BufferCount;
        logical.BufferUsage = desc->BufferUsage;
        logical.SwapEffect = desc->SwapEffect;
        logical.Flags = desc->Flags;
        logical.OutputWindow = window;
        logical.Windowed = !full || full->Windowed;
    }
    ComPtr<ID3D11Device> device;
    if (!desc || !out || !eligible(d, window, logical, device))
        return original_hwnd(f, d, window, desc, full, restrict_to, out);
    if (!restrict_to) {
        IDXGISwapChain *base = nullptr;
        auto hr = create(f, d, &logical, &base);
        if (FAILED(hr))
            return hr;
        hr = base->QueryInterface(IID_PPV_ARGS(out));
        base->Release();
        return hr;
    }
    setup_failure.clear();
    auto hdr = *desc;
    hdr.Format = options().output == HdrOutput::ScRgb16Float ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                             : DXGI_FORMAT_R10G10B10A2_UNORM;
    hdr.BufferCount = std::max(2u, hdr.BufferCount);
    hdr.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    creating = true;
    ComPtr<IDXGISwapChain1> sdr;
    auto native = *desc;
    auto host = std::make_unique<CarrierWindow>();
    auto hr = host->create(logical);
    native.Width = host->width;
    native.Height = host->height;
    native.BufferCount = std::max(2u, native.BufferCount);
    native.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    native.Flags = 0;
    native.BufferUsage |= DXGI_USAGE_SHADER_INPUT;
    if (SUCCEEDED(hr))
        hr = original_hwnd(f, d, host->window, &native, nullptr, nullptr, &sdr);
    if (FAILED(hr))
        fail(hr, "SDR carrier swapchain creation");
    if (SUCCEEDED(hr)) {
        hr = original_hwnd(f, d, window, &hdr, full, restrict_to, out);
        if (FAILED(hr))
            fail(hr, "HDR display swapchain creation");
    }
    creating = false;
    if (SUCCEEDED(hr)) {
        IDXGISwapChain *base = *out;
        hr = wrap(base, sdr.Get(), host, device.Get(), logical, &base);
        if (SUCCEEDED(hr)) {
            *out = static_cast<IDXGISwapChain1 *>(base);
            return hr;
        }
        (*out)->Release();
        *out = nullptr;
    }
    sdr.Reset();
    disable_output(hr);
    return original_hwnd(f, d, window, desc, full, restrict_to, out);
}
} // namespace
bool install_output(std::string &error) {
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        error = "HDR DXGI factory unavailable";
        return false;
    }
    ComPtr<IUnknown> unwrapped;
    if (SUCCEEDED(factory->QueryInterface(d3d12_identity::unwrapped_object,
                                          reinterpret_cast<void **>(unwrapped.GetAddressOf())))) {
        ComPtr<IDXGIFactory2> raw;
        if (FAILED(unwrapped.As(&raw))) {
            error = "HDR factory identity unavailable";
            return false;
        }
        factory = std::move(raw);
    }
    auto **v = *reinterpret_cast<void ***>(factory.Get());
    const auto add = [&](unsigned index, void *hook, void **saved) {
        void *t = v[index];
        if (MH_CreateHook(t, hook, saved) != MH_OK)
            return false;
        hooks.push_back(t);
        return true;
    };
    if (!add(10, reinterpret_cast<void *>(create), reinterpret_cast<void **>(&original)) ||
        !add(15, reinterpret_cast<void *>(create_hwnd), reinterpret_cast<void **>(&original_hwnd))) {
        remove_output();
        error = "HDR output factory hook unavailable";
        return false;
    }
    for (auto *t : hooks)
        if (MH_EnableHook(t) != MH_OK) {
            remove_output();
            error = "HDR output hook activation failed";
            return false;
        }
    return true;
}
void remove_output() {
    for (auto *t : hooks)
        MH_DisableHook(t); /* Native creation can be inside another addon; retain disabled trampolines through
                              teardown. */
    hooks.clear();
}
} // namespace xivrender::hdr
