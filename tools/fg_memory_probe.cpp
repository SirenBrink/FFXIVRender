// Test only. Never-shown provider handoffs with unchanged synthetic inputs and
// per-process CPU/GPU memory samples. This is not a scanout/quality benchmark.
#include "xivrender/frame_generation.hpp"
#include "xivrender/hdr.hpp"
#include <DirectXMath.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
using Clock = std::chrono::steady_clock;
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
void checked(HRESULT result, const char *operation) {
    require(SUCCEEDED(result), std::string(operation) + ": " + std::to_string(unsigned(result)));
}
double milliseconds(Clock::duration value) {
    return std::chrono::duration<double, std::milli>(value).count();
}
struct Window {
    HWND value = nullptr;
    Window(unsigned width, unsigned height) {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = DefWindowProcW;
        wc.lpszClassName = L"XIVRender hidden provider memory probe";
        require(RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
                "Hidden memory-probe class failed");
        value = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, -32000,
                                -32000, int(width), int(height), nullptr, nullptr, wc.hInstance, nullptr);
        require(value && !IsWindowVisible(value), "The memory probe must have a never-shown window");
    }
    ~Window() {
        if (value)
            DestroyWindow(value);
    }
};
ComPtr<ID3D11Texture2D> texture(ID3D11Device *device, unsigned width, unsigned height, DXGI_FORMAT format,
                                const void *data, unsigned pitch) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{data, pitch, 0};
    ComPtr<ID3D11Texture2D> value;
    checked(device->CreateTexture2D(&desc, &initial, &value), "Immutable probe input");
    return value;
}
struct Inputs {
    ComPtr<ID3D11Texture2D> color, depth, motion;
    xivrender::nr::FrameInput frame;
    xivrender::fg::Camera camera;
    Inputs(ID3D11Device *device, unsigned width, unsigned height) {
        const auto rw = std::max(1u, width * 2 / 3), rh = std::max(1u, height * 2 / 3);
        // These CPU initialization vectors die before the baseline memory
        // sample. All three GPU inputs then remain unchanged through handoffs.
        std::vector<std::array<std::uint16_t, 4>> colors(std::size_t(width) * height,
                                                         {0x3800, 0x3600, 0x3400, 0x3c00});
        std::vector<float> depths(std::size_t(rw) * rh, .5f);
        std::vector<std::array<float, 2>> vectors(std::size_t(rw) * rh, {0, 0});
        color = texture(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, colors.data(), width * 8);
        depth = texture(device, rw, rh, DXGI_FORMAT_R32_FLOAT, depths.data(), rw * 4);
        motion = texture(device, rw, rh, DXGI_FORMAT_R32G32_FLOAT, vectors.data(), rw * 8);
        frame.hdr = true;
        frame.sr_feature = this;
        auto &i = frame.images;
        i.color = color.Get();
        i.depth = depth.Get();
        i.motion = motion.Get();
        i.output_width = width;
        i.output_height = height;
        i.guide_width = i.motion_width = rw;
        i.guide_height = i.motion_height = rh;
        i.motion_scale_x = float(width);
        i.motion_scale_y = float(height);
        i.depth_inverted = i.linear_hdr = true;
        camera.valid = camera.inverted = camera.infinite = true;
        camera.near_plane = .1f;
        camera.far_plane = std::numeric_limits<float>::infinity();
        camera.fov = 1;
        camera.aspect = float(width) / float(height);
        const float cot = 1 / std::tan(.5f);
        DirectX::XMFLOAT4X4 view, projection;
        DirectX::XMStoreFloat4x4(&view, DirectX::XMMatrixIdentity());
        DirectX::XMStoreFloat4x4(&projection, DirectX::XMMatrixSet(cot / camera.aspect, 0, 0, 0, 0, cot, 0, 0,
                                                                   0, 0, 0, -1, 0, 0, camera.near_plane, 0));
        std::memcpy(camera.view.data(), &view, sizeof(view));
        std::memcpy(camera.projection.data(), &projection, sizeof(projection));
    }
};
struct Memory {
    std::uint64_t local = 0, local_budget = 0, nonlocal = 0, nonlocal_budget = 0, private_bytes = 0,
                  working_set = 0;
    DWORD handles = 0;
};
Memory snapshot(IDXGIAdapter3 *adapter) {
    Memory value;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
    checked(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local), "Local GPU memory");
    checked(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal),
            "Non-local GPU memory");
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof(process);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&process),
                                 sizeof(process)) != FALSE,
            "Process memory counters failed");
    require(GetProcessHandleCount(GetCurrentProcess(), &value.handles) != FALSE,
            "Process handle count failed");
    value.local = local.CurrentUsage;
    value.local_budget = local.Budget;
    value.nonlocal = nonlocal.CurrentUsage;
    value.nonlocal_budget = nonlocal.Budget;
    value.private_bytes = process.PrivateUsage;
    value.working_set = process.WorkingSetSize;
    return value;
}
struct Timing {
    double p50 = 0, p95 = 0, maximum = 0;
    unsigned slow = 0;
};
Timing summarize(std::vector<double> values) {
    Timing result;
    if (values.empty())
        return result;
    result.slow = unsigned(std::count_if(values.begin(), values.end(), [](double v) { return v >= 100; }));
    std::sort(values.begin(), values.end());
    result.p50 = values[(values.size() - 1) * 50 / 100];
    result.p95 = values[(values.size() - 1) * 95 / 100];
    result.maximum = values.back();
    return result;
}
const char *name(xivrender::FrameGeneration provider) {
    switch (provider) {
    case xivrender::FrameGeneration::DlssG:
        return "DLSSG";
    case xivrender::FrameGeneration::XeFG:
        return "XeFG";
    case xivrender::FrameGeneration::FsrFG:
        return "FSR";
    default:
        return "Off";
    }
}
void row(const char *record, unsigned cycle, unsigned phase, xivrender::FrameGeneration provider,
         unsigned requested_factor, const Memory &memory, const xivrender::fg::Status &status,
         std::uint64_t real = 0, std::uint64_t displayed = 0, std::uint64_t skipped = 0, double sync_ms = 0,
         const Timing &present = {}, const Timing &source = {}) {
    std::cout << "MEMORY_CSV," << record << ',' << cycle << ',' << phase << ',' << name(provider) << ','
              << requested_factor << ',' << status.factor << ',' << memory.local << ',' << memory.local_budget
              << ',' << memory.nonlocal << ',' << memory.nonlocal_budget << ',' << memory.private_bytes << ','
              << memory.working_set << ',' << memory.handles << ',' << status.provider_changes << ',' << real
              << ',' << displayed << ',' << skipped << ',' << status.failures << ',' << sync_ms << ','
              << present.p50 << ',' << present.p95 << ',' << present.maximum << ',' << present.slow << ','
              << source.p50 << ',' << source.p95 << ',' << source.maximum << ',' << source.slow << '\n';
}
unsigned argument(wchar_t *value, unsigned minimum, unsigned maximum, const char *description) {
    wchar_t *end = nullptr;
    const auto parsed = wcstoul(value, &end, 10);
    require(value != end && end && !*end && parsed >= minimum && parsed <= maximum,
            std::string("Invalid ") + description);
    return unsigned(parsed);
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::cout << std::unitbuf << std::fixed << std::setprecision(3);
    try {
        require(argc >= 2 && argc <= 6,
                "Usage: fg_memory_probe runtime [cycles=4] [width=3840] [height=2160] [frames/phase=120]");
        const unsigned cycles = argc > 2 ? argument(argv[2], 1, 20, "cycles") : 4;
        const unsigned width = argc > 3 ? argument(argv[3], 640, 7680, "width") : 3840;
        const unsigned height = argc > 4 ? argument(argv[4], 360, 4320, "height") : 2160;
        const unsigned frames = argc > 5 ? argument(argv[5], 30, 1000, "frames per phase") : 120;
        constexpr unsigned warmup = 10, source_sleep = 16;
        const auto runtime = std::filesystem::absolute(argv[1]);
        require(std::filesystem::is_directory(runtime), "The runtime directory does not exist");
        std::cout << "Hidden provider-memory probe: " << width << 'x' << height << " PQ, " << cycles
                  << " cycles of DLSSG6/XeFG4/FSR2/Off, " << frames
                  << " sources/phase; NR disabled.\n"
                     "The unchanged FP16 HDR color and 2/3-resolution depth/MV inputs are allocated once.\n"
                     "Source CPU includes SDK simulation sleep; intentional "
                  << source_sleep << "ms inter-source sleep is excluded. First " << warmup
                  << " sources per phase are excluded from timing/steady counters.\n"
                     "Memory is process-scoped usage, not a leak assertion; compare like providers across "
                     "cycles. Off retires the provider output and uses ordinary presentation; the "
                     "cached SDK/device may retain shared allocations.\n"
                     "No window is shown or focused. Hidden DLSS-G may independently remain at1X; this is "
                     "not a scanout, visible-generation or image-quality test.\n";
        Window window(width, height);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &device, nullptr, &context),
                "Memory probe D3D11 device");
        ComPtr<IDXGIFactory2> factory;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Memory probe factory");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> native_adapter;
        ComPtr<IDXGIAdapter3> adapter;
        checked(device.As(&dxgi), "Memory probe adapter device");
        checked(dxgi->GetAdapter(&native_adapter), "Memory probe adapter");
        checked(native_adapter.As(&adapter), "Memory probe adapter3");
        Inputs inputs(device.Get(), width, height);
        context->Flush();
        using xivrender::FrameGeneration;
        xivrender::Settings cfg;
        cfg.nr.enabled = false;
        cfg.fg.enabled = true;
        cfg.fg.backend = FrameGeneration::DlssG;
        cfg.fg.factor = 6;
        cfg.fg.dynamic = false;
        cfg.fg.xefg_unlock = true;
        cfg.hdr.enabled = true;
        cfg.hdr.output = xivrender::HdrOutput::Pq10;
        xivrender::fg::configure(cfg, runtime, true);
        xivrender::hdr::configure(cfg, true);
        std::cout << "MEMORY_CSV,record,cycle,phase,provider,requested_factor,observed_factor,"
                     "local_usage_bytes,local_budget_bytes,nonlocal_usage_bytes,nonlocal_budget_bytes,"
                     "private_bytes,working_set_bytes,handles,provider_changes,steady_real,"
                     "steady_displayed,steady_skipped,total_failures,synchronize_ms,present_p50_ms,"
                     "present_p95_ms,present_max_ms,present_ge100ms,source_p50_ms,source_p95_ms,"
                     "source_max_ms,source_ge100ms\n";
        row("before_presenter", 0, 0, FrameGeneration::Off, 1, snapshot(adapter.Get()),
            xivrender::fg::status());
        std::string error;
        std::uint64_t frame_id = 0;
        {
            xivrender::fg::Presenter output;
            DXGI_SWAP_CHAIN_DESC desc{};
            desc.BufferDesc.Width = width;
            desc.BufferDesc.Height = height;
            desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            desc.OutputWindow = window.value;
            desc.Windowed = TRUE;
            require(output.initialize(device.Get(), factory.Get(), window.value, desc,
                                      DXGI_FORMAT_R10G10B10A2_UNORM, error),
                    error);
            require(xivrender::fg::status().provider == FrameGeneration::DlssG,
                    "DLSS-G startup silently fell back: " + xivrender::fg::status().message);
            const std::array<FrameGeneration, 4> providers{FrameGeneration::DlssG, FrameGeneration::XeFG,
                                                           FrameGeneration::FsrFG, FrameGeneration::Off};
            for (unsigned cycle = 1; cycle <= cycles; ++cycle) {
                for (unsigned phase = 0; phase < providers.size(); ++phase) {
                    const auto provider = providers[phase];
                    cfg.fg.backend = provider;
                    cfg.fg.enabled = provider != FrameGeneration::Off;
                    cfg.fg.factor = provider == FrameGeneration::DlssG  ? 6u
                                    : provider == FrameGeneration::XeFG ? 4u
                                                                        : 2u;
                    xivrender::fg::configure(cfg);
                    const auto sync_begin = Clock::now();
                    require(output.synchronize(error), error);
                    const auto sync_ms = milliseconds(Clock::now() - sync_begin);
                    auto status = xivrender::fg::status();
                    require(status.provider == provider, std::string("Provider switch unavailable: ") +
                                                             name(provider) + ": " + status.transition +
                                                             "; " + status.last_error);
                    checked(output.chain()->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020),
                            "Memory probe PQ activation");
                    row("phase_before", cycle, phase + 1, provider, cfg.fg.enabled ? cfg.fg.factor : 1,
                        snapshot(adapter.Get()), status, 0, 0, 0, sync_ms);
                    std::vector<double> present_times, source_times;
                    present_times.reserve(frames - warmup);
                    source_times.reserve(frames - warmup);
                    xivrender::fg::Status steady;
                    for (unsigned frame = 1; frame <= frames; ++frame) {
                        const auto source_begin = Clock::now();
                        require(output.synchronize(error), error);
                        xivrender::fg::simulation_begin();
                        xivrender::fg::camera(inputs.camera);
                        xivrender::fg::simulation_end();
                        xivrender::fg::render_begin();
                        inputs.frame.frame_id = ++frame_id;
                        inputs.frame.reset = frame_id == 1;
                        xivrender::fg::capture(inputs.frame);
                        const float pq_color[]{.3f, .4f, .5f, 1};
                        context->ClearRenderTargetView(output.target(), pq_color);
                        const auto present_begin = Clock::now();
                        checked(output.present(0, DXGI_PRESENT_ALLOW_TEARING, true), "Memory probe Present");
                        const auto completed = Clock::now();
                        status = xivrender::fg::status();
                        require(!status.failures, "Provider failure: " + status.last_error);
                        require(status.provider == provider, "Provider changed during a stable phase");
                        if (frame == warmup)
                            steady = status;
                        if (frame > warmup) {
                            present_times.push_back(milliseconds(completed - present_begin));
                            source_times.push_back(milliseconds(completed - source_begin));
                            if (provider != FrameGeneration::Off) {
                                require(status.skipped == steady.skipped,
                                        "Stable native inputs were skipped: " + status.input_status);
                                require(status.missing_cameras == steady.missing_cameras &&
                                            status.invalid_cameras == steady.invalid_cameras &&
                                            status.stale_inputs == steady.stale_inputs &&
                                            status.overwritten_inputs == steady.overwritten_inputs,
                                        "Stable native camera/image cohorts regressed");
                            }
                        }
                        Sleep(source_sleep);
                        MSG message{};
                        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                            DispatchMessageW(&message);
                    }
                    context->Flush();
                    Sleep(100); // Let the last short present interval settle; no forced provider stop.
                    status = xivrender::fg::status();
                    row("phase_after", cycle, phase + 1, provider, cfg.fg.enabled ? cfg.fg.factor : 1,
                        snapshot(adapter.Get()), status, status.real_frames - steady.real_frames,
                        status.displayed - steady.displayed, status.skipped - steady.skipped, sync_ms,
                        summarize(present_times), summarize(source_times));
                }
            }
            cfg.fg.enabled = false;
            cfg.fg.backend = FrameGeneration::Off;
            xivrender::fg::configure(cfg);
            require(output.synchronize(error), error);
            require(output.drain(5000, error), error);
            context->ClearState();
            context->Flush();
        }
        Sleep(1000);
        row("after_presenter_idle1s", cycles, 0, FrameGeneration::Off, 1, snapshot(adapter.Get()),
            xivrender::fg::status());
        std::cout << "PASS: " << cycles << " repeated DLSSG/XeFG/FSR/Off cycles, " << frame_id
                  << " native inputs, unchanged input allocations, no provider errors or steady active "
                     "cohort skips. Compare like-provider memory/timing rows for retention or slowdown; "
                     "no absolute leak/growth threshold is asserted. Input textures and native D3D11 "
                     "owners remain alive in the final post-Presenter sample.\n";
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << "FAIL: " << failure.what() << "; FG=" << xivrender::fg::status().message << '\n';
        return 1;
    }
}
