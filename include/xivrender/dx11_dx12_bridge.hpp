#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>

namespace xivrender::interop {

struct SharedTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture11;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv11;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav11;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture12;
};

// Borrowed slot, valid until Bridge shutdown. Do not copy it or retain it across
// reinitialization. All shared resources must be COMMON at both API handoffs.
// The caller may record transitions while commands is open after TryBegin.
struct Frame {
    unsigned index = 0;
    SharedTexture colour_in, colour_out, depth, motion;
    ID3D12GraphicsCommandList *commands = nullptr;
    std::uint64_t completion_value = 0;
};

enum class BeginResult { Ready, Busy, Unavailable };

// Single processing-thread owner. It neither changes D3D11 shader/binding state
// nor owns any game resources. Initialize receives the authoritative native
// D3D11 device, not an NGX or ReShade proxy obtained through resource GetDevice.
// GPU work uses shared fences. TryBegin never waits; Begin optionally applies
// bounded CPU backpressure only when every command slot is still retiring.
class Bridge {
  public:
    Bridge();
    ~Bridge();
    Bridge(const Bridge &) = delete;
    Bridge &operator=(const Bridge &) = delete;

    // reuse_surfaces keeps independent command allocators but aliases one
    // surface bundle. It requires sequential Submit/EndFrame calls: GPU waits
    // order each next D3D11 producer behind the preceding output consumer.
    // Producers must use GPU copies/dispatches, not Map/UpdateSubresource on
    // shared images: CPU write fast paths are not ordered by GPU fence waits.
    // Leave false for future FG packets that retain separate frame surfaces.
    bool Initialize(ID3D11Device *native_device, unsigned width, unsigned height, unsigned slots,
                    std::string &error, bool reuse_surfaces = false);
    BeginResult TryBegin(Frame *&frame, std::string &error);
    // Wait only for an already-ended consumer, never an open/unsubmitted
    // recording. Timeout returns Busy and preserves every GPU owner. This
    // prevents an overloaded caller from alternating processed/clean frames
    // or allocating additional image bundles merely to queue more work.
    BeginResult Begin(Frame *&frame, unsigned timeout_ms, std::string &error);
    // Publish converted D3D11 inputs, wait on DX12, execute the closed caller
    // list and signal completion_fence()/frame.completion_value. Inputs must no
    // longer be bound as writable D3D11 views when this is called.
    bool Submit(Frame &frame, std::string &error);
    // Queues a GPU wait on the authoritative D3D11 immediate context.
    bool WaitForOutput(Frame &frame, std::string &error);
    // Call after all D3D11 output reads/copies/composition have been recorded.
    // A previous WaitForOutput is optional for initialization-only frames.
    bool EndFrame(Frame &frame, std::string &error);
    // Discards a recording ONLY before a model Create/Evaluate was called.
    // Model calls may leave initialization work even when they report failure;
    // such lists must be submitted/retired instead of abandoned.
    bool Abandon(Frame &frame, std::string &error);

    // Transition/teardown only. timeout=0 polls. No owner is released after a
    // timeout; retry once GPU retirement is observable. Destructor retains an
    // unresolved generation until process exit rather than freeing GPU owners.
    bool Drain(unsigned timeout_ms, std::string &error);
    bool Shutdown(unsigned timeout_ms, std::string &error);

    ID3D11Device *device11() const;
    ID3D11DeviceContext4 *context11() const;
    ID3D12Device *device12() const;
    ID3D12CommandQueue *queue12() const;
    ID3D12Fence *completion_fence() const;
    unsigned width() const;
    unsigned height() const;
    unsigned slot_count() const;
    bool reuses_surfaces() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xivrender::interop
