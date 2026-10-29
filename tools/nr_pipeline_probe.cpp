// Controlled, isolated hardware probe of the complete production NR pipeline.
// No game process, screenshot files or global runtime search changes. A copy
// beside a genuine ReShade dxgi.dll also exercises ReShade's actual wrappers.
#include "xivrender/nr_pipeline.hpp"
#include "xivrender/native_nr.hpp"
#include "xivrender/app.hpp"
#include "xivrender/d3d12_identity.hpp"
#include <d3d11_4.h>
#include <windows.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <future>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

namespace xivrender {
namespace host_probe {
std::mutex settings_mutex;
Settings settings;
void configure(const NeuralRenderingSettings &nr) {
    std::lock_guard lock(settings_mutex);
    settings.nr = nr;
}
} // namespace host_probe
Settings settings_snapshot() {
    std::lock_guard lock(host_probe::settings_mutex);
    return host_probe::settings;
}
void log_message(const std::string &text) {
    std::cout << text << '\n' << std::flush;
}
} // namespace xivrender

namespace {
using Microsoft::WRL::ComPtr;
using xivrender::nr::FrameInput;
using xivrender::nr::Pipeline;
unsigned width = 512, height = 512;
bool hdr_input_frame = false;
constexpr std::uint16_t alpha_half = 0x3800; // Exactly 0.5.

void check(HRESULT result, const char *operation) {
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}
void require(bool condition, const std::string &reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
};
struct Fixture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device> native_device;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Texture2D> clean, colour, depth, motion, staging;
    ComPtr<ID3D11Fence> fence;
    Handle event;
    std::uint64_t completed = 0, frame_id = 0;
    std::vector<std::array<std::uint16_t, 4>> pixels;

    ComPtr<ID3D11Texture2D> texture(DXGI_FORMAT format, const void *data, unsigned pitch,
                                    D3D11_USAGE usage = D3D11_USAGE_DEFAULT) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.Usage = usage;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial{data, pitch, 0};
        ComPtr<ID3D11Texture2D> result;
        check(device->CreateTexture2D(&desc, data ? &initial : nullptr, &result), "Create probe texture");
        return result;
    }
    Fixture() {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                &device, nullptr, &context),
              "Create hardware D3D11 device");
        native_device = device;
        ComPtr<IUnknown> unwrapped;
        if (SUCCEEDED(device->QueryInterface(xivrender::d3d12_identity::unwrapped_object,
                                             reinterpret_cast<void **>(unwrapped.GetAddressOf()))))
            check(unwrapped.As(&native_device), "Query authoritative native D3D11 device");
        check(device.As(&device5), "Query native D3D11Device5");
        check(context.As(&context4), "Query native D3D11Context4");
        check(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
              "Create probe readback fence");
        event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event.value != nullptr, "Create probe readback event failed");
        pixels.resize(width * height);
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                // Smooth ramps with small repeated detail avoid a uniform
                // identity input while keeping a stable zero-motion scene.
                auto &p = pixels[y * width + x];
                p = {std::uint16_t(0x3000 + (x % 32) * 16), std::uint16_t(0x3400 + (y % 16) * 8),
                     std::uint16_t(0x3800 + ((x / 32 + y / 32) % 4) * 16), alpha_half};
            }
        clean = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, pixels.data(), width * 8, D3D11_USAGE_IMMUTABLE);
        colour = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, pixels.data(), width * 8);
        const std::vector<float> depth_data(width * height, .5f);
        depth = texture(DXGI_FORMAT_R32_FLOAT, depth_data.data(), width * 4, D3D11_USAGE_IMMUTABLE);
        const std::vector<std::array<std::uint16_t, 2>> motion_data(width * height, {0, 0});
        motion = texture(DXGI_FORMAT_R16G16_FLOAT, motion_data.data(), width * 4, D3D11_USAGE_IMMUTABLE);
        D3D11_TEXTURE2D_DESC desc{};
        colour->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc, nullptr, &staging), "Create probe staging texture");
    }
    FrameInput frame() {
        // Real native SR supplies a fresh clean frame on every call. Avoid
        // accidentally feeding a previous NR composition back into the model.
        context->CopyResource(colour.Get(), clean.Get());
        FrameInput input;
        input.sr_feature = this;
        input.frame_id = ++frame_id;
        input.images.color = colour.Get();
        input.images.depth = depth.Get();
        input.images.motion = motion.Get();
        input.images.output_width = input.images.guide_width = input.images.motion_width = width;
        input.images.output_height = input.images.guide_height = input.images.motion_height = height;
        input.images.motion_scale_x = float(width);
        input.images.motion_scale_y = float(height);
        input.hdr = hdr_input_frame;
        input.images.linear_hdr = hdr_input_frame;
        input.images.depth_inverted = true;
        return input;
    }
    void readback(const std::string &label) {
        context->CopyResource(staging.Get(), colour.Get());
        const auto target = ++completed;
        check(context4->Signal(fence.Get(), target), "Signal probe readback fence");
        context->Flush();
        check(fence->SetEventOnCompletion(target, event.value), "Arm probe readback fence");
        require(WaitForSingleObject(event.value, 15000) == WAIT_OBJECT_0 &&
                    fence->GetCompletedValue() >= target,
                "Probe readback timed out; GPU owners will be retained by the production pipeline");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map probe readback");
        std::uint64_t invalid = 0, alpha_errors = 0, nonzero = 0, changed = 0;
        for (unsigned y = 0; y < height; ++y) {
            const auto *row = static_cast<const unsigned char *>(mapped.pData) + y * mapped.RowPitch;
            for (unsigned x = 0; x < width; ++x) {
                std::array<std::uint16_t, 4> actual{};
                std::memcpy(actual.data(), row + x * 8, 8);
                alpha_errors += actual[3] != alpha_half;
                for (unsigned channel = 0; channel < 3; ++channel) {
                    const auto half = actual[channel];
                    invalid += (half & 0x7c00) == 0x7c00;
                    nonzero += (half & 0x7fff) != 0;
                    changed += half != pixels[y * width + x][channel];
                }
            }
        }
        context->Unmap(staging.Get(), 0);
        std::cout << label << ": RGB nonzero=" << nonzero << ", changed=" << changed
                  << ", NaN/Inf=" << invalid << ", alpha mismatches=" << alpha_errors << '\n'
                  << std::flush;
        require(!invalid && !alpha_errors && nonzero, "Pipeline RGB/alpha readback validation failed");
        require(changed != 0,
                "NR reported applied frames but this synthetic image has no changed RGB samples");
    }
};

