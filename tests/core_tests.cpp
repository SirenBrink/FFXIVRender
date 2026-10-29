#include "xivrender/settings.hpp"
#include "xivrender/policies.hpp"

#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::string read(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void write(const std::filesystem::path &path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    check(bool(stream), "Could not write test fixture");
}
void save(const std::filesystem::path &path, const xivrender::Settings &settings, std::string &error) {
    if (!xivrender::save_settings(path, settings, error))
        throw std::runtime_error("Save failed: " + error);
}
void check_settings(const std::filesystem::path &folder) {
    using namespace xivrender;
    const auto path = folder / "XIVRender.ini";
    const auto missing = load_settings(path);
    check(!missing.found && missing.settings == Settings{}, "Missing settings must retain defaults");
    Settings changed;
    check(changed.nr.runtime_id == "310.9.16-compat", "Patched 310.9.16 must remain the default");
    changed.dlss.quality = DlssQuality::Performance;
    changed.dlss.model_preset = DlssModelPreset::L;
    changed.dlss.hdr_input = true;
    changed.dlss.automatic_mip_bias = false;
    changed.fg.backend = FrameGeneration::XeFG;
    changed.fg.factor = 6;
    changed.fg.dynamic = true;
    changed.fg.fsr_mfg = true;
    changed.fg.target_fps = 165;
    changed.fg.model_preset = FgModelPreset::C;
    changed.fg.hdr_input = true;
    changed.fg.experimental_xefg_5x_6x = true;
    changed.fg.reflex = false;
    changed.fg.latency = LatencyReduction::XeLL;
    Settings invalid_runtime;
    invalid_runtime.nr.runtime_id = "../outside";
    check(!normalize_settings(invalid_runtime).empty(), "Runtime traversal was accepted");
    check(invalid_runtime.nr.runtime_id == Settings{}.nr.runtime_id,
          "Invalid runtime id did not restore default");
    changed.nr.runtime_id = "310.8-compat";
    changed.nr.enabled = true;
    changed.nr.passes = 3;
    changed.nr.profile = {3, 2, .7f, .8f, .9f, .3f, false};
    for (unsigned i = 0; i < 7; ++i) {
        changed.nr.later_passes[i].inherit = false;
        changed.nr.later_passes[i].profile = {i % 4, i % 3, .3f + i * .1f, .6f, .1f, -.5f, i % 2 == 0};
    }
    changed.nr.feedback = .42f;
    changed.nr.apply_model = false;
    changed.nr.detail_strength = 1.5f;
    changed.nr.colour_strength = 2.4f;
    changed.nr.separate_skin = true;
    changed.nr.skin_colour = false;
    changed.nr.skin_detail = .6f;
    changed.nr.skin_colour_strength = .4f;
    changed.nr.environment_detail = .7f;
    changed.nr.environment_colour = .8f;
    changed.nr.model_scale = .73f;
    changed.nr.pause_on_focus_loss = false;
    changed.nr.foveated = true;
    changed.nr.central_fraction = .63f;
    changed.nr.work_fraction = .81f;
    changed.nr.bottleneck_interval = 4;
    changed.nr.first_pass_reuse_only = false;
    changed.nr.unverified_kernels = true;
    changed.nr.generate_before_sr_apply_after = true;
    changed.nr.residual_history = .21f;
    changed.nr.hdr_input = true;
    changed.hdr.enabled = true;
    changed.hdr.output = HdrOutput::ScRgb16Float;
    changed.hdr.peak_nits = 1400;
    changed.hdr.paper_white_nits = 220;
    changed.hdr.black_nits = .005f;
    changed.hdr.saturation = 1.13f;
    changed.hdr.vibrance = .28f;
    changed.hdr.contrast = 1.2f;
    changed.hdr.preserve_reshade_highlights = false;
    changed.hdr.screenshot_enabled = false;
    changed.hdr.screenshot_profile = ScreenshotProfile::StandardsPq;
    changed.hdr.screenshot_key = 0x78;
    changed.hdr.upgrade_game_screenshots = false;
    changed.reshade.automatic_depth = false;
    changed.reshade.rest_hud_isolation = false;
    changed.preview.dlss = true;
    changed.preview.nr = true;
    changed.preview.nr_instance_limit = 4;
    std::string error;
    save(path, changed, error);
    const auto loaded = load_settings(path);
    check(loaded.found && loaded.warnings.empty() && loaded.settings == changed,
          "All persistent controls must round-trip exactly");
    check(read(path).find("every_second_frame") == std::string::npos,
          "Alternating NR must not reappear in saved controls");
    check(read(path).find("approximate") == std::string::npos,
          "Approximate FG guides must not reappear in saved controls");
    Settings another = changed;
    another.hdr.output = HdrOutput::Pq12Requested;
    save(path, another, error);
    check(load_settings(path).settings == another,
          "Saving must replace the old file and retain a pending 12-bit request");
    const auto previous = read(path);
    auto invalid = another;
    invalid.hdr.peak_nits = std::numeric_limits<float>::quiet_NaN();
    check(!save_settings(path, invalid, error) && !error.empty(), "Nonfinite save must be rejected");
    check(read(path) == previous, "Rejected save must preserve the existing file");
    check(!save_settings(folder, changed, error), "Cannot replace a directory with a settings file");
    check(std::filesystem::is_directory(folder), "Failed replacement must preserve the target directory");
    for (const auto &file : std::filesystem::directory_iterator(folder))
        check(file.path().filename().string().find(".tmp-") == std::string::npos,
              "Temporary writes must be cleaned after failure");

    write(path, "[dlss]\nquality=99\nmodel_preset=-1\n[frame_generation]\nfactor=-4\ntarget_fps=10000\n"
                "[neural_rendering]\nenabled=true\nmodel_scale=NaN\npasses=0\nbottleneck_interval=5\n"
                "central_fraction=0.9\nwork_fraction=0.8\n[hdr]\npeak_nits=inf\npaper_white_nits=hello\n"
                "contrast=1.5\nvibrance=0.25junk\n[preview]\nnr_instance_limit=100\n");
    const auto malformed = load_settings(path);
    check(!malformed.warnings.empty(), "Malformed fields must report validation failures");
    check(malformed.settings.nr.enabled && malformed.settings.hdr.contrast == 1.5f,
          "Valid fields must survive independently");
    check(malformed.settings.nr.model_scale == Settings{}.nr.model_scale && malformed.settings.nr.passes == 1,
          "Invalid model dimensions/pass count must fall back");
    check(malformed.settings.nr.bottleneck_interval == 1 && malformed.settings.preview.nr_instance_limit == 1,
          "Invalid reuse and preview admission bounds must fall back");
    check(malformed.settings.nr.central_fraction < malformed.settings.nr.work_fraction,
          "Invalid compression ordering must be repaired");
    check(malformed.settings.dlss.quality == Settings{}.dlss.quality && malformed.settings.fg.factor == 2,
          "Invalid enumeration and signed factors must fall back");
    write(path, "[neural_rendering]\nfeedback=4\ndetail_strength=NaN\ncolour_strength=-1\n[nr_pass_1]\nstyle="
                "3\nintensity=0.75\n[nr_pass_8]\nskin=-4\ntone=0.25\n");
    const auto bad_profile = load_settings(path);
    check(!bad_profile.warnings.empty() && bad_profile.settings.nr.profile.style == 0 &&
              bad_profile.settings.nr.profile.intensity == .75f && bad_profile.settings.nr.feedback == 1 &&
              bad_profile.settings.nr.later_passes[6].profile.skin == -1 &&
              bad_profile.settings.nr.later_passes[6].profile.tone == .25f,
          "Invalid NR profile fields must restore independently without losing valid neighbours");
    NeuralRenderingSettings inherit;
    inherit.profile = {2, 1, .6f, .7f, 1.4f, -.2f, false};
    auto derived = nr_pass_profile(inherit, 1);
    check(derived.style == 1 && derived.intensity == .6f && derived.tone == 0 && !derived.auto_mask,
          "Later passes inherit pass 1 but must not compound local tone");
    inherit.later_passes[0].inherit = false;
    inherit.later_passes[0].profile.tone = .4f;
    check(nr_pass_profile(inherit, 1).tone == .4f, "Explicit later-pass tone must apply");
    const auto profile_path = folder / "NR-1.ini";
    check(save_nr_profile(profile_path, changed.nr, error), "NR profile save failed");
    const auto profile_text = read(profile_path);
    check(profile_text.find("[dlss]") == std::string::npos &&
              profile_text.find("runtime_id") == std::string::npos &&
              profile_text.find("pause_on_focus_loss") == std::string::npos,
          "NR profiles leaked unrelated settings");
    auto profile = changed.nr;
    profile.runtime_id = "310.9.16-compat";
    profile.enabled = false;
    profile.pause_on_focus_loss = true;
    profile.profile.style = 0;
    profile.passes = 1;
    check(load_nr_profile(profile_path, profile, error), "NR profile load failed");
    auto expected = changed.nr;
    expected.runtime_id = profile.runtime_id;
    expected.enabled = false;
    expected.pause_on_focus_loss = true;
    check(profile == expected, "NR profile lost tuning or changed runtime/focus/enabled state");
    const auto keep_profile = profile;
    write(profile_path, "[nr_profile]\nschema=1\n[nr_pass_1]\nintensity=NaN\n");
    check(!load_nr_profile(profile_path, profile, error) && profile == keep_profile,
          "Bad NR profile partially applied");
    write(profile_path, "[nr_profile]\nschema=999\n");
    check(!load_nr_profile(profile_path, profile, error) && profile == keep_profile,
          "Future NR profile was accepted");
    write(profile_path, "[neural_rendering]\npasses=2\n");
    check(!load_nr_profile(profile_path, profile, error) && profile == keep_profile,
          "Unrelated INI reset NR");
    write(path, "[xivrender]\nschema=999\n[neural_rendering]\nenabled=true\n");
    check(load_settings(path).settings == Settings{},
          "An unknown schema cannot partially apply future controls");
    write(path, "\xef\xbb\xbf[HDR]\ncontrast=1.25 ; user comment\n");
    check(load_settings(path).settings.hdr.contrast == 1.25f,
          "UTF-8 BOM and INI section case must be accepted");
    write(path, "[hdr]\ncontrast=1.25\ncontrast=broken\n");
    check(load_settings(path).settings.hdr.contrast == Settings{}.hdr.contrast,
          "An invalid duplicate must restore its field default");
}

