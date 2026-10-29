#include "xivrender/fg_backend.hpp"
#include "xivrender/fg_diagnostics.hpp"
#include "xivrender/ngx_logging.hpp"
#include "xivrender/fg_frame_tokens.hpp"
#include "xivrender/fg_present_count.hpp"
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_security.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell_d3d12.h>
#include <framegeneration/include/dx12/ffx_api_framegeneration_dx12.h>
#include <framegeneration/include/ffx_framegeneration.h>
#include <ffx_antilag2_dx12.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <atomic>
#include <array>
#include <cmath>

namespace xivrender::fg {
using Microsoft::WRL::ComPtr;
namespace {
struct Library {
    HMODULE handle = nullptr;
    std::string name;
    explicit Library(const std::filesystem::path &p) {
        name = p.string();
        diagnostics::event("module.load.begin", 0, 0, 0, 0, 0, name.c_str());
        if (p.filename() == L"sl.interposer.dll" && !sl::security::verifyEmbeddedSignature(p.c_str()))
            throw std::runtime_error("Streamline interposer signature validation failed");
        handle = LoadLibraryExW(p.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!handle)
            throw std::runtime_error("Cannot load " + p.filename().string() + ": " +
                                     std::to_string(GetLastError()));
        auto *base = reinterpret_cast<const unsigned char *>(handle);
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        diagnostics::event("module.load.end", 0, nt->FileHeader.TimeDateStamp,
                           nt->OptionalHeader.SizeOfImage, 0, 0, name.c_str());
    }
    ~Library() {
        if (handle) {
            const auto released = FreeLibrary(handle);
            HMODULE still_loaded = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(handle), &still_loaded);
            diagnostics::event("module.unload", 0, released ? 1 : 0, still_loaded ? 1 : 0,
                               0, 0, name.c_str());
        }
    }
    template <class F> F *get(const char *name) {
        auto f = reinterpret_cast<F *>(GetProcAddress(handle, name));
        if (!f)
            throw std::runtime_error(std::string("Missing FG runtime API: ") + name);
        return f;
    }
};
DXGI_SWAP_CHAIN_DESC1 chain_desc(const DXGI_SWAP_CHAIN_DESC &d, DXGI_FORMAT format) {
    DXGI_SWAP_CHAIN_DESC1 n{};
    n.Width = d.BufferDesc.Width;
    n.Height = d.BufferDesc.Height;
    n.Format = format;
    n.SampleDesc.Count = 1;
    n.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    n.BufferCount = 3;
    n.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    n.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    return n;
}
bool check(HRESULT hr, const char *what, std::string &error) {
    if (SUCCEEDED(hr))
        return true;
    error = std::string(what) + ": " + std::to_string(unsigned(hr));
    return false;
}
void teardown_failure(const char *what, int result) noexcept {
    try {
        report_failure(std::string(what) + " failed: result=" + std::to_string(result));
    } catch (...) {
        // Destruction cannot propagate an allocation/logging exception.
    }
}
// The non-interpolating path still owns the same private D3D12 output. This
// permits a failed live provider request to retain ordinary presentation
// without changing the game's D3D11 SDR carrier or ReShade effect runtime.
class Plain final : public Backend {
    std::uint64_t presents = 0;
    std::shared_ptr<Backend> latency;
    std::shared_ptr<Backend> output;
    ComPtr<ID3D12Device> latency_device;
    ComPtr<IDXGIFactory> latency_factory;
    std::atomic<bool> latency_started{false};
    bool latency_failed = false;

    void resume_latency() noexcept {
        if (!latency || latency_started || latency_failed)
            return;
        std::string error;
        try {
            latency_started = latency->start_latency(latency_device.Get(), latency_factory.Get(), error);
            if (!latency_started) {
                latency->stop();
                latency_failed = true;
                // Unsupported Anti-Lag on non-AMD hardware is an ordinary
                // capability outcome, not an FG rendering/retirement failure.
                report("Latency reduction unavailable during ordinary presentation: " + error);
            }
        } catch (const std::exception &e) {
            latency->stop();
            latency_failed = true;
            report_failure(std::string("Latency reduction unavailable during ordinary presentation: ") +
                           e.what());
        }
    }

  public:
    explicit Plain(std::shared_ptr<Backend> controller = {}, std::shared_ptr<Backend> renderer = {})
        : latency(std::move(controller)), output(std::move(renderer)) {
        if (output) {
            output->use_native_latency(false);
            output->set_present_markers([controller = latency](unsigned marker, std::uint64_t frame) {
                if (controller) controller->marker(marker, frame);
            });
        }
    }
    ~Plain() override {
        stop();
    }
    bool close(std::string &error) override {
        stop();
        if (output && !output->close(error)) return false;
        output.reset();
        if (latency && !latency->close(error)) return false;
        latency.reset(); latency_device.Reset(); latency_factory.Reset();
        diagnostics::event("ordinary.closed");
        return true;
    }
    bool create(ID3D12Device *d, IDXGIFactory *f, HWND window, const DXGI_SWAP_CHAIN_DESC &desc,
                DXGI_FORMAT fmt, ComPtr<ID3D12CommandQueue> &queue, ComPtr<IDXGISwapChain4> &out,
                std::string &error) override {
        if (output) {
            if (!output->create(d, f, window, desc, fmt, queue, out, error)) return false;
            output->retain_device(d, queue.Get());
            latency_device = d;
            latency_factory = f;
            resume_latency();
            return true;
        }
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!check(d->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "Ordinary presenting queue", error))
            return false;
        ComPtr<IDXGIFactory2> factory;
        if (!check(f->QueryInterface(IID_PPV_ARGS(&factory)), "Ordinary presentation factory", error))
            return false;
        const auto n = chain_desc(desc, fmt);
        ComPtr<IDXGISwapChain1> chain;
        if (!check(factory->CreateSwapChainForHwnd(queue.Get(), window, &n, nullptr, nullptr, &chain),
                   "Ordinary private swapchain", error) ||
            !check(chain.As(&out), "Ordinary swapchain interface", error))
            return false;
        latency_device = d;
        latency_factory = f;
        resume_latency();
        return true;
    }
    bool prepare(ID3D12GraphicsCommandList *cmd, const Packet &packet, unsigned factor, bool enabled, std::string &error) override {
        // Resize/drain retires latency calls as well as GPU work. Restore the
        // selected controller without creating any FG resources.
        resume_latency();
        return !output || output->prepare(cmd, packet, factor, enabled, error);
    }
    void marker(unsigned m, std::uint64_t frame) override {
        if (output && output->asynchronous_presentation() && (m == 4 || m == 5)) return;
        if (latency_started)
            latency->marker(m, frame);
    }
    void sleep(std::uint64_t frame) override {
        if (latency_started)
            latency->sleep(frame);
    }
    bool after_present(ID3D12CommandQueue *queue, std::string &error) override {
        if (output) return output->after_present(queue, error);
        ++presents;
        return true;
    }
    void stop() override {
        if (output) output->stop();
        if (latency)
            latency->stop();
        latency_started = false;
    }
    bool latency_available() const override {
        return latency_started && latency->latency_available();
    }
    unsigned maximum() const override {
        return output ? output->maximum() : 1;
    }
    std::uint64_t displayed() const override {
        return output ? output->displayed() : presents;
    }
    unsigned last_presented() const override {
        return output ? output->last_presented() : 1;
    }
    bool native_dynamic_supported() const override { return output && output->native_dynamic_supported(); }
    HRESULT present(IDXGISwapChain4 *chain, unsigned interval, unsigned flags) override {
        return output ? output->present(chain, interval, flags) : chain->Present(interval, flags);
    }
    HRESULT backbuffer(IDXGISwapChain4 *chain, ComPtr<ID3D12Resource> &image) override {
        return output ? output->backbuffer(chain, image)
                      : chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&image));
    }
    bool drain_output(unsigned timeout, std::string &error) override {
        return !output || output->drain_output(timeout, error);
    }
    bool asynchronous_presentation() const override { return output && output->asynchronous_presentation(); }
    void release_output() override { if (output) output->release_output(); }
    bool resize(unsigned w, unsigned h, std::string &error) override {
        return !output || output->resize(w, h, error);
    }
    std::string latency_name() const override {
        return !latency ? "Disabled" : latency_started ? latency->latency_name() : "Unavailable";
    }
};

