#include "xivrender/dx11_dx12_bridge.hpp"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <cassert>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <thread>
#include <atomic>

namespace {
using Microsoft::WRL::ComPtr;
using xivrender::interop::BeginResult;
using xivrender::interop::Bridge;
using xivrender::interop::Frame;
void check(HRESULT result, const char *operation) {
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}
void require(bool result, const std::string &error) {
    if (!result)
        throw std::runtime_error(error);
}
// A CPU-signalled gate makes allocator exhaustion deterministic without a
// heavy shader, long GPU spin, or adapter-wide workload. Always unblock it
// before Bridge/device destruction if a test operation throws.
struct QueueGate {
    ComPtr<ID3D12Fence> fence;
    explicit QueueGate(Bridge &bridge) {
        check(bridge.device12()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
              "Create bounded pressure gate");
        check(bridge.queue12()->Wait(fence.Get(), 1), "Queue bounded pressure gate");
    }
    ~QueueGate() {
        if (fence)
            fence->Signal(1);
    }
};
void barrier(ID3D12GraphicsCommandList *commands, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    commands->ResourceBarrier(1, &b);
}
struct Readback {
    ComPtr<ID3D12Resource> resource;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    explicit Readback(ID3D12Device *device, ID3D12Resource *source) {
        const auto source_desc = source->GetDesc();
        device->GetCopyableFootprints(&source_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&resource)),
              "Create test readback");
    }
    void record(ID3D12GraphicsCommandList *commands, ID3D12Resource *source) {
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = source;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    void compare(const void *expected, unsigned row_bytes, unsigned height) {
        void *mapped = nullptr;
        const D3D12_RANGE read{0, static_cast<SIZE_T>(bytes)};
        check(resource->Map(0, &read, &mapped), "Map test readback");
        for (unsigned y = 0; y < height; ++y)
            assert(std::memcmp(static_cast<const unsigned char *>(mapped) + footprint.Offset +
                                   y * footprint.Footprint.RowPitch,
                               static_cast<const unsigned char *>(expected) + y * row_bytes, row_bytes) == 0);
        const D3D12_RANGE no_write{};
        resource->Unmap(0, &no_write);
    }
};
bool run(D3D_DRIVER_TYPE driver, const char *name, bool permit_skip) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const auto created = D3D11CreateDevice(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                           &device, nullptr, &context);
    if (FAILED(created)) {
        std::cout << "SKIP: " << name << " D3D11 device unavailable: " << std::hex << unsigned(created)
                  << std::dec << '\n';
        return false;
    }
    Bridge bridge;
    std::string error;
    constexpr unsigned width = 128, height = 64;
    if (!bridge.Initialize(device.Get(), width, height, 2, error)) {
        if (permit_skip) {
            std::cout << "SKIP: " << name << " cross-API sharing unavailable: " << error << '\n';
            return false;
        }
        throw std::runtime_error(name + std::string(": ") + error);
    }
    assert(bridge.width() == width && bridge.height() == height && bridge.slot_count() == 2);
    assert(bridge.device11() == device.Get());
    assert(bridge.context11()->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE);
    assert(!bridge.Initialize(device.Get(), width, height, 2, error));
    Frame *first = nullptr, *second = nullptr, *third = nullptr;
    assert(bridge.TryBegin(first, error) == BeginResult::Ready);
    assert(bridge.TryBegin(second, error) == BeginResult::Ready);
    assert(first != second && first->index != second->index);
    assert(bridge.TryBegin(third, error) == BeginResult::Busy && third == nullptr);
    assert(!bridge.Drain(0, error));
    Frame foreign;
    foreign.index = first->index;
    assert(!bridge.Submit(foreign, error));
    require(bridge.Abandon(*first, error), error);
    require(bridge.Abandon(*second, error), error);
    require(bridge.Drain(5000, error), error);

    // Unrelated native pipeline bindings must remain intact across handoffs.
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    check(device->CreateSamplerState(&sd, &sampler), "Create test sampler");
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constant;
    check(device->CreateBuffer(&bd, nullptr, &constant), "Create test constant buffer");
    auto *sampler_raw = sampler.Get();
    auto *constant_raw = constant.Get();
    context->CSSetSamplers(3, 1, &sampler_raw);
    context->CSSetConstantBuffers(4, 1, &constant_raw);

    const std::array<std::uint16_t, 4> pixel{0x3400, 0x3800, 0x3a00, 0x3c00};
    std::vector<std::array<std::uint16_t, 4>> colours(width * height, pixel);
    std::vector<float> depths(width * height, .5f);
    std::vector<std::array<std::uint16_t, 2>> motions(width * height, {0x3000, 0x3800});
    Frame *frame = nullptr;
    assert(bridge.TryBegin(frame, error) == BeginResult::Ready);
    const auto input_desc = frame->colour_in.texture12->GetDesc();
    assert(input_desc.Width == width && input_desc.Height == height &&
           input_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    assert(frame->depth.texture12->GetDesc().Format == DXGI_FORMAT_R32_FLOAT);
    assert(frame->motion.texture12->GetDesc().Format == DXGI_FORMAT_R16G16_FLOAT);
    context->UpdateSubresource(frame->colour_in.texture11.Get(), 0, nullptr, colours.data(), width * 8, 0);
    context->UpdateSubresource(frame->depth.texture11.Get(), 0, nullptr, depths.data(), width * 4, 0);
    context->UpdateSubresource(frame->motion.texture11.Get(), 0, nullptr, motions.data(), width * 4, 0);
    Readback colour_readback(bridge.device12(), frame->colour_in.texture12.Get());
    Readback depth_readback(bridge.device12(), frame->depth.texture12.Get());
    Readback motion_readback(bridge.device12(), frame->motion.texture12.Get());
    auto *commands = frame->commands;
    for (auto *input :
         {frame->colour_in.texture12.Get(), frame->depth.texture12.Get(), frame->motion.texture12.Get()})
        barrier(commands, input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(commands, frame->colour_out.texture12.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyResource(frame->colour_out.texture12.Get(), frame->colour_in.texture12.Get());
    colour_readback.record(commands, frame->colour_in.texture12.Get());
    depth_readback.record(commands, frame->depth.texture12.Get());
    motion_readback.record(commands, frame->motion.texture12.Get());
    for (auto *input :
         {frame->colour_in.texture12.Get(), frame->depth.texture12.Get(), frame->motion.texture12.Get()})
        barrier(commands, input, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(commands, frame->colour_out.texture12.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COMMON);
    require(bridge.Submit(*frame, error), error);
    assert(frame->completion_value != 0);
    assert(!bridge.Submit(*frame, error));
    require(bridge.WaitForOutput(*frame, error), error);
    require(bridge.WaitForOutput(*frame, error), error); // Idempotent GPU wait.
    D3D11_TEXTURE2D_DESC staging_desc{};
    frame->colour_out.texture11->GetDesc(&staging_desc);
    staging_desc.BindFlags = staging_desc.MiscFlags = 0;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&staging_desc, nullptr, &staging), "Create test staging texture");
    context->CopyResource(staging.Get(), frame->colour_out.texture11.Get());
    require(bridge.EndFrame(*frame, error), error);
    require(bridge.Drain(5000, error), error);
    assert(bridge.completion_fence()->GetCompletedValue() >= frame->completion_value);
    colour_readback.compare(colours.data(), width * 8, height);
    depth_readback.compare(depths.data(), width * 4, height);
    motion_readback.compare(motions.data(), width * 4, height);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map test output staging");
    for (unsigned y = 0; y < height; ++y)
        assert(std::memcmp(static_cast<const unsigned char *>(mapped.pData) + y * mapped.RowPitch,
                           colours.data() + y * width, width * 8) == 0);
    context->Unmap(staging.Get(), 0);
    ComPtr<ID3D11SamplerState> observed_sampler;
    ComPtr<ID3D11Buffer> observed_constant;
    context->CSGetSamplers(3, 1, &observed_sampler);
    context->CSGetConstantBuffers(4, 1, &observed_constant);
    assert(observed_sampler.Get() == sampler.Get() && observed_constant.Get() == constant.Get());
    // These test-only D3D12 resources belong to this private generation. Do
    // not keep their COM vtables alive past its device/runtime shutdown.
    colour_readback.resource.Reset();
    depth_readback.resource.Reset();
    motion_readback.resource.Reset();

    // A creation-only list can use normal Submit/EndFrame with no CPU wait and
    // no output consumer. The next change/teardown drains it explicitly.
    assert(bridge.TryBegin(frame, error) == BeginResult::Ready);
    require(bridge.Submit(*frame, error), error);
    require(bridge.EndFrame(*frame, error), error);
    require(bridge.Shutdown(5000, error), error);
    assert(!bridge.device12() && bridge.TryBegin(frame, error) == BeginResult::Unavailable);

    // NR can reuse one image bundle with three independently retired command
    // recordings. Submit all three frames before a CPU drain, preserving each
    // output in a queued D3D11 consumer before overwriting the shared surfaces.
    require(bridge.Initialize(device.Get(), width, height, 3, error, true), error);
    assert(bridge.reuses_surfaces() && bridge.slot_count() == 3);
    std::array<ComPtr<ID3D11Texture2D>, 3> alias_staging;
    std::array<ComPtr<ID3D11Texture2D>, 3> alias_producers;
    std::array<std::array<std::uint16_t, 4>, 3> alias_pixels{};
    ID3D11Texture2D *shared_colour11 = nullptr;
    ID3D12Resource *shared_colour12 = nullptr;
    for (unsigned i = 0; i < alias_staging.size(); ++i) {
        assert(bridge.TryBegin(frame, error) == BeginResult::Ready);
        if (!i) {
            shared_colour11 = frame->colour_in.texture11.Get();
            shared_colour12 = frame->colour_in.texture12.Get();
        }
        assert(frame->colour_in.texture11.Get() == shared_colour11 &&
               frame->colour_in.texture12.Get() == shared_colour12);
        Frame *blocked = nullptr;
        assert(bridge.TryBegin(blocked, error) == BeginResult::Busy);
        alias_pixels[i] = {std::uint16_t(0x3000 + i * 0x100), 0x3800, 0x3a00, 0x3c00};
        std::fill(colours.begin(), colours.end(), alias_pixels[i]);
        D3D11_TEXTURE2D_DESC producer_desc{};
        frame->colour_in.texture11->GetDesc(&producer_desc);
        producer_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        producer_desc.MiscFlags = 0;
        producer_desc.Usage = D3D11_USAGE_IMMUTABLE;
        const D3D11_SUBRESOURCE_DATA initial{colours.data(), width * 8, 0};
        check(device->CreateTexture2D(&producer_desc, &initial, &alias_producers[i]),
              "Create GPU copy producer");
        // UpdateSubresource can use a CPU write fast path on software/shared
        // surfaces. NR's real producer is GPU compute, so exercise a GPU copy
        // here rather than CPU writes that are outside queued fence ordering.
        context->CopyResource(frame->colour_in.texture11.Get(), alias_producers[i].Get());
        barrier(frame->commands, frame->colour_in.texture12.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(frame->commands, frame->colour_out.texture12.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_DEST);
        frame->commands->CopyResource(frame->colour_out.texture12.Get(), frame->colour_in.texture12.Get());
        barrier(frame->commands, frame->colour_in.texture12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON);
        barrier(frame->commands, frame->colour_out.texture12.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_COMMON);
        require(bridge.Submit(*frame, error), error);
        assert(bridge.TryBegin(blocked, error) == BeginResult::Busy);
        require(bridge.WaitForOutput(*frame, error), error);
        assert(bridge.TryBegin(blocked, error) == BeginResult::Busy);
        check(device->CreateTexture2D(&staging_desc, nullptr, &alias_staging[i]),
              "Create aliased test consumer");
        context->CopyResource(alias_staging[i].Get(), frame->colour_out.texture11.Get());
        require(bridge.EndFrame(*frame, error), error);
    }
    require(bridge.Drain(5000, error), error);
    for (unsigned i = 0; i < alias_staging.size(); ++i) {
        check(context->Map(alias_staging[i].Get(), 0, D3D11_MAP_READ, 0, &mapped),
              "Map aliased test consumer");
        for (unsigned y = 0; y < height; ++y) {
            const auto *row = static_cast<const unsigned char *>(mapped.pData) + y * mapped.RowPitch;
            for (unsigned x = 0; x < width; ++x) {
                if (std::memcmp(row + x * 8, alias_pixels[i].data(), 8) != 0) {
                    std::array<std::uint16_t, 4> actual{};
                    std::memcpy(actual.data(), row + x * 8, 8);
                    context->Unmap(alias_staging[i].Get(), 0);
                    throw std::runtime_error(std::string(name) + " aliased frame " + std::to_string(i) +
                                             " pixel " + std::to_string(x) + "," + std::to_string(y) +
                                             " channel0=" + std::to_string(actual[0]) +
                                             " expected=" + std::to_string(alias_pixels[i][0]));
                }
            }
        }
        context->Unmap(alias_staging[i].Get(), 0);
    }
    require(bridge.Shutdown(5000, error), error);
    // Exhaust all three command recordings behind a short, explicit queue
    // gate. The shared image bundle is still GPU-ordered; none of its command
    // allocators may be reset until the corresponding D3D11 consumer retires.
    require(bridge.Initialize(device.Get(), width, height, 3, error, true), error);
    {
        QueueGate gate(bridge);
        for (unsigned i = 0; i < bridge.slot_count(); ++i) {
            require(bridge.TryBegin(frame, error) == BeginResult::Ready,
                    "A fresh pressure-test recording was unavailable: " + error);
            require(bridge.Submit(*frame, error), error);
            require(bridge.EndFrame(*frame, error), error);
        }
        Frame *blocked = nullptr;
        require(bridge.TryBegin(blocked, error) == BeginResult::Busy && !blocked,
                "Exhausted GPU recordings were reused before retirement");
        const auto timeout_start = GetTickCount64();
        require(bridge.Begin(blocked, 10, error) == BeginResult::Busy && !blocked,
                "Bounded begin reused an unretired allocator or failed to report timeout");
        require(GetTickCount64() - timeout_start < 1000,
                "Bounded begin exceeded its finite timeout by more than scheduling tolerance");
        require(gate.fence->GetCompletedValue() == 0,
                "Pressure gate unexpectedly opened before the recovery test");
        std::atomic<HRESULT> released{E_PENDING};
        std::jthread release_gate([fence = gate.fence, &released] {
            Sleep(25);
            released.store(fence->Signal(1), std::memory_order_release);
        });
        const auto recovered = bridge.Begin(frame, 1000, error);
        release_gate.join();
        check(released.load(std::memory_order_acquire), "Release bounded pressure gate");
        require(recovered == BeginResult::Ready && frame,
                "Bounded begin did not recover after GPU consumer retirement: " + error);
        require(bridge.Submit(*frame, error), error);
        require(bridge.EndFrame(*frame, error), error);
        require(bridge.Drain(5000, error), error);
    }
    require(bridge.Shutdown(5000, error), error);
    context->ClearState();
    std::cout << "PASS: " << name
              << " D3D11/D3D12 colour/depth/motion roundtrip, two-slot bounds, GPU "
                 "retirement, binding preservation, three-frame aliased surface ordering and bounded "
                 "pressure timeout/recovery\n";
    return true;
}
} // namespace
int main() {
    try {
        const bool warp = run(D3D_DRIVER_TYPE_WARP, "WARP", true);
        const bool hardware = run(D3D_DRIVER_TYPE_HARDWARE, "hardware", false);
        if (!warp && !hardware)
            std::cout << "SKIP: no supported cross-API adapter on this host\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
