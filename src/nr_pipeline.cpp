#include "xivrender/nr_pipeline.hpp"
#include "xivrender/nr_engine.hpp"
#include "xivrender/dx11_dx12_bridge.hpp"
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace xivrender::nr {
namespace {
using Microsoft::WRL::ComPtr;
struct Key {
    std::string runtime;
    unsigned width = 0, height = 0;
    ID3D11Device *device = nullptr;
    unsigned passes = 1;
    std::array<NrPassProfile, 8> profiles{};
    float center = 0, work = 1;
    bool linear_hdr = false, hdr_input = false;
    bool operator==(const Key &) const = default;
};
unsigned model_extent(unsigned output, float scale) {
    const double n = std::round(double(output) * scale);
    return unsigned(std::clamp(n, 64.0, 16384.0));
}
bool runtime_id(const std::string &id) {
    return !id.empty() && id.size() <= 64 && id != "." && id != ".." &&
           std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      c == '-' || c == '_' || c == '.';
           });
}
void transition(ID3D12GraphicsCommandList *commands, interop::Frame &frame, bool to_model) {
    const std::array<ID3D12Resource *, 4> images{frame.colour_in.texture12.Get(), frame.depth.texture12.Get(),
                                                 frame.motion.texture12.Get(),
                                                 frame.colour_out.texture12.Get()};
    std::array<D3D12_RESOURCE_BARRIER, 4> barriers{};
    for (unsigned i = 0; i < images.size(); ++i) {
        auto &b = barriers[i];
        const auto model_state =
            i == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {images[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        to_model ? D3D12_RESOURCE_STATE_COMMON : model_state,
                        to_model ? model_state : D3D12_RESOURCE_STATE_COMMON};
    }
    if (!to_model) {
        D3D12_RESOURCE_BARRIER output{};
        output.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        output.UAV.pResource = frame.colour_out.texture12.Get();
        commands->ResourceBarrier(1, &output);
    }
    commands->ResourceBarrier(unsigned(barriers.size()), barriers.data());
}
bool available_memory(ID3D11Device *device, std::uint64_t &available) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter3> budget;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter.As(&budget)))
        return false;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(budget->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) || !info.Budget)
        return false;
    available = info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0;
    return true;
}
bool memory_headroom(ID3D11Device *device, const Key &key, const SourceFrame &f, std::string &error) {
    std::uint64_t available = 0;
    if (!available_memory(device, available))
        return true;
    // Exact transfer/composite storage plus a conservative model/headroom
    // estimate. This is admission control, not a claim of model memory size.
    constexpr std::uint64_t mib = 1024ull * 1024;
    const auto pixels = std::uint64_t(key.width) * key.height;
    const auto storage =
        pixels * (key.passes > 1 ? 32 : 24) + std::uint64_t(f.output_width) * f.output_height * 8;
    const auto estimated_need = storage + pixels * 128 * key.passes + 768 * mib;
    if (available < estimated_need) {
        error = "Insufficient VRAM headroom for the requested NR resolution; reduce NR model resolution";
        return false;
    }
    return true;
}
} // namespace
struct Pipeline::Impl {
    ComPtr<ID3D11Device> attached;
    std::filesystem::path root;
    interop::Bridge bridge;
    Engine engine;
    Images images;
    PipelineStatus status;
    Key running, pending;
    ComPtr<ID3D12Fence> pending_completion;
    std::uint64_t pending_completion_value = 0;
    ULONGLONG pending_since = 0, budget_checked = 0;
    bool was_enabled = false, was_paused = false, reset_next = true, failed = false, recreate = false;
    const void *previous_sr = nullptr;
    unsigned previous_guide_width = 0, previous_guide_height = 0;
    std::uint64_t previous_frame = 0;
    bool frame_seen = false;
    float previous_feedback = 1;