struct StreamlineLease {
    static std::atomic<bool> &occupied() {
        static auto *value = new std::atomic<bool>{false};
        return *value;
    }
    StreamlineLease() {
        bool expected = false;
        if (!occupied().compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            throw std::runtime_error("The previous Streamline provider is still retiring");
    }
    ~StreamlineLease() {
        occupied().store(false, std::memory_order_release);
    }
    StreamlineLease(const StreamlineLease &) = delete;
    StreamlineLease &operator=(const StreamlineLease &) = delete;
};
constexpr bool unavailable_interpolation_result(sl::Result result) {
    return result == sl::Result::eErrorFeatureMissing || result == sl::Result::eErrorFeatureFailedToLoad ||
           result == sl::Result::eErrorFeatureNotSupported ||
           result == sl::Result::eErrorNoSupportedAdapterFound;
}
constexpr bool never_available_interpolation_is_unloaded(bool requested_loaded, bool created_output,
                                                         sl::Result change, sl::Result query) {
    // Streamline 2.14.16 rejects even an unload request when DLSS-G was
    // unavailable at startup. Only its documented unavailable outcomes are
    // harmless, and only before this owner has ever created a DLSS-G chain.
    return !requested_loaded && !created_output && unavailable_interpolation_result(change) &&
           (query == sl::Result::eOk || unavailable_interpolation_result(query));
}
static_assert(never_available_interpolation_is_unloaded(false, false,
                                                        sl::Result::eErrorNoSupportedAdapterFound,
                                                        sl::Result::eOk));
static_assert(never_available_interpolation_is_unloaded(false, false, sl::Result::eErrorFeatureFailedToLoad,
                                                        sl::Result::eErrorFeatureMissing));
static_assert(!never_available_interpolation_is_unloaded(true, false, sl::Result::eErrorFeatureNotSupported,
                                                         sl::Result::eErrorFeatureNotSupported));
static_assert(!never_available_interpolation_is_unloaded(false, true, sl::Result::eErrorFeatureFailedToLoad,
                                                         sl::Result::eErrorFeatureMissing));
static_assert(!never_available_interpolation_is_unloaded(false, false,
                                                         sl::Result::eErrorNoSupportedAdapterFound,
                                                         sl::Result::eErrorNotInitialized));
static_assert(!never_available_interpolation_is_unloaded(false, false, sl::Result::eErrorInvalidParameter,
                                                         sl::Result::eOk));
class Streamline final : public Backend {
    // slInit/slShutdown describe a process-wide SDK instance. An in-flight
    // latency lease may retain the preceding owner during a handoff; reject
    // overlap rather than initializing a second instance or blocking Present.
    StreamlineLease lifetime;
    Library lib;
    bool initialized = false;
    PFun_slShutdown *shutdown = nullptr;
    PFun_slGetFeatureFunction *get = nullptr;
    PFun_slGetNewFrameToken *get_token = nullptr;
    PFun_slSetConstants *constants = nullptr;
    PFun_slSetTagForFrame *tag = nullptr;
    PFun_slFreeResources *free_resources = nullptr;
    PFun_slSetFeatureLoaded *set_feature_loaded = nullptr;
    PFun_slIsFeatureLoaded *is_feature_loaded = nullptr;
    PFun_slDLSSGSetOptions *set = nullptr;
    PFun_slDLSSGGetState *state = nullptr;
    PFun_slReflexSetOptions *reflex = nullptr;
    PFun_slReflexSleep *reflex_sleep = nullptr;
    PFun_slPCLSetMarker *mark = nullptr;
    ComPtr<ID3D12Device> proxy_device;
    ComPtr<IDXGIFactory2> proxy_factory;
    ComPtr<ID3D12Device> bound_device;
    ComPtr<IUnknown> bound_factory;
    bool bind_attempted = false, device_ready = false, device_primed = false;
    bool interpolation_loaded = true, interpolation_api_ready = false;
    bool interpolation_output_created = false;
    sl::Result bind_result = sl::Result::eErrorDeviceNotCreated;
    std::uint64_t presents = 0;
    unsigned last_count = 1;
    bool running = false, reflex_on = false, supports_dynamic = false;
    sl::DLSSGOptions last_options{};
    bool have_options = false;
    sl::ViewportHandle viewport{0};
    unsigned limit = 2;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DirectX::XMFLOAT4X4 previous{};
    bool have_previous = false;
    const void *resolution_source = nullptr;
    unsigned resolution_output_width = 0, resolution_output_height = 0;
    unsigned dynamic_resolution_width = 0, dynamic_resolution_height = 0;
    FrameTokens<sl::FrameToken, sl::MAX_FRAMES_IN_FLIGHT> frame_tokens;
    template <class F> F *feature(sl::Feature f, const char *name) {
        void *out = nullptr;
        if (get(f, name, out) != sl::Result::eOk || !out)
            throw std::runtime_error(std::string("Streamline function unavailable: ") + name);
        return reinterpret_cast<F *>(out);
    }
    void clear_interpolation_functions() {
        set = nullptr;
        state = nullptr;
        interpolation_api_ready = false;
        have_previous = false;
        running = false;
        have_options = false;
        dynamic_resolution_width = dynamic_resolution_height = 0;
    }
    void acquire_interpolation_functions() {
        // Plugin entry points cannot survive slSetFeatureLoaded(false), even
        // though the interposer, native device, Reflex and PCL remain alive.
        auto *next_set = feature<PFun_slDLSSGSetOptions>(sl::kFeatureDLSS_G, "slDLSSGSetOptions");
        auto *next_state = feature<PFun_slDLSSGGetState>(sl::kFeatureDLSS_G, "slDLSSGGetState");
        set = next_set;
        state = next_state;
        interpolation_api_ready = true;
    }

