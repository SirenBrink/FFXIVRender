// Isolated direct-model test. No game, Streamline, registry or driver patches.
#include "xivrender/native_nr_api.hpp"
#include "xivrender/ngx_parameters.hpp"
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
bool evaluate(HMODULE library, xivrender::nr::Session *session, ID3D12Device *device,
              xivrender::ngx::Parameters &p) {
    auto create = reinterpret_cast<decltype(&XIVNR_Create)>(GetProcAddress(library, "XIVNR_Create"));
    auto eval = reinterpret_cast<decltype(&XIVNR_Evaluate)>(GetProcAddress(library, "XIVNR_Evaluate"));
    auto release = reinterpret_cast<decltype(&XIVNR_Release)>(GetProcAddress(library, "XIVNR_Release"));
    if (!create || !eval || !release)
        return false;
    Work work(device);
    auto colour = work.image(DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    auto depth = work.image(DXGI_FORMAT_R32_FLOAT, false);
    auto motion = work.image(DXGI_FORMAT_R16G16_FLOAT, false);
    auto output = work.image(DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    for (const auto *key : {"Width", "Height", "OutWidth", "OutHeight", "DLSSNR.Width", "DLSSNR.Height"})
        p.Set(key, 512u);
    p.Set("DLSSNR.Enabled", 1u);
    p.Set("CreationNodeMask", 1u);
    p.Set("VisibilityNodeMask", 1u);
    p.Set("DLSSNR.Hint.Render.Preset", 0u);
    p.Set("DLSSNR.Style", 0u);
    p.Set("DLSSNR.Intensity", 1.0f);
    p.Set("DLSSNR.LocalStructureStrength", 1.0f);
    p.Set("DLSSNR.LocalToneStrength", 1.0f);
    p.Set("DLSSNR.SkinStructureStrength", 1.0f);
    p.Set("DLSSNR.UseAutoMask", 1u);
    p.Set("DLSSNR.UICorrection", 0u);
    p.Set("DLSSNR.ControlMask", static_cast<void *>(nullptr));
    void *feature = nullptr;
    const auto created = create(session, work.commands.Get(), &p, &feature);
    std::cout << "Create 512x512 feature18: 0x" << std::hex << created << std::dec << '\n' << std::flush;
    // Failed creation can still record initialization; submit/fence before cleanup.
    work.submit();
    if (created != xivrender::nr::success || !feature) {
        if (feature)
            release(session, feature);
        return false;
    }
    work.next();
    p.Set("DLSSNR.Color", colour.Get());
    p.Set("DLSSNR.Depth", depth.Get());
    p.Set("DLSSNR.MVec", motion.Get());
    p.Set("DLSSNR.Output", output.Get());
    p.Set("DLSSNR.DepthInverted", 1u);
    p.Set("DLSSNR.Reset", 1u);
    p.Set("DLSSNR.MVecScaleX", 512.0f);
    p.Set("DLSSNR.MVecScaleY", 512.0f);
    for (const auto *prefix : {"Color", "Output", "Depth", "MVec"}) {
        const auto base = std::string("DLSSNR.") + prefix + "Subrect";
        p.Set((base + "BaseX").c_str(), 0u);
        p.Set((base + "BaseY").c_str(), 0u);
        p.Set((base + "Width").c_str(), 512u);
        p.Set((base + "Height").c_str(), 512u);
    }
    const auto evaluated = eval(session, work.commands.Get(), feature, &p);
    std::cout << "Evaluate reset frame: 0x" << std::hex << evaluated << std::dec << '\n' << std::flush;
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
    void *mapping = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    check(readback->Map(0, &range, &mapping));
    unsigned invalid = 0, nonzero = 0;
    for (unsigned y = 0; y < 512; ++y) {
        const auto *row =
            static_cast<unsigned char *>(mapping) + layout.Offset + y * layout.Footprint.RowPitch;
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
    const auto released = release(session, feature);
    std::cout << "Completed GPU fences: " << work.submitted << ", nonzero RGB samples: " << nonzero
              << ", NaN/Inf: " << invalid << "\nRelease: 0x" << std::hex << released << std::dec
              << "\nSynthetic output check only; image quality and older GPUs are untested\n";
    return evaluated == xivrender::nr::success && released == xivrender::nr::success && nonzero && !invalid;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc < 3) {
        std::cerr << "Usage: native_nr_probe <absolute bridge DLL> <absolute model DLL> [--evaluate "
                     "[absolute alternate DLL]]\n";
        return 2;
    }
    const std::filesystem::path bridge(argv[1]), model(argv[2]);
    if (!bridge.is_absolute() || !model.is_absolute())
        return 2;
    const auto library = LoadLibraryExW(bridge.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) {
        std::cerr << "Bridge load: " << GetLastError() << '\n';
        return 3;
    }
    auto open = reinterpret_cast<decltype(&XIVNR_Open)>(GetProcAddress(library, "XIVNR_Open"));
    auto close = reinterpret_cast<decltype(&XIVNR_Close)>(GetProcAddress(library, "XIVNR_Close"));
    if (!open || !close) {
        FreeLibrary(library);
        return 3;
    }
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return 4;
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
        return 5;
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        return 6;
    const auto data = (std::filesystem::absolute(argv[0]).parent_path() / L"native-nr-data");
    std::filesystem::create_directories(data);
    std::vector<std::filesystem::path> models{model};
    if (argc > 4) {
        if (!std::filesystem::path(argv[4]).is_absolute())
            return 2;
        models.emplace_back(argv[4]);
        models.push_back(model); // switch back in this same process/device too
    }
    bool all_passed = true;
    for (const auto &selected : models) {
        xivrender::ngx::Parameters parameters; // no module callbacks survive a generation
        xivrender::nr::Session *session = nullptr;
        std::wcout << L"Model: " << selected.wstring() << L'\n';
        // Unregistered/private application. Never impersonate FFXIV or another game.
        const auto result = open(selected.c_str(), data.c_str(), device.Get(), 0, &parameters, &session);
        std::cout << "Native NR initialize + populate: 0x" << std::hex << result << std::dec << '\n';
        unsigned long long callback = 0;
        const auto published = parameters.Get("DLSSNRComputeScalingRatioCallback", &callback);
        std::cout << "Scaling callback published: "
                  << (published == NVSDK_NGX_Result_Success && callback != 0) << '\n';
        bool tested = true;
        if (result == xivrender::nr::success && session && argc > 3 &&
            std::wstring(argv[3]) == L"--evaluate") {
            try {
                tested = evaluate(library, session, device.Get(), parameters);
            } catch (const std::exception &error) {
                std::cerr << "Probe exception; ending isolated process with owners retained: " << error.what()
                          << '\n'
                          << std::flush;
                ExitProcess(21);
            }
        } else
            std::cout << "No feature/evaluation/presentation tested\n";
        const auto stopped = session ? close(session) : xivrender::nr::success;
        std::cout << "Native NR shutdown: 0x" << std::hex << stopped << std::dec << '\n';
        if (stopped != xivrender::nr::success)
            retain_and_exit("Model shutdown did not release ownership");
        all_passed &= result == xivrender::nr::success && stopped == xivrender::nr::success && tested;
        if (!all_passed)
            break;
    }
    device.Reset();
    FreeLibrary(library);
    return all_passed ? 0 : 7;
}