void apply(Pipeline &pipeline, Fixture &fixture, const xivrender::NeuralRenderingSettings &settings,
           unsigned frames, const std::string &label) {
    const auto previous_applied = pipeline.Status().applied;
    const auto previous_failures = pipeline.Status().failures;
    const auto target_width =
        unsigned(std::max(64.0, std::round(double(width) * settings.model_scale *
                                           (settings.foveated ? settings.work_fraction : 1))));
    const auto target_height =
        unsigned(std::max(64.0, std::round(double(height) * settings.model_scale *
                                           (settings.foveated ? settings.work_fraction : 1))));
    const auto deadline = GetTickCount64() + 30000;
    std::string last_message;
    while (GetTickCount64() < deadline) {
        const auto input = fixture.frame();
        pipeline.Process(input, settings, true);
        const auto status = pipeline.Status();
        if (status.message != last_message) {
            last_message = status.message;
            std::cout << label << ": " << status.message << '\n' << std::flush;
        }
        require(!status.failed && status.failures == previous_failures,
                "Production NR pipeline failed: " + status.message);
        if (status.applied >= previous_applied + frames && status.active_runtime == settings.runtime_id &&
            status.width == target_width && status.height == target_height) {
            fixture.readback(label);
            std::cout << label << ": active runtime=" << status.active_runtime << ", model=" << status.width
                      << 'x' << status.height << ", creates=" << status.creations
                      << ", evals=" << status.evaluations << ", applied=" << status.applied
                      << ", skipped=" << status.skipped << '\n'
                      << std::flush;
            return;
        }
        Sleep(1);
    }
    throw std::runtime_error("Timed out waiting for " + label + ": " + pipeline.Status().message);
}

class RenderOwner {
    std::mutex mutex;
    std::condition_variable changed;
    std::queue<std::packaged_task<void()>> jobs;
    bool stopping = false;
    std::thread worker{[this] {
        for (;;) {
            std::packaged_task<void()> job;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stopping || !jobs.empty(); });
                if (jobs.empty())
                    return;
                job = std::move(jobs.front());
                jobs.pop();
            }
            job();
        }
    }};

  public:
    ~RenderOwner() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        worker.join();
    }
    void run(std::function<void()> operation) {
        std::packaged_task<void()> job(std::move(operation));
        auto result = job.get_future();
        {
            std::lock_guard lock(mutex);
            jobs.push(std::move(job));
        }
        changed.notify_one();
        result.get();
    }
};