  public:
    explicit Streamline(const std::filesystem::path &root) : lib(root / L"sl.interposer.dll") {
        auto init = lib.get<PFun_slInit>("slInit");
        shutdown = lib.get<PFun_slShutdown>("slShutdown");
        get = lib.get<PFun_slGetFeatureFunction>("slGetFeatureFunction");
        get_token = lib.get<PFun_slGetNewFrameToken>("slGetNewFrameToken");
        constants = lib.get<PFun_slSetConstants>("slSetConstants");
        tag = lib.get<PFun_slSetTagForFrame>("slSetTagForFrame");
        free_resources = lib.get<PFun_slFreeResources>("slFreeResources");
        set_feature_loaded = lib.get<PFun_slSetFeatureLoaded>("slSetFeatureLoaded");
        is_feature_loaded = lib.get<PFun_slIsFeatureLoaded>("slIsFeatureLoaded");
        const auto path = root.wstring();
        const wchar_t *paths[]{path.c_str()};
        const sl::Feature features[]{sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
        sl::Preferences p{};
        p.pathsToPlugins = paths;
        p.numPathsToPlugins = 1;
        p.pathToLogsAndData = path.c_str();
        p.featuresToLoad = features;
        p.numFeaturesToLoad = 3;
        p.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseDXGIFactoryProxy |
                  sl::PreferenceFlags::eUseFrameBasedResourceTagging |
                  sl::PreferenceFlags::eDisableCLStateTracking;
        p.engine = sl::EngineType::eCustom;
        p.engineVersion = "XIVRender-0.4.16";
        p.projectId = "be4ee394-a407-4b26-82c1-74250a367f68";
        p.renderAPI = sl::RenderAPI::eD3D12;
        p.logMessageCallback = [](sl::LogType t, const char *s) {
            // SDK callbacks may run on internal threads or reenter an SDK
            // call. Never take the renderer/status mutex from this callback.
            diagnostics::event("sdk.streamline", 0, unsigned(t), 0, 0, 0, s);
        };
        const auto r = init(p, sl::kSDKVersion);
        if (r != sl::Result::eOk)
            throw std::runtime_error("Streamline initialization failed: " + std::to_string(int(r)));
        initialized = true;
        diagnostics::event("streamline.initialized");
    }
    ~Streamline() {
        std::string error;
        if (!close(error)) report_failure(error);
    }
    bool close(std::string &error) override {
        if (!initialized) return true;
        diagnostics::event("streamline.shutdown.begin");
        stop();
        proxy_factory.Reset();
        proxy_device.Reset();
        if (initialized) {
            const auto result = shutdown();
            diagnostics::event("streamline.shutdown.end", 0, int(result));
            if (result != sl::Result::eOk) {
                error = "Streamline shutdown failed: result=" + std::to_string(int(result));
                return false;
            }
            initialized = false;
        }
        interpolation_loaded = false;
        reflex = nullptr; reflex_sleep = nullptr; mark = nullptr;
        clear_interpolation_functions();
        bound_device.Reset(); bound_factory.Reset();
        frame_tokens.clear();
        return true;
    }
    bool prepare_device(ID3D12Device *d, IDXGIFactory *f, std::string &error) override try {
        // An SDK instance binds exactly one private device. Provider changes
        // destroy both owners; never rebind a live SDK to a different device.
        if (bound_device && bound_device.Get() != d) {
            error = "Streamline output requested a different device; its original device remains bound";
            return false;
        }
        if (!bind_attempted) {
            bind_attempted = true;
            auto bind = lib.get<PFun_slSetD3DDevice>("slSetD3DDevice");
            int result = int(sl::Result::eErrorInvalidIntegration);
            diagnostics::event("streamline.bind.begin");
            if (!bind_streamline_ngx([&] { return int(bind(d)); }, result, error)) {
                diagnostics::event("streamline.bind.end", 0, result, 0, 0, 0, error.c_str());
                bind_result = sl::Result::eErrorInvalidIntegration;
                return false;
            }
            bind_result = static_cast<sl::Result>(result);
            diagnostics::event("streamline.bind.end", 0, result);
            if (bind_result == sl::Result::eOk)
                bound_device = d;
        }
        if (bind_result != sl::Result::eOk) {
            error = "Streamline rejected private FG device: result=" + std::to_string(int(bind_result));
            return false;
        }
        if (!device_ready) {
            reflex = feature<PFun_slReflexSetOptions>(sl::kFeatureReflex, "slReflexSetOptions");
            reflex_sleep = feature<PFun_slReflexSleep>(sl::kFeatureReflex, "slReflexSleep");
            mark = feature<PFun_slPCLSetMarker>(sl::kFeaturePCL, "slPCLSetMarker");
            auto upgrade = lib.get<PFun_slUpgradeInterface>("slUpgradeInterface");
            proxy_device = d;
            const auto result = upgrade(reinterpret_cast<void **>(proxy_device.GetAddressOf()));
            if (result != sl::Result::eOk) {
                proxy_device.Reset();
                error = "Streamline device proxy failed: result=" + std::to_string(int(result));
                return false;
            }
            device_ready = true;
        }
        ComPtr<IUnknown> factory_identity;
        if (FAILED(f->QueryInterface(IID_PPV_ARGS(&factory_identity)))) {
            error = "Streamline factory identity is unavailable";
            return false;
        }
        if (bound_factory && bound_factory.Get() != factory_identity.Get()) {
            error = "Streamline output requested a different presentation factory";
            return false;
        }
        if (!proxy_factory) {
            auto upgrade = lib.get<PFun_slUpgradeInterface>("slUpgradeInterface");
            if (FAILED(f->QueryInterface(IID_PPV_ARGS(&proxy_factory)))) {
                error = "Streamline factory interface is unavailable";
                return false;
            }
            const auto result = upgrade(reinterpret_cast<void **>(proxy_factory.GetAddressOf()));
            if (result != sl::Result::eOk) {
                proxy_factory.Reset();
                error = "Streamline factory proxy failed: result=" + std::to_string(int(result));
                return false;
            }
            bound_factory = factory_identity;
        }
        if (!device_primed) {
            sl::ReflexOptions ro{};
            ro.mode = sl::ReflexMode::eOff;
            ro.frameLimitUs = 0;
            ro.useMarkersToOptimize = false;
            const auto result = reflex(ro);
            if (result != sl::Result::eOk) {
                error = "Reflex device priming failed: result=" + std::to_string(int(result));
                return false;
            }
            reflex_on = false;
            retain_device(d, nullptr);
            device_primed = true;
        }
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
    bool set_interpolation_loaded(bool loaded, std::string &error) override try {
        if (interpolation_loaded != loaded) {
            if (running) {
                error = "Retire the DLSS-G output before changing its plugin hooks";
                return false;
            }
            // Streamline DLSS-G guide section 18 requires destroying the old
            // swapchain and unloading this feature before a native/non-DLSS-G
            // swapchain is created. eOff alone leaves presentation hooks live.
            bool actual_loaded = false;
            const auto query = is_feature_loaded(sl::kFeatureDLSS_G, actual_loaded);
            if (!loaded && !interpolation_output_created && query == sl::Result::eOk && !actual_loaded) {
                interpolation_loaded = false;
                clear_interpolation_functions();
                return true;
            }
            const auto result = set_feature_loaded(sl::kFeatureDLSS_G, loaded);
            if (result != sl::Result::eOk && !never_available_interpolation_is_unloaded(
                                                 loaded, interpolation_output_created, result, query)) {
                error = std::string(loaded ? "DLSS-G plugin loading" : "DLSS-G plugin unloading") +
                        " failed: result=" + std::to_string(int(result));
                return false;
            }
            interpolation_loaded = loaded;
            clear_interpolation_functions();
            // Capabilities belong to this retained SDK/device. In particular,
            // a known positive dynamic-MFG capability survives hook toggles.
        }
        // A direct backend user can enable the feature before device setup;
        // resolve its APIs once the device is ready in create(). Reflex-only
        // startup deliberately performs no DLSS-G feature-function lookups.
        if (interpolation_loaded && device_ready && !interpolation_api_ready)
            acquire_interpolation_functions();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
    bool start_latency(ID3D12Device *d, IDXGIFactory *f, std::string &error) override {
        if (!prepare_device(d, f, error))
            return false;
        if (reflex_on)
            return true;
        sl::ReflexOptions ro{};
        ro.mode = sl::ReflexMode::eLowLatency;
        ro.frameLimitUs = 0;
        ro.useMarkersToOptimize = false;
        const auto result = reflex(ro);
        if (result != sl::Result::eOk) {
            error = "Reflex initialization failed: result=" + std::to_string(int(result));
            return false;
        }
        frame_tokens.clear();
        reflex_on = true;
        retain_device(d, nullptr);
        return true;
    }
    bool create(ID3D12Device *d, IDXGIFactory *f, HWND window, const DXGI_SWAP_CHAIN_DESC &desc,
                DXGI_FORMAT fmt, ComPtr<ID3D12CommandQueue> &queue, ComPtr<IDXGISwapChain4> &chain,
                std::string &error) override {
        format = fmt;
        if (!prepare_device(d, f, error) || !set_interpolation_loaded(true, error))
            return false;
        // SDK lifetime continues, but each output gets fresh counters and
        // temporal history. Global presentation totals are owned by Presenter.
        presents = 0;
        last_count = 1;
        have_previous = false;
        frame_tokens.clear();
        dynamic_resolution_width = dynamic_resolution_height = 0;
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!check(proxy_device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "Streamline presenting queue",
                   error))
            return false;
        ComPtr<IDXGISwapChain1> c;
        auto n = chain_desc(desc, fmt);
        if (!check(proxy_factory->CreateSwapChainForHwnd(queue.Get(), window, &n, nullptr, nullptr, &c),
                   "Streamline swapchain", error))
            return false;
        interpolation_output_created = true;
        if (FAILED(c.As(&chain)))
            return false;
        if (!start_latency(d, f, error))
            return false;
        sl::DLSSGState st{};
        if (state(viewport, st, nullptr) != sl::Result::eOk) {
            error = "DLSS-G capability query failed";
            return false;
        }
        limit = std::clamp(st.numFramesToGenerateMax + 1, 2u, 6u);
        // This capability belongs to the retained device/runtime. An output's
        // ordinary warmup/off state must not erase an earlier positive query.
        supports_dynamic = supports_dynamic || st.bIsDynamicMFGSupported == sl::eTrue;
        return true;
    }
    bool prepare(ID3D12GraphicsCommandList *cmd, const Packet &p, unsigned factor, bool enabled,
                 std::string &error) override {
        if (!interpolation_loaded || !interpolation_api_ready) {
            error = "DLSS-G frame preparation requested while its plugin is unloaded";
            return false;
        }
        auto *token = frame_tokens.find(p.frame);
        // A live provider handoff happens after simulation. Its first output
        // has no simulation-start token; keep that frame ordinary, just as XeLL
        // does for a frame that did not pass its sleep/marker boundary.
        enabled = enabled && token;
        sl::DLSSGOptions o{};
        o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        o.onErrorCallback = [](const sl::APIError &e) {
            diagnostics::event("sdk.dxgi_error", 0, unsigned(e.hres));
        };
        const bool native_dynamic = p.dynamic && supports_dynamic;
        o.mode = !enabled         ? sl::DLSSGMode::eOff
                 : native_dynamic ? sl::DLSSGMode::eDynamic
                                  : sl::DLSSGMode::eOn;
        o.dynamicTargetFrameRate = native_dynamic ? float(p.target_fps) : 0.f;
        o.numFramesToGenerate = std::clamp(factor, 2u, limit) - 1;
        o.numBackBuffers = 3;
        o.mvecDepthWidth = p.render_width;
        o.mvecDepthHeight = p.render_height;
        o.colorWidth = p.width;
        o.colorHeight = p.height;
        o.colorBufferFormat = unsigned(format);
        o.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
        o.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
        o.hudLessBufferFormat = unsigned(format);
        if (enabled) {
            if (p.variable_resolution) {
                if (!p.render_width || !p.render_height) {
                    error = "DLSS-G dynamic resolution requires a valid native guide extent";
                    return false;
                }
                // Processing resolution must remain fixed while native DRS
                // moves the tagged subrect. Choose its first valid extent;
                // only a new native feature, mode or output starts a new range.
                if (!dynamic_resolution_width || !dynamic_resolution_height ||
                    resolution_source != p.source_feature || resolution_output_width != p.width ||
                    resolution_output_height != p.height) {
                    dynamic_resolution_width = p.render_width;
                    dynamic_resolution_height = p.render_height;
                    resolution_source = p.source_feature;
                    resolution_output_width = p.width;
                    resolution_output_height = p.height;
                }
            } else {
                dynamic_resolution_width = dynamic_resolution_height = 0;
            }
        }
        // MFG's dynamic multiplier and FFXIV's dynamic render resolution are
        // independent. Fixed addon quality ratios leave DRS disabled.
        if (dynamic_resolution_width && dynamic_resolution_height && (p.variable_resolution || !enabled)) {
            o.flags |= sl::DLSSGFlags::eDynamicResolutionEnabled;
            o.dynamicResWidth = dynamic_resolution_width;
            o.dynamicResHeight = dynamic_resolution_height;
        }
        if (!enabled && have_options) {
            // Missing/coalesced native inputs suspend one frame. Preserve the
            // dimensions and resources; eOff without Retain frees the model
            // and creates an allocation stutter on the next valid frame.
            o = last_options;
            o.mode = sl::DLSSGMode::eOff;
            o.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
        }
        if (enabled || running) {
            const auto result = set(viewport, o);
            if (!have_options || o.mode != last_options.mode ||
                o.numFramesToGenerate != last_options.numFramesToGenerate ||
                o.dynamicTargetFrameRate != last_options.dynamicTargetFrameRate || result != sl::Result::eOk)
            {
                diagnostics::event("dlssg.options", p.frame, unsigned(o.mode), o.numFramesToGenerate,
                                   o.dynamicTargetFrameRate, int(result));
                diagnostics::event("dlssg.resources", p.frame, unsigned(o.flags), o.mvecDepthWidth,
                                   o.mvecDepthHeight, o.dynamicResWidth,
                                   "Temporary off retains the preceding dimensions and allocations");
            }
            if (result != sl::Result::eOk) {
                error = "DLSS-G options rejected: result=" + std::to_string(int(result));
                return false;
            }
            last_options = o;
            have_options = true;
        }
        running = enabled;
        if (!enabled) {
            have_previous = false;
            return true;
        }
        if (!reflex_on) {
            sl::ReflexOptions r{};
            r.mode = sl::ReflexMode::eLowLatency;
            r.useMarkersToOptimize = false;
            if (reflex(r) != sl::Result::eOk) {
                error = "Reflex resume failed";
                return false;
            }
            reflex_on = true;
        }
        using namespace DirectX;
        auto view = XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4 *>(p.camera.view.data()));
        auto proj = XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4 *>(p.camera.projection.data()));
        auto vp = XMMatrixMultiply(view, proj);
        auto inv_view = XMMatrixInverse(nullptr, view);
        XMFLOAT4X4 now{}, iv{};
        XMStoreFloat4x4(&now, vp);
        XMStoreFloat4x4(&iv, inv_view);
        sl::Constants c{};
        std::memcpy(&c.cameraViewToClip, p.camera.projection.data(), 64);
        XMFLOAT4X4 temp{};
        XMStoreFloat4x4(&temp, XMMatrixInverse(nullptr, proj));
        std::memcpy(&c.clipToCameraView, &temp, 64);
        auto to_previous = have_previous
                               ? XMMatrixMultiply(XMMatrixInverse(nullptr, vp), XMLoadFloat4x4(&previous))
                               : XMMatrixIdentity();
        XMStoreFloat4x4(&temp, to_previous);
        std::memcpy(&c.clipToPrevClip, &temp, 64);
        XMStoreFloat4x4(&temp, XMMatrixInverse(nullptr, to_previous));
        std::memcpy(&c.prevClipToClip, &temp, 64);
        c.cameraPinholeOffset = {0, 0};
        c.jitterOffset = {p.jitter_x, p.jitter_y};
        c.mvecScale = {1.f / p.render_width, 1.f / p.render_height};
        c.cameraPos = {iv._41, iv._42, iv._43};
        c.cameraRight = {iv._11, iv._12, iv._13};
        c.cameraUp = {iv._21, iv._22, iv._23};
        c.cameraFwd = {iv._31, iv._32, iv._33};
        c.cameraNear = p.camera.near_plane;
        c.cameraFar = p.camera.infinite ? std::numeric_limits<float>::infinity() : p.camera.far_plane;
        c.cameraFOV = p.camera.fov;
        c.cameraAspectRatio = p.camera.aspect;
        c.depthInverted = p.camera.inverted ? sl::eTrue : sl::eFalse;
        c.cameraMotionIncluded = sl::eTrue;
        c.motionVectors3D = sl::eFalse;
        c.reset = (p.reset || !have_previous) ? sl::eTrue : sl::eFalse;
        c.orthographicProjection = sl::eFalse;
        c.motionVectorsDilated = sl::eFalse;
        c.motionVectorsJittered = sl::eFalse;
        if (constants(c, *token, viewport) != sl::Result::eOk) {
            error = "DLSS-G common constants rejected";
            return false;
        }
        sl::Resource d{sl::ResourceType::eTex2d, p.depth, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON};
        sl::Resource m{sl::ResourceType::eTex2d, p.motion, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON};
        sl::Resource h{sl::ResourceType::eTex2d, p.hudless, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON};
        sl::Resource b{sl::ResourceType::eTex2d, p.backbuffer, nullptr, nullptr,
                       D3D12_RESOURCE_STATE_PRESENT};
        sl::Extent extent{0, 0, p.render_width, p.render_height}, full{0, 0, p.width, p.height};
        const sl::ResourceTag tags[]{
            {&d, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent},
            {&m, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent},
            {&h, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &full},
            {&b, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &full}};
        if (tag(*token, viewport, tags, 4, cmd) != sl::Result::eOk) {
            error = "DLSS-G resource tags rejected";
            return false;
        }
        previous = now;
        have_previous = true;
        return true;
    }
    void marker(unsigned m, std::uint64_t id) override {
        if (mark) {
            auto *token = frame_tokens.find(id);
            if (token) {
                const auto result = mark(static_cast<sl::PCLMarker>(m), *token);
                if (result != sl::Result::eOk)
                    diagnostics::event("reflex.marker_error", id, m, int(result));
            } else diagnostics::event("reflex.missing_token", id, m);
        }
    }
    void sleep(std::uint64_t id) override {
        if (reflex_sleep) {
            const auto token = frame_tokens.issue(id, [&](std::uint32_t frame) {
                sl::FrameToken *issued = nullptr;
                const auto result = get_token(issued, &frame);
                if (result != sl::Result::eOk)
                    report("Streamline simulation token failed: result=" + std::to_string(int(result)));
                return result == sl::Result::eOk ? issued : nullptr;
            });
            if (token) {
                const auto result = reflex_sleep(*token);
                if (result != sl::Result::eOk)
                    diagnostics::event("reflex.sleep_error", id, int(result));
            }
        }
    }
    bool after_present(ID3D12CommandQueue *q, std::string &error) override {
        if (!interpolation_loaded || !interpolation_api_ready) {
            error = "DLSS-G present status requested while its plugin is unloaded";
            return false;
        }
        sl::DLSSGState s{};
        if (state(viewport, s, nullptr) != sl::Result::eOk) {
            error = "DLSS-G status query failed";
            return false;
        }
        last_count = s.numFramesActuallyPresented;
        diagnostics::event("dlssg.state", 0, last_count, unsigned(s.status),
                           double(s.lastPresentInputsProcessingCompletionFenceValue), s.numFramesToGenerateMax);
        presents += last_count;
        supports_dynamic = supports_dynamic || s.bIsDynamicMFGSupported == sl::eTrue;
        if (s.inputsProcessingCompletionFence && s.lastPresentInputsProcessingCompletionFenceValue)
            if (!check(q->Wait(static_cast<ID3D12Fence *>(s.inputsProcessingCompletionFence),
                               s.lastPresentInputsProcessingCompletionFenceValue),
                       "DLSS-G input retirement", error))
                return false;
        if (s.status != sl::DLSSGStatus::eOk) {
            error = "DLSS-G status bits=" + std::to_string(unsigned(s.status));
            return false;
        }
        return true;
    }
    void stop() override {
        if (interpolation_loaded && set && running) {
            sl::DLSSGOptions o{};
            o.mode = sl::DLSSGMode::eOff;
            const auto result = set(viewport, o);
            diagnostics::event(result == sl::Result::eOk ? "dlssg.stop" : "api.failure", 0,
                               int(result), 0, 0, 0, "slDLSSGSetOptions(Off)");
        }
        running = false;
        if (reflex && reflex_on) {
            sl::ReflexOptions r{};
            r.mode = sl::eOff;
            const auto result = reflex(r);
            diagnostics::event(result == sl::Result::eOk ? "reflex.stop" : "api.failure", 0,
                               int(result), 0, 0, 0, "slReflexSetOptions(Off)");
            reflex_on = false;
        }
        have_previous = false;
    }
    void release_output() override {
        stop();
        if (interpolation_loaded && device_ready && free_resources) {
            const auto result = free_resources(sl::kFeatureDLSS_G, viewport);
            diagnostics::event(result == sl::Result::eOk ? "dlssg.resources_freed" : "api.failure", 0,
                               int(result), 0, 0, 0, "slFreeResources(DLSS-G)");
            if (result != sl::Result::eOk)
                teardown_failure("DLSS-G output resource retirement", int(result));
        }
        release_presenting_queue();
        have_previous = false;
    }
    unsigned maximum() const override {
        return limit;
    }
    bool native_dynamic_supported() const override {
        return supports_dynamic;
    }
    std::uint64_t displayed() const override {
        return presents;
    }
    unsigned last_presented() const override {
        return last_count;
    }
    std::string latency_name() const override {
        return "NVIDIA Reflex";
    }
};
class Xe final : public Backend {
    Library fg, ll;
    xefg_swapchain_handle_t context = nullptr;
    xell_context_handle_t latency = nullptr;
    unsigned limit = 2, last_count = 1;
    std::uint64_t presents = 0;
    IDXGISwapChain4 *presenting_chain = nullptr; // Owned by the XeFG context.
    decltype(&xefgSwapChainSetEnabled) enable = nullptr;
    decltype(&xefgSwapChainSetNumInterpolatedFrames) count = nullptr;
    decltype(&xefgSwapChainTagFrameConstants) constants = nullptr;
    decltype(&xefgSwapChainD3D12TagFrameResource) tag = nullptr;
    decltype(&xefgSwapChainSetPresentId) present_id = nullptr;
    decltype(&xefgSwapChainGetLastPresentStatus) state = nullptr;
    decltype(&xellSleep) do_sleep = nullptr;
    decltype(&xellAddMarkerData) mark = nullptr;
    decltype(&xellSetSleepMode) sleep_mode = nullptr;
    std::atomic<bool> latency_on{false};
    // Simulation/render phases may overlap. Retain a small set of completed
    // sleeps rather than only the newest ID, and invalidate it after stop.
    std::array<std::atomic<std::uint64_t>, 64> slept_frames{};
    std::atomic<std::uint32_t> latency_epoch{1};
    bool unlocked = false;
    bool pacing_active = false;
    unsigned pacing_factor = 0;
    bool slept(std::uint64_t id) const {
        const auto epoch = latency_epoch.load(std::memory_order_acquire);
        const auto stamp = (std::uint64_t(epoch) << 32) | unsigned(id);
        return id && slept_frames[id % slept_frames.size()].load(std::memory_order_acquire) == stamp;
    }
    template <class F> auto api(const char *n) {
        return fg.get<F>(n);
    }

