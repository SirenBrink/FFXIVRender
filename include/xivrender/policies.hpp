#pragma once

#include "settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace xivrender {

inline unsigned jitter_phase_count(unsigned render_width, unsigned display_width) {
    if (!render_width || !display_width)
        return 0;
    const double ratio = double(display_width) / render_width;
    // Prevent malformed or temporarily absent dimensions from causing an
    // unbounded sequence. Native/DLAA receives eight samples.
    const double phases = std::ceil(8.0 * ratio * ratio);
    return unsigned(std::clamp(phases, 1.0, 512.0));
}

inline std::optional<float> mip_bias(unsigned render_width, unsigned display_width) {
    if (!render_width || !display_width)
        return std::nullopt;
    return float(std::log2(double(render_width) / display_width));
}

inline float halton(unsigned index, unsigned base) {
    if (base < 2)
        return 0.0f;
    float value = 0.0f, fraction = 1.0f;
    while (index) {
        fraction /= float(base);
        value += fraction * float(index % base);
        index /= base;
    }
    return value - 0.5f;
}

struct JitterSample {
    float x = 0, y = 0;
    unsigned phase = 0, count = 0;
};

// Sample/commit is transactional: skipped dispatches and generated presents
// must not consume temporal phases. A repeated commit cannot advance twice.
class TemporalSequence {
    std::uint64_t accepted_ = 0;
    std::optional<std::uint64_t> last_frame_;

  public:
    JitterSample sample(unsigned render_width, unsigned display_width) const {
        const auto count = jitter_phase_count(render_width, display_width);
        if (!count)
            return {};
        const auto phase = unsigned(accepted_ % count);
        return {halton(phase + 1, 2), halton(phase + 1, 3), phase, count};
    }
    bool commit(std::uint64_t frame_id) {
        if (last_frame_ && frame_id <= *last_frame_)
            return false;
        last_frame_ = frame_id;
        ++accepted_;
        return true;
    }
    void reset() {
        accepted_ = 0;
        last_frame_.reset();
    }
    std::uint64_t accepted_frames() const {
        return accepted_;
    }
};

inline bool nr_active(const NeuralRenderingSettings &settings, bool focused) {
    return settings.enabled && (!settings.pause_on_focus_loss || focused);
}

// DXGI has no conventional packed RGB 12-bits-per-channel swapchain format.
// Keep a 12-bit request visible for negotiation, without labelling a 10/16-bit
// allocation as 12-bit output or claiming control of the display wire signal.
inline unsigned output_buffer_bits(HdrOutput output) {
    switch (output) {
    case HdrOutput::Sdr8:
        return 8;
    case HdrOutput::Pq10:
        return 10;
    case HdrOutput::ScRgb16Float:
        return 16;
    default:
        return 0;
    }
}

inline unsigned supported_fg_factor(const FrameGenerationSettings &settings) {
    if (settings.backend == FrameGeneration::Off)
        return 1;
    const unsigned maximum =
        settings.backend == FrameGeneration::XeFG && !settings.experimental_xefg_5x_6x ? 4 : 6;
    return std::clamp(settings.factor, 2u, maximum);
}

struct MfgState {
    FrameGeneration backend = FrameGeneration::Off;
    unsigned requested = 1;
    unsigned observed = 1;
    bool active = false;
    bool dynamic = false;
};

class MfgController {
    MfgState state_;

  public:
    void request(const FrameGenerationSettings &settings) {
        const unsigned next = supported_fg_factor(settings);
        if (settings.backend != state_.backend || next != state_.requested ||
            settings.dynamic != state_.dynamic) {
            state_.observed = 1;
            state_.active = false;
        }
        state_.requested = next;
        state_.backend = settings.backend;
        state_.dynamic = settings.dynamic;
    }
    // Only accepted runtime telemetry establishes that generated outputs are
    // being presented. It never rewrites the configured ordinary FPS cap.
    void observe(unsigned outputs, bool accepted) {
        if (!accepted || !outputs || outputs > state_.requested) {
            state_.observed = 1;
            state_.active = false;
            return;
        }
        state_.observed = outputs;
        state_.active = outputs > 1;
    }
    MfgState state() const {
        return state_;
    }
};

} // namespace xivrender
