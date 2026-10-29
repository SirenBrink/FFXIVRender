#include "xivrender/dx11_dx12_bridge.hpp"
#include <dxgi1_6.h>
#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <vector>

namespace xivrender::interop {
namespace {
using Microsoft::WRL::ComPtr;
std::string failure(const char *operation, HRESULT result) {
    std::ostringstream out;
    out << operation << " failed (0x" << std::hex << static_cast<unsigned>(result) << ')';
    return out.str();
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
};
struct Library {
    HMODULE value = nullptr;
    ~Library() {
        if (value)
            FreeLibrary(value);
    }
};
enum class Phase { Idle, Recording, Submitted, Waiting, Retiring };
struct Slot {
    Frame frame;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    std::uint64_t retired_value = 0;
    Phase phase = Phase::Idle;
};
struct FencePair {
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Fence> fence11;
    std::uint64_t value = 0;
};
} // namespace

struct Bridge::Impl {
    Library d3d12_runtime; // Outlives every object created through its factory.
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11Device5> device11_5;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    FencePair producer, complete, retire;
    std::vector<Slot> slots;
    Handle event;
    unsigned width = 0, height = 0, cursor = 0;
    bool poisoned = false, reuse_surfaces = false;

    bool fail(const char *operation, HRESULT result, std::string &error) {
        poisoned = true;
        error = failure(operation, result);
        return false;
    }
    bool live(std::string &error) {
        auto result = device11->GetDeviceRemovedReason();
        if (FAILED(result))
            return fail("Native D3D11 device", result, error);
        result = device12->GetDeviceRemovedReason();
        if (FAILED(result))
            return fail("Interop D3D12 device", result, error);
        if (poisoned) {
            error = "Interop generation stopped after a submission error";
            return false;
        }
        return true;
    }
    Slot *slot(Frame &frame, std::string &error) {
        if (frame.index >= slots.size() || &slots[frame.index].frame != &frame) {
            error = "Foreign or copied interop frame";
            return nullptr;
        }
        return &slots[frame.index];
    }
    bool create_fence(FencePair &pair, std::string &error) {
        auto result = device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&pair.fence12));
        if (FAILED(result)) {
            error = failure("Create shared D3D12 fence", result);
            return false;
        }
        Handle handle;
        result =
            device12->CreateSharedHandle(pair.fence12.Get(), nullptr, GENERIC_ALL, nullptr, &handle.value);
        if (FAILED(result)) {
            error = failure("Create shared fence handle", result);
            return false;
        }
        result = device11_5->OpenSharedFence(handle.value, IID_PPV_ARGS(&pair.fence11));
        if (FAILED(result)) {
            error = failure("Open shared D3D11 fence", result);
            return false;
        }
        return true;
    }
    bool create_texture(SharedTexture &texture, DXGI_FORMAT format, std::string &error) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        auto result = device11->CreateTexture2D(&desc, nullptr, &texture.texture11);
        if (FAILED(result)) {
            error = failure("Create shared D3D11 texture", result);
            return false;
        }
        ComPtr<IDXGIResource1> resource;
        result = texture.texture11.As(&resource);
        if (FAILED(result)) {
            error = failure("Query shared texture", result);
            return false;
        }
        Handle handle;
        result = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                              nullptr, &handle.value);
        if (FAILED(result)) {
            error = failure("Create shared texture handle", result);
            return false;
        }
        result = device12->OpenSharedHandle(handle.value, IID_PPV_ARGS(&texture.texture12));
        if (FAILED(result)) {
            error = failure("Open shared D3D12 texture", result);
            return false;
        }
        result = device11->CreateShaderResourceView(texture.texture11.Get(), nullptr, &texture.srv11);
        if (FAILED(result)) {
            error = failure("Create shared texture SRV", result);
            return false;
        }
        result = device11->CreateUnorderedAccessView(texture.texture11.Get(), nullptr, &texture.uav11);
        if (FAILED(result)) {
            error = failure("Create shared texture UAV", result);
            return false;
        }
        return true;
    }
    bool wait(FencePair &pair, std::uint64_t value, ULONGLONG deadline, std::string &error) {
        while (value) {
            const auto completed = pair.fence12->GetCompletedValue();
            if (completed == std::numeric_limits<std::uint64_t>::max())
                return fail("Interop fence reports device removal", DXGI_ERROR_DEVICE_REMOVED, error);
            if (completed >= value)
                return true;
            const auto now = GetTickCount64();
            if (now >= deadline) {
                error = "Interop GPU work is still in flight; ownership retained";
                return false;
            }
            if (!event.value) {
                event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!event.value) {
                    error = failure("Create GPU retirement event", HRESULT_FROM_WIN32(GetLastError()));
                    return false;
                }
            }
            auto result = pair.fence12->SetEventOnCompletion(value, event.value);
            if (FAILED(result)) {
                error = failure("Set GPU retirement event", result);
                return false;
            }
            const auto remaining = std::min<ULONGLONG>(deadline - now, MAXDWORD - 1ull);
            const auto waited = WaitForSingleObject(event.value, static_cast<DWORD>(remaining));
            if (waited != WAIT_OBJECT_0) {
                error = "Interop GPU retirement wait did not complete; ownership retained";
                return false;
            }
        }
        return true;
    }
};