  public:
    Xe(const std::filesystem::path &root, bool unlock)
        : fg(root / L"fg/libxess_fg.dll"), ll(root / L"fg/libxell.dll") {
        std::string error;
        unlocked = unlock && unlock_xefg(fg.handle, error);
        if (unlock && !unlocked)
            report(error);
    }
    ~Xe() {
        std::string error;
        if (!close(error)) report_failure(error);
    }
    bool close(std::string &error) override {
        stop();
        if (context) {
            const auto result = fg.get<decltype(xefgSwapChainDestroy)>("xefgSwapChainDestroy")(context);
            diagnostics::event("xefg.destroy", 0, int(result));
            if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                error = "XeFG context teardown failed: result=" + std::to_string(int(result));
                return false;
            }
            context = nullptr; presenting_chain = nullptr; enable = nullptr;
        }
        if (latency) {
            const auto result = ll.get<decltype(xellDestroyContext)>("xellDestroyContext")(latency);
            diagnostics::event("xell.destroy", 0, int(result));
            if (result != XELL_RESULT_SUCCESS) {
                error = "XeLL context teardown failed: result=" + std::to_string(int(result));
                return false;
            }
            latency = nullptr; sleep_mode = nullptr; do_sleep = nullptr; mark = nullptr;
        }
        if (unlocked) {
            if (!restore_xefg(fg.handle, error)) return false;
            unlocked = false;
            diagnostics::event("xefg.hooks_restored");
        }
        return true;
    }
    bool start_latency(ID3D12Device *d, IDXGIFactory *, std::string &error) override {
        if (!latency) {
            const auto result =
                ll.get<decltype(xellD3D12CreateContext)>("xellD3D12CreateContext")(d, &latency);
            if (result != XELL_RESULT_SUCCESS) {
                error = "XeLL context creation failed: " + std::to_string(int(result));
                return false;
            }
            sleep_mode = ll.get<decltype(xellSetSleepMode)>("xellSetSleepMode");
            do_sleep = ll.get<decltype(xellSleep)>("xellSleep");
            mark = ll.get<decltype(xellAddMarkerData)>("xellAddMarkerData");
            const auto logging = ll.get<decltype(xellSetLoggingCallback)>("xellSetLoggingCallback")(
                latency, XELL_LOGGING_LEVEL_INFO, [](const char *text, xell_logging_level_t level) {
                    diagnostics::event("sdk.xell", 0, unsigned(level), 0, 0, 0, text);
                });
            diagnostics::event("xell.logging", 0, int(logging));
            retain_device(d, nullptr);
        }
        if (latency_on.load(std::memory_order_acquire))
            return true;
        xell_sleep_params_t sleep{};
        sleep.bLowLatencyMode = 1;
        const auto result = sleep_mode(latency, &sleep);
        if (result != XELL_RESULT_SUCCESS) {
            error = "XeLL sleep mode failed: " + std::to_string(int(result));
            return false;
        }
        latency_on.store(true, std::memory_order_release);
        return true;
    }
    bool create(ID3D12Device *d, IDXGIFactory *f, HWND window, const DXGI_SWAP_CHAIN_DESC &desc,
                DXGI_FORMAT fmt, ComPtr<ID3D12CommandQueue> &queue, ComPtr<IDXGISwapChain4> &chain,
                std::string &error) override {
#define XE_CALL(name, ...)                                                                                   \
    do {                                                                                                     \
        const auto result = fg.get<decltype(name)>(#name)(__VA_ARGS__);                                      \
        if (result < 0) {                                                                                    \
            error = #name " failed: " + std::to_string(int(result));                                         \
            return false;                                                                                    \
        }                                                                                                    \
    } while (false)
        XE_CALL(xefgSwapChainD3D12CreateContext, d, &context);
        if (!start_latency(d, f, error))
            return false;
        XE_CALL(xefgSwapChainSetLatencyReduction, context, latency);
        XE_CALL(
            xefgSwapChainSetLoggingCallback, context, XEFG_SWAPCHAIN_LOGGING_LEVEL_INFO,
            [](const char *s, xefg_swapchain_logging_level_t level, void *) {
                diagnostics::event("sdk.xefg", 0, unsigned(level), 0, 0, 0, s);
            },
            nullptr);
        xefg_swapchain_properties_t properties{};
        XE_CALL(xefgSwapChainGetProperties, context, &properties);
        limit = std::clamp(properties.maxSupportedInterpolations + 1, 2u, 4u);
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!check(d->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "XeFG presenting queue", error))
            return false;
        xefg_swapchain_d3d12_init_params_t p{};
        // Let XeFG retain its descriptors for the lifetime of queued GPU work.
        // A single external region at offset zero was reused by every frame,
        // even while preceding interpolation could still reference it.
        p.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;
        p.maxInterpolatedFrames = limit - 1;
        p.uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS;
        auto n = chain_desc(desc, fmt);
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(f->QueryInterface(IID_PPV_ARGS(&factory))))
            return false;
        XE_CALL(xefgSwapChainD3D12InitFromSwapChainDesc, context, window, &n, nullptr, queue.Get(),
                factory.Get(), &p);
        XE_CALL(xefgSwapChainD3D12GetSwapChainPtr, context, __uuidof(IDXGISwapChain4),
                reinterpret_cast<void **>(chain.GetAddressOf()));
        presenting_chain = chain.Get();
        XE_CALL(xefgSwapChainGetProperties, context, &properties);
        enable = fg.get<decltype(xefgSwapChainSetEnabled)>("xefgSwapChainSetEnabled");
        count =
            fg.get<decltype(xefgSwapChainSetNumInterpolatedFrames)>("xefgSwapChainSetNumInterpolatedFrames");
        constants = fg.get<decltype(xefgSwapChainTagFrameConstants)>("xefgSwapChainTagFrameConstants");
        tag = fg.get<decltype(xefgSwapChainD3D12TagFrameResource)>("xefgSwapChainD3D12TagFrameResource");
        present_id = fg.get<decltype(xefgSwapChainSetPresentId)>("xefgSwapChainSetPresentId");
        state = fg.get<decltype(xefgSwapChainGetLastPresentStatus)>("xefgSwapChainGetLastPresentStatus");
        return true;
#undef XE_CALL
    }
    bool prepare(ID3D12GraphicsCommandList *cmd, const Packet &p, unsigned factor, bool active,
                 std::string &error) override {
        BOOL exclusive = FALSE;
        if (presenting_chain && SUCCEEDED(presenting_chain->GetFullscreenState(&exclusive, nullptr)) &&
            exclusive)
            active = false;
        // This context was created for FFXIV's reverse-Z camera. Never feed
        // an unrelated forward-Z camera to it (for example an auxiliary UI).
        active = active && p.camera.inverted;
        // Intel requires XeLL to be enabled BEFORE enabling interpolation.
        // Restoring it after SetEnabled(true) creates a permanent rejection
        // loop following drain(), which disables the previous latency mode.
        if (active && !latency_on.load(std::memory_order_acquire)) {
            xell_sleep_params_t sleep{};
            sleep.bLowLatencyMode = 1;
            const auto result = sleep_mode ? sleep_mode(latency, &sleep) : XELL_RESULT_ERROR_UNINITIALIZED;
            if (result != XELL_RESULT_SUCCESS) {
                error = "XeLL resume failed: " + std::to_string(int(result));
                return false;
            }
            latency_on.store(true, std::memory_order_release);
        }
        // The handoff occurs after simulation. Its first Present has no
        // matching XeLL sleep/markers, so retain one ordinary warmup frame.
        active = active && slept(p.frame);
        const auto configured_factor = std::clamp(factor, 2u, limit);
        if (active && pacing_factor != configured_factor) {
            // The known-good OptiScaler path restarts XeFG when the factor
            // changes: configuring it while enabled can retain the old frame
            // limiter. Configure before the first enable, or present one
            // ordinary frame between disabling and re-enabling an active run.
            const bool restart = pacing_active;
            if (restart) {
                const auto disabled = enable(context, 0);
                if (disabled < 0) {
                    error = "XeFG factor transition disable failed: " + std::to_string(int(disabled));
                    return false;
                }
                pacing_active = false;
            }
            const auto count_result = count(context, configured_factor - 1);
            if (count_result < 0) {
                error = "XeFG factor rejected: " + std::to_string(int(count_result));
                return false;
            }
            pacing_factor = configured_factor;
            xefg_request_pacing_recovery();
            if (restart)
                active = false;
        }
        // Intel initializes interpolation disabled. Change that state only
        // at real transitions, after the required XeLL resume above.
        if (pacing_active != active) {
            const auto enable_result = enable(context, active ? 1u : 0u);
            if (enable_result < 0) {
                error = "XeFG enable failed: " + std::to_string(int(enable_result));
                return false;
            }
            pacing_active = active;
            xefg_request_pacing_recovery();
        }
        // Ordinary duplicate Presents have no native simulation identity.
        // Disabling interpolation is sufficient; do not tag a fabricated ID.
        if (!p.frame)
            return true;
        const auto id = unsigned(p.frame);
        // XeLL is attached to this private DX12 device. The native renderer
        // queues the preceding simulation's image, so its main-thread update
        // is not this device's submission boundary. Starting there makes the
        // measured interval include the following simulation's XeLL sleep.
        // Mark the actual DX12 recording/tagging phase with the presented ID.
        if (mark && latency_on.load(std::memory_order_acquire) && slept(p.frame)) {
            const auto marker_result = mark(latency, id, XELL_RENDERSUBMIT_START);
            diagnostics::event("xell.render_submit_begin", p.frame, int(marker_result));
            if (marker_result != XELL_RESULT_SUCCESS) {
                error = "XeLL private render submission marker failed";
                return false;
            }
        }
        const auto id_result = present_id(context, id);
        if (id_result < 0) {
            error = "XeFG present ID rejected: " + std::to_string(int(id_result));
            return false;
        }
        if (!active)
            return true;
        xefg_swapchain_frame_constant_data_t c{};
        std::memcpy(c.viewMatrix, p.camera.view.data(), 64);
        std::memcpy(c.projectionMatrix, p.camera.projection.data(), 64);
        c.jitterOffsetX = p.jitter_x;
        c.jitterOffsetY = p.jitter_y;
        c.motionVectorScaleX = c.motionVectorScaleY = 1;
        c.resetHistory = p.reset;
        // Intel documents zero when a reliable frame duration is unavailable.
        // Our CPU interval minus scheduler waits is not frame time: gameplay
        // showed it oscillating between 9 and 49 ms while XeLL learned from it.
        // Keep the pacer estimate diagnostic-only; do not feed it back to XeFG.
        c.frameRenderTime = 0;
        xefg_note_fed_frame_time(c.frameRenderTime);
        const auto constants_result = constants(context, id, &c);
        if (constants_result < 0) {
            error = "XeFG frame constants rejected: " + std::to_string(int(constants_result));
            return false;
        }
        const xefg_swapchain_resource_type_t kinds[]{
            XEFG_SWAPCHAIN_RES_DEPTH, XEFG_SWAPCHAIN_RES_MOTION_VECTOR, XEFG_SWAPCHAIN_RES_HUDLESS_COLOR};
        ID3D12Resource *resources[]{p.depth, p.motion, p.hudless};
        for (unsigned i = 0; i < 3; ++i) {
            xefg_swapchain_d3d12_resource_data_t r{};
            r.type = kinds[i];
            r.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
            r.pResource = resources[i];
            r.resourceSize = i == 2 ? xefg_swapchain_2d_t{p.width, p.height}
                                    : xefg_swapchain_2d_t{p.render_width, p.render_height};
            r.incomingState = D3D12_RESOURCE_STATE_COMMON;
            const auto tag_result = tag(context, cmd, id, &r);
            if (tag_result < 0) {
                error = "XeFG input resource " + std::to_string(i) +
                        " rejected: " + std::to_string(int(tag_result));
                return false;
            }
        }
        return true;
    }
    void marker(unsigned m, std::uint64_t id) override {
        // With XeFG, prepare() owns the private DX12 submission boundary.
        // Standalone XeLL keeps the ordinary caller-supplied marker sequence.
        if (m == XELL_RENDERSUBMIT_START && context) return;
        if (mark && m < 7 && latency_on.load(std::memory_order_acquire) && slept(id)) {
            const auto result = mark(latency, unsigned(id), static_cast<xell_latency_marker_type_t>(m));
            if (result != XELL_RESULT_SUCCESS)
                report("XeLL marker " + std::to_string(m) + " failed: " + std::to_string(int(result)));
        }
    }
    void sleep(std::uint64_t id) override {
        const auto epoch = latency_epoch.load(std::memory_order_acquire);
        if (do_sleep && latency_on.load(std::memory_order_acquire)) {
            const auto result = do_sleep(latency, unsigned(id));
            if (result != XELL_RESULT_SUCCESS) {
                report("XeLL sleep failed: " + std::to_string(int(result)));
                return;
            }
            if (latency_epoch.load(std::memory_order_acquire) == epoch &&
                latency_on.load(std::memory_order_acquire))
                slept_frames[id % slept_frames.size()].store((std::uint64_t(epoch) << 32) | unsigned(id),
                                                             std::memory_order_release);
        }
    }
    bool after_present(ID3D12CommandQueue *, std::string &error) override {
        xefg_swapchain_present_status_t s{};
        auto result = state(context, &s);
        if (result < 0) {
            error = "XeFG present status failed: " + std::to_string(int(result));
            return false;
        }
        if (s.frameGenResult < 0) {
            error = "XeFG interpolation error=" + std::to_string(int(s.frameGenResult));
            return false;
        }
        last_count = s.framesPresented;
        presents += last_count;
        return true;
    }
    void stop() override {
        if (pacing_active) {
            pacing_active = false;
            xefg_request_pacing_recovery();
        }
        latency_on.store(false, std::memory_order_release);
        latency_epoch.fetch_add(1, std::memory_order_acq_rel);
        for (auto &frame : slept_frames)
            frame.store(0, std::memory_order_release);
        if (enable) {
            const auto result = enable(context, 0);
            diagnostics::event(result == XEFG_SWAPCHAIN_RESULT_SUCCESS ? "xefg.stop" : "api.failure", 0,
                               int(result), 0, 0, 0, "xefgSwapChainSetEnabled(false)");
        }
        if (sleep_mode && latency) {
            const xell_sleep_params_t sleep{};
            const auto result = sleep_mode(latency, &sleep);
            diagnostics::event(result == XELL_RESULT_SUCCESS ? "xell.stop" : "api.failure", 0,
                               int(result), 0, 0, 0, "xellSetSleepMode(false)");
        }
    }
    unsigned maximum() const override {
        return limit;
    }
    std::uint64_t displayed() const override {
        return presents;
    }
    unsigned last_presented() const override {
        return last_count;
    }
    std::string latency_name() const override {
        return context && unlocked ? "XeLL + verified 4X unlock/pacing" : "Intel XeLL";
    }
};
class Fsr final : public Backend {
    Library lib;
    PfnFfxCreateContext create_context = nullptr;
    PfnFfxDestroyContext destroy_context = nullptr;
    PfnFfxConfigure configure_context = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    PfnFfxQuery query = nullptr;
    ffxContext swap_context = nullptr, model = nullptr;
    ComPtr<IDXGISwapChain4> chain;
    ComPtr<ID3D12CommandQueue> presenting_queue;
    AMD::AntiLag2DX12::Context lag{};
    bool anti_lag = false;
    bool native_latency = true;
    std::string latency_failure;
    std::atomic<bool> latency_on{false};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t sdk_frame = 0;
    PresentationCount presented;
    unsigned last_count = 1;
    bool configuration_active = false;
    ffxConfigureDescFrameGeneration last_configuration{};
    // Context creation descriptor chains remain alive until destruction, per SDK.
    ffxCreateBackendDX12Desc device_desc{};
    ffxCreateContextDescFrameGeneration model_desc{};
    ffxCreateContextDescFrameGenerationVersion api_version{};
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 swap_desc{};
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 swap_version{};
    ffxOverrideVersion override_version{};
    DXGI_SWAP_CHAIN_DESC1 output_desc{};
    static ffxReturnCode_t generate(ffxDispatchDescFrameGeneration *p, void *context) {
        auto *s = static_cast<Fsr *>(context);
        const auto result = s->dispatch(&s->model, &p->header);
        if (result != FFX_API_RETURN_OK)
            diagnostics::event("fsr.generate_error", 0, int(result));
        return result;
    }

