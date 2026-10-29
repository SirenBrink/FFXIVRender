#pragma once
#include "settings.hpp"
#include <dxgiformat.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>
namespace xivrender::fg {
inline unsigned presentation_flags(FrameGeneration provider, unsigned interval, unsigned flags,
                                   bool windowed_tearing_supported) {
    flags &= ~DXGI_PRESENT_DO_NOT_WAIT;
    if (interval) return flags & ~DXGI_PRESENT_ALLOW_TEARING;
    if (provider == FrameGeneration::XeFG && windowed_tearing_supported)
        flags |= DXGI_PRESENT_ALLOW_TEARING;
    return flags;
}
// Latency reduction also benefits ordinary real frames. The legacy INI key
// `reflex` enables the separately selected controller with interpolation off.
// DLSS-G/XeFG retain their required native latency integration.
inline FrameGeneration latency_provider(const FrameGenerationSettings &settings) {
    // These SDKs require their own latency integration. Do not stack a
    // second controller and two independent CPU sleeps on the same frame.
    if (settings.enabled && (settings.backend == FrameGeneration::DlssG ||
                             settings.backend == FrameGeneration::XeFG))
        return settings.backend;
    if (((!settings.enabled || settings.backend == FrameGeneration::Off) && !settings.reflex) ||
        settings.latency == LatencyReduction::Off)
        return FrameGeneration::Off;
    switch (settings.latency) {
    case LatencyReduction::XeLL: return FrameGeneration::XeFG;
    case LatencyReduction::AntiLag2: return FrameGeneration::FsrFG;
    default: return FrameGeneration::DlssG;
    }
}
inline bool presentation_requested(const FrameGenerationSettings &settings) {
    return (settings.enabled && settings.backend != FrameGeneration::Off) ||
           latency_provider(settings) != FrameGeneration::Off;
}
inline unsigned maximum_factor(FrameGeneration backend, unsigned runtime_max, bool unlock, bool fsr_mfg = false) {
    if (backend == FrameGeneration::Off)
        return 1;
    if (backend == FrameGeneration::FsrFG)
        return std::min(fsr_mfg ? 4u : 2u, runtime_max);
    if (backend == FrameGeneration::XeFG)
        return std::min(unlock ? 4u : runtime_max, std::max(2u, runtime_max));
    return std::min(6u, runtime_max);
}
inline bool supported_format(FrameGeneration backend, DXGI_FORMAT format, bool hdr) {
    if (!hdr)
        return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM;
    return format == DXGI_FORMAT_R10G10B10A2_UNORM ||
           ((backend == FrameGeneration::FsrFG || backend == FrameGeneration::Off) &&
            format == DXGI_FORMAT_R16G16B16A16_FLOAT);
}
inline DXGI_FORMAT requested_output_format(const HdrSettings &settings) {
    if (!settings.enabled)
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    return settings.output == HdrOutput::ScRgb16Float ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                     : DXGI_FORMAT_R10G10B10A2_UNORM;
}
inline HdrSettings committed_output_settings(HdrSettings requested, DXGI_FORMAT format) {
    // A failed handoff retains the old output. Tone mapping must follow the
    // committed swapchain, even while the user request remains pending.
    requested.enabled = format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                        format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    requested.output = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? HdrOutput::ScRgb16Float
                       : requested.enabled ? HdrOutput::Pq10 : HdrOutput::Sdr8;
    return requested;
}
// Manual dMFG fallback uses base-frame time, never the generated Present rate.
// Changes require a sustained decision and never mutate an in-flight packet.
class DynamicFactor {
    double average_ = 0;
    std::uint64_t changed_ = 0, candidate_since_ = 0;
    unsigned candidate_ = 0, current_ = 2, target_ = 0;
    bool tracking_ = false;
  public:
    unsigned update(double ms, unsigned target, unsigned maximum, bool enabled, unsigned fixed,
                    std::uint64_t now) {
        const auto ceiling = std::max(2u, maximum);
        const auto bounded = std::clamp(current_, 2u, ceiling);
        if (!enabled) {
            current_ = std::clamp(fixed, 2u, ceiling);
            tracking_ = false; average_ = 0; candidate_ = 0;
            return current_;
        }
        if (!tracking_ || bounded != current_ || target != target_) {
            current_ = bounded; target_ = target; changed_ = now;
            average_ = 0; candidate_ = 0; tracking_ = true;
        }
        if (!std::isfinite(ms) || ms < 1 || ms > 250 || !target || now < changed_) {
            average_ = 0; candidate_ = 0; changed_ = now;
            return current_;
        }
        // Port the locally tested OptiScaler policy: elapsed-time smoothing,
        // asymmetric headroom and dwell, one step at a time, 3s between changes.
        average_ += average_ ? (ms - average_) * (1.0 - std::exp(-ms / 500.0)) : ms;
        const auto fps = 1000.0 / average_;
        unsigned wanted = current_;
        if (current_ < ceiling && fps * current_ < target * .93) ++wanted;
        else if (current_ > 2 && fps * (current_ - 1) > target * 1.10) --wanted;
        if (wanted == current_) { candidate_ = 0; return current_; }
        if (candidate_ != wanted) { candidate_ = wanted; candidate_since_ = now; return current_; }
        const auto dwell = wanted > current_ ? 1000u : 2000u;
        if (now - changed_ >= 3000 && now - candidate_since_ >= dwell) {
            current_ = wanted; changed_ = now; candidate_ = 0; average_ = 0;
        }
        return current_;
    }
};
} // namespace xivrender::fg