void host_controls(Fixture &fixture, const std::filesystem::path &root,
                   xivrender::NeuralRenderingSettings settings) {
    using namespace xivrender;
    RenderOwner render;
    native_nr::set_root(root);
    native_nr::attach_device(fixture.native_device.Get());
    settings.pause_on_focus_loss = false; // Headless probe has no focused game window.
    const auto apply_host = [&](const std::string &label) {
        host_probe::configure(settings);
        const auto previous = native_nr::snapshot().applied;
        const auto deadline = GetTickCount64() + 30000;
        while (GetTickCount64() < deadline) {
            render.run([&] { native_nr::after_upscale(fixture.frame()); });
            const auto state = native_nr::snapshot();
            require(!state.failed, label + ": " + state.message);
            if (state.active && state.applied >= previous + 3) {
                fixture.readback(label);
                return;
            }
            Sleep(1);
        }
        throw std::runtime_error("Timed out waiting for host controls: " + label);
    };
    apply_host("Production host on render thread");
    const auto before_disable = native_nr::snapshot();
    settings.enabled = false;
    host_probe::configure(settings);
    native_nr::enabled_changed(false); // This thread is the UI/Present caller.
    native_nr::service();
    require(native_nr::snapshot().active_runtime == before_disable.active_runtime,
            "UI/Present released the model instead of queuing cleanup");
    const auto deadline = GetTickCount64() + 15000;
    while (native_nr::needs_service() && GetTickCount64() < deadline) {
        render.run([&] { native_nr::after_upscale(fixture.frame()); });
        Sleep(1);
    }
    require(!native_nr::needs_service() && native_nr::snapshot().active_runtime.empty(),
            "Render-thread disable failed to retire the model");
    require(native_nr::snapshot().evaluations == before_disable.evaluations,
            "Disabled host submitted another NR evaluation");
    settings.enabled = true;
    host_probe::configure(settings);
    native_nr::enabled_changed(true);
    apply_host("UI re-enable on render owner");
    auto generation = native_nr::snapshot().creations;
    native_nr::enabled_changed(false);
    native_nr::enabled_changed(true);
    apply_host("Rapid UI off/on before next render frame");
    require(native_nr::snapshot().creations == generation + 1,
            "Rapid off/on lost its generation-retirement request");
    generation = native_nr::snapshot().creations;
    native_nr::retry();
    apply_host("UI retry on render owner");
    require(native_nr::snapshot().creations == generation + 1, "UI retry did not recreate NR");
    settings.runtime_id = "310.8-compat";
    apply_host("Host live version change to 310.8");
    settings.runtime_id = "310.9.16-compat";
    apply_host("Host live version change back to 310.9.16");
    render.run([&] { native_nr::shutdown(15000); });
    require(!native_nr::needs_service(), "Host shutdown retained completed GPU work");
    std::cout << "PASS: production NR host, UI/Present deferred disable, rapid off/on, retry, live versions "
                 "and render-thread retirement\n";
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 2 && argc != 4 && argc != 5) {
        std::cerr << "Usage: nr_pipeline_probe <absolute private addon-root containing XIVRender/runtime> "
                     "[width height [--host-control-test|--ui-thread-disable]]\n";
        return 2;
    }
    try {
        if (argc >= 4) {
            width = static_cast<unsigned>(std::stoul(argv[2]));
            height = static_cast<unsigned>(std::stoul(argv[3]));
            require(width >= 128 && height >= 128 && width <= 4096 && height <= 4096,
                    "Probe dimensions must be within 128..4096");
        }
        const std::filesystem::path root(argv[1]);
        require(root.is_absolute(), "An absolute private probe root is required");
        const auto runtime = root / L"XIVRender/runtime";
        for (const auto &path :
             {runtime / L"XIVRender_nvngx.dll_nr.dll", runtime / L"nr/310.9.16-compat/nvngx_dlssnr.dll",
              runtime / L"nr/310.8-compat/nvngx_dlssnr.dll"})
            require(std::filesystem::is_regular_file(path),
                    "Required private probe file missing: " + path.string());
        Fixture fixture;
        Pipeline pipeline;
        pipeline.Attach(fixture.native_device.Get(), root);
        xivrender::NeuralRenderingSettings settings;
        settings.enabled = true;
        settings.pause_on_focus_loss = true;
        settings.model_scale = 1;
        if (argc == 5) {
            if (std::wstring_view(argv[4]) == L"--host-control-test") {
                host_controls(fixture, root, settings);
                return 0;
            }
            require(std::wstring_view(argv[4]) == L"--ui-thread-disable", "Unknown probe mode");
            std::promise<void> ready, finish;
            auto ready_result = ready.get_future();
            auto finished = finish.get_future();
            std::thread render([&] {
                try {
                    apply(pipeline, fixture, settings, 5, "Render-thread NR initialization");
                    ready.set_value();
                } catch (...) {
                    ready.set_exception(std::current_exception());
                }
                finished.wait();
            });
            try {
                ready_result.get();
                std::cout << "Calling NR disable/retirement on the UI thread\n" << std::flush;
                const auto deadline = GetTickCount64() + 15000;
                do {
                    pipeline.Service(false);
                    if (!pipeline.NeedsService())
                        break;
                    Sleep(1);
                } while (GetTickCount64() < deadline);
                require(!pipeline.NeedsService(), "Cross-thread NR disable did not retire");
            } catch (...) {
                finish.set_value();
                render.join();
                throw;
            }
            finish.set_value();
            render.join();
            std::cout << "PASS: cross-thread NR model retirement\n";
            return 0;
        }
        apply(pipeline, fixture, settings, 3, "310.9.16 initial");
        const auto before_pause = pipeline.Status().evaluations;
        for (unsigned i = 0; i < 8; ++i) {
            pipeline.Process(fixture.frame(), settings, false);
            const auto paused = pipeline.Status();
            require(paused.paused && !paused.active && paused.evaluations == before_pause,
                    "Focus pause submitted NR work or lost its paused status");
        }
        apply(pipeline, fixture, settings, 1, "310.9.16 focus resume");
        settings.runtime_id = "310.8-compat";
        apply(pipeline, fixture, settings, 3, "Live change to 310.8");
        settings.runtime_id = "310.9.16-compat";
        apply(pipeline, fixture, settings, 3, "Live change back to 310.9.16");
        settings.model_scale = .5f;
        apply(pipeline, fixture, settings, 3, "Live change to 50% model resolution");
        settings.passes = 2;
        settings.profile.style = 1;
        settings.later_passes[0].inherit = false;
        settings.later_passes[0].profile.style = 2;
        apply(pipeline, fixture, settings, 3, "Two independent model passes / Natural + Cinematic");
        settings.feedback = .5f;
        apply(pipeline, fixture, settings, 2, "Live multipass feedback");
        settings.passes = 3;
        settings.foveated = true;
        settings.central_fraction = .6f;
        settings.work_fraction = .8f;
        apply(pipeline, fixture, settings, 3, "Three passes with matched peripheral compression");
        settings.runtime_id = "310.8-compat";
        apply(pipeline, fixture, settings, 3, "310.8 multipass / peripheral compression");
        settings.runtime_id = "310.9.16-compat";
        settings.passes = 1;
        settings.foveated = false;
        settings.profile = {};
        apply(pipeline, fixture, settings, 2, "Return to uniform single pass");
        const auto before_retry = pipeline.Status().creations;
        pipeline.Fault("Controlled probe failure while the model is ready");
        require(pipeline.Status().failed, "Controlled pipeline fault was not latched");
        pipeline.Retry();
        apply(pipeline, fixture, settings, 1, "Same-setting retry after controlled failure");
        require(pipeline.Status().creations == before_retry + 1,
                "Retry reused the failed model generation instead of retiring/reopening it");
        // A bright linear-HDR source through both supported model versions and
        // both model-input policies. Recreate history on every domain change.
        hdr_input_frame = true;
        for (auto &pixel : fixture.pixels) {
            pixel[0] = 0x4400; // 4.0, well beyond SDR white.
            pixel[1] = 0x4000; // 2.0.
        }
        fixture.clean = fixture.texture(DXGI_FORMAT_R16G16B16A16_FLOAT, fixture.pixels.data(), width * 8,
                                        D3D11_USAGE_IMMUTABLE);
        settings.model_scale = .5f;
        settings.passes = 2;
        settings.foveated = true;
        for (const auto *hdr_runtime : {"310.9.16-compat", "310.8-compat"}) {
            settings.runtime_id = hdr_runtime;
            for (const bool scene_hdr : {false, true}) {
                settings.hdr_input = scene_hdr;
                apply(pipeline, fixture, settings, 3,
                      std::string(hdr_runtime) +
                          (scene_hdr ? " scene HDR multipass" : " SDR NR input on HDR scene"));
            }
        }
        settings.enabled = false;
        const auto before_disable = pipeline.Status().evaluations;
        const auto deadline = GetTickCount64() + 15000;
        do {
            pipeline.Process(fixture.frame(), settings, true);
            const auto status = pipeline.Status();
            require(!status.active && status.evaluations == before_disable, "Disabled NR submitted work");
            if (status.active_runtime.empty() && status.message == "NR is disabled")
                break;
            Sleep(1);
        } while (GetTickCount64() < deadline);
        std::string error;
        require(pipeline.Shutdown(15000, error), error);
        require(pipeline.Status().active_runtime.empty(), "NR model remained active after shutdown");
        fixture.context->ClearState();
        std::cout << "PASS: complete production NR pipeline, live runtime/resolution changes, focus pause, "
                     "GPU composition, exact alpha, failed-generation retry and disabled retirement\n"
                     "Synthetic hardware data-path test only; game image quality remains unverified.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