  public:
    void use_native_latency(bool enabled) override { native_latency = enabled; }
    explicit Fsr(const std::filesystem::path &root)
        : lib(root / L"fg/amd_fidelityfx_framegeneration_dx12.dll") {
        create_context = lib.get<decltype(ffxCreateContext)>("ffxCreateContext");
        destroy_context = lib.get<decltype(ffxDestroyContext)>("ffxDestroyContext");
        configure_context = lib.get<decltype(ffxConfigure)>("ffxConfigure");
        dispatch = lib.get<decltype(ffxDispatch)>("ffxDispatch");
        query = lib.get<decltype(ffxQuery)>("ffxQuery");
        ffxConfigureDescGlobalDebug1 logging{};
        logging.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
        logging.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS | FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
        logging.fpMessage = [](uint32_t level, const wchar_t *message) {
            char text[4096]{};
            if (message) WideCharToMultiByte(CP_UTF8, 0, message, -1, text, sizeof(text), nullptr, nullptr);
            diagnostics::event("sdk.fsr", 0, level, 0, 0, 0, text);
        };
        diagnostics::event("fsr.logging", 0, int(configure_context(nullptr, &logging.header)));
    }
    ~Fsr() {
        std::string error;
        if (!close(error)) report_failure(error);
    }
    bool close(std::string &error) override {
        stop();
        if (model) {
            const auto result = destroy_context(&model, nullptr);
            diagnostics::event("fsr.model_destroy", 0, int(result));
            if (result != FFX_API_RETURN_OK) {
                error = "FSR interpolation context teardown failed: result=" + std::to_string(int(result));
                return false;
            }
            model = nullptr;
        }
        chain.Reset();
        if (swap_context) {
            const auto result = destroy_context(&swap_context, nullptr);
            diagnostics::event("fsr.swapchain_destroy", 0, int(result));
            if (result != FFX_API_RETURN_OK) {
                error = "FSR swapchain context teardown failed: result=" + std::to_string(int(result));
                return false;
            }
            swap_context = nullptr;
        }
        if (anti_lag) {
            AMD::AntiLag2DX12::DeInitialize(&lag);
            anti_lag = false;
        }
        presenting_queue.Reset();
        return true;
    }
    bool start_latency(ID3D12Device *d, IDXGIFactory *, std::string &error) override {
        if (!anti_lag) {
            const auto result = AMD::AntiLag2DX12::Initialize(&lag, d);
            if (result != S_OK || !lag.m_pAntiLagAPI) {
                error = "AMD Anti-Lag 2 initialization failed: result=" +
                        std::to_string(unsigned(result));
                latency_failure = error;
                diagnostics::event("antilag.initialize_failed", 0, unsigned(result), 0, 0, 0, error.c_str());
                return false;
            }
            latency_failure.clear();
            anti_lag = true;
            retain_device(d, nullptr);
        }
        latency_on.store(true, std::memory_order_release);
        return true;
    }
    bool create(ID3D12Device *d, IDXGIFactory *f, HWND window, const DXGI_SWAP_CHAIN_DESC &desc,
                DXGI_FORMAT fmt, ComPtr<ID3D12CommandQueue> &queue, ComPtr<IDXGISwapChain4> &out,
                std::string &error) override {
        format = fmt;
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!check(d->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "FSR presenting queue", error))
            return false;
        presenting_queue = queue;
        output_desc = chain_desc(desc, fmt);
        swap_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        swap_version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
        swap_desc.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12,
                            &swap_version.header};
        swap_desc.swapchain = chain.GetAddressOf();
        swap_desc.hwnd = window;
        swap_desc.desc = &output_desc;
        swap_desc.dxgiFactory = f;
        swap_desc.gameQueue = queue.Get();
        if (create_context(&swap_context, &swap_desc.header, nullptr) != FFX_API_RETURN_OK) {
            error = "FSR presentation context failed";
            return false;
        }
        out = chain;
        UINT baseline = 0;
        chain->GetLastPresentCount(&baseline);
        presented.reset(baseline, GetTickCount64());
        device_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        device_desc.device = d;
        api_version.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION, &device_desc.header};
        api_version.version = FFX_FRAMEGENERATION_VERSION;
        model_desc.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, &api_version.header};
        model_desc.displaySize = model_desc.maxRenderSize = {output_desc.Width, output_desc.Height};
        model_desc.backBufferFormat = ffxApiGetSurfaceFormatDX12(fmt);
        model_desc.flags = 0;
        if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM || fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
            model_desc.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
        // Enumerate implementations. Classic FG is available on NVIDIA as well
        // as AMD; do not mistake the newest ML-only implementation for support.
        std::array<uint64_t, 16> ids{};
        std::array<const char *, 16> names{};
        uint64_t count = ids.size();
        ffxQueryDescGetVersions versions{{FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, nullptr},
                                         FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,
                                         d,
                                         &count,
                                         ids.data(),
                                         names.data()};
        if (query(nullptr, &versions.header) == FFX_API_RETURN_OK) {
            for (unsigned i = 0; i < std::min<uint64_t>(count, ids.size()); ++i)
                if (names[i] && std::string_view(names[i]).find("3.1") != std::string_view::npos) {
                    override_version.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, model_desc.header.pNext};
                    override_version.versionId = ids[i];
                    model_desc.header.pNext = &override_version.header;
                    break;
                }
        }
        // Depth conventions come from the validated native camera, not a
        // generic reversed/infinite assumption. Create on the first frame.
        std::string latency_error;
        if (native_latency) start_latency(d, f, latency_error);
        if (anti_lag) {
            struct Data {
                AMD::AntiLag2DX12::Context *context;
                bool enabled;
            };
            const Data data{&lag, true};
            const GUID id{0x5083ae5b, 0x8070, 0x4fca, {0x8e, 0xe5, 0x35, 0x82, 0xdd, 0x36, 0x7d, 0x13}};
            chain->SetPrivateData(id, sizeof(data), &data);
        }
        return true;
    }
    bool prepare(ID3D12GraphicsCommandList *cmd, const Packet &p, unsigned, bool active,
                 std::string &error) override {
        if (active && anti_lag)
            latency_on.store(true, std::memory_order_release);
        if (active) {
            const auto depth_flags = (p.camera.inverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0u) |
                                     (p.camera.infinite ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0u);
            constexpr unsigned depth_mask =
                FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
            if (model && (model_desc.flags & depth_mask) != depth_flags) {
                stop();
                ComPtr<ID3D12Fence> retired;
                if (!check(device_desc.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retired)),
                           "FSR camera transition fence", error))
                    return false;
                HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!event) {
                    error = "FSR camera transition event failed";
                    return false;
                }
                const bool ready = SUCCEEDED(presenting_queue->Signal(retired.Get(), 1)) &&
                                   SUCCEEDED(retired->SetEventOnCompletion(1, event)) &&
                                   WaitForSingleObject(event, 3000) == WAIT_OBJECT_0 &&
                                   retired->GetCompletedValue() != UINT64_MAX && retired->GetCompletedValue() >= 1;
                CloseHandle(event);
                if (!ready) {
                    error = "FSR camera transition is still pending";
                    return false;
                }
                destroy_context(&model, nullptr);
                model = nullptr;
            }
            if (!model) {
                model_desc.flags = (model_desc.flags & ~depth_mask) | depth_flags;
                if (create_context(&model, &model_desc.header, nullptr) != FFX_API_RETURN_OK) {
                    error = "FSR frame interpolation context failed";
                    return false;
                }
            }
        }
        if (!model)
            return true;
        // FidelityFX requires exactly consecutive IDs for its own resources.
        // Native simulation IDs can skip, or be zero on a no-guide Present;
        // keep native image identity/reset validation outside this SDK domain.
        const auto frame = ++sdk_frame;
        if (configuration_active != active) {
            configuration_active = active;
            presented.reset_window(GetTickCount64());
            last_count = 1;
        }
        ffxConfigureDescFrameGeneration c{};
        c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        c.swapChain = chain.Get();
        c.frameGenerationEnabled = active;
        c.allowAsyncWorkloads = false;
        c.frameGenerationCallback = generate;
        c.frameGenerationCallbackUserContext = this;
        c.frameID = frame;
        c.generationRect = {0, 0, int(p.width), int(p.height)};
        c.HUDLessColor = ffxApiGetResourceDX12(p.hudless, FFX_API_RESOURCE_STATE_COMMON);
        last_configuration = c;
        if (configure_context(&model, &c.header) != FFX_API_RETURN_OK) {
            error = "FSR frame configuration failed";
            return false;
        }
        if (!active)
            return true;
        ffxDispatchDescFrameGenerationPrepareV2 d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        d.frameID = frame;
        d.commandList = cmd;
        d.renderSize = {p.render_width, p.render_height};
        d.jitterOffset = {p.jitter_x, p.jitter_y};
        d.motionVectorScale = {1, 1};
        d.frameTimeDelta = p.milliseconds;
        d.reset = p.reset;
        d.cameraNear = p.camera.near_plane;
        d.cameraFar = p.camera.far_plane;
        d.cameraFovAngleVertical = p.camera.fov;
        d.viewSpaceToMetersFactor = 1;
        d.depth = ffxApiGetResourceDX12(p.depth, FFX_API_RESOURCE_STATE_COMMON);
        d.motionVectors = ffxApiGetResourceDX12(p.motion, FFX_API_RESOURCE_STATE_COMMON);
        using namespace DirectX;
        XMFLOAT4X4 inv{};
        XMStoreFloat4x4(&inv, XMMatrixInverse(nullptr, XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4 *>(
                                                           p.camera.view.data()))));
        const float pos[]{inv._41, inv._42, inv._43}, up[]{inv._21, inv._22, inv._23},
            right[]{inv._11, inv._12, inv._13}, forward[]{inv._31, inv._32, inv._33};
        std::memcpy(d.cameraPosition, pos, 12);
        std::memcpy(d.cameraUp, up, 12);
        std::memcpy(d.cameraRight, right, 12);
        std::memcpy(d.cameraForward, forward, 12);
        if (dispatch(&model, &d.header) != FFX_API_RETURN_OK) {
            error = "FSR prepare dispatch failed";
            return false;
        }
        return true;
    }
    void marker(unsigned m, std::uint64_t) override {
        if (anti_lag && m == 3 && latency_on.load(std::memory_order_acquire))
            AMD::AntiLag2DX12::MarkEndOfFrameRendering(&lag);
    }
    void sleep(std::uint64_t) override {
        if (anti_lag && latency_on.load(std::memory_order_acquire))
            AMD::AntiLag2DX12::Update(&lag, true, 0);
    }
    bool after_present(ID3D12CommandQueue *, std::string &) override {
        UINT count = 0;
        if (SUCCEEDED(chain->GetLastPresentCount(&count))) {
            if (!presented.observe(count, GetTickCount64()))
                last_count = 1;
            else if (presented.ready())
                last_count = std::clamp(unsigned(std::lround(presented.ratio())), 1u, 2u);
        } else {
            presented.query_failed(GetTickCount64());
            last_count = 1;
        }
        return true;
    }
    void stop() override {
        latency_on.store(false, std::memory_order_release);
        configuration_active = false;
        presented.reset_window(GetTickCount64());
        last_count = 1;
        if (model) {
            auto c = last_configuration;
            c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            c.swapChain = chain.Get();
            c.frameGenerationEnabled = false;
            c.presentCallback = nullptr;
            c.HUDLessColor = {};
            const auto result = configure_context(&model, &c.header);
            diagnostics::event(result == FFX_API_RETURN_OK ? "fsr.stop" : "api.failure", 0,
                               int(result), 0, 0, 0, "ffxConfigure(frameGenerationEnabled=false)");
        }
        if (anti_lag)
            AMD::AntiLag2DX12::Update(&lag, false, 0);
    }
    bool resize(unsigned w, unsigned h, std::string &error) override {
        stop();
        if (model) {
            const auto result = destroy_context(&model, nullptr);
            diagnostics::event("fsr.resize_destroy", 0, int(result));
            if (result != FFX_API_RETURN_OK) {
                error = "FSR model retirement before resize failed: result=" + std::to_string(int(result));
                return false;
            }
            model = nullptr;
        }
        model_desc.displaySize = model_desc.maxRenderSize = {w, h};
        (void)error;
        last_configuration = {};
        return true;
    }
    bool latency_available() const override {
        return anti_lag;
    }
    unsigned maximum() const override {
        return 2;
    }
    std::uint64_t displayed() const override {
        return presented.total();
    }
    unsigned last_presented() const override {
        return last_count;
    }
    std::string latency_name() const override {
        return anti_lag ? "AMD Anti-Lag 2" : "Anti-Lag 2 unavailable: " + latency_failure;
    }
};
} // namespace
std::shared_ptr<Backend> make_plain_backend(std::shared_ptr<Backend> latency) {
    return std::make_shared<Plain>(std::move(latency));
}
std::shared_ptr<Backend> with_latency(std::shared_ptr<Backend> output, std::shared_ptr<Backend> latency) {
    return std::make_shared<Plain>(std::move(latency), std::move(output));
}
std::shared_ptr<Backend> make_backend(FrameGeneration backend, const std::filesystem::path &root, bool unlock,
                                      std::string &error) {
    try {
        if (backend == FrameGeneration::Off)
            return make_plain_backend();
        if (backend == FrameGeneration::DlssG)
            return std::make_shared<Streamline>(root);
        if (backend == FrameGeneration::XeFG)
            return std::make_shared<Xe>(root, unlock);
        if (backend == FrameGeneration::FsrFG)
            return std::make_shared<Fsr>(root);
    } catch (const std::exception &e) {
        error = e.what();
    }
    return {};
}
} // namespace xivrender::fg
