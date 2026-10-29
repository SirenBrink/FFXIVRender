// Test only. A never-shown provider chain is stalled at its CPU Present entry
// while a precreated render worker captures the following native cohort.
#include "xivrender/frame_generation.hpp"
#include "xivrender/hdr.hpp"
#include "xivrender/d3d11_work.hpp"
#include <MinHook.h>
#include <DirectXMath.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
void checked(HRESULT result, const char *operation) {
    require(SUCCEEDED(result), std::string(operation) + ": " + std::to_string(unsigned(result)));
}
struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() {
        require(handle != nullptr, "Overlap event creation failed");
    }
    ~Event() {
        CloseHandle(handle);
    }
};
struct RenderWorker {
    Event start, done, ready;
    HANDLE thread = nullptr;
    xivrender::nr::FrameInput input;
    std::atomic<bool> cancel{false}, gate_open{false}, finished_inside_gate{false};
    std::atomic<bool> capture_ok{false};
    std::string error;
    static DWORD WINAPI entry(void *context) {
        auto &self = *static_cast<RenderWorker *>(context);
        SetEvent(self.ready.handle);
        WaitForSingleObject(self.start.handle, INFINITE);
        if (!self.cancel) {
            try {
                xivrender::d3d11_work::Guard render_owner;
                xivrender::fg::capture(self.input);
                self.capture_ok = true;
                self.finished_inside_gate = self.gate_open.load();
            } catch (const std::exception &failure) {
                self.error = failure.what();
            }
        }
        SetEvent(self.done.handle);
        return 0;
    }
    explicit RenderWorker(const xivrender::nr::FrameInput &next) : input(next) {
        thread = CreateThread(nullptr, 0, entry, this, 0, nullptr);
        require(thread != nullptr, "Render worker creation failed");
        if (WaitForSingleObject(ready.handle, 1000) != WAIT_OBJECT_0) {
            cancel = true;
            SetEvent(start.handle);
            if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
                TerminateProcess(GetCurrentProcess(), 1);
            CloseHandle(thread);
            thread = nullptr;
            throw std::runtime_error("Render worker did not start");
        }
    }
    ~RenderWorker() {
        cancel = true;
        gate_open = false;
        SetEvent(start.handle);
        if (thread) {
            if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) {
                // Only this isolated test process is terminated. Releasing a
                // blocked worker's D3D11/SDK owners would otherwise be unsafe.
                std::cerr << "FAIL: Render worker did not retire within five seconds\n";
                TerminateProcess(GetCurrentProcess(), 1);
            }
            CloseHandle(thread);
        }
    }
};
using Present = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
Present original_present = nullptr;
struct Gate {
    IDXGISwapChain4 *chain = nullptr;
    ComPtr<IDXGISwapChain4> native_owner;
    HWND window = nullptr;
    bool discover_native = false;
    RenderWorker *worker = nullptr;
    DWORD app_thread = 0;
    std::atomic<bool> armed{false}, entered{false}, completed_before_release{false};
    ULONGLONG blocked_ms = 0;
};
Gate *gate = nullptr;
HRESULT STDMETHODCALLTYPE stalled_present(IDXGISwapChain *chain, UINT interval, UINT flags) {
    auto *current = gate;
    bool exact_chain = current && chain == static_cast<IDXGISwapChain *>(current->chain);
    if (current && current->discover_native && current->armed && !exact_chain &&
        GetCurrentThreadId() == current->app_thread) {
        DXGI_SWAP_CHAIN_DESC desc{};
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(chain->GetDesc(&desc)) && desc.OutputWindow == current->window &&
            SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device))) &&
            SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&current->native_owner)))) {
            current->chain = current->native_owner.Get();
            exact_chain = true;
        }
    }
    if (current && exact_chain && GetCurrentThreadId() == current->app_thread &&
        !(flags & DXGI_PRESENT_TEST) && current->armed.exchange(false)) {
        current->entered = true;
        auto &worker = *current->worker;
        worker.gate_open = true;
        const auto begin = GetTickCount64();
        SetEvent(worker.start.handle);
        const auto result = WaitForSingleObject(worker.done.handle, 250);
        current->blocked_ms = GetTickCount64() - begin;
        current->completed_before_release = result == WAIT_OBJECT_0 && worker.finished_inside_gate;
        // Always unblock SDK presentation, including the old implementation's
        // lock-starvation path. The regression fails after both threads retire.
        worker.gate_open = false;
    }
    return original_present(chain, interval, flags);
}
struct Hooks {
    Hooks() {
        require(MH_Initialize() == MH_OK, "Overlap hook initialization failed");
    }
    void attach(IDXGISwapChain4 *chain) {
        auto *address = (*reinterpret_cast<void ***>(chain))[8];
        require(MH_CreateHook(address, reinterpret_cast<void *>(stalled_present),
                              reinterpret_cast<void **>(&original_present)) == MH_OK &&
                    MH_EnableHook(address) == MH_OK,
                "Provider Present gate hook failed");
    }
    ~Hooks() {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
};
// Emulate the private SDK's valid waitable handle even on this ordinary
// provider control. The original forwarding bug must fail this test without
// relying on a focused DLSS-G output or consuming any real pacer signals.
struct PrivatePacer {
    IDXGISwapChain2 *chain = nullptr;
    HANDLE handle = nullptr;
    UINT maximum = 5;
    unsigned sets = 0, gets = 0, waits = 0;
};
PrivatePacer *private_pacer = nullptr;
using SetLatency = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain2 *, UINT);
using GetLatency = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain2 *, UINT *);
using GetWaitable = HANDLE(STDMETHODCALLTYPE *)(IDXGISwapChain2 *);
SetLatency original_set_latency = nullptr;
GetLatency original_get_latency = nullptr;
GetWaitable original_get_waitable = nullptr;
HRESULT STDMETHODCALLTYPE tracked_set_latency(IDXGISwapChain2 *chain, UINT maximum) {
    if (private_pacer && chain == private_pacer->chain) {
        ++private_pacer->sets;
        private_pacer->maximum = maximum;
        return S_OK;
    }
    return original_set_latency(chain, maximum);
}
HRESULT STDMETHODCALLTYPE tracked_get_latency(IDXGISwapChain2 *chain, UINT *maximum) {
    if (private_pacer && chain == private_pacer->chain) {
        ++private_pacer->gets;
        if (!maximum)
            return E_INVALIDARG;
        *maximum = private_pacer->maximum;
        return S_OK;
    }
    return original_get_latency(chain, maximum);
}
HANDLE STDMETHODCALLTYPE tracked_get_waitable(IDXGISwapChain2 *chain) {
    if (private_pacer && chain == private_pacer->chain) {
        ++private_pacer->waits;
        return private_pacer->handle;
    }
    return original_get_waitable(chain);
}
void verify_waitable_contract(IDXGISwapChain *output, IDXGISwapChain4 *native) {
    ComPtr<IDXGISwapChain2> logical;
    checked(output->QueryInterface(IID_PPV_ARGS(&logical)), "Outer Output pacing interface");
    DXGI_SWAP_CHAIN_DESC desc{};
    checked(output->GetDesc(&desc), "Outer Output logical pacing flags");
    require(!(desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),
            "Wrapper control must model FFXIV's non-waitable contract");
    Event unused_signal;
    PrivatePacer observed{native, unused_signal.handle};
    struct PacingHooks {
        std::vector<void *> addresses;
        ~PacingHooks() {
            private_pacer = nullptr;
            for (auto *address : addresses) {
                MH_DisableHook(address);
                MH_RemoveHook(address);
            }
        }
        void attach(void *address, void *replacement, void **original) {
            require(MH_CreateHook(address, replacement, original) == MH_OK,
                    "Private pacer observation hook creation failed");
            addresses.push_back(address);
            require(MH_EnableHook(address) == MH_OK, "Private pacer observation hook enable failed");
        }
    } hooks;
    const auto table = *reinterpret_cast<void ***>(native);
    hooks.attach(table[31], reinterpret_cast<void *>(tracked_set_latency),
                 reinterpret_cast<void **>(&original_set_latency));
    hooks.attach(table[32], reinterpret_cast<void *>(tracked_get_latency),
                 reinterpret_cast<void **>(&original_get_latency));
    hooks.attach(table[33], reinterpret_cast<void *>(tracked_get_waitable),
                 reinterpret_cast<void **>(&original_get_waitable));
    private_pacer = &observed;
    // Positive sensor control: direct private access is visible and returns a
    // usable handle. The wrapper must never forward any of these operations.
    checked(native->SetMaximumFrameLatency(7), "Private pacer sensor set control");
    UINT maximum = 0;
    checked(native->GetMaximumFrameLatency(&maximum), "Private pacer sensor get control");
    require(maximum == 7 && native->GetFrameLatencyWaitableObject() == unused_signal.handle &&
                observed.sets == 1 && observed.gets == 1 && observed.waits == 1,
            "Private pacer sensors did not observe their direct control calls");
    observed.sets = observed.gets = observed.waits = 0;
    // Preserve the native SDR carrier's HRESULTs; their exact values are an OS
    // implementation detail. The assertions concern private pacing isolation.
    const auto set_result = logical->SetMaximumFrameLatency(1);
    const auto get_result = logical->GetMaximumFrameLatency(&maximum);
    require(logical->GetFrameLatencyWaitableObject() == nullptr,
            "Output exposed the private provider's waitable pacing signal");
    require(!observed.sets && !observed.gets && !observed.waits && observed.maximum == 7,
            "Output queried or modified the private provider's pacing policy");
    std::cout << "PASS: logical non-waitable Output hides the private SDK signal and preserves its "
                 "latency policy; native carrier Set/Get HRESULTs="
              << unsigned(set_result) << "/" << unsigned(get_result) << '\n';
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}
ComPtr<ID3D11Texture2D> texture(ID3D11Device *device, unsigned width, unsigned height, DXGI_FORMAT format,
                                const void *data, unsigned pitch) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{data, pitch, 0};
    ComPtr<ID3D11Texture2D> value;
    checked(device->CreateTexture2D(&desc, &initial, &value), "Numbered input texture");
    return value;
}
struct Stage {
    ComPtr<ID3D11Texture2D> colour, depth, motion;
    std::array<unsigned char, 4> stamp{};
    xivrender::nr::FrameInput frame;
    Stage(ID3D11Device *device, unsigned width, unsigned height, unsigned number, const void *feature) {
        const unsigned gw = width / 2, gh = height / 2;
        stamp = number == 1 ? std::array<unsigned char, 4>{211, 37, 61, 255}
                            : std::array<unsigned char, 4>{29, 193, 83, 255};
        std::vector<std::array<unsigned char, 4>> pixels(std::size_t(width) * height, stamp);
        std::vector<float> z(std::size_t(gw) * gh, number == 1 ? .25f : .75f);
        std::vector<std::array<float, 2>> mv(std::size_t(gw) * gh,
                                             {number == 1 ? -.5f / float(width) : .75f / float(width), 0});
        colour = texture(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, pixels.data(), width * 4);
        depth = texture(device, gw, gh, DXGI_FORMAT_R32_FLOAT, z.data(), gw * 4);
        motion = texture(device, gw, gh, DXGI_FORMAT_R32G32_FLOAT, mv.data(), gw * 8);
        frame.sr_feature = feature;
        auto &i = frame.images;
        i.color = colour.Get();
        i.depth = depth.Get();
        i.motion = motion.Get();
        i.output_width = width;
        i.output_height = height;
        i.guide_width = i.motion_width = gw;
        i.guide_height = i.motion_height = gh;
        i.motion_scale_x = float(width);
        i.motion_scale_y = float(height);
        i.depth_inverted = true;
    }
};
xivrender::fg::Camera camera(unsigned width, unsigned height, float position) {
    using namespace DirectX;
    xivrender::fg::Camera value;
    value.near_plane = .1f;
    value.far_plane = std::numeric_limits<float>::infinity();
    value.fov = 1;
    value.aspect = float(width) / float(height);
    value.inverted = value.infinite = value.valid = true;
    const auto cot = 1.f / std::tan(.5f);
    XMFLOAT4X4 view, projection;
    XMStoreFloat4x4(&view, XMMatrixTranslation(-position, 0, 0));
    XMStoreFloat4x4(&projection,
                    XMMatrixSet(cot / value.aspect, 0, 0, 0, 0, cot, 0, 0, 0, 0, 0, -1, 0, 0, .1f, 0));
    std::memcpy(value.view.data(), &view, sizeof(view));
    std::memcpy(value.projection.data(), &projection, sizeof(projection));
    return value;
}
void queue_camera(const xivrender::fg::Camera &value) {
    xivrender::fg::simulation_begin();
    xivrender::fg::camera(value);
    xivrender::fg::simulation_end();
}
void fill_final(ID3D11DeviceContext *context, xivrender::fg::Presenter &output, const Stage &stage) {
    ComPtr<ID3D11Resource> final;
    output.target()->GetResource(&final);
    context->CopyResource(final.Get(), stage.colour.Get());
}
void verify_final(ID3D11Device *device, ID3D11DeviceContext *context, xivrender::fg::Presenter &output,
                  const Stage &stage) {
    ComPtr<ID3D11Resource> final;
    output.target()->GetResource(&final);
    ComPtr<ID3D11Texture2D> source;
    checked(final.As(&source), "Private final texture");
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    checked(device->CreateTexture2D(&desc, nullptr, &readback), "Private final readback");
    context->CopyResource(readback.Get(), source.Get());
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> fence;
    checked(device->QueryInterface(IID_PPV_ARGS(&device5)), "D3D11 readback fence device");
    checked(context->QueryInterface(IID_PPV_ARGS(&context4)), "D3D11 readback fence context");
    checked(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "D3D11 readback fence");
    checked(context4->Signal(fence.Get(), 1), "Private final readback signal");
    context->Flush();
    Event completed;
    checked(fence->SetEventOnCompletion(1, completed.handle), "Private final completion event");
    require(WaitForSingleObject(completed.handle, 5000) == WAIT_OBJECT_0, "Private final readback timed out");
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Private final readback map");
    std::array<unsigned char, 4> pixel{};
    std::memcpy(pixel.data(), mapped.pData, pixel.size());
    context->Unmap(readback.Get(), 0);
    require(pixel == stage.stamp, "The following cohort's private final colour stamp was overwritten");
}
void wrapper_case(ID3D11Device *device11, ID3D11DeviceContext *context, IDXGIFactory2 *factory,
                  DXGI_SWAP_CHAIN_DESC desc, const std::filesystem::path &runtime, Gate &controlled,
                  Hooks &hooks, bool serialize_control) {
    // Learn native DXGI Present without competing for Output's factory hooks.
    // This seed chain is hidden, never presented, and released before the real
    // wrapper is created. The gate later discovers the real chain by HWND+D12.
    {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        checked(device11->QueryInterface(IID_PPV_ARGS(&dxgi)), "Seed DXGI device");
        checked(dxgi->GetAdapter(&adapter), "Seed DXGI adapter");
        checked(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
                "Seed native D3D12 device");
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        checked(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "Seed native D3D12 queue");
        DXGI_SWAP_CHAIN_DESC1 seed{};
        seed.Width = desc.BufferDesc.Width;
        seed.Height = desc.BufferDesc.Height;
        seed.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        seed.SampleDesc.Count = 1;
        seed.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        seed.BufferCount = 2;
        seed.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        seed.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        ComPtr<IDXGISwapChain1> raw;
        ComPtr<IDXGISwapChain4> chain;
        checked(
            factory->CreateSwapChainForHwnd(queue.Get(), desc.OutputWindow, &seed, nullptr, nullptr, &raw),
            "Seed native DXGI chain");
        checked(raw.As(&chain), "Seed native IDXGISwapChain4");
        hooks.attach(chain.Get());
    }
    xivrender::Settings settings;
    settings.fg.enabled = true;
    settings.fg.backend = xivrender::FrameGeneration::DlssG;
    xivrender::fg::configure(settings, runtime, true);
    settings.fg.enabled = false;
    settings.fg.backend = xivrender::FrameGeneration::Off;
    xivrender::fg::configure(settings); // Retain the startup Output route, use Plain now.
    xivrender::hdr::configure(settings, true);
    require(xivrender::fg::output_requested(), "Startup output route was unexpectedly lost");
    std::string error;
    require(xivrender::hdr::install_output(error), error);
    struct RemoveOutput {
        ~RemoveOutput() {
            xivrender::hdr::remove_output();
        }
    } remove_output;
    ComPtr<IDXGISwapChain> output;
    struct NativeOwner {
        Gate &value;
        ~NativeOwner() {
            value.armed = false;
            value.chain = nullptr;
            value.native_owner.Reset();
        }
    } native_owner{controlled};
    checked(factory->CreateSwapChain(device11, &desc, &output), "Real outer Output creation");
    xivrender::hdr::TransportInfo transport{};
    UINT transport_bytes = sizeof(transport);
    checked(output->GetPrivateData(xivrender::hdr::transport_info, &transport_bytes, &transport),
            "Real Output transport metadata");
    require(transport_bytes == sizeof(transport) && transport.window != 0 &&
                transport.window != reinterpret_cast<std::uint64_t>(desc.OutputWindow),
            "Swapchain was not wrapped with the private SDR carrier");
    require(xivrender::fg::status().available &&
                xivrender::fg::status().provider == xivrender::FrameGeneration::Off,
            "Wrapper control did not create its ordinary D3D12 Presenter");
    ComPtr<ID3D11Texture2D> buffer;
    ComPtr<ID3D11RenderTargetView> target;
    checked(output->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Outer Output native D3D11 buffer");
    checked(device11->CreateRenderTargetView(buffer.Get(), nullptr, &target), "Outer Output native RTV");
    const float colour[]{.1f, .2f, .3f, 1};
    context->ClearRenderTargetView(target.Get(), colour);
    const auto baseline = xivrender::fg::status();
    RenderWorker worker({});
    controlled.window = desc.OutputWindow;
    controlled.discover_native = true;
    controlled.app_thread = GetCurrentThreadId();
    controlled.worker = &worker;
    controlled.armed = true;
    if (serialize_control) {
        xivrender::d3d11_work::Guard serialized_owner;
        checked(output->Present(0, DXGI_PRESENT_ALLOW_TEARING), "Serialized outer Output Present");
    } else {
        checked(output->Present(0, DXGI_PRESENT_ALLOW_TEARING), "Real outer Output Present");
    }
    require(WaitForSingleObject(worker.done.handle, 5000) == WAIT_OBJECT_0,
            "Outer Output render worker did not retire after the gate");
    require(controlled.entered && controlled.native_owner,
            "The real outer Output's native D3D12 Present was not gated");
    require(worker.capture_ok && worker.error.empty(), "Outer Output paused capture failed");
    require(serialize_control ? !controlled.completed_before_release && !worker.finished_inside_gate
                              : controlled.completed_before_release.load(),
            serialize_control ? "Outer Output negative control did not detect its held D3D11 owner"
                              : "Outer Output retained the D3D11 owner across paced Present");
    const auto completed = xivrender::fg::status();
    require(completed.captured_frames == baseline.captured_frames + 1 &&
                completed.input_status == "FG input paused" && !completed.failures,
            "Paused capture did not traverse the real Output/Presenter capture sink");
    verify_waitable_contract(output.Get(), controlled.native_owner.Get());
    context->ClearState();
    context->Flush();
    std::cout << "PASS: real outer Output callback "
              << (serialize_control ? "expected serialized negative control"
                                    : "released native D3D11 ownership")
              << "; native D3D12 Present gate=" << controlled.blocked_ms
              << "ms; paused render capture completed "
              << (serialize_control ? "after the outer control owner released" : "before gate release")
              << ". This wrapper-off test makes no valid-cohort, colour or FG-quality claim.\n";
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::cout << std::unitbuf;
    HWND window = nullptr;
    try {
        require(argc >= 2, "Usage: fg_overlap_probe runtime [backend=3 (1=DLSSG/2=XeFG/3=FSR)] "
                           "[--serialize-control] [--output-wrapper]");
        unsigned backend = 3;
        bool serialize_control = false, output_wrapper = false;
        for (int index = 2; index < argc; ++index) {
            if (std::wstring_view(argv[index]) == L"--serialize-control")
                serialize_control = true;
            else if (std::wstring_view(argv[index]) == L"--output-wrapper")
                output_wrapper = true;
            else {
                require(index == 2, "Unknown overlap probe option");
                backend = unsigned(_wtoi(argv[index]));
            }
        }
        require(backend >= 1 && backend <= 3, "Overlap probe requires an actual FG provider");
        constexpr unsigned width = 640, height = 360;
        WNDCLASSW wc{};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"XIVRender hidden render overlap regression";
        RegisterClassW(&wc);
        window =
            CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, wc.lpszClassName, WS_POPUP,
                            -32000, -32000, width, height, nullptr, nullptr, wc.hInstance, nullptr);
        require(window != nullptr && !IsWindowVisible(window), "Hidden overlap window creation failed");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &device, nullptr, &context),
                "Overlap D3D11 device");
        ComPtr<IDXGIFactory2> factory;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Overlap DXGI factory");
        Gate controlled;
        gate = &controlled;
        Hooks hooks;
        xivrender::Settings settings;
        settings.fg.enabled = true;
        settings.fg.backend = static_cast<xivrender::FrameGeneration>(backend);
        settings.fg.factor = 2;
        settings.hdr.enabled = false;
        xivrender::fg::configure(settings, std::filesystem::absolute(argv[1]), true);
        xivrender::hdr::configure(settings, true);
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = width;
        desc.BufferDesc.Height = height;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        std::string error;
        if (output_wrapper) {
            wrapper_case(device.Get(), context.Get(), factory.Get(), desc, std::filesystem::absolute(argv[1]),
                         controlled, hooks, serialize_control);
            gate = nullptr;
            DestroyWindow(window);
            return 0;
        }
        {
            xivrender::fg::Presenter output;
            require(output.initialize(device.Get(), factory.Get(), window, desc, DXGI_FORMAT_R8G8B8A8_UNORM,
                                      error),
                    error);
            require(xivrender::fg::status().available &&
                        xivrender::fg::status().provider == settings.fg.backend,
                    "Actual provider was unavailable or silently fell back");
            controlled.chain = output.chain();
            controlled.app_thread = GetCurrentThreadId();
            hooks.attach(output.chain());
            Stage first(device.Get(), width, height, 1, &output),
                next(device.Get(), width, height, 2, &output);
            const auto camera1 = camera(width, height, 0), camera2 = camera(width, height, .01f);
            for (unsigned frame = 1; frame <= 12; ++frame) {
                queue_camera(camera1);
                xivrender::fg::render_begin();
                first.frame.frame_id = frame;
                first.frame.reset = frame == 1;
                xivrender::fg::capture(first.frame);
                fill_final(context.Get(), output, first);
                checked(output.present(0, DXGI_PRESENT_ALLOW_TEARING, true), "Warmup Present");
                Sleep(20);
            }
            queue_camera(camera1);
            xivrender::fg::render_begin();
            first.frame.frame_id = 13;
            first.frame.reset = false;
            xivrender::fg::capture(first.frame);
            fill_final(context.Get(), output, first);
            const auto baseline = xivrender::fg::status();
            require(baseline.input_status == "Native FG inputs ready", "First numbered input was rejected");
            next.frame.frame_id = 14;
            RenderWorker worker(next.frame);
            // Simulate the real producer getting one frame ahead of render.
            // The worker consumes this precise FIFO camera while Present13 is gated.
            queue_camera(camera2);
            controlled.worker = &worker;
            controlled.armed = true;
            if (serialize_control) {
                // Deliberately reproduce the former causal lock boundary:
                // production unlocks its nested recursive owner, while this
                // outer test-only owner still serializes render behind Present.
                xivrender::d3d11_work::Guard serialized_owner;
                checked(output.present(0, DXGI_PRESENT_ALLOW_TEARING, true), "Serialized control Present");
            } else {
                checked(output.present(0, DXGI_PRESENT_ALLOW_TEARING, true), "Controlled older Present");
            }
            require(WaitForSingleObject(worker.done.handle, 5000) == WAIT_OBJECT_0,
                    "Following capture did not retire after the gate released");
            require(controlled.entered, "The exact app-thread provider Present was not gated");
            require(worker.capture_ok, "Following render capture failed");
            if (serialize_control) {
                require(!controlled.completed_before_release && !worker.finished_inside_gate,
                        "The negative control did not detect the deliberately held D3D11 lock");
                std::cout << "Expected serialized negative control: worker remained blocked during the "
                             "250ms SDK-entry gate and completed after the outer D3D11 owner released.\n";
            } else {
                require(controlled.completed_before_release,
                        "Render capture remained blocked by the paced SDK Present (250ms gate)");
            }
            require(worker.error.empty(), worker.error);
            const auto captured = xivrender::fg::status();
            require(captured.captured_frames == baseline.captured_frames + 1 &&
                        captured.input_status == "Native FG inputs ready",
                    "Following frame capture was lost when the older Present returned");
            require(captured.missing_cameras == baseline.missing_cameras &&
                        captured.invalid_cameras == baseline.invalid_cameras &&
                        captured.stale_inputs == baseline.stale_inputs &&
                        captured.overwritten_inputs == baseline.overwritten_inputs,
                    "Overlap mismatched or overwrote the queued camera/image cohort");
            require(output.prepare_colour(next.colour.Get(), error), error);
            verify_final(device.Get(), context.Get(), output, next);
            checked(output.present(0, DXGI_PRESENT_ALLOW_TEARING, true), "Following cohort Present");
            const auto completed = xivrender::fg::status();
            require(completed.real_frames == baseline.real_frames + 2 &&
                        completed.skipped == baseline.skipped && !completed.failures,
                    "The older return invalidated the next cohort or the provider failed");
            require(output.drain(5000, error), error);
            context->ClearState();
            context->Flush();
            std::cout << "PASS: " << (serialize_control ? "expected serialized negative control; " : "")
                      << "actual provider " << backend << " CPU Present gate=" << controlled.blocked_ms
                      << "ms; render worker captured frame14 "
                      << (serialize_control ? "after outer-lock release" : "before gate release")
                      << "; frame13 return preserved "
                         "frame14 colour/cohort and both Presents used valid native input.\n"
                         "Colour readback covers the private final target; native scanout/interpolation "
                         "quality is covered separately by the sequence probe.\n";
        }
        gate = nullptr;
        DestroyWindow(window);
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << "FAIL: " << failure.what() << "; provider=" << xivrender::fg::status().message << '\n';
        gate = nullptr;
        if (window)
            DestroyWindow(window);
        return 1;
    }
}
