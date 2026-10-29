// Native ABI mock + WARP resources. No NVIDIA model files or GPU kernels.
#include "xivrender/nr_engine.hpp"
#include <nvsdk_ngx.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using namespace xivrender::nr;

// Existing test-only mock ABI, shared with native_nr_bridge_tests.cpp.
struct MockNrMetrics {
    unsigned inits = 0, populates = 0, creates = 0, evaluations = 0;
    unsigned releases = 0, shutdowns = 0, callbacks = 0, live_handles = 0;
    unsigned api_version = 0, feature = 0, typed_mask = 0, width = 0, height = 0;
    unsigned init_parameters_null = 0, progress_null = 0;
    std::uint64_t application_id = 0;
    std::uintptr_t device = 0, commands = 0, parameters = 0, handle = 0;
};

namespace {
constexpr unsigned failed = 0xbad10002;
void hr(HRESULT value) {
    if (FAILED(value))
        throw std::runtime_error("D3D12 fixture error: " + std::to_string(value));
}
struct Mock {
    HMODULE module = nullptr;
    using Results = void(__cdecl *)(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
    using Snapshot = void(__cdecl *)(MockNrMetrics *);
    Results results = nullptr;
    Snapshot snapshot = nullptr;
    explicit Mock(const std::filesystem::path &path) {
        // Acquire only after Engine.Open: preloading must be rejected by the
        // native bridge's exclusive model ownership check.
        module = LoadLibraryExW(path.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        assert(module);
        results = reinterpret_cast<Results>(GetProcAddress(module, "MockNrResults"));
        snapshot = reinterpret_cast<Snapshot>(GetProcAddress(module, "MockNrSnapshot"));
        assert(results && snapshot);
    }
    ~Mock() {
        FreeLibrary(module);
    }
    MockNrMetrics metrics() const {
        MockNrMetrics result;
        snapshot(&result);
        return result;
    }
};
struct Fixture {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> color, depth, motion, output;
    Fixture() {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&commands)));
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        color = image(DXGI_FORMAT_R16G16B16A16_FLOAT, false);
        depth = image(DXGI_FORMAT_R32_FLOAT, false);
        motion = image(DXGI_FORMAT_R16G16_FLOAT, false);
        output = image(DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    }
    ComPtr<ID3D12Resource> image(DXGI_FORMAT format, bool writable) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 512;
        desc.Height = 512;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.Flags = writable ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> resource;
        hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                           nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    }
    Evaluation frame() const {
        Evaluation result;
        result.color = color.Get();
        result.depth = depth.Get();
        result.motion = motion.Get();
        result.output = output.Get();
        result.motion_scale_x = 512.0f;
        result.motion_scale_y = -512.0f;
        return result;
    }
};

