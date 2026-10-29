#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <windows.h>

#include "xivrender/app.hpp"
#include "xivrender/overlay.hpp"
#include "xivrender/native_nr.hpp"
#include "xivrender/hdr.hpp"
#include "xivrender/frame_generation.hpp"
#include "xivrender/fg_diagnostics.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace xivrender {
namespace {

void tooltip(const char *text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Draft values stay local to the overlay until editing ends. Native model
// recreation must never chase every mouse movement while a slider is held.
bool nr_slider(const char *label, float &value, float low, float high, const char *format = "%.2f") {
    struct Draft {
        float value = 0;
        bool active = false;
    };
    static std::unordered_map<ImGuiID, Draft> drafts;
    auto &draft = drafts[ImGui::GetID(label)];
    if (!draft.active)
        draft.value = value;
    const bool edited =
        ImGui::SliderFloat(label, &draft.value, low, high, format, ImGuiSliderFlags_AlwaysClamp);
    const bool active = ImGui::IsItemActive();
    const bool commit = ImGui::IsItemDeactivatedAfterEdit() || (edited && !active);
    draft.active = active;
    if (commit && value != draft.value) {
        value = draft.value;
        return true;
    }
    return false;
}
bool nr_profile(NrPassProfile &p) {
    bool changed = false;
    const char *styles[]{"Standard", "Natural", "Cinematic"};
    int style = int(p.style);
    if (ImGui::Combo("Appearance profile", &style, styles, IM_ARRAYSIZE(styles))) {
        p.style = unsigned(style);
        changed = true;
    }
    changed |= nr_slider("Intensity", p.intensity, 0, 2);
    changed |= nr_slider("Local structure", p.structure, 0, 2);
    changed |= nr_slider("Local tone", p.tone, 0, 2);
    changed |= nr_slider("Skin structure", p.skin, -1, 2);
    tooltip("-1 uses the model's default skin structure.");
    changed |= ImGui::Checkbox("Automatic skin mask", &p.auto_mask);
    const char *presets[]{"Default", "Preset 1", "Preset 2", "Preset 3"};
    int preset = int(p.preset);
    if (ImGui::Combo("Model preset hint", &preset, presets, IM_ARRAYSIZE(presets))) {
        p.preset = unsigned(preset);
        changed = true;
    }
    tooltip("Passes the preset hint to the selected NR runtime. Its visual effect depends on that runtime "
            "and is unverified. Use Appearance profile for style selection.");
    return changed;
}
bool nr_controls(NeuralRenderingSettings &s) {
    bool changed = false;
    float passes = float(s.passes);
    if (nr_slider("NR passes", passes, 1, 8, "%.0f")) {
        s.passes = unsigned(passes + .5f);
        changed = true;
    }
    tooltip("Each pass has independent temporal history and uses additional GPU work and model memory. Start "
            "with two passes at a reduced model resolution.");
    ImGui::BeginDisabled(s.passes < 2);
    changed |= nr_slider("Multipass feedback", s.feedback, 0, 1, "%.3f");
    tooltip("How much of the previous pass reaches the next. 1 uses the full result; 0 runs each pass from "
            "the clean image. No frames are skipped.");
    ImGui::EndDisabled();
    for (unsigned i = 0; i < s.passes; ++i) {
        ImGui::PushID(int(i));
        const auto name = "Pass " + std::to_string(i + 1);
        if (ImGui::TreeNodeEx(name.c_str(), i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            if (!i)
                changed |= nr_profile(s.profile);
            else {
                auto &pass = s.later_passes[i - 1];
                if (ImGui::Checkbox("Inherit pass 1 (local tone = 0)", &pass.inherit)) {
                    if (!pass.inherit) {
                        pass.profile = s.profile;
                        pass.profile.tone = 0;
                    }
                    changed = true;
                }
                ImGui::BeginDisabled(pass.inherit);
                if (pass.inherit) {
                    auto inherited = nr_pass_profile(s, i);
                    nr_profile(inherited);
                } else
                    changed |= nr_profile(pass.profile);
                ImGui::EndDisabled();
            }
            if (ImGui::SmallButton("Reset pass")) {
                if (!i)
                    s.profile = {};
                else
                    s.later_passes[i - 1] = {};
                changed = true;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (ImGui::TreeNode("Effect strength and colour")) {
        changed |= ImGui::Checkbox("Show NR effect", &s.apply_model);
        tooltip("Hide the correction for comparison. The model continues running; disable NR to stop its GPU "
                "cost.");
        changed |= nr_slider("Detail strength", s.detail_strength, 0, 2);
        changed |= nr_slider("Colour strength", s.colour_strength, 0, 4);
        tooltip("Scales the colour component of the NR correction. 0 retains the source chroma; 1 applies "
                "the full correction.");
        changed |= ImGui::Checkbox("Separate skin and environment", &s.separate_skin);
        tooltip("Approximate colour-based skin selection. Warm materials can also be selected.");
        ImGui::BeginDisabled(!s.separate_skin);
        changed |= ImGui::Checkbox("Allow skin colour changes", &s.skin_colour);
        changed |= nr_slider("Skin detail", s.skin_detail, 0, 1);
        changed |= nr_slider("Skin colour", s.skin_colour_strength, 0, 1);
        changed |= nr_slider("Environment detail", s.environment_detail, 0, 1);
        changed |= nr_slider("Environment colour", s.environment_colour, 0, 1);
        ImGui::EndDisabled();
        if (ImGui::SmallButton("Reset effect strength")) {
            s.apply_model = true;
            s.detail_strength = s.colour_strength = 1;
            s.separate_skin = false;
            s.skin_colour = true;
            s.skin_detail = s.skin_colour_strength = s.environment_detail = s.environment_colour = 1;
            changed = true;
        }
        ImGui::TreePop();
    }
    changed |= ImGui::Checkbox("Foveated NR / peripheral compression", &s.foveated);
    tooltip("Preserves central pixel density while reducing the NR grid at the edges. Color, depth, motion "
            "and residual composition use the same transform. The original DLSS image remains intact.");
    if (s.foveated) {
        if (nr_slider("Central region", s.central_fraction, .1f, .95f)) {
            s.work_fraction = std::max(s.work_fraction, s.central_fraction + .01f);
            changed = true;
        }
        changed |= nr_slider("Peripheral work fraction", s.work_fraction, s.central_fraction + .01f, 1.f);
        ImGui::TextDisabled("NR grid: %.1f%% of uniform pixels", s.work_fraction * s.work_fraction * 100);
    }
    ImGui::TextWrapped(
        "Pass and model-grid changes apply after editing ends. Save settings to retain all NR choices.");
    return changed;
}

bool hdr_controls(Settings &s) {
    bool changed = false;
    ImGui::SeparatorText("RenoDX scene HDR");
    if (ImGui::Checkbox("Enable scene HDR", &s.hdr.enabled)) {
        if (s.hdr.enabled) {
            s.dlss.hdr_input = true;
            s.nr.hdr_input = true;
        }
        changed = true;
    }
    tooltip("Uses the game's original extended scene lighting through RenoDX FFXIV shaders. Saves an "
            "SDR-compatible layer for existing ReShade effects. Enable Windows HDR before launching.");
    const char *formats[]{"HDR10 / 10-bit PQ", "16-bit floating point / scRGB"};
    int output = s.hdr.output == HdrOutput::ScRgb16Float ? 1 : 0;
    if (ImGui::Combo("HDR output", &output, formats, IM_ARRAYSIZE(formats))) {
        s.hdr.output = output ? HdrOutput::ScRgb16Float : HdrOutput::Pq10;
        changed = true;
    }
    tooltip("Applies at the next presentation boundary. 16-bit scRGB supports ordinary presentation and FSR FG. DLSS-G and "
            "XeFG integrations require 10-bit HDR10/PQ, while scene processing can remain FP16.");
    ImGui::TextWrapped("%s", hdr::status().c_str());
    if (s.hdr.enabled != hdr::active()) {
        ImGui::TextWrapped("Output change is pending. If it cannot be applied, the previous output is retained; check the presentation status.");
    }
    const auto slider = [&](const char *label, float &v, float low, float high, float fallback,
                            const char *format) {
        ImGui::PushID(label);
        bool edit = nr_slider(label, v, low, high, format);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) {
            v = fallback;
            edit = true;
        }
        ImGui::PopID();
        return edit;
    };
    changed |= slider("Peak brightness", s.hdr.peak_nits, std::max(203.f, s.hdr.paper_white_nits), 4000, 1000,
                      "%.0f nits");
    changed |= slider("Paper white", s.hdr.paper_white_nits, 48, std::min(1000.f, s.hdr.peak_nits), 203,
                      "%.0f nits");
    changed |= slider("Black level", s.hdr.black_nits, 0, 1, 0, "%.3f nits");
    changed |= slider("HDR saturation", s.hdr.saturation, 0, 2, 1, "%.2f");
    changed |= slider("HDR vibrance", s.hdr.vibrance, -1, 1, 0, "%.2f");
    changed |= slider("HDR contrast", s.hdr.contrast, .5f, 1.5f, 1, "%.2f");
    changed |= ImGui::Checkbox("Preserve HDR highlights through ReShade", &s.hdr.preserve_reshade_highlights);
    tooltip("ReShade effects receive bounded SDR. Their measured colour edits are carried onto retained HDR "
            "scene highlights.");
    changed |= ImGui::Checkbox("Use scene HDR range for NR", &s.nr.hdr_input);
    tooltip("Offers the extended scene range to NR through a bounded reversible proxy. Off uses a clipped "
            "SDR model input while preserving the clean HDR scene. Applies live and resets NR history.");
    ImGui::TextDisabled(
        "HDR DLSS uses the native model with linear scene input and separate output/history.");
    return changed;
}

void staged_features() {
    if (!ImGui::CollapsingHeader("Features being ported"))
        return;
    ImGui::TextWrapped("These features are not active in this test build. Their saved settings are "
                       "retained for later tests.");

    ImGui::BulletText("NR bottleneck reuse and PreSR");
    ImGui::BulletText("DLSS-G model preset overrides and Ada MFG unlock");

    ImGui::BulletText("HDR screenshots with ICC profiles");
    ImGui::BulletText("REST HUD isolation");
    ImGui::BulletText("Character-preview and portrait upscaling / NR");
    ImGui::TextWrapped("Supported HDR buffer formats: 10-bit PQ and 16-bit floating point. A 12-bit output "
                       "request is not an available buffer format in this build.");
}

} // namespace

void draw_overlay(reshade::api::effect_runtime *runtime) {
    (void)runtime;
    auto settings = settings_snapshot();
    const auto status = status_snapshot();
    static std::string save_message;
    bool changed = false;

    ImGui::TextUnformatted("XIVRender");
    ImGui::TextWrapped("Enable DLSS in FFXIV's graphics settings to use these controls.");
    if (status.conflict)
        ImGui::TextWrapped("DLSS controls are unavailable while another upscaler controller is active.");
    else if (!status.native_dlss_hooks)
        ImGui::TextWrapped("Waiting for FFXIV DLSS to become available.");
    else if (!status.evaluations)
        ImGui::TextWrapped("DLSS controls are ready. Waiting for a rendered game frame.");
    else if (GetTickCount64() - status.last_evaluation_tick > 2000)
        ImGui::TextWrapped("No recent DLSS frames. Check that FFXIV's DLSS graphics option is enabled.");
    else
        ImGui::TextUnformatted("Receiving native DLSS frames.");
    if (!status.message.empty())
        ImGui::TextWrapped("%s", status.message.c_str());
    ImGui::SeparatorText("DLSS resolution");
    if (status.evaluated_resolution) {
        ImGui::Text("Input:  %u x %u", status.render_width, status.render_height);
        ImGui::Text("Output: %u x %u", status.output_width, status.output_height);
        ImGui::TextDisabled("Last successful DLSS frame");
    } else {
        ImGui::TextDisabled("Input:  Waiting for DLSS");
        ImGui::TextDisabled("Output: Waiting for DLSS");
    }

    ImGui::Separator();
    ImGui::BeginDisabled(status.conflict);
    changed |= ImGui::Checkbox("Enable DLSS controls", &settings.dlss.enabled);
    tooltip("Apply XIVRender's quality, model and texture-detail choices. FFXIV still owns the game's DLSS "
            "graphics option. Frame generation, latency reduction, NR and ReShade depth work independently "
            "when these controls are off.");
    ImGui::BeginDisabled(!settings.dlss.enabled || !status.native_dlss_hooks);

    const char *quality_names[] = {"Native / DLAA", "Ultra Quality", "Quality",
                                   "Balanced",      "Performance",   "Ultra Performance", "Game / native DRS"};
    int quality = std::clamp(static_cast<int>(settings.dlss.quality), 0, 6);
    ImGui::BeginDisabled(!status.native_quality);
    if (ImGui::Combo("Quality", &quality, quality_names, IM_ARRAYSIZE(quality_names))) {
        settings.dlss.quality = static_cast<DlssQuality>(quality);
        changed = true;
    }
    tooltip(status.native_quality
                ? "Changes resolution live. Game / native DRS preserves FFXIV's size ranges and raster jitter; model selection remains independent."
                : "Live quality control is waiting for this game version to be recognized.");
    ImGui::EndDisabled();

    const char *preset_names[] = {"Automatic", "Preset A", "Preset B", "Preset C", "Preset D",
                                  "Preset E",  "Preset F", "Preset G", "Preset H", "Preset I",
                                  "Preset J",  "Preset K", "Preset L", "Preset M"};
    int preset = std::clamp(static_cast<int>(settings.dlss.model_preset), 0, 13);
    if (ImGui::Combo("Model preset", &preset, preset_names, IM_ARRAYSIZE(preset_names))) {
        settings.dlss.model_preset = static_cast<DlssModelPreset>(preset);
        changed = true;
    }
    tooltip("Recreates native DLSS with the requested model, without changing quality. "
            "Availability depends on the installed NVIDIA DLSS runtime.");
    if (status.native_dlss_hooks && settings.dlss.enabled &&
        status.last_created_model != int(settings.dlss.model_preset))
        ImGui::TextDisabled("Model change pending a new native DLSS feature.");
    changed |= ImGui::Checkbox("Automatic texture detail", &settings.dlss.automatic_mip_bias);
    tooltip("Updates texture mip bias when the actual render resolution changes.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (status.render_width && status.render_height && status.display_width && status.display_height) {
        if (settings.dlss.automatic_mip_bias)
            ImGui::Text("Texture bias: %.5f", status.mip_bias);
        if (status.jitter_phases)
            ImGui::Text("Jitter phase target: %u", status.jitter_phases);
    } else
        ImGui::TextDisabled("Render resolution has not been measured yet.");

    ImGui::SeparatorText("Frame Generation");
    const auto fg_state = fg::status();
    changed |= ImGui::Checkbox("Enable frame generation", &settings.fg.enabled);
    const char *fg_names[]{"Off", "NVIDIA DLSS-G", "Intel XeFG", "AMD FSR FG"};
    int fg_backend = std::clamp(int(settings.fg.backend), 0, 3);
    if (ImGui::Combo("Frame generation provider", &fg_backend, fg_names, IM_ARRAYSIZE(fg_names))) {
        settings.fg.backend = static_cast<FrameGeneration>(fg_backend);
        settings.fg.factor = std::min(settings.fg.factor, fg_backend == 3 ? (settings.fg.fsr_mfg ? 4u : 2u) : fg_backend == 2 ? 4u : 6u);
        changed = true;
    }
    const char *latency_names[]{"Automatic", "Off", "NVIDIA Reflex", "Intel XeLL", "AMD Anti-Lag 2"};
    int latency = std::clamp(int(settings.fg.latency), 0, 4);
    if (ImGui::Combo("Latency reduction", &latency, latency_names, IM_ARRAYSIZE(latency_names))) {
        settings.fg.latency = static_cast<LatencyReduction>(latency);
        changed = true;
    }
    tooltip("Selects latency reduction separately for FSR and ordinary frames. Automatic uses Reflex. "
            "Availability is checked on the current device; a menu choice does not imply support.");
    changed |= ImGui::Checkbox("Keep latency reduction enabled with FG off", &settings.fg.reflex);
    if (settings.fg.enabled && settings.fg.backend == FrameGeneration::XeFG)
        ImGui::TextWrapped("XeFG requires XeLL. Your latency selection is retained for FSR and FG off.");
    if (settings.fg.enabled && settings.fg.backend == FrameGeneration::DlssG)
        ImGui::TextWrapped("DLSS-G uses its required Reflex integration. Your selection is retained for FSR and FG off.");
    ImGui::TextDisabled("Provider and latency changes apply live once FG presentation has started.");
    if (!fg::output_requested())
        ImGui::TextWrapped("Waiting for the presentation bridge; check the status for initialization errors.");
    if (settings.fg.backend == FrameGeneration::FsrFG) {
        if (ImGui::Checkbox("FSR multi-frame generation (DLSS Enabler)", &settings.fg.fsr_mfg)) {
            if (!settings.fg.fsr_mfg) settings.fg.factor = 2;
            changed = true;
        }
        tooltip("Enables 2X, 3X and 4X using DLSS Enabler. Off keeps classic FidelityFX 2X. "
                "The provider is safely recreated when this changes. MFG supports SDR and 10-bit HDR/PQ.");
        if (settings.fg.fsr_mfg && settings.hdr.enabled && settings.hdr.output == HdrOutput::ScRgb16Float)
            ImGui::TextWrapped("FSR MFG needs 10-bit HDR/PQ. Classic FSR 2X also supports scRGB.");
    }
    const int fg_limit = settings.fg.backend == FrameGeneration::FsrFG  ? (settings.fg.fsr_mfg ? 4 : 2)
                         : settings.fg.backend == FrameGeneration::XeFG ? 4
                                                                        : 6;
    int fg_factor = std::clamp(int(settings.fg.factor), 2, fg_limit);
    if (ImGui::SliderInt("FG multiplier", &fg_factor, 2, fg_limit, "%dX")) {
        settings.fg.factor = unsigned(fg_factor);
        changed = true;
    }
    if (settings.fg.backend != FrameGeneration::FsrFG) {
        changed |= ImGui::Checkbox("Dynamic frame generation", &settings.fg.dynamic);
        int target = int(settings.fg.target_fps);
        if (ImGui::SliderInt("Target display FPS", &target, 30, 360)) {
            settings.fg.target_fps = unsigned(target);
            changed = true;
        }
    }
    if (settings.fg.backend == FrameGeneration::XeFG) {
        changed |= ImGui::Checkbox("Enable verified XeFG 4X unlock", &settings.fg.xefg_unlock);
        tooltip("Uses the verified 1.3.1.78 provider and generated-frame pacing. Changes retire and recreate "
                "the provider live.");
    }
    if (settings.fg.enabled && settings.hdr.enabled && settings.hdr.output == HdrOutput::ScRgb16Float &&
        settings.fg.backend != FrameGeneration::FsrFG)
        ImGui::TextWrapped("DLSS-G and XeFG require 10-bit PQ HDR. Select 10-bit HDR, or use FSR "
                           "FG for 16-bit scRGB.");
    ImGui::TextWrapped("%s", fg_state.message.c_str());
    if (!fg_state.transition.empty())
        ImGui::TextWrapped("%s", fg_state.transition.c_str());
    if (!fg_state.last_error.empty())
        ImGui::TextWrapped("Last FG error: %s", fg_state.last_error.c_str());
    if (settings.fg.backend == FrameGeneration::DlssG && !fg_state.compatibility.empty())
        ImGui::TextWrapped("%s", fg_state.compatibility.c_str());
    if (fg_state.available) {
        if (fg_state.rate_measured) {
            ImGui::Text("Observed output: %.2fX; device limit: %uX", fg_state.observed_ratio,
                        fg_state.maximum);
            ImGui::Text("Real: %.1f FPS; presented: %.1f FPS", fg_state.real_fps, fg_state.displayed_fps);
        } else
            ImGui::Text("Measuring output rate; device limit: %uX", fg_state.maximum);
        ImGui::TextWrapped("Latency: %s", fg_state.latency_name.c_str());
    }

    ImGui::SeparatorText("Neural Rendering");
    const auto nr_status = native_nr::snapshot();
    ImGui::BeginDisabled(status.conflict);
    changed |= ImGui::Checkbox("Enable Neural Rendering", &settings.nr.enabled);
    tooltip("Runs NR over the game scene after native DLSS, before HUD and ReShade effects.");
    const char *nr_versions[]{"310.9.16 (compatibility)", "310.8 (compatibility)"};
    int nr_version = settings.nr.runtime_id == "310.8-compat" ? 1 : 0;
    if (ImGui::Combo("NR version", &nr_version, nr_versions, IM_ARRAYSIZE(nr_versions))) {
        settings.nr.runtime_id = nr_version ? "310.8-compat" : "310.9.16-compat";
        changed = true;
    }
    tooltip("Changes live. The previous model retires safely before the selected version loads.");
    float nr_percent = settings.nr.model_scale * 100;
    if (nr_slider("NR model resolution", nr_percent, 25, 200, "%.0f%%")) {
        settings.nr.model_scale = nr_percent / 100;
        changed = true;
    }
    tooltip("Lower values reduce NR cost. The NR change is applied over the original DLSS detail. "
            "Hold Ctrl and click the value to enter an exact percentage.");
    changed |= ImGui::Checkbox("Pause NR while game is unfocused", &settings.nr.pause_on_focus_loss);
    if (settings.nr.enabled && nr_status.requested_runtime.empty())
        ImGui::TextWrapped("Waiting for a native DLSS frame.");
    else
        ImGui::TextWrapped("%s", nr_status.message.c_str());
    if (!nr_status.active_runtime.empty())
        ImGui::Text("Loaded NR: %s | Model: %u x %u", nr_status.active_runtime.c_str(), nr_status.width,
                    nr_status.height);
    if (nr_status.failed && ImGui::Button("Retry NR"))
        native_nr::retry();
    changed |= nr_controls(settings.nr);
    if (ImGui::TreeNode("Saved NR profiles")) {
        static int slot = 0;
        static std::string message;
        const char *slots[]{"Profile 1", "Profile 2", "Profile 3", "Profile 4",
                            "Profile 5", "Profile 6", "Profile 7", "Profile 8"};
        ImGui::Combo("Profile slot", &slot, slots, IM_ARRAYSIZE(slots));
        if (ImGui::Button("Save NR profile")) {
            std::string error;
            message = save_nr_slot(unsigned(slot + 1), settings.nr, error) ? "NR profile saved." : error;
        }
        ImGui::SameLine();
        if (ImGui::Button("Load NR profile")) {
            std::string error;
            if (load_nr_slot(unsigned(slot + 1), settings.nr, error)) {
                changed = true;
                message = "NR profile loaded.";
            } else
                message = error;
        }
        tooltip("Loads NR tuning and performance choices. Keeps the current runtime, enabled state and focus "
                "behavior. Other features are unchanged.");
        if (!message.empty())
            ImGui::TextWrapped("%s", message.c_str());
        ImGui::TreePop();
    }
    ImGui::EndDisabled();

    changed |= hdr_controls(settings);
    if (changed) {
        request_settings(settings);
        save_message = "Changes requested. Save settings to keep them.";
    }
    if (ImGui::Button("Save settings")) {
        std::string error;
        save_message =
            save_requested_settings(error) ? "Settings saved." : "Could not save settings: " + error;
    }
    ImGui::SameLine();
    if (ImGui::Button("Check runtime files"))
        request_rescan();
    tooltip("Read file versions and availability. This does not reload libraries or change DLSS settings. "
            "Restart after replacing game or rendering libraries.");
    if (!save_message.empty())
        ImGui::TextWrapped("%s", save_message.c_str());

    changed = false;
    auto depth_settings = settings_snapshot();
    if (ImGui::Checkbox("Automatic ReShade depth", &depth_settings.reshade.automatic_depth)) {
        request_settings(depth_settings);
        save_message = "Changes requested. Save settings to keep them.";
    }
    tooltip("Use the active game depth region automatically. Existing manual depth transforms are retained.");
    ImGui::Separator();
    staged_features();
    if (ImGui::CollapsingHeader("Diagnostics")) {
        ImGui::TextWrapped("FG recording: %s", fg::diagnostics::path().c_str());
        if (fg::diagnostics::error())
            ImGui::Text("FG log write error: %u", fg::diagnostics::error());
        ImGui::Text("State: %s", status.phase.c_str());
        ImGui::Text("DLSS hooks: %s", status.native_dlss_hooks ? "Ready" : "Not active");
        ImGui::Text("Live quality: %s", status.native_quality ? "Ready" : "Not active");
        ImGui::Text("DLSS evaluations: %llu", static_cast<unsigned long long>(status.evaluations));
        ImGui::Text("Feature creations: %llu", static_cast<unsigned long long>(status.creates));
        ImGui::Text("Failures: %llu", static_cast<unsigned long long>(status.failures));
        ImGui::Text("Real presents: %llu", static_cast<unsigned long long>(status.real_presents));
        ImGui::Text("NR: %llu evaluations, %llu applied, %llu skipped, %llu failures",
                    static_cast<unsigned long long>(nr_status.evaluations),
                    static_cast<unsigned long long>(nr_status.applied),
                    static_cast<unsigned long long>(nr_status.skipped),
                    static_cast<unsigned long long>(nr_status.failures));
        ImGui::TextWrapped("NR kernels: %s", nr_status.precision.c_str());
        if (status.evaluated_resolution) {
            ImGui::Text("Input source: %s", status.resolution_source.c_str());
            ImGui::Text("Input texture allocation: %u x %u", status.input_allocation_width,
                        status.input_allocation_height);
        }
        if (!status.runtime.empty())
            ImGui::TextWrapped("%s", status.runtime.c_str());
        if (!status.signatures.empty())
            ImGui::TextWrapped("%s", status.signatures.c_str());
    }
}

} // namespace xivrender
