#pragma once
#include "frame_generation.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <functional>
namespace xivrender::fg {
struct Packet {
    ID3D12Resource *depth = nullptr, *motion = nullptr, *hudless = nullptr;
    ID3D12Resource *backbuffer = nullptr;
    unsigned width = 0, height = 0, render_width = 0, render_height = 0;
    float jitter_x = 0, jitter_y = 0, milliseconds = 16.67f;
    bool reset = true, valid = false, hdr = false, dynamic = false;
    // Native FFXIV DRS is independent of a dynamic interpolation multiplier.
    bool variable_resolution = false;
    const void *source_feature = nullptr;
    unsigned target_fps = 0;
    std::uint64_t frame = 0;
    Camera camera;
};
class Backend {
    Microsoft::WRL::ComPtr<ID3D12Device> owner_device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> owner_queue_;

  protected:
    void release_presenting_queue() {
        owner_queue_.Reset();
    }

  public:
    void retain_device(ID3D12Device *d, ID3D12CommandQueue *q) {
        owner_device_ = d;
        owner_queue_ = q;
    }
    virtual bool latency_available() const {
        return true;
    }
    virtual void use_native_latency(bool) {}
    virtual bool native_dynamic_supported() const {
        return false;
    }
    virtual ~Backend() = default;
    // Bind an SDK instance to this generation's device, without an output.
    virtual bool prepare_device(ID3D12Device *, IDXGIFactory *, std::string &) {
        return true;
    }
    // Latency reduction does not require an interpolating output/swapchain.
    // Ordinary output can own a controller without an interpolation context.
    virtual bool start_latency(ID3D12Device *, IDXGIFactory *, std::string &) {
        return false;
    }
    // Disable interpolation hooks before creating a latency-only output.
    virtual bool set_interpolation_loaded(bool, std::string &) {
        return true;
    }
    virtual bool create(ID3D12Device *, IDXGIFactory *, HWND, const DXGI_SWAP_CHAIN_DESC &, DXGI_FORMAT,
                        Microsoft::WRL::ComPtr<ID3D12CommandQueue> &,
                        Microsoft::WRL::ComPtr<IDXGISwapChain4> &, std::string &) = 0;
    virtual bool prepare(ID3D12GraphicsCommandList *, const Packet &, unsigned factor, bool enabled,
                         std::string &) = 0;
    virtual void marker(unsigned marker, std::uint64_t frame) = 0;
    virtual void sleep(std::uint64_t frame) = 0;
    virtual bool after_present(ID3D12CommandQueue *, std::string &) = 0;
    virtual HRESULT present(IDXGISwapChain4 *chain, unsigned interval, unsigned flags) {
        return chain->Present(interval, flags);
    }
    virtual HRESULT backbuffer(IDXGISwapChain4 *chain, Microsoft::WRL::ComPtr<ID3D12Resource> &image) {
        return chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&image));
    }
    virtual bool drain_output(unsigned, std::string &) { return true; }
    virtual bool asynchronous_presentation() const { return false; }
    virtual void set_present_markers(std::function<void(unsigned, std::uint64_t)>) {}
    virtual void stop() = 0;
    // Caller first retires GPU input use and all latency calls. This releases
    // a provider's output resources before SDK shutdown.
    virtual void release_output() {}
    // Called only after GPU and latency calls have retired and all swapchain
    // references have been dropped. Failures block the next provider.
    virtual bool close(std::string &) { return true; }
    virtual unsigned maximum() const = 0;
    virtual std::uint64_t displayed() const = 0;
    virtual unsigned last_presented() const = 0;
    virtual bool resize(unsigned, unsigned, std::string &) {
        return true;
    }
    virtual std::string latency_name() const = 0;
};
std::shared_ptr<Backend> make_backend(FrameGeneration, const std::filesystem::path &, bool unlock,
                                      std::string &);
std::shared_ptr<Backend> make_plain_backend(std::shared_ptr<Backend> latency = {});
std::shared_ptr<Backend> make_enabler_backend(const std::filesystem::path &, std::string &);
std::shared_ptr<Backend> with_latency(std::shared_ptr<Backend> output, std::shared_ptr<Backend> latency);
bool unlock_xefg(HMODULE, std::string &);
bool restore_xefg(HMODULE, std::string &);
double xefg_render_time_ms();
void xefg_note_fed_frame_time(double milliseconds);
void xefg_request_pacing_recovery();
} // namespace xivrender::fg