// A fence whose owner cannot be established must be rejected before calling
// a model. Forward ordinary operations to a real WARP fence.
struct UnownedFence final : ID3D12Fence {
    ID3D12Fence *fence;
    explicit UnownedFence(ID3D12Fence *value) : fence(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override {
        if (out)
            *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return 1;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID id, UINT *size, void *data) override {
        return fence->GetPrivateData(id, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID id, UINT size, const void *data) override {
        return fence->SetPrivateData(id, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID id, const IUnknown *data) override {
        return fence->SetPrivateDataInterface(id, data);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {
        return fence->SetName(name);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **out) override {
        if (out)
            *out = nullptr;
        return E_NOINTERFACE;
    }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override {
        return fence->GetCompletedValue();
    }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) override {
        return fence->SetEventOnCompletion(value, event);
    }
    HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override {
        return fence->Signal(value);
    }
};

void completion_preflight(const EngineConfig &config) {
    Fixture f;
    Engine engine;
    std::string error;
    assert(engine.Open(f.device.Get(), config, error));
    Mock mock(config.model);
    assert(engine.ValidateCompletionFence(f.fence.Get(), error));
    assert(!engine.ValidateCompletionFence(nullptr, error));
    UnownedFence unowned(f.fence.Get());
    assert(!engine.ValidateCompletionFence(&unowned, error));
    assert(!engine.HasPendingSubmission() && mock.metrics().creates == 0);

    // D3D12 returns a singleton for each adapter. A second WARP CreateDevice
    // cannot provide a foreign identity; exercise a true second adapter when
    // available, without requiring NVIDIA kernels or hardware for this suite.
    ComPtr<IDXGIFactory4> factory;
    hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        ComPtr<ID3D12Device> other;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&other))))
            continue;
        ComPtr<IUnknown> a, b;
        hr(f.device.As(&a));
        hr(other.As(&b));
        if (a.Get() == b.Get())
            continue;
        ComPtr<ID3D12Fence> foreign;
        hr(other->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&foreign)));
        assert(!engine.ValidateCompletionFence(foreign.Get(), error));
        assert(!engine.HasPendingSubmission() && mock.metrics().creates == 0);
        break;
    }
    // A rejected preflight leaves a valid generation usable and closeable.
    assert(engine.ValidateCompletionFence(f.fence.Get(), error));
    assert(engine.RecordCreate(f.commands.Get(), error));
    assert(engine.MarkSubmitted(f.fence.Get(), 1, error));
    hr(f.fence->Signal(1));
    assert(engine.Ready() && engine.TryClose(error));
}

void creation_and_retirement(const EngineConfig &config) {
    Fixture f;
    Engine engine;
    std::string error;
    assert(engine.Open(f.device.Get(), config, error));
    Mock mock(config.model);
    assert(!engine.Ready());
    assert(engine.RecordCreate(f.commands.Get(), error));
    assert(engine.HasPendingSubmission());
    assert(!engine.TryClose(error));
    assert(mock.metrics().releases == 0 && mock.metrics().shutdowns == 0);
    assert(!engine.MarkSubmitted(f.fence.Get(), 0, error));
    assert(engine.HasPendingSubmission());
    assert(engine.MarkSubmitted(f.fence.Get(), 1, error));
    assert(!engine.Ready() && !engine.TryClose(error));
    hr(f.fence->Signal(1));
    assert(engine.Ready());
    auto frame = f.frame();
    assert(engine.RecordEvaluate(f.commands.Get(), frame, error));
    const auto seen = mock.metrics();
    assert(seen.creates == 1 && seen.evaluations == 1 && seen.feature == 18);
    assert(seen.width == 512 && seen.height == 512);
    auto *parameters = reinterpret_cast<NVSDK_NGX_Parameter *>(seen.parameters);
    unsigned reset = 0;
    float scale_x = 0, scale_y = 0;
    ID3D12Resource *color = nullptr;
    assert(parameters->Get("DLSSNR.Reset", &reset) == NVSDK_NGX_Result_Success && reset == 1);
    assert(parameters->Get("DLSSNR.Color", &color) == NVSDK_NGX_Result_Success && color == f.color.Get());
    assert(parameters->Get("DLSSNR.MVecScaleX", &scale_x) == NVSDK_NGX_Result_Success && scale_x == 512);
    assert(parameters->Get("DLSSNR.MVecScaleY", &scale_y) == NVSDK_NGX_Result_Success && scale_y == -512);
    assert(!engine.MarkSubmitted(f.fence.Get(), 1, error)); // Cannot reuse an old completion token.
    assert(engine.MarkSubmitted(f.fence.Get(), 2, error));
    assert(!engine.TryClose(error));
    assert(mock.metrics().releases == 0);
    hr(f.fence->Signal(2));
    assert(engine.RecordEvaluate(f.commands.Get(), frame, error));
    assert(parameters->Get("DLSSNR.Reset", &reset) == NVSDK_NGX_Result_Success && reset == 0);
    assert(engine.MarkSubmitted(f.fence.Get(), 3, error));
    hr(f.fence->Signal(3));
    const auto evals = mock.metrics().evaluations;
    frame.output = frame.color;
    assert(!engine.RecordEvaluate(f.commands.Get(), frame, error));
    assert(!engine.HasPendingSubmission() && mock.metrics().evaluations == evals);
    frame = f.frame();
    frame.motion_scale_x = std::numeric_limits<float>::quiet_NaN();
    assert(!engine.RecordEvaluate(f.commands.Get(), frame, error));
    assert(!engine.HasPendingSubmission());
    assert(engine.TryClose(error));
    assert(mock.metrics().releases == 1 && mock.metrics().shutdowns == 1 && mock.metrics().live_handles == 0);
    assert(engine.Status().phase == EnginePhase::Closed);
}