void check_temporal() {
    using namespace xivrender;
    check(jitter_phase_count(2560, 3840) == 18, "Quality must receive 18 phases");
    check(jitter_phase_count(2258, 3840) == 24, "Balanced must receive 24 phases");
    check(jitter_phase_count(1920, 3840) == 32, "Performance must receive 32 phases");
    check(jitter_phase_count(3840, 3840) == 8, "DLAA must retain a temporal phase sequence");
    check(jitter_phase_count(0, 3840) == 0 && jitter_phase_count(1, 0) == 0,
          "Missing extents cannot invent phases");
    check(halton(1, 0) == 0.0f && halton(1, 1) == 0.0f, "Invalid Halton bases cannot loop or divide by zero");
    check(!mip_bias(0, 3840), "Missing render size cannot overwrite a sampler bias");
    check(std::abs(*mip_bias(1920, 3840) + 1.0f) < .00001f, "Performance automatic bias must be -1");
    check(*mip_bias(3840, 3840) == 0.0f, "DLAA automatic bias must be zero");
    TemporalSequence sequence;
    const auto first = sequence.sample(2560, 3840);
    check(first.phase == 0 && first.x >= -.5f && first.x <= .5f && first.y >= -.5f && first.y <= .5f,
          "Jitter must be in render-pixel units");
    check(sequence.sample(2560, 3840).phase == first.phase,
          "Sampling without an accepted render frame cannot advance jitter");
    check(sequence.commit(12) && !sequence.commit(12) && !sequence.commit(11),
          "Generated/repeated/out-of-order frames cannot advance phases");
    check(sequence.accepted_frames() == 1 && sequence.sample(2560, 3840).phase == 1,
          "Exactly one accepted frame must advance exactly one phase");
    for (unsigned i = 1; i < 18; ++i)
        check(sequence.commit(12 + i), "Distinct real frames must be accepted");
    check(sequence.sample(2560, 3840).phase == 0, "The full Quality sequence must wrap after 18 real frames");
    sequence.reset();
    check(sequence.commit(0) && sequence.accepted_frames() == 1, "Restart must allow a fresh frame-ID epoch");
}

