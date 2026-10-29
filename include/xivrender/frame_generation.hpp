#pragma once
#include "nr_pipeline.hpp"
#include "fg_telemetry.hpp"
#include <dxgi1_6.h>
#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <functional>
#include <optional>
#include <tuple>
#include <wrl/client.h>
struct ID3D12Device;

namespace xivrender::fg {
class Backend;
// Standalone probes only, before creating any presentation device.
void enable_probe_debug();
bool probe_debug_enabled();
struct Camera {
    std::array<float, 16> view{}, projection{};
    float near_plane = 0, far_plane = 0, fov = 0, aspect = 0;
    bool inverted = true, infinite = false, valid = false;
};
struct Status {
    bool available = false, active = false, latency = false, native_dynamic = false;
    unsigned factor = 1, maximum = 1;
    bool rate_measured = false;
    double observed_ratio = 0, real_fps = 0, displayed_fps = 0;
    FrameGeneration provider = FrameGeneration::Off;
    std::uint64_t real_frames = 0, displayed = 0, skipped = 0, failures = 0;
    std::uint64_t provider_changes = 0;
    std::uint64_t guide_allocations = 0;
    std::uint64_t simulation_frames = 0, camera_samples = 0, valid_cameras = 0, captured_frames = 0;
    std::uint64_t missing_cameras = 0, invalid_cameras = 0, camera_overflows = 0, stale_inputs = 0,
                  overwritten_inputs = 0, history_resets = 0;
    unsigned input_age = 0;
    std::string input_status = "Waiting for native scene input";
    std::string message = "Frame generation is disabled", latency_name;
    std::string transition;
    std::string last_error;
    std::string compatibility;
};
void configure(const Settings &, const std::filesystem::path &root = {}, bool startup = false);
bool requested();
bool output_requested();
FrameGenerationSettings options();
Status status();
void camera(const Camera &);
// Simulation and render submission run on different native threads. Markers
// carry the same real-frame id through to the presenting thread.
void simulation_begin();
void simulation_end();
void render_begin();
void capture(const nr::FrameInput &);
void before_effects(ID3D11Texture2D *);
void report(const std::string &);
void report_failure(const std::string &) noexcept;
// Native SR release must retire its dependent FG work before the game can
// destroy NGX state. Acquire before the D3D11 context lease.
std::unique_lock<std::recursive_mutex> lock_native_source();
bool retire_native_source(const void *, std::string &);

class Presenter {
  public:
    Presenter();
    ~Presenter();
    // Output acquires this before its own D3D11 transaction/mutex. Recursive
    // ownership lets its public calls retain the same output lifetime lease.
    std::unique_lock<std::recursive_mutex> lock_presentation() {
        return std::unique_lock<std::recursive_mutex>(presentation_mutex_);
    }
    bool initialize(ID3D11Device *, IDXGIFactory *, HWND, const DXGI_SWAP_CHAIN_DESC &, DXGI_FORMAT,
                    std::string &);
    IDXGISwapChain4 *chain() const;
    ID3D12Device *device() const {
        return device12_.Get();
    }
    std::uint64_t generation() const {
        return generation_;
    }
    ID3D11RenderTargetView *target() const;
    // Apply a requested provider/latency change at the presentation boundary.
    // ReShade's original D3D11 carrier and its buffers remain owned by Output.
    bool synchronize(std::string &error);
    bool prepare_colour(ID3D11Texture2D *sdr, std::string &error);
    // Called once after ReShade has finished. The target is full presentation
    // color in the selected SDR/PQ/scRGB output domain.
    // The callback seals the caller's current frame and releases its outer
    // D3D11 guard before paced SDK presentation. It must not throw.
    HRESULT present(UINT interval, UINT flags, bool focused,
                    const std::function<void()> &before_sdk_present = {});
    bool resize(unsigned width, unsigned height, std::string &);
    bool drain(unsigned timeout_ms, std::string &);

  private:
    struct Impl;
    // Protect SDK/output lifetime across a paced Present while native D3D11
    // capture is allowed to prepare the next frame independently.
    std::recursive_mutex &presentation_mutex_;
    Microsoft::WRL::ComPtr<ID3D12Device> device12_;
    std::shared_ptr<Backend> streamline_;
    bool streamline_attempted_ = false;
    std::string streamline_error_;
    std::unique_ptr<Impl> impl_;
    std::optional<std::tuple<FrameGeneration, bool, FrameGeneration, DXGI_FORMAT, bool>> blocked_;
    std::uint64_t generation_ = 0;
    bool initialize_locked(ID3D11Device *, IDXGIFactory *, HWND, const DXGI_SWAP_CHAIN_DESC &, DXGI_FORMAT,
                           const Settings &, std::string &);
};
} // namespace xivrender::fg
