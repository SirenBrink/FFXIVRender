#include "xivrender/frame_generation.hpp"
#include "xivrender/fg_diagnostics.hpp"
#include "xivrender/fg_policy.hpp"
#include "xivrender/hdr.hpp"
#include "xivrender/nr_pipeline.hpp"
#include "xivrender/d3d11_identity.hpp"
#include <MinHook.h>
#include <DirectXMath.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <fstream>
#include <atomic>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
using Microsoft::WRL::ComPtr;
bool desktop_auto_start = false;
std::atomic<bool> start_test{false}, test_cancelled{false};
LRESULT CALLBACK test_window(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_KEYDOWN && w == VK_SPACE)
        start_test = true;
    if (message == WM_DESTROY) {
        test_cancelled = true;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool latency_matches(const xivrender::fg::Status &s, const xivrender::FrameGenerationSettings &settings) {
    using xivrender::FrameGeneration;
    // The selected controller is independent where the FG SDK permits it.
    const auto provider = xivrender::fg::latency_provider(settings);
    if (provider == FrameGeneration::Off)
        return !s.latency && s.latency_name == "Disabled";
    if (provider == FrameGeneration::DlssG)
        return s.latency && s.latency_name == "NVIDIA Reflex";
    if (provider == FrameGeneration::XeFG)
        return s.latency && s.latency_name.find("XeLL") != std::string::npos;
    if (provider == FrameGeneration::FsrFG) {
        // A rejected driver initialization must not disable FSR or claim success.
        return s.latency ? s.latency_name == "AMD Anti-Lag 2"
                         : s.latency_name.find("Anti-Lag 2 unavailable") != std::string::npos ||
                               (!settings.enabled && s.latency_name == "Unavailable");
    }
    return !s.latency;
}
ComPtr<ID3D11Texture2D> texture(ID3D11Device *d, unsigned w, unsigned h, DXGI_FORMAT format, const void *data,
                                unsigned pitch) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
    ComPtr<ID3D11Texture2D> t;
    require(SUCCEEDED(d->CreateTexture2D(&desc, data ? &init : nullptr, &t)),
            "FG test texture creation failed");
    return t;
}
void combined_nr_test(ID3D11Device *device, ID3D11DeviceContext *context, xivrender::fg::Presenter &output,
                      xivrender::Settings cfg, const std::filesystem::path &runtime, unsigned width,
                      unsigned height, bool hidden, bool live, bool full_nr, bool native_drs) {
    using xivrender::FrameGeneration;
    using namespace DirectX;
    // Pipeline consumes the same production layout as the addon, not the
    // presenter's runtime-directory argument.
    const auto root = runtime.parent_path().parent_path();
    require(root / L"XIVRender/runtime" == runtime,
            "Combined NR test requires an addon-root/XIVRender/runtime directory");
    require(std::filesystem::is_regular_file(runtime / L"XIVRender_nvngx.dll_nr.dll") &&
                std::filesystem::is_regular_file(runtime / L"nr/310.9.16-compat/nvngx_dlssnr.dll"),
            "Combined NR test requires the production private NR bridge and 310.9.16 model");
    constexpr std::uint16_t alpha = 0x3800; // Exactly 0.5, independent of model alpha.
    constexpr unsigned depth_x = 13, depth_y = 7, motion_x = 19, motion_y = 11;
    const auto guide_width = std::max(1u, width * 2257u / 3840u);
    const auto guide_height = std::max(1u, height * 1270u / 2160u);
    const auto padded_width = (guide_width + motion_x + 63u) & ~63u;
    const auto padded_height = (guide_height + motion_y + 63u) & ~63u;
    std::vector<std::array<std::uint16_t, 4>> pixels(std::size_t(width) * height);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            auto &p = pixels[std::size_t(y) * width + x];
            p = {std::uint16_t(0x3000 + (x % 32) * 16), std::uint16_t(0x3400 + (y % 16) * 8),
                 std::uint16_t(0x3800 + ((x / 32 + y / 32) % 4) * 16), alpha};
            if (cfg.hdr.enabled && x > width * 3 / 4)
                p[0] = 0x4400; // Linear 4.0 highlight, above SDR white.
        }
    std::vector<float> depth(std::size_t(padded_width) * padded_height, .125f);
    for (unsigned y = 0; y < guide_height; ++y)
        std::fill_n(depth.data() + std::size_t(depth_y + y) * padded_width + depth_x, guide_width, .5f);
    const std::vector<std::array<std::uint16_t, 2>> motion(std::size_t(padded_width) * padded_height, {0, 0});
    auto clean = texture(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, pixels.data(), width * 8);
    auto colour = texture(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, pixels.data(), width * 8);
    auto z =
        texture(device, padded_width, padded_height, DXGI_FORMAT_R32_FLOAT, depth.data(), padded_width * 4);
    auto mv = texture(device, padded_width, padded_height, DXGI_FORMAT_R16G16_FLOAT, motion.data(),
                      padded_width * 4);
    ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC staging_desc{};
    colour->GetDesc(&staging_desc);
    staging_desc.BindFlags = 0;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(device->CreateTexture2D(&staging_desc, nullptr, &staging)),
            "NR staging creation failed");
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> readback_fence;
    require(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))) &&
                SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4))) &&
                SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&readback_fence))),
            "NR readback fence creation failed");
    struct Event {
        HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ~Event() {
            if (value)
                CloseHandle(value);
        }
    } event;
    require(event.value != nullptr, "NR readback event creation failed");
    std::uint64_t readback_value = 0, frame_id = 0;
    xivrender::nr::Pipeline pipeline;
    pipeline.Attach(device, root);
    cfg.nr.enabled = true;
    cfg.nr.passes = 2;
    cfg.nr.model_scale = full_nr ? 1.f : .5f;
    cfg.nr.hdr_input = cfg.hdr.enabled;
    cfg.nr.pause_on_focus_loss = true;
    const auto fresh_frame = [&] {
        // A GPU copy supplies a clean native-SR output each frame. Never feed
        // the prior frame's NR composition back into the model accidentally.
        context->CopyResource(colour.Get(), clean.Get());
        xivrender::nr::FrameInput f;
        f.sr_feature = &pipeline;
        f.frame_id = ++frame_id;
        f.reset = frame_id == 1;
        f.hdr = cfg.hdr.enabled;
        auto &i = f.images;
        i.color = colour.Get();
        i.depth = z.Get();
        i.motion = mv.Get();
        i.output_width = width;
        i.output_height = height;
        i.guide_width = i.motion_width = guide_width;
        i.guide_height = i.motion_height = guide_height;
        if (native_drs) {
            constexpr unsigned percentages[]{100, 80, 95, 85, 90};
            const auto scale = percentages[frame_id % std::size(percentages)];
            i.guide_width = i.motion_width = std::max(1u, guide_width * scale / 100);
            i.guide_height = i.motion_height = std::max(1u, guide_height * scale / 100);
        }
        i.depth_x = depth_x;
        i.depth_y = depth_y;
        i.motion_x = motion_x;
        i.motion_y = motion_y;
        i.motion_scale_x = float(width);
        i.motion_scale_y = float(height);
        i.depth_inverted = true;
        i.linear_hdr = cfg.hdr.enabled;
        return f;
    };
    const auto readback = [&](bool expect_change, const char *label) {
        context->CopyResource(staging.Get(), colour.Get());
        const auto token = ++readback_value;
        require(SUCCEEDED(context4->Signal(readback_fence.Get(), token)), "NR readback signal failed");
        context->Flush();
        require(SUCCEEDED(readback_fence->SetEventOnCompletion(token, event.value)) &&
                    WaitForSingleObject(event.value, 15000) == WAIT_OBJECT_0 &&
                    readback_fence->GetCompletedValue() >= token,
                "NR combined readback timed out");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
                "NR readback map failed");
        std::uint64_t changed = 0, invalid = 0, alpha_errors = 0;
        for (unsigned y = 0; y < height; ++y) {
            const auto *row =
                static_cast<const unsigned char *>(mapped.pData) + std::size_t(y) * mapped.RowPitch;
            for (unsigned x = 0; x < width; ++x) {
                std::array<std::uint16_t, 4> actual{};
                std::memcpy(actual.data(), row + std::size_t(x) * 8, 8);
                alpha_errors += actual[3] != alpha;
                for (unsigned c = 0; c < 3; ++c) {
                    invalid += (actual[c] & 0x7c00) == 0x7c00;
                    changed += actual[c] != pixels[std::size_t(y) * width + x][c];
                }
            }
        }
        context->Unmap(staging.Get(), 0);
        std::cout << label << ": changed RGB=" << changed << ", NaN/Inf=" << invalid
                  << ", alpha errors=" << alpha_errors << '\n'
                  << std::flush;
        require(!invalid && !alpha_errors && (expect_change ? changed != 0 : changed == 0),
                "NR RGB/alpha control readback failed");
    };
    auto disabled = cfg.nr;
    disabled.enabled = false;
    require(!pipeline.Process(fresh_frame(), disabled, true) && pipeline.Status().applied == 0,
            "Disabled NR control incorrectly reported an applied frame");
    readback(false, "Disabled NR clean control");
    // Creation may complete asynchronously. Warm it separately so no clean
    // initialization frame can be counted as steady NR/FG evidence.
    const auto deadline = GetTickCount64() + 30000;
    while (pipeline.Status().applied < 3 && GetTickCount64() < deadline) {
        pipeline.Process(fresh_frame(), cfg.nr, true);
        require(!pipeline.Status().failed, "NR combined warmup failed: " + pipeline.Status().message);
        if (pipeline.Status().applied < 3)
            Sleep(1);
    }
    require(pipeline.Status().applied >= 3, "NR combined warmup timed out: " + pipeline.Status().message);
    const auto ready = pipeline.Status();
    xivrender::fg::Camera camera;
    camera.valid = camera.infinite = camera.inverted = true;
    camera.near_plane = .1f;
    camera.far_plane = std::numeric_limits<float>::infinity();
    camera.fov = 1;
    camera.aspect = float(width) / height;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4 *>(camera.view.data()), XMMatrixIdentity());
    // FFXIV's row-major right-handed, reversed-Z infinite projection.
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4 *>(camera.projection.data()),
                    XMMatrixSet(1 / std::tan(.5f) / camera.aspect, 0, 0, 0, 0, 1 / std::tan(.5f), 0, 0, 0, 0,
                                0, -1, 0, 0, .1f, 0));
    struct Phase {
        FrameGeneration provider;
        bool enabled;
        unsigned frames;
    };
    std::vector<Phase> phases{{cfg.fg.backend, true, 120}};
    if (live) {
        const bool scrgb = cfg.hdr.enabled && cfg.hdr.output == xivrender::HdrOutput::ScRgb16Float;
        if (!scrgb) {
            phases.push_back({FrameGeneration::XeFG, true, 120});
            phases.push_back({FrameGeneration::FsrFG, true, 120});
            phases.push_back({FrameGeneration::DlssG, true, 120});
        }
        phases.push_back({scrgb ? FrameGeneration::FsrFG : FrameGeneration::DlssG, false, 30});
        phases.push_back({scrgb ? FrameGeneration::FsrFG : FrameGeneration::DlssG, true, 120});
    }
    auto previous = xivrender::fg::status();
    require(output.device() != nullptr, "Combined NR/FG has no private presentation device");
    for (const auto &phase : phases) {
        const auto expected_provider = phase.enabled ? phase.provider : FrameGeneration::Off;
        const auto prior_generation = output.generation();
        const auto prior_changes = previous.provider_changes;
        const bool output_change = previous.provider != expected_provider;
        cfg.fg.backend = phase.provider;
        cfg.fg.enabled = phase.enabled;
        cfg.fg.factor = phase.provider == FrameGeneration::DlssG  ? 6u
                        : phase.provider == FrameGeneration::XeFG ? 4u
                                                                  : cfg.fg.fsr_mfg ? 4u : 2u;
        xivrender::fg::configure(cfg);
        unsigned generated = 0;
        const auto before_phase = pipeline.Status();
        const auto phase_start = GetTickCount64();
        for (unsigned n = 0; n < phase.frames; ++n) {
            std::string error;
            require(output.synchronize(error), "NR/FG provider synchronization failed: " + error);
            require(output.generation() == prior_generation + unsigned(output_change) &&
                        xivrender::fg::status().provider_changes == prior_changes + unsigned(output_change),
                    "NR/FG did not perform exactly one output handoff for the requested provider");
            require(output.device() != nullptr, "NR/FG handoff lost its presentation device");
            xivrender::fg::simulation_begin(); // Only the selected provider's pacing is allowed here.
            xivrender::fg::camera(camera);
            xivrender::fg::simulation_end();
            xivrender::fg::render_begin();
            const auto input = fresh_frame();
            const auto before = pipeline.Status();
            const bool applied = pipeline.Process(input, cfg.nr, true);
            const auto after = pipeline.Status();
            require(applied && after.active && !after.failed && after.applied == before.applied + 1 &&
                        after.evaluations == before.evaluations + 1 && after.skipped == ready.skipped &&
                        after.failures == ready.failures && after.creations == ready.creations,
                    "Steady NR frame was omitted or recreated during FG: " + after.message);
            xivrender::fg::capture(input);
            const float gray[]{.3f, .4f, .5f, 1};
            context->ClearRenderTargetView(output.target(), gray);
            require(SUCCEEDED(output.present(0, DXGI_PRESENT_ALLOW_TEARING, true)),
                    "Combined NR/FG Present failed: " + xivrender::fg::status().message);
            const auto observed = xivrender::fg::status();
            require(!observed.failures && observed.real_frames >= previous.real_frames &&
                        observed.displayed >= previous.displayed &&
                        observed.provider_changes >= previous.provider_changes,
                    "Combined NR/FG provider failed or reset cumulative counters: " + observed.message);
            if (n >= 4) {
                require(observed.provider == expected_provider,
                        "Combined NR/FG settled on the wrong provider");
                require(latency_matches(observed, cfg.fg),
                        "Combined NR/FG latency controller does not match its provider/checkbox");
                if (!phase.enabled)
                    require(!observed.active && observed.factor == 1 &&
                                observed.displayed == previous.displayed + 1,
                            "Disabled FG did not use the ordinary one-Present output route");
                if (observed.active && observed.factor == cfg.fg.factor)
                    ++generated;
            }
            previous = observed;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                DispatchMessageW(&message);
            require(!test_cancelled, "Combined NR/FG test was cancelled");
        }
        require(hidden || !phase.enabled || generated >= 3,
                "Combined NR/FG did not verify generated frames on the focused output");
        const auto after_phase = pipeline.Status();
        std::cout << "Combined NR/FG provider=" << unsigned(phase.provider) << " enabled=" << phase.enabled
                  << ": steady NR=" << after_phase.applied - before_phase.applied
                  << ", paced=" << after_phase.paced - before_phase.paced
                  << ", new skips=" << after_phase.skipped - before_phase.skipped
                  << ", generated observations=" << generated
                  << ", elapsed ms=" << GetTickCount64() - phase_start << '\n'
                  << std::flush;
    }
    readback(true, "Final production NR/FG image");
    std::string error;
    require(output.drain(3000, error), error);
    require(pipeline.Shutdown(15000, error), "NR combined shutdown failed: " + error);
    std::cout << "PASS: real two-pass NR before FG, continuous steady application, provider/latency changes, "
                 "ordinary Off output, isolated provider devices, finite RGB, preserved alpha and bounded "
                 "retirement\n";
    if (hidden)
        std::cout << "Hidden lifecycle/NR queue test only; generated observations do not prove visible "
                     "interpolation\n";
}
int wmain(int argc, wchar_t **argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::wstring_view(argv[index]) == L"--d12-debug") {
            xivrender::fg::enable_probe_debug();
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        }
    }
    try {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        const bool interactive = argc == 1 || desktop_auto_start;
        if (interactive) {
            wchar_t path[32768]{};
            GetModuleFileNameW(nullptr, path, 32768);
            auto *log =
                new std::ofstream(std::filesystem::path(path).parent_path() / L"fg-desktop-output.txt");
            std::cout.rdbuf(log->rdbuf());
            std::cerr.rdbuf(log->rdbuf());
            std::cout << std::unitbuf;
            std::cerr << std::unitbuf;
        }
        std::vector<std::wstring> defaults;
        std::vector<wchar_t *> arguments;
        if (argc == 1) {
            wchar_t exe[32768]{};
            GetModuleFileNameW(nullptr, exe, 32768);
            defaults = {exe, (std::filesystem::path(exe).parent_path().parent_path() / L"runtime").wstring(),
                        L"1", L"pq", L"--wait-focus"};
            std::ifstream selection(std::filesystem::path(exe).parent_path() / L"fg-desktop-case.txt");
            std::string mode;
            if (selection >> mode && (mode == "sdr" || mode == "pq" || mode == "scrgb"))
                defaults[3] = std::wstring(mode.begin(), mode.end());
            unsigned backend = 1, width = 1280, height = 720, transport = 0;
            if (selection >> backend >> width >> height >> transport) {
                defaults[2] = std::to_wstring(backend);
                defaults.push_back(std::to_wstring(width));
                defaults.push_back(std::to_wstring(height));
                defaults.push_back(transport ? L"--through-output" : L"--direct");
                std::string runtime;
                if (selection >> runtime)
                    defaults[1] = std::filesystem::path(runtime).wstring();
                std::string test_mode;
                if (selection >> test_mode && test_mode == "live")
                    defaults.push_back(L"live");
            }
            for (auto &v : defaults)
                arguments.push_back(v.data());
            argc = int(arguments.size());
            argv = arguments.data();
        }
        if (argc < 3) {
            std::cerr << "Usage: fg_probe runtime backend[1/2/3] [sdr/pq/scrgb] [--wait-focus] "
                         "[width height --direct [live]] [--hidden] [--nr|--nr-full]\n";
            return 2;
        }
        const unsigned w = argc > 5 ? unsigned(_wtoi(argv[5])) : 1280,
                       h = argc > 6 ? unsigned(_wtoi(argv[6])) : 720;
        const bool through_output = argc > 7 && std::wstring_view(argv[7]) == L"--through-output";
        const bool live =
            argc > 8 && (std::wstring_view(argv[8]) == L"live" || std::wstring_view(argv[8]) == L"--live");
        bool hidden = false, with_nr = false, full_nr = false, native_drs = false, settings_stress = false;
        bool pipeline_lag = false;
        bool enabler = false;
        for (int index = 1; index < argc; ++index) {
            hidden |= std::wstring_view(argv[index]) == L"--hidden";
            with_nr |= std::wstring_view(argv[index]) == L"--nr";
            full_nr |= std::wstring_view(argv[index]) == L"--nr-full";
            native_drs |= std::wstring_view(argv[index]) == L"--native-drs";
            settings_stress |= std::wstring_view(argv[index]) == L"--settings-stress";
            pipeline_lag |= std::wstring_view(argv[index]) == L"--pipeline-lag";
            enabler |= std::wstring_view(argv[index]) == L"--enabler";
        }
        with_nr |= full_nr;
        // Hidden runs may exercise the real Output wrapper too. They validate
        // transport/lifetime and coherent requests, not focused interpolation.
        require(!with_nr || !through_output, "Combined NR acceptance uses the direct presenter only");
        require(!settings_stress || (live && !with_nr), "Settings stress requires live mode without --nr");
        require(!pipeline_lag || !with_nr, "Pipeline-lag mode uses the ordinary FG probe");
        require(w >= 640 && w <= 7680 && h >= 360 && h <= 4320, "Invalid test dimensions");
        WNDCLASSW wc{};
        wc.lpfnWndProc = test_window;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"XIVRender isolated FG probe";
        RegisterClassW(&wc);
        HWND prior = hidden ? nullptr : GetForegroundWindow();
        HWND window = CreateWindowExW(hidden ? WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW : WS_EX_APPWINDOW,
                                      wc.lpszClassName, L"XIVRender FG validation",
                                      hidden ? WS_POPUP : WS_OVERLAPPEDWINDOW, hidden ? -32000 : 0,
                                      hidden ? -32000 : 0, w, h, nullptr, nullptr, wc.hInstance, nullptr);
        require(window, "FG probe window failed");
        if (!hidden) {
            ShowWindow(window, SW_SHOWNORMAL);
            SetForegroundWindow(window);
        }
        std::cout << "Window focus=" << (GetForegroundWindow() == window)
                  << ", active=" << (GetActiveWindow() == window) << ", visible=" << IsWindowVisible(window)
                  << "\n"
                  << std::flush;
        ComPtr<ID3D11Device> d;
        ComPtr<ID3D11DeviceContext> context;
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                            D3D11_SDK_VERSION, &d, nullptr, &context)),
                "D3D11 hardware unavailable");
        ComPtr<IDXGIFactory> factory;
        require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory failed");
        xivrender::Settings cfg;
        cfg.dlss.enabled = !native_drs;
        cfg.dlss.quality = native_drs ? xivrender::DlssQuality::Game : xivrender::DlssQuality::Balanced;
        cfg.fg.enabled = true;
        cfg.fg.backend = static_cast<xivrender::FrameGeneration>(_wtoi(argv[2]));
        cfg.fg.factor = 2;
        cfg.fg.fsr_mfg = enabler;
        for (int index = 1; index < argc; ++index) {
            if (std::wstring_view(argv[index]) == L"--native-xefg") cfg.fg.xefg_unlock = false;
            if (std::wstring_view(argv[index]) == L"--latency-xell") cfg.fg.latency = xivrender::LatencyReduction::XeLL;
            if (std::wstring_view(argv[index]) == L"--latency-antilag") cfg.fg.latency = xivrender::LatencyReduction::AntiLag2;
            if (std::wstring_view(argv[index]) == L"--latency-off") cfg.fg.latency = xivrender::LatencyReduction::Off;
        }

        cfg.hdr.enabled = argc > 3 && std::wstring_view(argv[3]) != L"sdr";
        cfg.hdr.output = argc > 3 && std::wstring_view(argv[3]) == L"scrgb"
                             ? xivrender::HdrOutput::ScRgb16Float
                             : xivrender::HdrOutput::Pq10;
        const bool live_scrgb =
            live && cfg.hdr.enabled && cfg.hdr.output == xivrender::HdrOutput::ScRgb16Float;
        if (hidden)
            require(cfg.fg.backend == xivrender::FrameGeneration::XeFG ||
                        cfg.fg.backend == xivrender::FrameGeneration::FsrFG,
                    "Hidden acceptance must start with XeFG or FSR FG");
        if (live && !hidden)
            cfg.fg.backend =
                live_scrgb ? xivrender::FrameGeneration::FsrFG : xivrender::FrameGeneration::DlssG;
        const auto initial_provider = cfg.fg.backend;
        const auto format = !cfg.hdr.enabled                               ? DXGI_FORMAT_R8G8B8A8_UNORM
                            : cfg.hdr.output == xivrender::HdrOutput::Pq10 ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                                           : DXGI_FORMAT_R16G16B16A16_FLOAT;
        xivrender::fg::configure(cfg, std::filesystem::absolute(argv[1]), true);
        xivrender::hdr::configure(cfg, true);
        if (through_output) {
            require(MH_Initialize() == MH_OK, "Output interception initialization failed");
            std::string error;
            require(xivrender::hdr::install_output(error), error);
        }
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = w;
        desc.BufferDesc.Height = h;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        std::string error;
        {
            xivrender::fg::Presenter output;
            ComPtr<IDXGISwapChain> host_chain;
            ComPtr<ID3D11Texture2D> host_buffer;
            ComPtr<ID3D11RenderTargetView> host_target;
            if (through_output) {
                require(SUCCEEDED(factory->CreateSwapChain(d.Get(), &desc, &host_chain)),
                        "ReShade output creation failed");
                require(xivrender::fg::status().available,
                        "FG silently fell back during output creation: " + xivrender::fg::status().message);
                require(SUCCEEDED(host_chain->GetBuffer(0, IID_PPV_ARGS(&host_buffer))),
                        "SDR carrier buffer unavailable");
                require(SUCCEEDED(d->CreateRenderTargetView(host_buffer.Get(), nullptr, &host_target)),
                        "SDR carrier RTV failed");
                ComPtr<ID3D11Device> owner;
                require(SUCCEEDED(host_chain->GetDevice(IID_PPV_ARGS(&owner))),
                        "ReShade lost the native D3D11 identity");
                require(xivrender::d3d11_identity::native_device(owner.Get()) ==
                            xivrender::d3d11_identity::native_device(d.Get()),
                        "ReShade output changed the native D3D11 device");
            } else {
                require(output.initialize(d.Get(), factory.Get(), window, desc, format, error), error);
                require(
                    SUCCEEDED(output.chain()->SetColorSpace1(
                        !cfg.hdr.enabled                          ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                        : format == DXGI_FORMAT_R10G10B10A2_UNORM ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                                                  : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)),
                    "FG colorspace failed");
            }
            DXGI_SWAP_CHAIN_DESC actual{};
            if (through_output)
                host_chain->GetDesc(&actual);
            else
                output.chain()->GetDesc(&actual);
            std::cout << "SDK output HWND matches=" << (actual.OutputWindow == window) << "\n" << std::flush;
            if (with_nr) {
                combined_nr_test(d.Get(), context.Get(), output, cfg, std::filesystem::absolute(argv[1]), w,
                                 h, hidden, live, full_nr, native_drs);
            } else {
                require(through_output || output.device(),
                        "Direct FG probe has no private presentation device");
                unsigned output_width = w, output_height = h, guide_width = 0, guide_height = 0;
                constexpr unsigned depth_x = 13, depth_y = 7, motion_x = 19, motion_y = 11;
                ComPtr<ID3D11Texture2D> z, mv, color;
                xivrender::fg::Camera camera;
                camera.valid = true;
                camera.near_plane = .1f;
                camera.far_plane = 10000;
                camera.fov = 1;
                camera.infinite = true;
                camera.inverted = true;
                using namespace DirectX;
                XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4 *>(camera.view.data()), XMMatrixIdentity());
                const auto allocate_inputs = [&] {
                    // Match Balanced's native-style active region rather than
                    // pretending that padded allocation dimensions are scene size.
                    guide_width = std::max(1u, output_width * 2257u / 3840u);
                    guide_height = std::max(1u, output_height * 1270u / 2160u);
                    if (native_drs) {
                        guide_width = output_width;
                        guide_height = output_height;
                    }
                    const unsigned padded_width = (guide_width + motion_x + 63u) & ~63u,
                                   padded_height = (guide_height + motion_y + 63u) & ~63u;
                    std::vector<float> depth(padded_width * padded_height, .125f),
                        motion(padded_width * padded_height * 2, 0),
                        colour(output_width * output_height * 4, .5f);
                    if (enabler)
                        for (unsigned index = 0; index < padded_width * padded_height; ++index) motion[index * 2] = -2.f;
                    for (unsigned y = 0; y < guide_height; ++y)
                        std::fill_n(depth.data() + (depth_y + y) * padded_width + depth_x, guide_width, .5f);
                    z = texture(d.Get(), padded_width, padded_height, DXGI_FORMAT_R32_FLOAT, depth.data(),
                                padded_width * 4);
                    mv = texture(d.Get(), padded_width, padded_height, DXGI_FORMAT_R32G32_FLOAT,
                                 motion.data(), padded_width * 8);
                    color = texture(d.Get(), output_width, output_height, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                    colour.data(), output_width * 16);
                    camera.aspect = float(output_width) / output_height;
                    const auto p = XMMatrixSet(1 / std::tan(.5f) / camera.aspect, 0, 0, 0, 0,
                                               1 / std::tan(.5f), 0, 0, 0, 0, 0, 1, 0, 0, .1f, 0);
                    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4 *>(camera.projection.data()), p);
                    std::cout << "Native guide region " << guide_width << 'x' << guide_height
                              << " at depth origin " << depth_x << ',' << depth_y << " and motion origin "
                              << motion_x << ',' << motion_y << " inside " << padded_width << 'x'
                              << padded_height << "; output=" << output_width << 'x' << output_height << '\n'
                              << std::flush;
                };
                allocate_inputs();
                std::cout << "Initialized provider " << _wtoi(argv[2])
                          << ", maximum=" << xivrender::fg::status().maximum
                          << ", latency=" << xivrender::fg::status().latency_name << '\n'
                          << std::flush;
                if (interactive) {
                    std::cout << "Press Space in the test window to begin\n" << std::flush;
                    const auto deadline = GetTickCount64() + 300000;
                    while (!start_test && !test_cancelled && GetTickCount64() < deadline) {
                        if (desktop_auto_start && GetForegroundWindow() == window)
                            start_test = true;
                        MSG m{};
                        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
                            DispatchMessageW(&m);
                        Sleep(50);
                    }
                    require(start_test, "Interactive GPU test was not started");
                }
                if (!hidden && argc > 4 && std::wstring_view(argv[4]) == L"--wait-focus") {
                    std::cout << "Waiting for focused test window\n" << std::flush;
                    const auto deadline = GetTickCount64() + 60000;
                    while (GetForegroundWindow() != window && GetTickCount64() < deadline) {
                        MSG m{};
                        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
                            DispatchMessageW(&m);
                        Sleep(50);
                    }
                    require(GetForegroundWindow() == window,
                            "Normal desktop did not focus the isolated test window");
                }
                unsigned recovered_max_frames = 0;
                std::array<unsigned, 6> verified_live_frames{};
                std::array<unsigned, 6> generated_live_frames{};
                auto previous_status = xivrender::fg::status();
                std::uint64_t retained_generation = 0, retained_changes = 0;
                unsigned steady_drs_frames = 0;
                const unsigned frame_count = settings_stress ? 650u : live ? 200u : 90u;
                if (live_scrgb)
                    std::cout
                        << "Live scRGB: unsupported DLSS-G/XeFG targets are skipped; testing FSR and Off"
                           " transitions\n"
                        << std::flush;
                if (pipeline_lag) {
                    xivrender::fg::simulation_begin();
                    xivrender::fg::camera(camera);
                    xivrender::fg::simulation_end();
                    xivrender::fg::render_begin();
                }
                for (unsigned frame = 1; frame <= frame_count; ++frame) {
                    bool option_only_change = false;
                    if (settings_stress && frame >= 205) {
                        using xivrender::FrameGeneration;
                        struct Request {
                            unsigned at;
                            FrameGeneration provider;
                            unsigned factor;
                            bool dynamic;
                            unsigned target;
                            bool enabled, latency;
                        };
                        constexpr Request edits[]{
                            {205, FrameGeneration::DlssG, 2, false, 120, true, true},
                            {245, FrameGeneration::DlssG, 6, false, 120, true, true},
                            {285, FrameGeneration::DlssG, 6, true, 120, true, true},
                            {325, FrameGeneration::DlssG, 6, true, 165, true, true},
                            {365, FrameGeneration::DlssG, 3, false, 120, true, true},
                            {405, FrameGeneration::XeFG, 4, false, 120, true, true},
                            {465, FrameGeneration::FsrFG, 2, false, 120, true, true},
                            {525, FrameGeneration::FsrFG, 2, false, 120, false, false},
                            {585, FrameGeneration::XeFG, 4, false, 120, true, true}};
                        for (const auto &edit : edits) if (edit.at == frame) {
                            option_only_change = cfg.fg.backend == edit.provider && cfg.fg.enabled && edit.enabled;
                            cfg.fg.backend = edit.provider; cfg.fg.factor = edit.factor;
                            cfg.fg.dynamic = edit.dynamic; cfg.fg.target_fps = edit.target;
                            cfg.fg.enabled = edit.enabled; cfg.fg.reflex = edit.latency;
                            xivrender::fg::configure(cfg);
                            std::cout << "Stress edit at " << frame << ": provider=" << unsigned(edit.provider)
                                      << " factor=" << edit.factor << " dynamic=" << edit.dynamic
                                      << " target=" << edit.target << " enabled=" << edit.enabled << '\n' << std::flush;
                        }
                    }
                    if (enabler && !live && frame == 20) {
                        cfg.fg.factor = 3;
                        xivrender::fg::configure(cfg);
                    }
                    if (!live && frame == 40) {
                        cfg.fg.factor = xivrender::fg::status().maximum;
                        xivrender::fg::configure(cfg);
                    }
                    if (live && (frame == 20 || frame == 40 || frame == 80 || frame == 120 || frame == 150 ||
                                 frame == 165)) {
                        using xivrender::FrameGeneration;
                        if (frame == 20)
                            cfg.fg.factor = live_scrgb ? 2u : initial_provider == FrameGeneration::FsrFG ? (enabler ? 4u : 2u)
                                            : initial_provider == FrameGeneration::XeFG              ? 4u
                                                                                                     : 6u;
                        if (frame == 40) {
                            cfg.fg.backend = live_scrgb ? FrameGeneration::FsrFG : FrameGeneration::XeFG;
                            cfg.fg.factor = live_scrgb ? 2u : 4u;
                        }
                        if (frame == 80) {
                            cfg.fg.backend = FrameGeneration::FsrFG;
                            cfg.fg.factor = enabler ? 3u : 2u;
                        }
                        if (frame == 120) {
                            cfg.fg.backend = live_scrgb ? FrameGeneration::FsrFG : FrameGeneration::DlssG;
                            cfg.fg.factor = live_scrgb ? 2u : 6u;
                        }
                        if (frame == 150)
                            cfg.fg.enabled = false;
                        if (frame == 165)
                            cfg.fg.enabled = true;
                        xivrender::fg::configure(cfg);
                        std::cout << "Live request at frame " << frame
                                  << ": provider=" << (cfg.fg.enabled ? unsigned(cfg.fg.backend) : 0u)
                                  << ", factor=" << cfg.fg.factor << '\n'
                                  << std::flush;
                    }
                    if (!through_output)
                        require(output.synchronize(error),
                                "Direct provider synchronization failed: " + error);
                    if (!through_output)
                        require(output.device() != nullptr, "Provider or Off handoff lost its presentation device");
                    const unsigned down_frame = live ? 55u : 48u, restore_frame = live ? 95u : 70u;
                    const bool resized = frame == down_frame || frame == restore_frame;
                    if (resized) {
                        output_width = frame == down_frame ? std::max(640u, w * 3u / 4u) : w;
                        output_height = frame == down_frame ? std::max(360u, h * 3u / 4u) : h;
                        context->ClearState();
                        context->Flush();
                        if (through_output) {
                            host_target.Reset();
                            host_buffer.Reset();
                            require(SUCCEEDED(host_chain->ResizeBuffers(0, output_width, output_height,
                                                                        DXGI_FORMAT_UNKNOWN, 0)),
                                    "ReShade output resize failed");
                            require(SUCCEEDED(host_chain->GetBuffer(0, IID_PPV_ARGS(&host_buffer))),
                                    "Resized SDR carrier buffer unavailable");
                            require(SUCCEEDED(
                                        d->CreateRenderTargetView(host_buffer.Get(), nullptr, &host_target)),
                                    "Resized SDR carrier RTV failed");
                        } else {
                            require(output.drain(3000, error), error);
                            require(SUCCEEDED(output.chain()->ResizeBuffers(
                                        0, output_width, output_height, DXGI_FORMAT_UNKNOWN,
                                        DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)),
                                    "Provider output resize failed");
                            require(output.resize(output_width, output_height, error), error);
                        }
                        allocate_inputs();
                        std::cout << "PASS: output resized at frame " << frame << '\n' << std::flush;
                    }
                    xivrender::fg::simulation_begin();
                    xivrender::fg::camera(camera);
                    xivrender::fg::simulation_end();
                    xivrender::fg::render_begin();
                    xivrender::nr::FrameInput input;
                    input.frame_id = frame;
                    input.sr_feature = &output;
                    input.reset = frame == 1 || resized;
                    input.hdr = cfg.hdr.enabled;
                    auto &i = input.images;
                    i.color = color.Get();
                    i.depth = z.Get();
                    i.motion = mv.Get();
                    i.output_width = output_width;
                    i.output_height = output_height;
                    i.guide_width = i.motion_width = guide_width;
                    i.guide_height = i.motion_height = guide_height;
                    if (native_drs) {
                        constexpr unsigned percentages[]{100, 58, 50, 77, 67};
                        const auto scale = percentages[frame % std::size(percentages)];
                        i.guide_width = i.motion_width = std::max(1u, guide_width * scale / 100);
                        i.guide_height = i.motion_height = std::max(1u, guide_height * scale / 100);
                    }
                    i.depth_x = depth_x;
                    i.depth_y = depth_y;
                    i.motion_x = motion_x;
                    i.motion_y = motion_y;
                    i.motion_scale_x = i.motion_scale_y = 1;
                    i.linear_hdr = cfg.hdr.enabled;
                    const bool intentional_gap = settings_stress && (frame == 230 || frame == 350);
                    // A missing guide still consumes this native evaluation's
                    // camera cohort. Omitting capture altogether would instead
                    // simulate a permanently unmatched producer/evaluator FIFO.
                    if (intentional_gap) i.motion = nullptr;
                    std::vector<unsigned> audit_pixels;
                    if (enabler && !through_output && !cfg.hdr.enabled) {
                        std::vector<float> source(output_width * output_height * 4);
                        audit_pixels.resize(output_width * output_height);
                        for (unsigned y = 0; y < output_height; ++y) for (unsigned x = 0; x < output_width; ++x) {
                            const bool bar = x >= output_width / 4 + frame * 2 && x < output_width / 4 + frame * 2 + 90;
                            const float r = bar ? .9f : .1f, green = bar ? .2f : .15f, b = .2f;
                            const auto n = y * output_width + x;
                            source[n * 4] = r; source[n * 4 + 1] = green; source[n * 4 + 2] = b; source[n * 4 + 3] = 1;
                            audit_pixels[n] = unsigned(r * 255 + .5f) | (unsigned(green * 255 + .5f) << 8) | (51u << 16) | 0xff000000u;
                        }
                        context->UpdateSubresource(color.Get(), 0, nullptr, source.data(), output_width * 16, 0);
                    }
                    xivrender::fg::capture(input);
                    const auto submitted_input = xivrender::fg::status();
                    const float gray[]{.3f, .4f, .5f, 1};
                    context->ClearRenderTargetView(through_output ? host_target.Get() : output.target(),
                                                   gray);
                    if (!audit_pixels.empty()) {
                        ComPtr<ID3D11Resource> target;
                        output.target()->GetResource(&target);
                        context->UpdateSubresource(target.Get(), 0, nullptr, audit_pixels.data(), output_width * 4, 0);
                    }
                    if (through_output)
                        xivrender::fg::before_effects(host_buffer.Get());
                    const auto result = through_output ? host_chain->Present(0, 0)
                                                       : output.present(0, DXGI_PRESENT_ALLOW_TEARING,
                                                                        live || frame < 60 || frame > 65);
                    require(SUCCEEDED(result), "FG Present failed " + std::to_string(unsigned(result)) +
                                                   ": " + xivrender::fg::status().message);
                    if (frame == 32 || (live && frame == 70)) {
                        auto source_lock = xivrender::fg::lock_native_source();
                        require(xivrender::fg::retire_native_source(&output, error), "Native source retirement failed: " + error);
                        steady_drs_frames = 0;
                        std::cout << "PASS: retired native source before its simulated release\n";
                    }
                    const auto observed = xivrender::fg::status();
                    if (live && frame == 71)
                        require(observed.provider == xivrender::FrameGeneration::XeFG &&
                                    observed.provider_changes == previous_status.provider_changes + 1,
                                "XeFG did not recreate its context after native source retirement");
                    if (option_only_change)
                        require(observed.provider_changes == previous_status.provider_changes,
                                "A multiplier/dynamic/target edit restarted the provider SDK");
                    if (native_drs) {
                        if (resized || intentional_gap || observed.provider_changes != previous_status.provider_changes)
                            steady_drs_frames = 0;
                        else
                            ++steady_drs_frames;
                        if (steady_drs_frames > 4 && observed.provider != xivrender::FrameGeneration::Off) {
                            require(observed.guide_allocations == previous_status.guide_allocations,
                                    "Native DRS reallocated FG guides within unchanged capacity");
                            require(observed.history_resets == previous_status.history_resets,
                                    "Native DRS extent change unnecessarily reset FG history");
                        }
                    }
                    if (!live && !through_output && frame >= 60 && frame <= 67) {
                        // This direct probe supplies false focus for frames
                        // 60-65, then returns to true. The SDK may independently
                        // choose 1X for a hidden window, but the addon must keep
                        // submitting coherent input without resets/disable.
                        require(submitted_input.input_status == "Native FG inputs ready",
                                "Window focus change rejected native FG capture");
                        require(observed.skipped == previous_status.skipped,
                                "Window focus change disabled the provider request");
                        require(observed.history_resets == previous_status.history_resets,
                                "Window focus change reset coherent native FG history");
                        if (frame == 67)
                            std::cout
                                << "PASS: focus loss/regain retained native FG input and provider requests; "
                                   "actual background generation remains SDK/OS-dependent\n";
                    }
                    if (live && frame == 149) {
                        retained_changes = observed.provider_changes;
                        retained_generation = output.generation();
                    }
                    if (live && frame >= 150 && frame < 405) {
                        const unsigned output_handoffs = frame < 165 ? 1u : 2u;
                        require(observed.provider_changes == retained_changes + output_handoffs,
                                "FG checkbox off/on did not hand off to ordinary output and back");
                        if (!through_output)
                            require(output.generation() == retained_generation + output_handoffs,
                                    "FG checkbox off/on did not create the expected fresh output generation");
                        if (frame < 165)
                            require(
                                observed.provider == xivrender::FrameGeneration::Off && !observed.active &&
                                    latency_matches(observed, cfg.fg) && observed.factor == 1 &&
                                    observed.displayed == previous_status.displayed + 1,
                                "FG Off did not retain ordinary output and the requested latency controller");
                    }
                    require(observed.real_frames >= previous_status.real_frames &&
                                observed.displayed >= previous_status.displayed &&
                                observed.provider_changes >= previous_status.provider_changes,
                            "Provider change reset cumulative presentation counters");
                    previous_status = observed;
                    if (frame > restore_frame && observed.active && observed.factor == observed.maximum)
                        ++recovered_max_frames;
                    if (live) {
                        using xivrender::FrameGeneration;
                        const unsigned phase = frame < 40    ? 0u
                                               : frame < 80  ? 1u
                                               : frame < 120 ? 2u
                                               : frame < 150 ? 3u
                                               : frame < 165 ? 4u
                                                             : 5u;
                        const FrameGeneration providers[]{
                            initial_provider,
                            live_scrgb ? FrameGeneration::FsrFG : FrameGeneration::XeFG,
                            FrameGeneration::FsrFG,
                            live_scrgb ? FrameGeneration::FsrFG : FrameGeneration::DlssG,
                            FrameGeneration::Off,
                            live_scrgb ? FrameGeneration::FsrFG : FrameGeneration::DlssG};
                        const unsigned settled[]{25u, 65u, 105u, 140u, 155u, 185u};
                        if (frame >= settled[phase]) {
                            const auto expected = settings_stress && frame >= 205
                                ? (cfg.fg.enabled ? cfg.fg.backend : FrameGeneration::Off) : providers[phase];
                            require(observed.provider == expected,
                                    "Live provider did not match the settled request at frame " +
                                        std::to_string(frame) + ": " + observed.transition);
                            require(latency_matches(observed, cfg.fg),
                                    "Live latency controller did not match its provider/checkbox");
                            if (through_output) {
                                UINT bytes = sizeof(UINT) * 2, info[2]{};
                                require(SUCCEEDED(host_chain->GetPrivateData(xivrender::hdr::output_info,
                                                                             &bytes, info)),
                                        "Live output metadata was lost");
                                const auto space = !cfg.hdr.enabled ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                                                   : format == DXGI_FORMAT_R10G10B10A2_UNORM
                                                       ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                                       : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
                                require(info[0] == unsigned(format) && info[1] == unsigned(space),
                                        "Live provider changed the actual output format/colorspace");
                                ComPtr<ID3D11Texture2D> retained;
                                require(SUCCEEDED(host_chain->GetBuffer(0, IID_PPV_ARGS(&retained))) &&
                                            retained.Get() == host_buffer.Get(),
                                        "Live provider replaced ReShade's SDR carrier buffer");
                                ComPtr<ID3D11Device> owner;
                                require(SUCCEEDED(host_chain->GetDevice(IID_PPV_ARGS(&owner))) &&
                                            xivrender::d3d11_identity::native_device(owner.Get()) ==
                                                xivrender::d3d11_identity::native_device(d.Get()),
                                        "Live provider changed ReShade's native D3D11 device");
                            }
                            const bool generated = observed.active && observed.factor == cfg.fg.factor;
                            if (expected == FrameGeneration::Off || generated || hidden)
                                ++verified_live_frames[phase];
                            if (generated)
                                ++generated_live_frames[phase];
                        }
                    }
                    if (frame % 15 == 0) {
                        auto s = xivrender::fg::status();
                        std::cout << "Frame " << frame << " active=" << s.active << " factor=" << s.factor
                                  << " provider=" << unsigned(s.provider) << " changes=" << s.provider_changes
                                  << " displayed=" << s.displayed << " failures=" << s.failures << " "
                                  << s.message << '\n'
                                  << std::flush;
                        if (!s.last_error.empty())
                            std::cout << "Last provider error: " << s.last_error << '\n' << std::flush;
                    }
                    // Leave time for the maximum multiplier within a normal
                    // display interval, rather than intentionally saturating it.
                    Sleep(32);
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                        DispatchMessageW(&message);
                }
                auto s = xivrender::fg::status();
                require(!s.failures, "Provider reported FG failures: " + s.message);
                if (!hidden) {
                    require(s.displayed > frame_count, "No generated frames were verified");
                    require(recovered_max_frames >= 3,
                            "Provider did not sustain its maximum factor after resize");
                }
                std::cout << "Verified maximum-factor frames after resize: " << recovered_max_frames << '\n';
                if (live) {
                    for (unsigned phase = 0; phase < verified_live_frames.size(); ++phase) {
                        require(verified_live_frames[phase] >= 3,
                                "Insufficient generated/ordinary proof for live phase " +
                                    std::to_string(phase));
                        std::cout << "Verified live phase " << phase << ": " << verified_live_frames[phase]
                                  << " settled frames, " << generated_live_frames[phase]
                                  << " verified generated frames\n";
                    }
                    const unsigned minimum_changes =
                        live_scrgb ? 2u : (initial_provider == xivrender::FrameGeneration::XeFG ? 4u : 5u);
                    require(s.provider_changes >= minimum_changes,
                            "Expected live provider transitions did not occur");
                    std::cout
                        << "PASS: live provider changes, latency ownership, ordinary Off output, "
                           "fresh re-enabled output, isolated presentation devices and monotonic counters\n";
                    if (hidden)
                        std::cout
                            << "Hidden/background lifecycle test: generated-frame counts above are "
                               "observations; an unfocused DLSS-G provider is not required to interpolate\n";
                }
                if (!through_output)
                    require(output.drain(3000, error), error);
                host_target.Reset();
                host_buffer.Reset();
                host_chain.Reset();
                std::cout
                    << (hidden
                            ? "PASS: hidden provider lifecycle, native guides and retirement\n"
                            : "PASS: FG provider presentation, native guides, HDR format and retirement\n");
            }
        }
        context->ClearState();
        context->Flush();
        // Match the addon's explicit teardown before ReShade/process detach.
        // The through-output host owns HDR state and hooks even in SDR mode.
        xivrender::hdr::shutdown();
        if (through_output) xivrender::hdr::remove_output();
        DestroyWindow(window);
        if (prior)
            SetForegroundWindow(prior);
        xivrender::fg::diagnostics::shutdown();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "; status=" << xivrender::fg::status().message << '\n';
        xivrender::fg::diagnostics::event("probe.failed", 0, 0, 0, 0, 0, e.what());
        xivrender::fg::diagnostics::shutdown();
        return 1;
    }
}
