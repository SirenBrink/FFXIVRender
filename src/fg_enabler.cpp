#include "xivrender/fg_backend.hpp"
#include "xivrender/fg_diagnostics.hpp"
#include "xivrender/ngx_parameters.hpp"
#include <nvsdk_ngx.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <fstream>
#include <vector>
#include <d3d12sdklayers.h>

namespace xivrender::fg {
namespace {
using Microsoft::WRL::ComPtr;
void transition(ID3D12GraphicsCommandList *cmd, ID3D12Resource *image,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    cmd->ResourceBarrier(1, &b);
}
bool checked(HRESULT result, const char *operation, std::string &error) {
    if (SUCCEEDED(result)) return true;
    error = std::string(operation) + ": " + std::to_string(unsigned(result));
    return false;
}
bool ngx_checked(NVSDK_NGX_Result result, const char *operation, std::string &error) {
    if (NVSDK_NGX_SUCCEED(result)) return true;
    error = std::string(operation) + ": " + std::to_string(unsigned(result));
    return false;
}
// Explicit headless API: never install this library as a game DLL proxy or
// replace the native DLSS model. Each output belongs to this private device.
class Enabler final : public Backend {
    HMODULE library = nullptr;
    using Init = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t *, ID3D12Device *,
                                     NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *);
    using Populate = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter *);
    using Create = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
                                        NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
    using Evaluate = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *,
                                          NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);
    using Release = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle *);
    using Shutdown = NVSDK_NGX_Result (*)(ID3D12Device *);
    Init init = nullptr;
    Populate populate = nullptr;
    Create create_feature = nullptr;
    Evaluate evaluate = nullptr;
    Release release = nullptr;
    Shutdown shutdown = nullptr;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr, timer = nullptr;
    std::uint64_t serial = 0, real_frames = 0;
    std::atomic<std::uint64_t> total{0};
    NVSDK_NGX_Handle *feature = nullptr;
    NgxParameters parameters;
    std::filesystem::path cache;
    std::array<ComPtr<ID3D12Resource>, 3> generated;
    ComPtr<ID3D12Resource> real;
    unsigned width = 0, height = 0, ready = 0;
    std::atomic<unsigned> last{1};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool initialized = false, reset_next = true;
    double frame_ms = 16.67;
    struct Group {
        std::array<ComPtr<ID3D12Resource>, 3> generated;
        ComPtr<ID3D12Resource> real;
        unsigned count = 0, interval = 0, flags = 0;
        std::uint64_t frame = 0;
        double milliseconds = 16.67;
        bool busy = false;
    };
    std::array<Group, 3> groups;
    unsigned current = 0;
    ComPtr<ID3D12Resource> carrier;
    ComPtr<IDXGISwapChain4> output;
    ComPtr<ID3D12Fence> display_fence;
    HANDLE display_event = nullptr;
    std::uint64_t display_serial = 0;
    std::mutex mutex;
    std::condition_variable available;
    std::deque<unsigned> pending;
    std::thread worker;
    bool quitting = false;
    std::atomic<HRESULT> worker_result{S_OK};
    std::uint64_t current_frame = 0;
    std::function<void(unsigned, std::uint64_t)> present_markers;
    bool audit(std::string &error) {
        if (!probe_debug_enabled()) return true;
        ComPtr<ID3D12InfoQueue> debug;
        if (SUCCEEDED(device.As(&debug))) {
            bool failed = false;
            const auto count = debug->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T size = 0; debug->GetMessage(i, nullptr, &size);
                std::vector<unsigned char> bytes(size);
                auto *message = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
                if (SUCCEEDED(debug->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    diagnostics::event("enabler.debug_error", 0, message->ID, 0, 0, 0, message->pDescription);
                    failed = true;
                }
            }
            debug->ClearStoredMessages();
            if (failed) { error = "Enabler D3D12 debug validation failed"; return false; }
        }
        if (!ready || (real_frames != 25 && real_frames != 45 && real_frames != 85)) return true;
        ComPtr<ID3D12CommandAllocator> a;
        ComPtr<ID3D12GraphicsCommandList> c;
        if (!checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "Audit allocator", error) ||
            !checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&c)), "Audit commands", error)) return false;
        const auto desc = real->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT rows = 0; UINT64 row_bytes = 0, bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows, &row_bytes, &bytes);
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes * (ready + 1);
        buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        ComPtr<ID3D12Resource> data;
        if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&data)), "Audit readback", error)) return false;
        for (unsigned i = 0; i <= ready; ++i) {
            auto *r = i < ready ? generated[i].Get() : real.Get();
            transition(c.Get(), r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = data.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint = layout; to.PlacedFootprint.Offset = bytes * i;
            D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = r; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            c->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            transition(c.Get(), r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        if (!checked(c->Close(), "Audit close", error)) return false;
        ID3D12CommandList *list = c.Get(); queue->ExecuteCommandLists(1, &list);
        if (!drain(error)) return false;
        void *mapped = nullptr;
        if (!checked(data->Map(0, nullptr, &mapped), "Audit map", error)) return false;
        const auto directory = cache.parent_path().parent_path().parent_path() / L"build/enabler-pixels";
        std::filesystem::create_directories(directory);
        for (unsigned i = 0; i <= ready; ++i) {
            const auto name = std::to_string(real_frames) + "-" + std::to_string(i) + "-" +
                              std::to_string(width) + "x" + std::to_string(height) + "-" + std::to_string(unsigned(format)) + ".raw";
            std::ofstream file(directory / name, std::ios::binary);
            for (UINT y = 0; y < rows; ++y)
                file.write(static_cast<const char *>(mapped) + bytes * i + layout.Footprint.RowPitch * y,
                           std::streamsize(row_bytes));
        }
        D3D12_RANGE untouched{}; data->Unmap(0, &untouched);
        return true;
    }
    bool display_drain() {
        const auto value = ++display_serial;
        if (FAILED(queue->Signal(display_fence.Get(), value))) return false;
        auto done = display_fence->GetCompletedValue();
        if (done == UINT64_MAX) return false;
        if (done < value) {
            if (FAILED(display_fence->SetEventOnCompletion(value, display_event)) ||
                WaitForSingleObject(display_event, 3000) != WAIT_OBJECT_0) return false;
            done = display_fence->GetCompletedValue();
        }
        return done != UINT64_MAX && done >= value;
    }
    void wait_until(std::chrono::steady_clock::time_point deadline) {
        const auto us = std::chrono::duration<double, std::micro>(deadline - std::chrono::steady_clock::now()).count();
        if (us > 0) {
            LARGE_INTEGER due{}; due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(us * 10));
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 1000);
        }
    }
    void display_loop() {
        auto next_start = std::chrono::steady_clock::now();
        for (;;) {
            unsigned index;
            {
                std::unique_lock lock(mutex);
                available.wait(lock, [&] { return quitting || !pending.empty(); });
                if (pending.empty()) return;
                index = pending.front(); pending.pop_front();
            }
            auto &group = groups[index];
            auto result = worker_result.load();
            if (SUCCEEDED(result) && group.count && !group.interval) wait_until(next_start);
            const auto start = std::chrono::steady_clock::now();
            const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(group.count ? group.milliseconds : 0.0));
            const bool marked = group.frame && present_markers && SUCCEEDED(result);
            if (marked) present_markers(4, group.frame);
            unsigned completed = 0;
            for (unsigned i = 0; i <= group.count && SUCCEEDED(result); ++i) {
                if (i && !group.interval) wait_until(start + period * i / (group.count + 1));
                ComPtr<ID3D12Resource> back;
                result = output->GetBuffer(output->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
                if (SUCCEEDED(result)) result = allocator->Reset();
                if (SUCCEEDED(result)) result = commands->Reset(allocator.Get(), nullptr);
                if (FAILED(result)) break;
                auto *source = i < group.count ? group.generated[i].Get() : group.real.Get();
                transition(commands.Get(), source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                transition(commands.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
                commands->CopyResource(back.Get(), source);
                transition(commands.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                transition(commands.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
                result = commands->Close();
                if (FAILED(result)) break;
                ID3D12CommandList *list = commands.Get(); queue->ExecuteCommandLists(1, &list);
                diagnostics::event("enabler.display_begin", group.frame, i, group.count + 1);
                result = output->Present(group.interval, group.flags);
                diagnostics::event("enabler.display_end", group.frame, i, group.count + 1, unsigned(result));
                if (SUCCEEDED(result)) { ++completed; ++total; }
                if (!display_drain()) result = DXGI_ERROR_DEVICE_REMOVED;
            }
            next_start = start + period;
            if (marked) present_markers(5, group.frame);
            last.store(std::max(1u, completed));
            if (FAILED(result)) worker_result.store(result);
            {
                std::lock_guard lock(mutex);
                group.busy = false;
            }
            available.notify_all();
        }
    }
    template<class T> T api(const char *name) {
        auto value = reinterpret_cast<T>(GetProcAddress(library, name));
        if (!value) throw std::runtime_error(std::string("DLSS Enabler export missing: ") + name);
        return value;
    }
    bool drain(std::string &error) {
        if (!queue || !fence) return true;
        const auto target = ++serial;
        if (!checked(queue->Signal(fence.Get(), target), "Enabler fence signal", error)) return false;
        auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) { error = "Enabler device removed"; return false; }
        if (completed < target) {
            if (!checked(fence->SetEventOnCompletion(target, event), "Enabler fence event", error) ||
                WaitForSingleObject(event, 3000) != WAIT_OBJECT_0) {
                error = "Enabler GPU retirement timed out"; return false;
            }
            completed = fence->GetCompletedValue();
        }
        if (completed == UINT64_MAX || completed < target) {
            error = "Enabler GPU retirement was not confirmed"; return false;
        }
        return true;
    }
    bool images(const Packet &p, std::string &error) {
        if (real && width == p.width && height == p.height) return true;
        if (width != p.width || height != p.height) {
            if (!drain(error)) return false;
            if (feature && !ngx_checked(release(feature), "Enabler resize release", error)) return false;
            feature = nullptr;
            reset_next = true;
        }
        auto desc = p.backbuffer->GetDesc();
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                       D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&real)), "Enabler real image", error))
            return false;
        for (auto &r : generated)
            if (!checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                           D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)), "Enabler generated image", error))
                return false;
        width = p.width; height = p.height;
        groups[current].real = real;
        groups[current].generated = generated;
        return true;
    }
  public:
    explicit Enabler(const std::filesystem::path &root) : cache(root / L"fg/enabler-cache") {
        const auto path = root / L"fg/dlss-enabler-headless.dll";
        library = LoadLibraryExW(path.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!library) throw std::runtime_error("DLSS Enabler headless runtime could not load");
        try {
            init = api<Init>("DLSSG_NVSDK_NGX_D3D12_Init_Ext");
            populate = api<Populate>("DLSSG_NVSDK_NGX_D3D12_PopulateParameters_Impl");
            create_feature = api<Create>("DLSSG_NVSDK_NGX_D3D12_CreateFeature");
            evaluate = api<Evaluate>("DLSSG_NVSDK_NGX_D3D12_EvaluateFeature");
            release = api<Release>("DLSSG_NVSDK_NGX_D3D12_ReleaseFeature");
            shutdown = api<Shutdown>("DLSSG_NVSDK_NGX_D3D12_Shutdown1");
        } catch (...) { FreeLibrary(library); library = nullptr; throw; }
    }
    ~Enabler() override {
        std::string error;
        if (!close(error)) {
            // A failed device must not leave a joinable std::thread or free
            // images whose retirement could not be established.
            { std::lock_guard lock(mutex); quitting = true; }
            available.notify_all();
            if (worker.joinable()) worker.join();
            device.Detach(); queue.Detach(); allocator.Detach(); commands.Detach();
            fence.Detach(); display_fence.Detach(); carrier.Detach(); output.Detach(); real.Detach();
            for (auto &r : generated) r.Detach();
            for (auto &g : groups) { g.real.Detach(); for (auto &r : g.generated) r.Detach(); }
            report_failure(error); return;
        }
        if (event) CloseHandle(event);
        if (display_event) CloseHandle(display_event);
        if (timer) CloseHandle(timer);
        if (library) FreeLibrary(library);
    }
    bool create(ID3D12Device *d, IDXGIFactory *f, HWND window, const DXGI_SWAP_CHAIN_DESC &desc,
                DXGI_FORMAT fmt, ComPtr<ID3D12CommandQueue> &out_queue,
                ComPtr<IDXGISwapChain4> &out, std::string &error) override {
        // The headless NGX contract describes HDR as PQ. Do not reinterpret
        // scRGB as PQ; ordinary presentation remains available on rejection.
        if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            error = "FSR MFG currently requires SDR or 10-bit PQ output"; return false;
        }
        device = d; format = fmt;
        std::filesystem::create_directories(cache);
        if (!ngx_checked(init(0, cache.c_str(), d, NVSDK_NGX_Version_API, nullptr), "Enabler init", error)) return false;
        initialized = true;
        if (!ngx_checked(populate(&parameters), "Enabler capabilities", error)) return false;
        unsigned maximum = 0;
        parameters.Get("DLSSG.MultiFrameCountMax", &maximum);
        diagnostics::event("enabler.capabilities", 0, maximum);
        if (maximum < 3) { error = "DLSS Enabler does not advertise 4X support"; return false; }
        D3D12_COMMAND_QUEUE_DESC q{};
        if (!checked(d->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "Enabler queue", error) ||
            !checked(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Enabler allocator", error) ||
            !checked(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "Enabler commands", error) ||
            !checked(commands->Close(), "Enabler commands close", error) ||
            !checked(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Enabler fence", error) ||
            !checked(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&display_fence)), "Enabler display fence", error)) return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        display_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (!event || !display_event || !timer) { error = "Enabler wait handles unavailable"; return false; }
        ComPtr<IDXGIFactory2> factory;
        if (!checked(f->QueryInterface(IID_PPV_ARGS(&factory)), "Enabler factory", error)) return false;
        DXGI_SWAP_CHAIN_DESC1 n{};
        n.Width = desc.BufferDesc.Width; n.Height = desc.BufferDesc.Height; n.Format = fmt;
        n.SampleDesc.Count = 1; n.BufferCount = 3; n.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        n.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; n.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        ComPtr<IDXGISwapChain1> chain;
        if (!checked(factory->CreateSwapChainForHwnd(queue.Get(), window, &n, nullptr, nullptr, &chain), "Enabler swapchain", error) ||
            !checked(chain.As(&out), "Enabler swapchain interface", error)) return false;
        out_queue = queue;
        output = out;
        worker = std::thread([this] { display_loop(); });
        return true;
    }
    HRESULT backbuffer(IDXGISwapChain4 *chain, ComPtr<ID3D12Resource> &image) override {
        if (FAILED(worker_result.load())) return worker_result.load();
        DXGI_SWAP_CHAIN_DESC1 desc{};
        auto result = chain->GetDesc1(&desc);
        if (FAILED(result)) return result;
        if (!carrier || carrier->GetDesc().Width != desc.Width || carrier->GetDesc().Height != desc.Height) {
            D3D12_RESOURCE_DESC r{};
            r.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; r.Width = desc.Width; r.Height = desc.Height;
            r.DepthOrArraySize = 1; r.MipLevels = 1; r.SampleDesc.Count = 1; r.Format = desc.Format;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            carrier.Reset();
            result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &r,
                         D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&carrier));
            if (FAILED(result)) return result;
        }
        image = carrier;
        return S_OK;
    }
    bool prepare(ID3D12GraphicsCommandList *cmd, const Packet &p, unsigned factor, bool active, std::string &error) override {
        ready = 0;
        current_frame = p.frame;
        {
            std::unique_lock lock(mutex);
            if (!available.wait_for(lock, std::chrono::seconds(3), [&] {
                    return std::any_of(groups.begin(), groups.end(), [](const Group &g) { return !g.busy; });
                })) { error = "Enabler output queue did not retire"; return false; }
            current = 0;
            while (groups[current].busy) ++current;
            real = groups[current].real; generated = groups[current].generated;
        }
        auto image_packet = p;
        image_packet.width = unsigned(p.backbuffer->GetDesc().Width);
        image_packet.height = p.backbuffer->GetDesc().Height;
        if (!images(image_packet, error)) return false;
        parameters.Set("DLSSG.BackbufferWidth", p.width);
        parameters.Set("DLSSG.BackbufferHeight", p.height);
        parameters.Set("Width", p.width); parameters.Set("Height", p.height);
        parameters.Set("DLSSG.CmdQueue", static_cast<void *>(queue.Get()));
        transition(cmd, p.backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cmd, real.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(real.Get(), p.backbuffer);
        transition(cmd, p.backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        transition(cmd, real.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!active) {
            transition(cmd, real.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            reset_next = true; return true;
        }
        if (!feature) {
            if (!ngx_checked(create_feature(cmd, NVSDK_NGX_Feature_FrameGeneration, &parameters, &feature), "Enabler feature", error)) {
                transition(cmd, real.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                return false;
            }
        }
        transition(cmd, p.depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(cmd, p.motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(cmd, p.hudless, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        parameters.Set("DLSSG.Backbuffer", static_cast<void *>(real.Get()));
        parameters.Set("DLSSG.Depth", static_cast<void *>(p.depth));
        parameters.Set("DLSSG.MVecs", static_cast<void *>(p.motion));
        parameters.Set("DLSSG.HUDLess", static_cast<void *>(p.hudless));
        parameters.Set("DLSSG.ColorBuffersHDR", p.hdr ? 1u : 0u);
        parameters.Set("DLSSG.DepthInverted", p.camera.inverted ? 1u : 0u);
        parameters.Set("DLSSG.CameraNear", p.camera.near_plane);
        parameters.Set("DLSSG.CameraFar", p.camera.infinite ? 0.f : p.camera.far_plane);
        parameters.Set("DLSSG.CameraFOV", p.camera.fov);
        parameters.Set("DLSSG.CameraAspectRatio", p.camera.aspect);
        parameters.Set("DLSSG.CameraViewToClip", const_cast<float *>(p.camera.projection.data()));
        parameters.Set("DLSSG.MvecScaleX", 1.f); parameters.Set("DLSSG.MvecScaleY", 1.f);
        parameters.Set("DLSSG.MvecDilated", 0u); parameters.Set("DLSSG.MvecJittered", 0u);
        parameters.Set("DLSSG.JitterOffsetX", p.jitter_x); parameters.Set("DLSSG.JitterOffsetY", p.jitter_y);
        parameters.Set("DLSSG.Reset", (p.reset || reset_next) ? 1u : 0u);
        parameters.Set("DLSSG.EnableInterp", 1u);
        // The headless API otherwise resets/closes the supplied list itself.
        // This list is already recording and is submitted by our transport.
        parameters.Set("DLSSG.IsRecording", 1u);
        parameters.Set("DLSSG.FrameIndex", static_cast<unsigned>(real_frames++));
        const unsigned count = std::clamp(factor, 2u, 4u) - 1;
        parameters.Set("DLSSG.MultiFrameCount", count);
        parameters.Set("DLSSG.NumFramesToGenerate", count);
        for (const auto *prefix : {"Backbuffer", "HUDLess", "Depth", "MVecs"}) {
            const std::string key = std::string("DLSSG.") + prefix + "Subrect";
            const bool guide = std::string_view(prefix) == "Depth" || std::string_view(prefix) == "MVecs";
            parameters.Set((key + "BaseX").c_str(), 0u); parameters.Set((key + "BaseY").c_str(), 0u);
            parameters.Set((key + "Width").c_str(), guide ? p.render_width : p.width);
            parameters.Set((key + "Height").c_str(), guide ? p.render_height : p.height);
        }
        bool success = true;
        for (unsigned i = 0; i < count && success; ++i) {
            transition(cmd, generated[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            parameters.Set("DLSSG.MultiFrameIndex", i + 1);
            parameters.Set("DLSSG.OutputInterpolated", static_cast<void *>(generated[i].Get()));
            const auto result = evaluate(cmd, feature, &parameters, nullptr);
            diagnostics::event("enabler.evaluate", p.frame, i + 1, count, unsigned(result));
            success = ngx_checked(result, "Enabler interpolation", error);
            transition(cmd, generated[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        }
        transition(cmd, real.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(cmd, p.depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        transition(cmd, p.motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        transition(cmd, p.hudless, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        if (!success) { reset_next = true; return false; }
        ready = (p.reset || reset_next) ? 0u : count;
        reset_next = false;
        frame_ms = std::clamp(double(p.milliseconds), 1.0, 100.0);
        return true;
    }
    HRESULT present(IDXGISwapChain4 *chain, unsigned interval, unsigned flags) override {
        if (flags & DXGI_PRESENT_TEST) return chain->Present(interval, flags);
        if (FAILED(worker_result.load())) return worker_result.load();
        std::string error;
        if (!audit(error)) { report_failure(error); return E_FAIL; }
        {
            std::lock_guard lock(mutex);
            auto &group = groups[current];
            if (!group.real || group.busy) return E_FAIL;
            group.count = ready; group.interval = interval; group.flags = flags;
            group.frame = current_frame;
            if (!interval) {
                BOOL fullscreen = TRUE;
                if (SUCCEEDED(chain->GetFullscreenState(&fullscreen, nullptr)) && !fullscreen)
                    group.flags |= DXGI_PRESENT_ALLOW_TEARING;
            } else group.flags &= ~DXGI_PRESENT_ALLOW_TEARING;
            group.milliseconds = frame_ms;
            group.busy = true;
            pending.push_back(current);
        }
        ready = 0;
        available.notify_all();
        return S_OK;
    }
    bool drain_output(unsigned timeout, std::string &error) override {
        std::unique_lock lock(mutex);
        if (!available.wait_for(lock, std::chrono::milliseconds(timeout), [&] {
                return std::none_of(groups.begin(), groups.end(), [](const Group &g) { return g.busy; });
            })) { error = "Enabler presentation worker retirement timed out"; return false; }
        if (FAILED(worker_result.load())) { error = "Enabler presentation worker failed"; return false; }
        return true;
    }
    void release_output() override {
        {
            std::lock_guard lock(mutex); quitting = true;
        }
        available.notify_all();
        if (worker.joinable()) worker.join();
        output.Reset();
    }
    bool resize(unsigned, unsigned, std::string &error) override {
        if (!drain_output(3000, error) || !drain(error)) return false;
        for (auto &g : groups) { g.real.Reset(); for (auto &r : g.generated) r.Reset(); }
        real.Reset(); for (auto &r : generated) r.Reset(); carrier.Reset();
        return true;
    }
    bool after_present(ID3D12CommandQueue *, std::string &error) override {
        if (FAILED(worker_result.load())) { error = "Enabler asynchronous Present failed"; return false; }
        return drain(error);
    }
    void marker(unsigned, std::uint64_t) override {}
    bool asynchronous_presentation() const override { return true; }
    void set_present_markers(std::function<void(unsigned, std::uint64_t)> callback) override {
        present_markers = std::move(callback);
    }
    void sleep(std::uint64_t) override {}
    void stop() override { ready = 0; reset_next = true; }
    bool close(std::string &error) override {
        stop();
        if (!drain_output(3000, error)) return false;
        release_output();
        if (!drain(error)) return false;
        if (feature && !ngx_checked(release(feature), "Enabler release", error)) return false;
        feature = nullptr;
        if (initialized && !ngx_checked(shutdown(device.Get()), "Enabler shutdown", error)) return false;
        initialized = false;
        for (auto &r : generated) r.Reset();
        for (auto &g : groups) { g.real.Reset(); for (auto &r : g.generated) r.Reset(); }
        carrier.Reset(); real.Reset(); commands.Reset(); allocator.Reset(); fence.Reset(); display_fence.Reset(); queue.Reset(); device.Reset();
        return true;
    }
    bool latency_available() const override { return false; }
    unsigned maximum() const override { return 4; }
    unsigned last_presented() const override { return last; }
    std::uint64_t displayed() const override { return total; }
    std::string latency_name() const override { return "Disabled"; }
};
}
std::shared_ptr<Backend> make_enabler_backend(const std::filesystem::path &root, std::string &error) {
    try { return std::make_shared<Enabler>(root); }
    catch (const std::exception &e) { error = e.what(); return {}; }
}
}