void failures_retain_ownership(const EngineConfig &config) {
    Fixture f;
    Engine engine;
    std::string error;
    assert(engine.Open(f.device.Get(), config, error));
    Mock mock(config.model);
    mock.results(1, 1, failed, 1, 1, 1, 1);
    assert(!engine.RecordCreate(f.commands.Get(), error));
    assert(engine.HasPendingSubmission() && mock.metrics().live_handles == 1);
    assert(!engine.TryClose(error));
    assert(engine.MarkSubmitted(f.fence.Get(), 1, error));
    assert(!engine.TryClose(error) && mock.metrics().releases == 0);
    hr(f.fence->Signal(1));
    mock.results(1, 1, 1, 1, failed, 1, 0);
    assert(!engine.TryClose(error));
    assert(mock.metrics().live_handles == 1 && mock.metrics().shutdowns == 0);
    mock.results(1, 1, 1, 1, 1, failed, 0);
    assert(!engine.TryClose(error));
    assert(mock.metrics().live_handles == 0 && mock.metrics().shutdowns == 1);
    mock.results(1, 1, 1, 1, 1, 1, 0);
    assert(engine.TryClose(error));
    assert(mock.metrics().releases == 2 && mock.metrics().shutdowns == 2);
}

void failed_evaluation_is_submitted(const EngineConfig &config) {
    Fixture f;
    Engine engine;
    std::string error;
    assert(engine.Open(f.device.Get(), config, error));
    Mock mock(config.model);
    assert(engine.RecordCreate(f.commands.Get(), error));
    assert(engine.MarkSubmitted(f.fence.Get(), 1, error));
    hr(f.fence->Signal(1));
    mock.results(1, 1, 1, failed, 1, 1, 0);
    assert(!engine.RecordEvaluate(f.commands.Get(), f.frame(), error));
    assert(engine.HasPendingSubmission());
    assert(!engine.TryClose(error));
    assert(engine.MarkSubmitted(f.fence.Get(), 2, error));
    assert(!engine.TryClose(error));
    hr(f.fence->Signal(2));
    assert(engine.TryClose(error));
    assert(mock.metrics().releases == 1 && mock.metrics().shutdowns == 1);
}

