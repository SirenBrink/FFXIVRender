#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xivrender {

// Append new values: existing INI quality IDs must retain their meaning.
enum class DlssQuality { Dlaa, UltraQuality, Quality, Balanced, Performance, UltraPerformance, Game };
// A saved request is not a declaration that the installed runtime supports it.
enum class DlssModelPreset { Automatic, A, B, C, D, E, F, G, H, I, J, K, L, M };
enum class FrameGeneration { Off, DlssG, XeFG, FsrFG };
enum class LatencyReduction { Automatic, Off, Reflex, XeLL, AntiLag2 };
enum class FgModelPreset { Automatic, A, B, C };
enum class NrPrecision { Fp8 };
enum class HdrOutput { Sdr8, Pq10, Pq12Requested, ScRgb16Float };
enum class ScreenshotProfile { StandardsPq, IccOnlyPq };

struct DlssSettings {
    bool enabled = true;
    DlssQuality quality = DlssQuality::Game;
    DlssModelPreset model_preset = DlssModelPreset::Automatic;
    bool hdr_input = false;
    bool automatic_mip_bias = false;
    bool operator==(const DlssSettings &) const = default;
};

// Model hints and optional texture detail do not require ownership of the
// game's resolution policy. Native DRS must retain its ranges and jitter.
inline bool fixed_dlss_resolution(const DlssSettings &s) {
    return s.enabled && s.quality != DlssQuality::Game;
}

struct FrameGenerationSettings {
    bool enabled = false;
    FrameGeneration backend = FrameGeneration::DlssG;
    unsigned factor = 2;
    bool dynamic = false;
    unsigned target_fps = 120;
    FgModelPreset model_preset = FgModelPreset::Automatic;
    bool hdr_input = false;
    bool experimental_xefg_5x_6x = false;
    // Saved as `reflex` for compatibility; keep the selected provider's
    // latency reduction active independently when frame generation is off.
    bool reflex = true;
    LatencyReduction latency = LatencyReduction::Automatic;
    bool xefg_unlock = true;
    bool fsr_mfg = false;
    bool operator==(const FrameGenerationSettings &) const = default;
};

struct NrPassProfile {
    unsigned preset = 0, style = 0;
    float intensity = 1, structure = 1, tone = 1, skin = -1;
    bool auto_mask = true;
    bool operator==(const NrPassProfile &) const = default;
};
struct NrPassOverride {
    bool inherit = true;
    NrPassProfile profile{0, 0, 1, 1, 0, -1, true};
    bool operator==(const NrPassOverride &) const = default;
};

struct NeuralRenderingSettings {
    std::string runtime_id = "310.9.16-compat";
    bool enabled = false;
    unsigned passes = 1;
    NrPassProfile profile;
    std::array<NrPassOverride, 7> later_passes;
    float feedback = 1;
    float detail_strength = 1, colour_strength = 1;
    bool apply_model = true;
    bool separate_skin = false, skin_colour = true;
    float skin_detail = 1, skin_colour_strength = 1;
    float environment_detail = 1, environment_colour = 1;
    float model_scale = 1.0f;
    bool pause_on_focus_loss = true;
    bool foveated = false;
    float central_fraction = 0.8f;
    float work_fraction = 0.9f;
    NrPrecision precision = NrPrecision::Fp8;
    // 1 computes every bottleneck. 2-4 request cache reuse, never skipped NR frames.
    unsigned bottleneck_interval = 1;
    bool first_pass_reuse_only = true;
    bool unverified_kernels = false;
    bool generate_before_sr_apply_after = false;
    float residual_history = 0.0f;
    bool hdr_input = false;
    bool operator==(const NeuralRenderingSettings &) const = default;
};

inline NrPassProfile nr_pass_profile(const NeuralRenderingSettings &s, unsigned pass) {
    if (!pass)
        return s.profile;
    if (pass <= s.later_passes.size() && !s.later_passes[pass - 1].inherit)
        return s.later_passes[pass - 1].profile;
    auto p = s.profile;
    p.tone = 0; // Avoid compounding local tone on inherited later passes.
    return p;
}

struct HdrSettings {
    bool enabled = false;
    HdrOutput output = HdrOutput::Pq10;
    float peak_nits = 1000.0f;
    float paper_white_nits = 203.0f;
    float black_nits = 0.0f;
    float saturation = 1.0f;
    float vibrance = 0.0f;
    float contrast = 1.0f;
    bool preserve_reshade_highlights = true;
    bool screenshot_enabled = true;
    ScreenshotProfile screenshot_profile = ScreenshotProfile::IccOnlyPq;
    unsigned screenshot_key = 0x2c; // Print Screen; capture ownership is negotiated at runtime.
    bool upgrade_game_screenshots = true;
    bool operator==(const HdrSettings &) const = default;
};

struct ReShadeSettings {
    bool automatic_depth = true;
    bool rest_hud_isolation = true;
    bool operator==(const ReShadeSettings &) const = default;
};

struct PreviewSettings {
    bool dlss = false;
    bool nr = false;
    unsigned nr_instance_limit = 1;
    // The output is always twice each native extent, not a fixed 576x960 target.
    bool operator==(const PreviewSettings &) const = default;
};

struct Settings {
    DlssSettings dlss;
    FrameGenerationSettings fg;
    NeuralRenderingSettings nr;
    HdrSettings hdr;
    ReShadeSettings reshade;
    PreviewSettings preview;
    bool operator==(const Settings &) const = default;
};

struct LoadResult {
    Settings settings;
    std::vector<std::string> warnings;
    bool found = false;
};

// Invalid fields are restored independently to their defaults. No runtime
// request is applied here: adapters must still validate their capabilities.
std::vector<std::string> normalize_settings(Settings &settings);
LoadResult load_settings(const std::filesystem::path &path);
// Rejects invalid settings; a failed write leaves the previous file intact.
bool save_settings(const std::filesystem::path &path, const Settings &settings, std::string &error);

// NR-only profiles never change runtime selection, enabled state, focus pause,
// or another feature's settings. Loading is transactional.
bool save_nr_profile(const std::filesystem::path &, const NeuralRenderingSettings &, std::string &error);
bool load_nr_profile(const std::filesystem::path &, NeuralRenderingSettings &, std::string &error);

} // namespace xivrender
