#include "xivrender/nr_engine.hpp"
#include "xivrender/nr_feedback.hpp"
#include "xivrender/native_nr_api.hpp"
#include "xivrender/ngx_parameters.hpp"
#include "xivrender/d3d12_identity.hpp"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace xivrender::nr {
namespace {
using Microsoft::WRL::ComPtr;

bool same_device(ID3D12Device *expected, ID3D12DeviceChild *child) {
    return d3d12_identity::same_device(expected, child);
}

std::string result_text(const char *operation, std::uint32_t result) {
    std::ostringstream text;
    text << operation << " returned 0x" << std::hex << result;
    return text.str();
}

bool image(ID3D12Device *device, ID3D12Resource *resource, DXGI_FORMAT format, unsigned width,
           unsigned height, bool output) {
    if (!same_device(device, resource))
        return false;
    const auto desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == format &&
           desc.Width >= width && desc.Height >= height && desc.DepthOrArraySize == 1 &&
           desc.MipLevels == 1 && desc.SampleDesc.Count == 1 &&
           (output ? (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0
                   : (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0);
}
} // namespace

struct Engine::Impl {
    struct Retirement {
        ComPtr<ID3D12Fence> fence;
        std::uint64_t value = 0;
        std::array<ComPtr<ID3D12Resource>, 4> resources;
        ComPtr<ID3D12DescriptorHeap> descriptors;
    };
    enum class Record { None, Create, Evaluate };

    HMODULE bridge = nullptr;
    decltype(&XIVNR_Open) open = nullptr;
    decltype(&XIVNR_Create) create = nullptr;
    decltype(&XIVNR_Evaluate) evaluate = nullptr;
    decltype(&XIVNR_Release) release = nullptr;
    decltype(&XIVNR_Close) close = nullptr;
    ComPtr<ID3D12Device> device;
    std::array<std::unique_ptr<ngx::Parameters>, 8> parameters;
    std::unique_ptr<Feedback> feedback;
    ComPtr<ID3D12DescriptorHeap> pending_descriptors;
    Session *session = nullptr;
    std::array<void *, 8> features{};
    EngineConfig config;
    EngineStatus status;
    Record record = Record::None;
    std::array<ComPtr<ID3D12Resource>, 4> pending_resources;
    std::array<Retirement, 8> retirements;
    ComPtr<ID3D12Fence> creation_fence;
    std::uint64_t creation_value = 0;
    ComPtr<ID3D12Fence> last_submission_fence;
    std::uint64_t last_submission_value = 0;
    bool first_evaluation = true;

    ~Impl() {
        if (bridge)
            FreeLibrary(bridge);
    }

    bool reject(std::string &error, const std::string &message, std::uint32_t result = invalid_argument) {
        error = message;
        status.result = result;
        status.message = message;
        return false;
    }

    bool failed(std::string &error, const char *operation, std::uint32_t result) {
        ++status.failures;
        status.phase = EnginePhase::Failed;
        return reject(error, result_text(operation, result), result);
    }

    bool completed(ID3D12Fence *fence, std::uint64_t value) {
        const auto done = fence->GetCompletedValue();
        if (done == std::numeric_limits<std::uint64_t>::max()) {
            status.phase = EnginePhase::DeviceLost;
            status.message = "NR completion fence reports device removal; owners retained";
            status.result = static_cast<std::uint32_t>(DXGI_ERROR_DEVICE_REMOVED);
            return false;
        }
        return done >= value;
    }

    void collect() {
        if (status.phase == EnginePhase::DeviceLost)
            return;
        for (auto &entry : retirements)
            if (entry.fence && completed(entry.fence.Get(), entry.value))
                entry = {};
        if (status.phase == EnginePhase::DeviceLost)
            return;
        if (status.phase == EnginePhase::Creating && creation_fence &&
            completed(creation_fence.Get(), creation_value)) {
            creation_fence.Reset();
            creation_value = 0;
            status.phase = EnginePhase::Ready;
            status.message = "NR feature ready";
        }
    }

    Retirement *vacant() {
        collect();
        if (status.phase == EnginePhase::DeviceLost)
            return nullptr;
        for (auto &entry : retirements)
            if (!entry.fence)
                return &entry;
        return nullptr;
    }

    void configure_parameters(unsigned index) {
        auto &p = *parameters[index];
        const auto &tuning = config.profiles[index];
        for (const auto *key : {"Width", "Height", "OutWidth", "OutHeight", "DLSSNR.Width", "DLSSNR.Height"})
            p.Set(key, std::string_view(key).find("Height") != std::string_view::npos ? config.height
                                                                                      : config.width);
        p.Set("DLSSNR.Enabled", 1u);
        p.Set("CreationNodeMask", 1u);
        p.Set("VisibilityNodeMask", 1u);
        p.Set("DLSSNR.Hint.Render.Preset", tuning.preset);
        p.Set("DLSSNR.Style", tuning.style);
        p.Set("DLSSNR.Intensity", tuning.intensity);
        p.Set("DLSSNR.LocalStructureStrength", tuning.structure);
        p.Set("DLSSNR.LocalToneStrength", tuning.tone);
        p.Set("DLSSNR.SkinStructureStrength", tuning.skin);
        p.Set("DLSSNR.UseAutoMask", tuning.auto_mask ? 1u : 0u);
        p.Set("DLSSNR.UICorrection", 0u);
        p.Set("DLSSNR.ControlMask", static_cast<void *>(nullptr));
        for (const auto *prefix : {"Color", "Output", "Depth", "MVec"}) {
            const auto key = std::string("DLSSNR.") + prefix;
            p.Set(key.c_str(), static_cast<ID3D12Resource *>(nullptr));
            p.Set((key + "SubrectBaseX").c_str(), 0u);
            p.Set((key + "SubrectBaseY").c_str(), 0u);
            p.Set((key + "SubrectWidth").c_str(), config.width);
            p.Set((key + "SubrectHeight").c_str(), config.height);
        }
    }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::~Engine() {
    if (!impl_)
        return;
    std::string error;
    try {
        if (TryClose(error))
            return;
    } catch (...) {
    }
    // Destroying caller-owned resources while the GPU or model still owns them
    // can remove the device or unload executing code. Retain this exceptional
    // generation for process teardown; normal callers poll TryClose first.
    (void)impl_.release();
}

bool Engine::Open(ID3D12Device *device, const EngineConfig &config, std::string &error) {
    auto &s = *impl_;
    error.clear();
    if (s.session || s.bridge || s.status.phase != EnginePhase::Closed)
        return s.reject(error, "Close the current NR generation before opening another", busy);
    if (!device || !config.width || !config.height || config.width > 16384 || config.height > 16384 ||
        !config.passes || config.passes > 8 || !config.bridge.is_absolute() || !config.model.is_absolute() ||
        !config.data.is_absolute())
        return s.reject(error, "NR requires a device, bounded dimensions and absolute private paths");
    for (unsigned i = 0; i < config.passes; ++i) {
        const auto &p = config.profiles[i];
        if (p.preset > 3 || p.style > 2 || !std::isfinite(p.intensity) || p.intensity < 0 ||
            p.intensity > 2 || !std::isfinite(p.structure) || p.structure < 0 || p.structure > 2 ||
            !std::isfinite(p.tone) || p.tone < 0 || p.tone > 2 || !std::isfinite(p.skin) || p.skin < -1 ||
            p.skin > 2)
            return s.reject(error, "NR pass profile is invalid");
    }
    try {
        if (!std::filesystem::is_regular_file(config.bridge) ||
            !std::filesystem::is_regular_file(config.model))
            return s.reject(error, "The selected private NR bridge or model file is unavailable",
                            invalid_model);
        std::filesystem::create_directories(config.data);
        s.config = config;
        s.device = device;
        s.parameters[0] = std::make_unique<ngx::Parameters>();
        s.bridge = LoadLibraryExW(config.bridge.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!s.bridge) {
            s.device.Reset();
            s.parameters = {};
            return s.reject(error, "Could not load the private NR caller bridge", load_failed);
        }
        s.open = reinterpret_cast<decltype(s.open)>(GetProcAddress(s.bridge, "XIVNR_Open"));
        s.create = reinterpret_cast<decltype(s.create)>(GetProcAddress(s.bridge, "XIVNR_Create"));
        s.evaluate = reinterpret_cast<decltype(s.evaluate)>(GetProcAddress(s.bridge, "XIVNR_Evaluate"));
        s.release = reinterpret_cast<decltype(s.release)>(GetProcAddress(s.bridge, "XIVNR_Release"));
        s.close = reinterpret_cast<decltype(s.close)>(GetProcAddress(s.bridge, "XIVNR_Close"));
        if (!s.open || !s.create || !s.evaluate || !s.release || !s.close)
            return s.failed(error, "NR caller exports", missing_export);
        const auto result =
            s.open(config.model.c_str(), config.data.c_str(), device, 0, s.parameters[0].get(), &s.session);
        if (result != success || !s.session)
            return s.failed(error, "NR initialization", result == success ? internal_error : result);
        for (unsigned i = 0; i < config.passes; ++i) {
            if (i)
                s.parameters[i] = std::make_unique<ngx::Parameters>(*s.parameters[0]);
            s.configure_parameters(i);
        }
        if (config.passes > 1) {
            s.feedback = std::make_unique<Feedback>();
            if (!s.feedback->Initialize(device, config.width, config.height, error)) {
                s.status.phase = EnginePhase::Failed;
                ++s.status.failures;
                return false;
            }
        }
        s.status.phase = EnginePhase::Open;
        s.status.width = config.width;
        s.status.height = config.height;
        s.status.result = success;
        s.status.message = "NR runtime opened; feature creation pending";
        s.first_evaluation = true;
        return true;
    } catch (...) {
        return s.failed(error, "NR initialization exception", internal_error);
    }
}

bool Engine::ValidateCompletionFence(ID3D12Fence *fence, std::string &error) {
    error.clear();
    if (!fence)
        return impl_->reject(error, "NR completion fence is missing");
    if (!same_device(impl_->device.Get(), fence))
        return impl_->reject(error, "NR completion fence belongs to a different or unrecognized device");
    return true;
}

bool Engine::RecordCreate(ID3D12GraphicsCommandList *commands, std::string &error) {
    auto &s = *impl_;
    error.clear();
    if (s.status.phase != EnginePhase::Open || s.record != Impl::Record::None || !s.session)
        return s.reject(error, "NR creation is not ready or an earlier record is awaiting submission", busy);
    if (!same_device(s.device.Get(), commands) || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return s.reject(error, "NR creation requires a direct command list on its configured device");
    if (!s.vacant())
        return s.reject(error, "NR retirement capacity is busy", busy);
    s.record = Impl::Record::Create;
    try {
        for (unsigned i = 0; i < s.config.passes; ++i) {
            const auto result = s.create(s.session, commands, s.parameters[i].get(), &s.features[i]);
            if (result != success || !s.features[i])
                return s.failed(error, "NR feature creation", result == success ? internal_error : result);
            ++s.status.creates;
        }
        s.status.phase = EnginePhase::CreationRecorded;
        s.status.result = success;
        s.status.message = "NR creation recorded; GPU completion pending";
        return true;
    } catch (...) {
        return s.failed(error, "NR feature creation exception", internal_error);
    }
}

bool Engine::Ready() {
    impl_->collect();
    return impl_->status.phase == EnginePhase::Ready;
}

bool Engine::RecordEvaluate(ID3D12GraphicsCommandList *commands, const Evaluation &frame,
                            std::string &error) {
    auto &s = *impl_;
    error.clear();
    if (!Ready() || s.record != Impl::Record::None)
        return s.reject(error, "NR feature is not ready or an earlier record is awaiting submission", busy);
    if (!same_device(s.device.Get(), commands) || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return s.reject(error, "NR evaluation requires a direct command list on its configured device");
    if (!s.vacant())
        return s.reject(error, "NR frame retirement capacity is busy", busy);
    if (frame.color == frame.output ||
        !image(s.device.Get(), frame.color, DXGI_FORMAT_R16G16B16A16_FLOAT, s.config.width, s.config.height,
               false) ||
        !image(s.device.Get(), frame.depth, DXGI_FORMAT_R32_FLOAT, s.config.width, s.config.height, false) ||
        !image(s.device.Get(), frame.motion, DXGI_FORMAT_R16G16_FLOAT, s.config.width, s.config.height,
               false) ||
        !image(s.device.Get(), frame.output, DXGI_FORMAT_R16G16B16A16_FLOAT, s.config.width, s.config.height,
               true) ||
        !std::isfinite(frame.feedback) || frame.feedback < 0 || frame.feedback > 1 ||
        !std::isfinite(frame.motion_scale_x) || !std::isfinite(frame.motion_scale_y) ||
        std::abs(frame.motion_scale_x) > 65536.0f || std::abs(frame.motion_scale_y) > 65536.0f)
        return s.reject(error, "NR color/depth/motion/output resources or motion scales are incompatible");
    try {
        if (s.feedback &&
            !s.feedback->Begin(frame.color, frame.output, s.config.passes, s.pending_descriptors, error))
            return false;
        s.pending_resources = {frame.color, frame.depth, frame.motion, frame.output};
        for (unsigned i = 0; i < s.config.passes; ++i) {
            ID3D12Resource *color = frame.color;
            if (i) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition = {frame.output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
                commands->ResourceBarrier(1, &barrier);
                color = s.feedback->Mix(commands, s.pending_descriptors.Get(), i, frame.feedback);
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                commands->ResourceBarrier(1, &barrier);
                // Each native pass must actually write its result. A successful
                // no-op may not masquerade as the previous pass's output.
                s.feedback->ClearOutput(commands, s.pending_descriptors.Get(), i, frame.output);
            }
            auto &p = *s.parameters[i];
            p.Set("DLSSNR.Color", color);
            p.Set("DLSSNR.Depth", frame.depth);
            p.Set("DLSSNR.MVec", frame.motion);
            p.Set("DLSSNR.Output", frame.output);
            p.Set("DLSSNR.DepthInverted", frame.depth_inverted ? 1u : 0u);
            p.Set("DLSSNR.Reset", frame.reset || s.first_evaluation ? 1u : 0u);
            p.Set("DLSSNR.MVecScaleX", frame.motion_scale_x);
            p.Set("DLSSNR.MVecScaleY", frame.motion_scale_y);
            // A failed native call may still record work. Retain all owners
            // and descriptors until the exact submitted GPU fence completes.
            s.record = Impl::Record::Evaluate;
            const auto result = s.evaluate(s.session, commands, s.features[i], &p);
            if (result != success) {
                if (s.feedback)
                    s.feedback->Finish(commands);
                return s.failed(error, "NR evaluation", result);
            }
        }
        if (s.feedback)
            s.feedback->Finish(commands);
        s.first_evaluation = false;
        s.status.result = success;
        s.status.message = "NR evaluation recorded";
        ++s.status.evaluations;
        return true;
    } catch (...) {
        return s.failed(error, "NR evaluation exception", internal_error);
    }
}

bool Engine::HasPendingSubmission() const {
    return impl_->record != Impl::Record::None;
}

bool Engine::MarkSubmitted(ID3D12Fence *fence, std::uint64_t value, std::string &error) {
    auto &s = *impl_;
    error.clear();
    if (s.record == Impl::Record::None)
        return true;
    if (!value || value == std::numeric_limits<std::uint64_t>::max())
        return s.reject(error, "NR submission requires a finite nonzero completion token");
    if (!ValidateCompletionFence(fence, error))
        return false;
    if (s.last_submission_fence.Get() == fence && value <= s.last_submission_value)
        return s.reject(error, "NR completion tokens must advance for each recorded submission");
    auto *entry = s.vacant();
    if (!entry)
        return s.reject(error, "NR submission retirement capacity is exhausted; retain this generation",
                        busy);
    entry->fence = fence;
    entry->value = value;
    entry->resources = std::move(s.pending_resources);
    entry->descriptors = std::move(s.pending_descriptors);
    if (s.record == Impl::Record::Create && s.status.phase == EnginePhase::CreationRecorded) {
        s.creation_fence = fence;
        s.creation_value = value;
        s.status.phase = EnginePhase::Creating;
    }
    s.record = Impl::Record::None;
    s.last_submission_fence = fence;
    s.last_submission_value = value;
    return true;
}

bool Engine::TryClose(std::string &error) {
    auto &s = *impl_;
    error.clear();
    s.collect();
    if (s.record != Impl::Record::None)
        return s.reject(error, "NR command recording still needs submission", busy);
    if (s.status.phase == EnginePhase::DeviceLost)
        return s.reject(error, "NR device was removed; model generation retained", busy);
    for (const auto &entry : s.retirements)
        if (entry.fence)
            return s.reject(error, "NR GPU work has not retired", busy);
    try {
        for (auto &feature : s.features) {
            if (!feature)
                continue;
            const auto result = s.release(s.session, feature);
            if (result != success)
                return s.failed(error, "NR feature release", result);
            feature = nullptr;
        }
        if (s.session) {
            const auto result = s.close(s.session);
            if (result != success)
                return s.failed(error, "NR runtime close", result);
            s.session = nullptr;
        }
    } catch (...) {
        return s.failed(error, "NR retirement exception", internal_error);
    }
    s.creation_fence.Reset();
    s.creation_value = 0;
    s.last_submission_fence.Reset();
    s.last_submission_value = 0;
    s.parameters = {};
    s.feedback.reset();
    s.pending_descriptors.Reset();
    s.device.Reset();
    if (s.bridge) {
        FreeLibrary(s.bridge);
        s.bridge = nullptr;
    }
    s.open = nullptr;
    s.create = nullptr;
    s.evaluate = nullptr;
    s.release = nullptr;
    s.close = nullptr;
    s.status.phase = EnginePhase::Closed;
    s.status.result = success;
    s.status.message = "NR runtime closed";
    return true;
}

EngineStatus Engine::Status() const {
    return impl_->status;
}

} // namespace xivrender::nr
