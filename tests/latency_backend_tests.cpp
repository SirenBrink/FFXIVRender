#include "xivrender/fg_backend.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

using Microsoft::WRL::ComPtr;
using xivrender::fg::Backend;
namespace {
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
void checked(HRESULT result, const char *operation) {
    require(SUCCEEDED(result), std::string(operation) + ": result=" + std::to_string(unsigned(result)));
}
struct HiddenWindow {
    HWND handle = nullptr;
    HiddenWindow() {
        WNDCLASSW c{};
        c.lpfnWndProc = DefWindowProcW;
        c.hInstance = GetModuleHandleW(nullptr);
        c.lpszClassName = L"XIVRender latency-only test";
        require(RegisterClassW(&c) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
                "Cannot register hidden latency test window");
        handle = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, c.lpszClassName, L"", WS_POPUP, -32000,
                                 -32000, 160, 96, nullptr, nullptr, c.hInstance, nullptr);
        require(handle != nullptr && !IsWindowVisible(handle), "Latency test window is not hidden");
    }
    ~HiddenWindow() {
        if (handle)
            DestroyWindow(handle);
    }
};
// This double guards the boundary that caused the regression: ordinary
// presentation must call latency APIs without creating or driving FG output.
// The actual swapchain/queue, command submission and retirement stay real.
class RecordingController final : public Backend {
  public:
    unsigned starts = 0, stops = 0, sleeps = 0, markers = 0, fg_creates = 0;
    std::uint64_t last_sleep = 0;
    bool fail = false, active = false;
    bool start_latency(ID3D12Device *d, IDXGIFactory *f, std::string &error) override {
        require(d && f, "Latency startup lost its native device/factory");
        ++starts;
        active = !fail;
        if (fail)
            error = "Deliberately unsupported latency controller";
        return active;
    }
    bool create(ID3D12Device *, IDXGIFactory *, HWND, const DXGI_SWAP_CHAIN_DESC &, DXGI_FORMAT,
                ComPtr<ID3D12CommandQueue> &, ComPtr<IDXGISwapChain4> &, std::string &) override {
        ++fg_creates;
        throw std::runtime_error("Latency-only output attempted to create FG resources");
    }
    bool prepare(ID3D12GraphicsCommandList *, const xivrender::fg::Packet &, unsigned, bool,
                 std::string &) override {
        throw std::runtime_error("Latency-only output attempted to prepare FG");
    }
    void marker(unsigned m, std::uint64_t id) override {
        require(active && m <= 5 && id == last_sleep, "Latency marker lacks matching active sleep");
        ++markers;
    }
    void sleep(std::uint64_t id) override {
        require(active && id > last_sleep, "Latency sleep lost active state or monotonic frame identity");
        last_sleep = id;
        ++sleeps;
    }
    bool after_present(ID3D12CommandQueue *, std::string &) override {
        throw std::runtime_error("Latency-only output queried FG present status");
    }
    void stop() override {
        active = false;
        ++stops;
    }
    bool latency_available() const override {
        return active;
    }
    unsigned maximum() const override {
        return 6;
    }
    std::uint64_t displayed() const override {
        return 0;
    }
    unsigned last_presented() const override {
        return 6;
    }
    std::string latency_name() const override {
        return "Recording controller";
    }
};
struct OrdinaryOutput {
    std::shared_ptr<Backend> backend;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain4> chain;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12DescriptorHeap> views;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t submitted = 0;
    HANDLE event = nullptr;
    explicit OrdinaryOutput(ID3D12Device *device, IDXGIFactory *factory, HWND window,
                            std::shared_ptr<Backend> controller, bool composed = false)
        : backend(composed ? xivrender::fg::with_latency(xivrender::fg::make_plain_backend(), std::move(controller))
                           : xivrender::fg::make_plain_backend(std::move(controller))) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = 160;
        desc.BufferDesc.Height = 96;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        std::string error;
        require(backend->create(device, factory, window, desc, desc.BufferDesc.Format, queue, chain, error),
                "Ordinary output creation failed: " + error);
        backend->retain_device(device, queue.Get());
        require(backend->maximum() == 1 && backend->last_presented() == 1,
                "Ordinary output exposes an interpolation factor");
        DXGI_SWAP_CHAIN_DESC1 actual{};
        checked(chain->GetDesc1(&actual), "Read ordinary swapchain description");
        require(actual.BufferCount == 3 && actual.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD,
                "Latency-only output changed the native swapchain contract");
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                "Create latency test allocator");
        checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&commands)),
                "Create latency test commands");
        checked(commands->Close(), "Close initial command list");
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.NumDescriptors = 1;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        checked(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&views)), "Create latency test RTV heap");
        checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                "Create latency test fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event != nullptr, "Cannot create latency test completion event");
    }
    void wait() {
        if (!submitted || fence->GetCompletedValue() >= submitted)
            return;
        checked(fence->SetEventOnCompletion(submitted, event), "Register latency test completion");
        require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "Latency test GPU retirement timed out");
    }
    void frame(ID3D12Device *device, std::uint64_t id) {
        wait();
        std::string error;
        require(backend->prepare(nullptr, {}, 1, false, error),
                "Ordinary output preparation failed: " + error);
        backend->sleep(id);
        backend->marker(0, id);
        backend->marker(1, id);
        backend->marker(2, id);
        checked(allocator->Reset(), "Reset latency test allocator");
        checked(commands->Reset(allocator.Get(), nullptr), "Reset latency test commands");
        ComPtr<ID3D12Resource> back;
        checked(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)),
                "Get ordinary D3D12 backbuffer");
        auto handle = views->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(back.Get(), nullptr, handle);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = back.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commands->ResourceBarrier(1, &b);
        const float colour[]{float(id % 7) / 7.f, .25f, .5f, 1.f};
        commands->ClearRenderTargetView(handle, colour, 0, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        commands->ResourceBarrier(1, &b);
        checked(commands->Close(), "Close latency test commands");
        ID3D12CommandList *list = commands.Get();
        queue->ExecuteCommandLists(1, &list);
        backend->marker(3, id);
        backend->marker(4, id);
        checked(chain->Present(0, DXGI_PRESENT_ALLOW_TEARING), "Ordinary hidden Present");
        backend->marker(5, id);
        const auto before = backend->displayed();
        require(backend->after_present(queue.Get(), error) && backend->displayed() == before + 1 &&
                    backend->last_presented() == 1,
                "Ordinary output did not count exactly one Present");
        checked(queue->Signal(fence.Get(), ++submitted), "Signal latency test completion");
        wait();
    }
    ~OrdinaryOutput() {
        backend->stop();
        chain.Reset();
        if (event)
            CloseHandle(event);
    }
};
void run(ID3D12Device *d, IDXGIFactory *f, HWND window, const std::shared_ptr<Backend> &controller,
         bool real) {
    const auto starting_failures = xivrender::fg::status().failures;
    std::uint64_t id = 1;
    const unsigned frames = real ? 12u : 3u;
    {
        OrdinaryOutput output(d, f, window, controller);
        require(output.backend->latency_available(), "Standalone latency controller did not start");
        const auto name = output.backend->latency_name();
        for (unsigned n = 0; n < frames; ++n)
            output.frame(d, id++);
        output.backend->stop();
        require(!output.backend->latency_available(), "Stopping output retained active latency calls");
        // A resize/drain must not permanently disable the selected controller.
        output.frame(d, id++);
        require(output.backend->latency_available() && output.backend->latency_name() == name,
                "Latency reduction did not resume after retirement");
        require(controller->displayed() == 0, "Latency-only controller accumulated FG presents");
    }
    {
        // Explicitly disabled latency retains ordinary rendering. No provider
        // controller may be driven while the saved checkbox is off.
        OrdinaryOutput disabled(d, f, window, {});
        require(!disabled.backend->latency_available() && disabled.backend->latency_name() == "Disabled",
                "Explicit latency-off selection was ignored");
        for (unsigned n = 0; n < frames; ++n)
            disabled.frame(d, id++);
    }
    {
        // Reuse one cached Streamline owner for the entire process lifetime;
        // this is the same owner used before an off/on output handoff.
        OrdinaryOutput resumed(d, f, window, controller);
        for (unsigned n = 0; n < frames; ++n)
            resumed.frame(d, id++);
        require(resumed.backend->latency_available() && controller->displayed() == 0,
                "Latency-only output could not be re-enabled without FG");
    }
    require(xivrender::fg::status().failures == starting_failures,
            "Latency-only output recorded an SDK failure");
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        require(argc == 1 || argc == 3, "Usage: latency_backend_tests [runtime-directory provider(1/2)]");
        const bool real = argc == 3;
        ComPtr<IDXGIFactory4> factory;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create latency test factory");
        ComPtr<IDXGIAdapter> adapter;
        if (!real)
            checked(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "Create WARP latency test adapter");
        ComPtr<ID3D12Device> device;
        checked(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
                "Create latency test device");
        HiddenWindow window;
        if (real) {
            const std::wstring_view selected(argv[2]);
            require(selected == L"1" || selected == L"2",
                    "Real standalone latency test expects Reflex or XeLL");
            const auto provider =
                selected == L"1" ? xivrender::FrameGeneration::DlssG : xivrender::FrameGeneration::XeFG;
            std::string error;
            auto controller =
                xivrender::fg::make_backend(provider, std::filesystem::absolute(argv[1]), false, error);
            require(bool(controller), "Cannot load latency provider: " + error);
            require(controller->prepare_device(device.Get(), factory.Get(), error),
                    "Cannot prepare standalone latency device: " + error);
            require(controller->set_interpolation_loaded(false, error),
                    "Cannot unload interpolation before ordinary presentation: " + error);
            xivrender::fg::report("Standalone latency test running");
            run(device.Get(), factory.Get(), window.handle, controller, true);
            require(xivrender::fg::status().message == "Standalone latency test running",
                    "Latency SDK emitted a warning/error: " + xivrender::fg::status().message);
            std::cout << "PASS: hidden native ordinary swapchain with " << controller->latency_name()
                      << "; latency markers, retirement resume and explicit off/on; no FG output created\n";
        } else {
            auto controller = std::make_shared<RecordingController>();
            run(device.Get(), factory.Get(), window.handle, controller, false);
            require(controller->starts == 3 && controller->fg_creates == 0 && controller->sleeps == 7 &&
                        controller->markers == controller->sleeps * 6,
                    "Ordinary presentation did not preserve latency lifecycle/marker forwarding");
            auto independent = std::make_shared<RecordingController>();
            {
                OrdinaryOutput composed(device.Get(), factory.Get(), window.handle, independent, true);
                composed.frame(device.Get(), 1);
                composed.backend->stop();
                composed.frame(device.Get(), 2);
                require(independent->starts == 2 && independent->sleeps == 2 && independent->fg_creates == 0,
                        "Independent latency did not resume or touched FG creation");
                composed.chain.Reset();
                std::string close_error;
                composed.backend->release_output();
                require(composed.backend->close(close_error), "Composed close failed: " + close_error);
            }
            auto unsupported = std::make_shared<RecordingController>();
            unsupported->fail = true;
            const auto failures = xivrender::fg::status().failures;
            {
                OrdinaryOutput fallback(device.Get(), factory.Get(), window.handle, unsupported);
                require(!fallback.backend->latency_available() &&
                            fallback.backend->latency_name() == "Unavailable",
                        "Unsupported latency controller was reported as active");
                fallback.frame(device.Get(), 100);
                fallback.frame(device.Get(), 101);
                require(unsupported->starts == 1 && unsupported->fg_creates == 0 &&
                            unsupported->sleeps == 0 && unsupported->markers == 0 &&
                            xivrender::fg::status().failures == failures,
                        "Unavailable latency repeatedly restarted or interrupted ordinary rendering");
            }
            std::cout << "PASS: native WARP ordinary presentation preserves latency forwarding, resume, "
                         "explicit off/on and unsupported-provider fallback without FG resources\n";
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