Bridge::Bridge() = default;
Bridge::~Bridge() {
    std::string error;
    if (!Shutdown(1000, error)) {
        // The process owns outstanding GPU work. Do not destroy its resources,
        // allocators or shared fences merely because a timeout elapsed.
        (void)impl_.release();
    }
}
bool Bridge::Initialize(ID3D11Device *native_device, unsigned width, unsigned height, unsigned count,
                        std::string &error, bool reuse_surfaces) {
    error.clear();
    if (impl_) {
        error = "Drain and shut down the current interop generation before reinitializing";
        return false;
    }
    if (!native_device || !width || !height || width > 16384 || height > 16384 || count < 2 || count > 3) {
        error = "Invalid interop device, extent or bounded slot count";
        return false;
    }
    auto next = std::make_unique<Impl>();
    next->device11 = native_device;
    next->width = width;
    next->height = height;
    next->reuse_surfaces = reuse_surfaces;
    auto result = native_device->QueryInterface(IID_PPV_ARGS(&next->device11_5));
    if (FAILED(result)) {
        error = failure("Native ID3D11Device5 support", result);
        return false;
    }
    ComPtr<ID3D11DeviceContext> immediate;
    native_device->GetImmediateContext(&immediate);
    result = immediate.As(&next->context11);
    if (FAILED(result) || immediate->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        error = failure("Native immediate ID3D11DeviceContext4 support", result);
        return false;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    if (FAILED(result = native_device->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(result = dxgi->GetAdapter(&adapter)) || FAILED(result = adapter->GetDesc(&adapter_desc))) {
        error = failure("Read native D3D11 adapter", result);
        return false;
    }
    // NR is a private compute consumer, not another ReShade game device.
    // The exported D3D12CreateDevice route gets wrapped and broadcasts device
    // destruction to unrelated game addons (legacy REST owns global D3D11
    // buffers). A private factory gives us an independent native device,
    // without changing process-wide SDK/debug state or suppressing callbacks.
    ComPtr<ID3D12DeviceFactory> factory;
    next->d3d12_runtime.value = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto get_interface = next->d3d12_runtime.value
                                   ? reinterpret_cast<decltype(&D3D12GetInterface)>(
                                         GetProcAddress(next->d3d12_runtime.value, "D3D12GetInterface"))
                                   : nullptr;
    result = get_interface ? get_interface(CLSID_D3D12DeviceFactory, IID_PPV_ARGS(&factory)) : E_NOINTERFACE;
    if (FAILED(result)) {
        error = failure("Private D3D12 device factory is unavailable", result);
        return false;
    }
    result = factory->SetFlags(D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON);
    if (SUCCEEDED(result))
        result = factory->CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&next->device12));
    if (FAILED(result)) {
        error = failure("Create private same-adapter D3D12 device", result);
        return false;
    }
    const auto luid = next->device12->GetAdapterLuid();
    if (luid.LowPart != adapter_desc.AdapterLuid.LowPart ||
        luid.HighPart != adapter_desc.AdapterLuid.HighPart) {
        error = "Interop device adapter does not match the native D3D11 adapter";
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = next->device12->CreateCommandQueue(&queue, IID_PPV_ARGS(&next->queue12));
    if (FAILED(result)) {
        error = failure("Create interop D3D12 queue", result);
        return false;
    }
    if (!next->create_fence(next->producer, error) || !next->create_fence(next->complete, error) ||
        !next->create_fence(next->retire, error))
        return false;
    next->slots.resize(count);
    for (unsigned i = 0; i < count; ++i) {
        auto &slot = next->slots[i];
        slot.frame.index = i;
        if (i && reuse_surfaces) {
            const auto &shared = next->slots.front().frame;
            slot.frame.colour_in = shared.colour_in;
            slot.frame.colour_out = shared.colour_out;
            slot.frame.depth = shared.depth;
            slot.frame.motion = shared.motion;
        } else {
            if (!next->create_texture(slot.frame.colour_in, DXGI_FORMAT_R16G16B16A16_FLOAT, error) ||
                !next->create_texture(slot.frame.colour_out, DXGI_FORMAT_R16G16B16A16_FLOAT, error) ||
                !next->create_texture(slot.frame.depth, DXGI_FORMAT_R32_FLOAT, error) ||
                !next->create_texture(slot.frame.motion, DXGI_FORMAT_R16G16_FLOAT, error))
                return false;
        }
        result = next->device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&slot.allocator));
        if (FAILED(result)) {
            error = failure("Create interop command allocator", result);
            return false;
        }
        result = next->device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(),
                                                   nullptr, IID_PPV_ARGS(&slot.commands));
        if (FAILED(result) || FAILED(result = slot.commands->Close())) {
            error = failure("Create interop command list", result);
            return false;
        }
        slot.frame.commands = slot.commands.Get();
    }
    impl_ = std::move(next);
    return true;
}
BeginResult Bridge::TryBegin(Frame *&frame, std::string &error) {
    frame = nullptr;
    error.clear();
    if (!impl_) {
        error = "Interop generation is not initialized";
        return BeginResult::Unavailable;
    }
    auto &p = *impl_;
    if (!p.live(error))
        return BeginResult::Unavailable;
    // Aliased surfaces may be reused before GPU completion only after the
    // preceding output wait and its D3D11 consumers were enqueued. Otherwise
    // a caller could overwrite input for an older, unsubmitted recording.
    if (p.reuse_surfaces && std::any_of(p.slots.begin(), p.slots.end(), [](const Slot &slot) {
            return slot.phase == Phase::Recording || slot.phase == Phase::Submitted ||
                   slot.phase == Phase::Waiting;
        }))
        return BeginResult::Busy;
    const auto completed12 = p.complete.fence12->GetCompletedValue();
    const auto consumed11 = p.retire.fence12->GetCompletedValue();
    if (completed12 == std::numeric_limits<std::uint64_t>::max() ||
        consumed11 == std::numeric_limits<std::uint64_t>::max()) {
        p.fail("Interop fence reports device removal", DXGI_ERROR_DEVICE_REMOVED, error);
        return BeginResult::Unavailable;
    }
    for (unsigned n = 0; n < p.slots.size(); ++n) {
        const auto index = (p.cursor + n) % static_cast<unsigned>(p.slots.size());
        auto &slot = p.slots[index];
        if (slot.phase != Phase::Idle && slot.phase != Phase::Retiring)
            continue;
        if (slot.frame.completion_value > completed12 || slot.retired_value > consumed11)
            continue;
        auto result = slot.allocator->Reset();
        if (FAILED(result)) {
            p.fail("Reset retired interop allocator", result, error);
            return BeginResult::Unavailable;
        }
        result = slot.commands->Reset(slot.allocator.Get(), nullptr);
        if (FAILED(result)) {
            p.fail("Reset retired interop command list", result, error);
            return BeginResult::Unavailable;
        }
        slot.phase = Phase::Recording;
        slot.frame.completion_value = 0;
        slot.retired_value = 0;
        p.cursor = (index + 1) % static_cast<unsigned>(p.slots.size());
        frame = &slot.frame;
        return BeginResult::Ready;
    }
    return BeginResult::Busy;
}
BeginResult Bridge::Begin(Frame *&frame, unsigned timeout_ms, std::string &error) {
    const auto result = TryBegin(frame, error);
    if (result != BeginResult::Busy || !timeout_ms)
        return result;
    auto &p = *impl_;
    // With one shared image bundle, an earlier open producer/consumer must
    // first be explicitly ended by its caller. A CPU wait cannot fix it.
    if (p.reuse_surfaces && std::any_of(p.slots.begin(), p.slots.end(), [](const Slot &slot) {
            return slot.phase == Phase::Recording || slot.phase == Phase::Submitted ||
                   slot.phase == Phase::Waiting;
        })) {
        error = "Interop frame must be ended before waiting for another command slot";
        return BeginResult::Busy;
    }
    const Slot *oldest = nullptr;
    for (const auto &slot : p.slots)
        if (slot.phase == Phase::Retiring && slot.retired_value &&
            (!oldest || slot.retired_value < oldest->retired_value))
            oldest = &slot;
    if (!oldest) {
        error = "Interop has no ended consumer available for command retirement";
        return BeginResult::Busy;
    }
    // EndFrame queued the model-completion wait, all D3D11 consumers and
    // this signal, then flushed. Its completion has no dependency on a new
    // frame or a future Present. Keep the same bounded allocator/image set.
    if (!p.wait(p.retire, oldest->retired_value, GetTickCount64() + timeout_ms, error))
        return p.live(error) ? BeginResult::Busy : BeginResult::Unavailable;
    return TryBegin(frame, error);
}
bool Bridge::Submit(Frame &frame, std::string &error) {
    error.clear();
    if (!impl_) {
        error = "Interop generation is not initialized";
        return false;
    }
    auto &p = *impl_;
    auto *slot = p.slot(frame, error);
    if (!slot || !p.live(error))
        return false;
    if (slot->phase != Phase::Recording) {
        error = "Interop frame is not recording";
        return false;
    }
    auto result = slot->commands->Close();
    if (FAILED(result))
        return p.fail("Close interop command list", result, error);
    const auto input = ++p.producer.value;
    result = p.context11->Signal(p.producer.fence11.Get(), input);
    if (FAILED(result))
        return p.fail("Publish D3D11 input", result, error);
    // Do not leave the producer signal in the same unsubmitted D3D11 batch as
    // the later wait for D3D12. This also progresses while minimized/unfocused.
    p.context11->Flush();
    result = p.queue12->Wait(p.producer.fence12.Get(), input);
    if (FAILED(result))
        return p.fail("Queue D3D12 input wait", result, error);
    ID3D12CommandList *lists[]{slot->commands.Get()};
    p.queue12->ExecuteCommandLists(1, lists);
    frame.completion_value = ++p.complete.value;
    result = p.queue12->Signal(p.complete.fence12.Get(), frame.completion_value);
    if (FAILED(result))
        return p.fail("Signal D3D12 completion after execution", result, error);
    slot->phase = Phase::Submitted;
    return true;
}
bool Bridge::WaitForOutput(Frame &frame, std::string &error) {
    error.clear();
    if (!impl_) {
        error = "Interop generation is not initialized";
        return false;
    }
    auto &p = *impl_;
    auto *slot = p.slot(frame, error);
    if (!slot || !p.live(error))
        return false;
    if (slot->phase == Phase::Waiting)
        return true;
    if (slot->phase != Phase::Submitted || !frame.completion_value) {
        error = "Interop output was not submitted";
        return false;
    }
    const auto result = p.context11->Wait(p.complete.fence11.Get(), frame.completion_value);
    if (FAILED(result))
        return p.fail("Queue D3D11 output wait", result, error);
    slot->phase = Phase::Waiting;
    return true;
}
bool Bridge::EndFrame(Frame &frame, std::string &error) {
    if (!WaitForOutput(frame, error))
        return false;
    auto &p = *impl_;
    auto *slot = p.slot(frame, error);
    const auto value = ++p.retire.value;
    const auto result = p.context11->Signal(p.retire.fence11.Get(), value);
    if (FAILED(result))
        return p.fail("Signal D3D11 consumer retirement", result, error);
    p.context11->Flush();
    slot->retired_value = value;
    slot->phase = Phase::Retiring;
    return true;
}
bool Bridge::Abandon(Frame &frame, std::string &error) {
    error.clear();
    if (!impl_) {
        error = "Interop generation is not initialized";
        return false;
    }
    auto &p = *impl_;
    auto *slot = p.slot(frame, error);
    if (!slot || !p.live(error))
        return false;
    if (slot->phase != Phase::Recording) {
        error = "Only an unsubmitted interop recording can be abandoned";
        return false;
    }
    const auto closed = slot->commands->Close();
    if (FAILED(closed))
        return p.fail("Close abandoned interop recording", closed, error);
    const auto value = ++p.retire.value;
    const auto result = p.context11->Signal(p.retire.fence11.Get(), value);
    if (FAILED(result))
        return p.fail("Retire abandoned D3D11 writes", result, error);
    p.context11->Flush();
    slot->retired_value = value;
    slot->phase = Phase::Retiring;
    return true;
}
bool Bridge::Drain(unsigned timeout_ms, std::string &error) {
    error.clear();
    if (!impl_)
        return true;
    auto &p = *impl_;
    for (auto &slot : p.slots) {
        if (slot.phase == Phase::Recording) {
            error = "Interop recording has not been submitted or abandoned; ownership retained";
            return false;
        }
        if (slot.phase == Phase::Submitted || slot.phase == Phase::Waiting)
            if (!EndFrame(slot.frame, error))
                return false;
    }
    p.context11->Flush();
    const auto deadline = GetTickCount64() + timeout_ms;
    return p.wait(p.producer, p.producer.value, deadline, error) &&
           p.wait(p.complete, p.complete.value, deadline, error) &&
           p.wait(p.retire, p.retire.value, deadline, error);
}
bool Bridge::Shutdown(unsigned timeout_ms, std::string &error) {
    if (!Drain(timeout_ms, error))
        return false;
    impl_.reset();
    return true;
}
ID3D11Device *Bridge::device11() const {
    return impl_ ? impl_->device11.Get() : nullptr;
}
ID3D11DeviceContext4 *Bridge::context11() const {
    return impl_ ? impl_->context11.Get() : nullptr;
}
ID3D12Device *Bridge::device12() const {
    return impl_ ? impl_->device12.Get() : nullptr;
}
ID3D12CommandQueue *Bridge::queue12() const {
    return impl_ ? impl_->queue12.Get() : nullptr;
}
ID3D12Fence *Bridge::completion_fence() const {
    return impl_ ? impl_->complete.fence12.Get() : nullptr;
}
unsigned Bridge::width() const {
    return impl_ ? impl_->width : 0;
}
unsigned Bridge::height() const {
    return impl_ ? impl_->height : 0;
}
unsigned Bridge::slot_count() const {
    return impl_ ? static_cast<unsigned>(impl_->slots.size()) : 0;
}
bool Bridge::reuses_surfaces() const {
    return impl_ && impl_->reuse_surfaces;
}
} // namespace xivrender::interop