void reopened_generation_resets_history(const EngineConfig &config) {
    Fixture f;
    Engine engine;
    std::string error;
    for (unsigned generation = 0; generation < 3; ++generation) {
        assert(engine.Open(f.device.Get(), config, error));
        Mock mock(config.model);
        assert(engine.RecordCreate(f.commands.Get(), error));
        const auto create_token = std::uint64_t(2 * generation + 1);
        assert(engine.MarkSubmitted(f.fence.Get(), create_token, error));
        hr(f.fence->Signal(create_token));
        assert(engine.Ready());
        assert(engine.RecordEvaluate(f.commands.Get(), f.frame(), error));
        const auto seen = mock.metrics();
        auto *parameters = reinterpret_cast<NVSDK_NGX_Parameter *>(seen.parameters);
        unsigned reset = 0;
        assert(parameters->Get("DLSSNR.Reset", &reset) == NVSDK_NGX_Result_Success && reset == 1);
        assert(engine.MarkSubmitted(f.fence.Get(), create_token + 1, error));
        hr(f.fence->Signal(create_token + 1));
        assert(engine.TryClose(error));
        assert(mock.metrics().shutdowns == 1);
        // Mock's last module reference releases here. Next generation must
        // initialize/populate again, rather than retaining unloaded callbacks.
    }
}
void multipass_profiles_and_retirement(EngineConfig config) {
    Fixture f;
    Engine engine;
    config.passes = 3;
    for (unsigned i = 0; i < 3; ++i)
        config.profiles[i] = {i, 2 - i, .4f + i * .2f, .7f, .1f * i, -.3f, true};
    std::string error;
    assert(engine.Open(f.device.Get(), config, error));
    Mock mock(config.model);
    using Parameters = NVSDK_NGX_Parameter *(__cdecl *)(unsigned);
    const auto params = reinterpret_cast<Parameters>(GetProcAddress(mock.module, "MockNrParameters"));
    assert(params);
    assert(engine.RecordCreate(f.commands.Get(), error));
    assert(mock.metrics().creates == 3 && mock.metrics().live_handles == 3);
    assert(params(0) != params(1) && params(1) != params(2));
    for (unsigned i = 0; i < 3; ++i) {
        unsigned preset = 99, style = 99;
        float intensity = 0, skin = 0;
        assert(params(i)->Get("DLSSNR.Hint.Render.Preset", &preset) == NVSDK_NGX_Result_Success &&
               preset == i);
        assert(params(i)->Get("DLSSNR.Style", &style) == NVSDK_NGX_Result_Success && style == 2 - i);
        assert(params(i)->Get("DLSSNR.Intensity", &intensity) == NVSDK_NGX_Result_Success &&
               intensity == config.profiles[i].intensity);
        assert(params(i)->Get("DLSSNR.SkinStructureStrength", &skin) == NVSDK_NGX_Result_Success &&
               skin == -.3f);
        void *callback = nullptr;
        assert(params(i)->Get("DLSSNRComputeScalingRatioCallback", &callback) == NVSDK_NGX_Result_Success &&
               callback);
    }
    assert(engine.MarkSubmitted(f.fence.Get(), 1, error));
    hr(f.fence->Signal(1));
    auto frame = f.frame();
    frame.feedback = .5f;
    assert(engine.RecordEvaluate(f.commands.Get(), frame, error));
    assert(mock.metrics().evaluations == 3);
    for (unsigned i = 0; i < 3; ++i) {
        unsigned reset = 0;
        assert(params(i)->Get("DLSSNR.Reset", &reset) == NVSDK_NGX_Result_Success && reset == 1);
    }
    assert(!engine.TryClose(error) && mock.metrics().releases == 0);
    assert(engine.MarkSubmitted(f.fence.Get(), 2, error));
    hr(f.fence->Signal(2));
    assert(engine.RecordEvaluate(f.commands.Get(), frame, error));
    assert(mock.metrics().evaluations == 6);
    for (unsigned i = 0; i < 3; ++i) {
        unsigned reset = 1;
        assert(params(i)->Get("DLSSNR.Reset", &reset) == NVSDK_NGX_Result_Success && reset == 0);
    }
    assert(engine.MarkSubmitted(f.fence.Get(), 3, error));
    hr(f.fence->Signal(3));
    frame.feedback = 1.1f;
    assert(!engine.RecordEvaluate(f.commands.Get(), frame, error) && !engine.HasPendingSubmission());
    assert(mock.metrics().evaluations == 6);
    assert(engine.TryClose(error));
    assert(mock.metrics().releases == 3 && mock.metrics().live_handles == 0 && mock.metrics().shutdowns == 1);
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 3)
        return 2;
    try {
        const EngineConfig config{std::filesystem::path(argv[1]), std::filesystem::path(argv[2]),
                                  std::filesystem::temp_directory_path() / L"xivrender-nr-engine-test", 512,
                                  512};
        completion_preflight(config);
        creation_and_retirement(config);
        failures_retain_ownership(config);
        failed_evaluation_is_submitted(config);
        reopened_generation_resets_history(config);
        multipass_profiles_and_retirement(config);
        std::cout << "PASS: asynchronous NR initialization, frame contracts, fenced retirement, partial "
                     "failures and live generation reset\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