void check_runtime_policies() {
    using namespace xivrender;
    Settings settings;
    settings.nr.enabled = true;
    check(nr_active(settings.nr, true) && !nr_active(settings.nr, false),
          "Focus loss must suppress requested NR");
    check(settings.nr.enabled, "Focus loss cannot mutate the saved NR enable preference");
    check(nr_active(settings.nr, true), "Focus recovery must restore the prior NR request");
    settings.nr.enabled = false;
    check(!nr_active(settings.nr, true), "Focus recovery cannot enable NR disabled by the user");
    settings.fg.factor = 6;
    MfgController controller;
    controller.request(settings.fg);
    check(controller.state().requested == 6 && !controller.state().active,
          "A 6X request is not proof of active MFG");
    controller.observe(6, true);
    check(controller.state().active && controller.state().observed == 6,
          "Actual accepted output must establish MFG factor");
    settings.fg.factor = 3;
    controller.request(settings.fg);
    check(!controller.state().active && controller.state().observed == 1,
          "Factor transitions must invalidate stale output telemetry");
    controller.observe(6, true);
    check(!controller.state().active, "Old-factor telemetry cannot revive stale factor state");
    controller.observe(3, true);
    settings.fg.backend = FrameGeneration::XeFG;
    controller.request(settings.fg);
    check(!controller.state().active,
          "Provider changes at the same factor must invalidate old-provider telemetry");
    settings.fg.factor = 6;
    controller.request(settings.fg);
    check(controller.state().requested == 4, "XeFG 5X/6X must remain explicitly experimental");
    settings.fg.experimental_xefg_5x_6x = true;
    controller.request(settings.fg);
    check(controller.state().requested == 6, "Experimental XeFG request must retain the requested 6X factor");
    controller.observe(4, false);
    check(!controller.state().active && controller.state().observed == 1,
          "Failed submission cannot claim pacing ownership");
    settings.fg.backend = FrameGeneration::Off;
    controller.request(settings.fg);
    check(controller.state().requested == 1 && !controller.state().active,
          "FG off must retain the full-rate ordinary path");
    check(output_buffer_bits(HdrOutput::Pq10) == 10 && output_buffer_bits(HdrOutput::ScRgb16Float) == 16,
          "Implemented output formats must report actual buffer precision");
    check(output_buffer_bits(HdrOutput::Pq12Requested) == 0,
          "A pending 12-bit display request must not masquerade as an implemented DXGI buffer");
}
} // namespace

int main() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("xivrender-core-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(folder);
        check_settings(folder);
        check_temporal();
        check_runtime_policies();
        check(std::filesystem::weakly_canonical(folder).parent_path() ==
                  std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()),
              "Test cleanup escaped its temporary parent");
        std::filesystem::remove_all(folder);
        std::cout << "PASS: settings round-trip/validation/atomic replacement, temporal phases, focus and "
                     "requested/observed MFG policies\n";
        return 0;
    } catch (const std::exception &error) {
        // Leave fixtures on failure so a failed atomic-replacement check can be inspected.
        std::cerr << "FAIL: " << error.what() << " (fixtures: " << folder.string() << ")\n";
        return 1;
    }
}
