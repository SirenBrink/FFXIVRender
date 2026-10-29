// Isolated Engine GPU test. No game, Streamline, registry or driver patches.
#include "xivrender/nr_engine.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstring>
#include <cmath>
#include <stdexcept>
using Microsoft::WRL::ComPtr;

namespace {
[[noreturn]] void retain_and_exit(const char *reason) {
    std::cerr << reason << "; terminating isolated probe with owners retained\n" << std::flush;
    ExitProcess(20);
}
void check(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("D3D12 operation failed: " + std::to_string(result));
}
struct Work {
    ID3D12Device *device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    unsigned long long submitted = 0;
    explicit Work(ID3D12Device *d) : device(d) {
        D3D12_COMMAND_QUEUE_DESC q{};
        check(d->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        check(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&commands)));
        check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }
    void submit() {
        check(commands->Close());
        ID3D12CommandList *lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), ++submitted)))
            retain_and_exit("Queue signal failed after submission");
        const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            retain_and_exit("Fence event creation failed after submission");
        if (FAILED(fence->SetEventOnCompletion(submitted, event)))
            retain_and_exit("Fence completion setup failed after submission");
        const auto result = WaitForSingleObject(event, 10000);
        CloseHandle(event);
        if (result != WAIT_OBJECT_0) {
            std::cerr << "GPU timeout; terminating isolated probe without freeing in-flight owners\n"
                      << std::flush;
            ExitProcess(20);
        }
    }
    void next() {
        check(allocator->Reset());
        check(commands->Reset(allocator.Get(), nullptr));
    }
    void transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        commands->ResourceBarrier(1, &barrier);
    }
    ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              type == D3D12_HEAP_TYPE_UPLOAD
                                                  ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                  : D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&result)));
        return result;
    }
    ComPtr<ID3D12Resource> image(DXGI_FORMAT format, bool output) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = 512;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = format;
        if (output)
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> result;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&result)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &bytes);
        auto upload = buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
        void *mapping = nullptr;
        const D3D12_RANGE empty{};
        check(upload->Map(0, &empty, &mapping));
        std::memset(mapping, 0, static_cast<size_t>(bytes));
        for (unsigned y = 0; y < 512; ++y) {
            auto *row = static_cast<unsigned char *>(mapping) + layout.Offset + y * layout.Footprint.RowPitch;
            for (unsigned x = 0; x < 512; ++x) {
                if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    // A successful no-op/partial write cannot pass the output check.
                    const std::uint16_t pixel[]{
                        output ? std::uint16_t(0x7e00) : std::uint16_t(0x3000 + (x % 32) * 16),
                        output ? std::uint16_t(0x7e00) : std::uint16_t(0x3400),
                        output ? std::uint16_t(0x7e00) : std::uint16_t(0x3800), 0x3c00};
                    std::memcpy(row + x * 8, pixel, sizeof(pixel));
                } else if (format == DXGI_FORMAT_R32_FLOAT) {
                    const float depth = .5f;
                    std::memcpy(row + x * 4, &depth, sizeof(depth));
                }
            }
        }
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = layout;
        dst.pResource = result.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(result.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                          : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        uploads.push_back(upload);
        return result;
    }
};
bool run_generation(xivrender::nr::Engine &engine, const xivrender::nr::EngineConfig &config,
                    ID3D12Device *device) {
    Work work(device);
    auto color = work.image(DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    auto depth = work.image(DXGI_FORMAT_R32_FLOAT, false);
    auto motion = work.image(DXGI_FORMAT_R16G16_FLOAT, false);
    auto output = work.image(DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    std::string error;
    if (!engine.Open(device, config, error))
        throw std::runtime_error(error);
    const bool created = engine.RecordCreate(work.commands.Get(), error);
    work.submit();
    if (!engine.MarkSubmitted(work.fence.Get(), work.submitted, error))
        retain_and_exit(error.c_str());
    if (!created || !engine.Ready())
        throw std::runtime_error("Engine create did not become ready: " + error);
    work.next();
    xivrender::nr::Evaluation frame;
    frame.color = color.Get(); frame.depth = depth.Get();
    frame.motion = motion.Get(); frame.output = output.Get();
    frame.motion_scale_x = frame.motion_scale_y = 512.0f;
    const bool evaluated = engine.RecordEvaluate(work.commands.Get(), frame, error);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = output.Get();
    work.commands->ResourceBarrier(1, &uav);
    work.transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto desc = output->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &bytes);
    auto readback = work.buffer(bytes, D3D12_HEAP_TYPE_READBACK);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = output.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = layout;
    work.commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    work.submit();
    if (!engine.MarkSubmitted(work.fence.Get(), work.submitted, error))
        retain_and_exit(error.c_str());
    void *mapping = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    check(readback->Map(0, &range, &mapping));
    unsigned invalid = 0, nonzero = 0;
    for (unsigned y = 0; y < 512; ++y) {
        const auto *row = static_cast<unsigned char *>(mapping) + layout.Offset + y * layout.Footprint.RowPitch;
        for (unsigned x = 0; x < 512; ++x)
            for (unsigned c = 0; c < 3; ++c) {
                std::uint16_t half = 0;
                std::memcpy(&half, row + x * 8 + c * 2, 2);
                invalid += (half & 0x7c00) == 0x7c00;
                nonzero += (half & 0x7fff) != 0;
            }
    }
    const D3D12_RANGE none{};
    readback->Unmap(0, &none);
    if (!engine.TryClose(error))
        retain_and_exit(error.c_str());
    std::wcout << L"Model: " << config.model.wstring() << L'\n';
    std::cout << "GPU submissions: " << work.submitted << ", nonzero RGB samples: " << nonzero
              << ", NaN/Inf: " << invalid << ", Engine eval returned success: " << evaluated
              << "\nSynthetic output check only; game guides, appearance and performance untested\n" << std::flush;
    return evaluated && !invalid && nonzero == 512u * 512u * 3u;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc < 3 || argc > 4)
        return 2;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return 3;
    for (unsigned i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        if (candidate && SUCCEEDED(candidate->GetDesc1(&desc)) && desc.VendorId == 0x10de &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter = candidate;
            std::wcout << L"Adapter: " << desc.Description << L'\n';
            break;
        }
    }
    if (!adapter)
        return 4;
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        return 5;
    try {
        xivrender::nr::Engine engine;
        const auto data = std::filesystem::absolute(argv[0]).parent_path() / L"nr-engine-data";
        std::vector<std::filesystem::path> models{argv[2]};
        if (argc == 4) { models.emplace_back(argv[3]); models.emplace_back(argv[2]); }
        for (const auto &model : models) {
            const xivrender::nr::EngineConfig config{std::filesystem::path(argv[1]), model, data, 512, 512};
            if (!run_generation(engine, config, device.Get()))
                return 6;
        }
        return 0;
    } catch (const std::exception &error) {
        retain_and_exit(error.what());
    }
}