    bool fail(const std::string &message) {
        failed = true;
        reset_next = true;
        status.active = false;
        status.failed = true;
        status.message = message;
        ++status.failures;
        return false;
    }
    bool retire(std::string &error) {
        // Submit can succeed before engine bookkeeping rejects the token.
        // Preserve and retry that exact GPU signal; never discard the native
        // recording or replace completion with a CPU-signalled fence.
        if (pending_completion) {
            if (engine.HasPendingSubmission() &&
                !engine.MarkSubmitted(pending_completion.Get(), pending_completion_value, error))
                return false;
            pending_completion.Reset();
            pending_completion_value = 0;
        }
        if (!engine.TryClose(error))
            return false;
        if (!bridge.Shutdown(0, error))
            return false;
        images.Reset();
        running = {};
        status.active_runtime.clear();
        status.width = status.height = 0;
        reset_next = true;
        previous_sr = nullptr;
        frame_seen = false;
        return true;
    }
    bool publish(interop::Frame &frame, std::string &error) {
        if (!bridge.Submit(frame, error))
            return fail(error);
        if (engine.HasPendingSubmission()) {
            pending_completion = bridge.completion_fence();
            pending_completion_value = frame.completion_value;
            if (!engine.MarkSubmitted(pending_completion.Get(), pending_completion_value, error)) {
                std::string retired;
                bridge.EndFrame(frame, retired);
                return fail(error);
            }
            pending_completion.Reset();
            pending_completion_value = 0;
        }
        return true;
    }
};
Pipeline::Pipeline() : impl_(std::make_unique<Impl>()) {}
Pipeline::~Pipeline() {
    try {
        std::string error;
        if (Shutdown(0, error))
            return;
    } catch (...) {
    }
    (void)impl_.release();
}
void Pipeline::Attach(ID3D11Device *device, const std::filesystem::path &root) {
    impl_->attached = device;
    impl_->root = root;
}
void Pipeline::Retry() {
    auto &s = *impl_;
    s.failed = false;
    s.status.failed = false;
    s.reset_next = true;
    s.recreate = true;
    // A failed model generation must retire before a fresh Open.
    s.pending = {};
}
bool Pipeline::NeedsService() const {
    return impl_->engine.Status().phase != EnginePhase::Closed || impl_->bridge.device12();
}
void Pipeline::Fault(const std::string &error) {
    impl_->fail(error);
}
void Pipeline::Service(bool enabled) {
    auto &s = *impl_;
    if (enabled && !s.failed)
        return;
    const auto failure = s.status.message;
    std::string error;
    const bool done = !NeedsService() || s.retire(error);
    s.status.active = false;
    s.reset_next = true;
    if (enabled)
        s.status.message = failure;
    else {
        s.was_enabled = false;
        s.failed = false;
        s.status.failed = false;
        s.status.paused = false;
        s.status.message = done ? "NR is disabled" : "NR is disabled; retiring GPU work";
    }
}
bool Pipeline::Process(const FrameInput &input, const NeuralRenderingSettings &cfg, bool focused) {
    auto &s = *impl_;
    auto &report = s.status;
    report.active = false;
    report.paused = false;
    report.requested_runtime = cfg.runtime_id;
    std::string error;
    if (!cfg.enabled) {
        Service(false);
        return false;
    }
    if (!s.was_enabled) {
        s.was_enabled = true;
        s.failed = false;
        report.failed = false;
        s.reset_next = true;
    }
    if (cfg.pause_on_focus_loss && !focused) {
        s.was_paused = true;
        s.reset_next = true;
        report.paused = true;
        report.message = "NR paused while the game is unfocused";
        return false;
    }
    if (s.was_paused) {
        s.was_paused = false;
        s.reset_next = true;
    }
    auto f = input.images;
    f.linear_hdr = input.hdr;
    if (!s.attached || !s.root.is_absolute()) {
        report.message = "NR is waiting for the native game device";
        return false;
    }
    if (input.motion_jittered) {
        s.reset_next = true;
        ++report.skipped;
        report.message = "NR is waiting for supported native motion guides";
        return false;
    }
    if (!runtime_id(cfg.runtime_id) || !std::isfinite(cfg.model_scale) || !f.output_width ||
        !f.output_height || f.output_width > 16384 || f.output_height > 16384 || !f.guide_width ||
        !f.guide_height || !std::isfinite(f.jitter_x) || !std::isfinite(f.jitter_y) ||
        std::abs(f.jitter_x) > 2 || std::abs(f.jitter_y) > 2)
        return s.fail("NR received invalid native frame metadata");
    if (cfg.foveated && (!std::isfinite(cfg.central_fraction) || !std::isfinite(cfg.work_fraction) ||
                         cfg.central_fraction < .1f || cfg.central_fraction > .95f ||
                         cfg.work_fraction <= cfg.central_fraction || cfg.work_fraction > 1))
        return s.fail("Invalid NR peripheral compression grid");
    const auto scale = std::clamp(cfg.model_scale, .25f, 2.f);
    Key target{cfg.runtime_id, model_extent(f.output_width, scale * (cfg.foveated ? cfg.work_fraction : 1)),
               model_extent(f.output_height, scale * (cfg.foveated ? cfg.work_fraction : 1)),
               s.attached.Get()};
    target.linear_hdr = input.hdr;
    target.hdr_input = cfg.hdr_input;
    target.center = cfg.foveated ? cfg.central_fraction : 0;
    target.work = cfg.foveated ? cfg.work_fraction : 1;
    target.passes = std::clamp(cfg.passes, 1u, 8u);
    for (unsigned i = 0; i < target.passes; ++i)
        target.profiles[i] = nr_pass_profile(cfg, i);
    if (!(target == s.pending)) {
        s.pending = target;
        s.pending_since = GetTickCount64();
        s.failed = false;
        report.failed = false;
    }
    if (s.failed) {
        Service(true);
        return false;
    }
    const auto engine_state = s.engine.Status();
    if (s.recreate || !(target == s.running) || engine_state.phase == EnginePhase::Closed ||
        engine_state.phase == EnginePhase::Failed || engine_state.phase == EnginePhase::DeviceLost) {
        s.reset_next = true;
        if (s.running.device && GetTickCount64() - s.pending_since < 200) {
            report.message = "Applying NR runtime/resolution change";
            return false;
        }
        if (!s.retire(error)) {
            report.message = "NR change pending GPU retirement: " + error;
            return false;
        }
        if (!memory_headroom(s.attached.Get(), target, f, error))
            return s.fail(error);
        if (!s.bridge.Initialize(s.attached.Get(), target.width, target.height, 3, error, true))
            return s.fail(error);
        const auto runtime = s.root / L"XIVRender/runtime";
        EngineConfig options{runtime / L"XIVRender_nvngx.dll_nr.dll",
                             runtime / L"nr" / std::filesystem::path(cfg.runtime_id) / L"nvngx_dlssnr.dll",
                             s.root / L"XIVRender/cache/nr", target.width, target.height};
        options.passes = target.passes;
        options.profiles = target.profiles;
        if (!s.engine.Open(s.bridge.device12(), options, error))
            return s.fail(error);
        if (!s.engine.ValidateCompletionFence(s.bridge.completion_fence(), error))
            return s.fail(error);
        s.recreate = false;
        s.running = target;
        s.budget_checked = 0;
        report.active_runtime = target.runtime;
        report.width = target.width;
        report.height = target.height;
        report.precision = s.engine.Status().precision;
    }
    if (s.frame_seen && input.frame_id <= s.previous_frame) {
        ++report.skipped;
        report.message = "NR ignored a repeated real-frame identifier";
        return false;
    }
    interop::Frame *frame = nullptr;
    auto begin = s.bridge.TryBegin(frame, error);
    if (begin == interop::BeginResult::Busy) {
        ++report.paced;
        begin = s.bridge.Begin(frame, 250, error);
        if (begin == interop::BeginResult::Busy)
            return s.fail(
                "NR stopped after command retirement timed out; reduce model resolution or retry: " + error);
    }
    if (begin != interop::BeginResult::Ready || !frame)
        return s.fail(error);
    if (s.engine.Status().phase == EnginePhase::Open) {
        const bool created = s.engine.RecordCreate(frame->commands, error);
        const auto model_error = error;
        if (!s.engine.HasPendingSubmission()) {
            std::string ignored;
            s.bridge.Abandon(*frame, ignored);
            return s.fail(model_error);
        }
        if (!s.publish(*frame, error))
            return false;
        if (!s.bridge.EndFrame(*frame, error))
            return s.fail(error);
        if (!created)
            return s.fail(model_error);
        ++report.creations;
        report.message = "NR model initialization is completing on the GPU";
        return false;
    }
    if (!s.engine.Ready()) {
        std::string ignored;
        s.bridge.Abandon(*frame, ignored);
        report.message = "NR model initialization is completing on the GPU";
        return false;
    }
    if (!s.budget_checked || GetTickCount64() - s.budget_checked >= 1000) {
        s.budget_checked = GetTickCount64();
        std::uint64_t available = 0;
        if (available_memory(s.attached.Get(), available) && available < 256ull * 1024 * 1024) {
            std::string ignored;
            s.bridge.Abandon(*frame, ignored);
            s.fail("NR stopped to preserve VRAM headroom; reduce NR model resolution or retry when memory is "
                   "available");
            Service(true);
            return false;
        }
    }
    if (!s.images.Prepare(s.attached.Get(), f, target.width, target.height, frame->colour_in.uav11.Get(),
                          frame->depth.uav11.Get(), frame->motion.uav11.Get(), error, &cfg)) {
        std::string ignored;
        s.bridge.Abandon(*frame, ignored);
        return s.fail(error);
    }
    // Detect a successful no-op or partial model write per pixel. This clear
    // is GPU ordered with the producer fence; never CPU-map shared surfaces.
    const float unwritten[4]{std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::quiet_NaN(), 0};
    s.bridge.context11()->ClearUnorderedAccessViewFloat(frame->colour_out.uav11.Get(), unwritten);
    const bool reset = input.reset || s.reset_next || cfg.feedback != s.previous_feedback ||
                       input.sr_feature != s.previous_sr || f.guide_width != s.previous_guide_width ||
                       f.guide_height != s.previous_guide_height;
    transition(frame->commands, *frame, true);
    Evaluation evaluation{frame->colour_in.texture12.Get(),
                          frame->depth.texture12.Get(),
                          frame->motion.texture12.Get(),
                          frame->colour_out.texture12.Get(),
                          reset,
                          f.depth_inverted,
                          float(target.width),
                          float(target.height),
                          cfg.feedback};
    const bool evaluated = s.engine.RecordEvaluate(frame->commands, evaluation, error);
    const auto model_error = error;
    transition(frame->commands, *frame, false);
    if (!s.engine.HasPendingSubmission()) {
        // Preparation has already queued GPU writes on D3D11. Even a native
        // argument rejection must publish/retire those writes before teardown.
        if (!s.publish(*frame, error))
            return false;
        if (!s.bridge.EndFrame(*frame, error))
            return s.fail(error);
        return s.fail(model_error);
    }
    if (!s.publish(*frame, error))
        return false;
    if (!evaluated) {
        std::string ignored;
        s.bridge.EndFrame(*frame, ignored);
        return s.fail(model_error);
    }
    ++report.evaluations;
    if (!s.bridge.WaitForOutput(*frame, error))
        return s.fail(error);
    const bool applied =
        s.images.Composite(s.attached.Get(), f, target.width, target.height, frame->colour_in.srv11.Get(),
                           frame->colour_out.srv11.Get(), error, &cfg);
    const auto composition_error = error;
    if (!s.bridge.EndFrame(*frame, error))
        return s.fail(error);
    if (!applied)
        return s.fail(composition_error);
    s.previous_feedback = cfg.feedback;
    s.reset_next = false;
    s.previous_sr = input.sr_feature;
    s.previous_guide_width = f.guide_width;
    s.previous_guide_height = f.guide_height;
    s.previous_frame = input.frame_id;
    s.frame_seen = true;
    report.active = true;
    report.failed = false;
    ++report.applied;
    report.message = "NR active: " + std::to_string(target.passes) +
                     (target.passes == 1 ? " pass" : " passes") + " after DLSS";
    return true;
}
bool Pipeline::Shutdown(unsigned timeout_ms, std::string &error) {
    if (!impl_)
        return true;
    const auto deadline = GetTickCount64() + timeout_ms;
    do {
        if (impl_->retire(error)) {
            impl_->attached.Reset();
            return true;
        }
        if (!timeout_ms || GetTickCount64() >= deadline)
            return false;
        Sleep(1);
    } while (true);
}
PipelineStatus Pipeline::Status() const {
    return impl_->status;
}
} // namespace xivrender::nr
