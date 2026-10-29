#include "xivrender/settings.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <string_view>
#include <system_error>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace xivrender {
namespace {

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto &c : result)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return result;
}

template <class T> bool parse(std::string_view text, T &value) {
    if constexpr (std::is_same_v<T, std::string>) {
        value = std::string(text);
        return true;
    } else if constexpr (std::is_same_v<T, bool>) {
        const auto name = lower(text);
        if (name == "true" || name == "1") {
            value = true;
            return true;
        }
        if (name == "false" || name == "0") {
            value = false;
            return true;
        }
        return false;
    } else if constexpr (std::is_enum_v<T>) {
        int number = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            return false;
        value = static_cast<T>(number);
        return true;
    } else {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            return false;
        if constexpr (std::is_floating_point_v<T>)
            return std::isfinite(value);
        else
            return true;
    }
}

template <class T> std::string format(T value) {
    if constexpr (std::is_same_v<T, std::string>)
        return value;
    else if constexpr (std::is_same_v<T, bool>)
        return value ? "true" : "false";
    else if constexpr (std::is_enum_v<T>)
        return format(static_cast<int>(value));
    else {
        std::array<char, 96> buffer{};
        const auto result = [&] {
            if constexpr (std::is_floating_point_v<T>)
                return std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                     std::chars_format::general, std::numeric_limits<T>::max_digits10);
            else
                return std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        }();
        return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : std::string{};
    }
}

struct Field {
    std::string section, key;
    std::function<std::string(const Settings &)> get;
    std::function<bool(Settings &, std::string_view)> set;
};

const std::vector<Field> &fields() {
#define FIELD(section, key, member)                                                                          \
    Field {                                                                                                  \
        section, key, [](const Settings &s) { return format(s.member); },                                    \
            [](Settings &s, std::string_view text) {                                                         \
                auto value = s.member;                                                                       \
                if (!parse(text, value))                                                                     \
                    return false;                                                                            \
                s.member = value;                                                                            \
                return true;                                                                                 \
            }                                                                                                \
    }
    static const std::vector<Field> list = [] {
        std::vector<Field> result = {
            FIELD("dlss", "enabled", dlss.enabled),
            FIELD("dlss", "quality", dlss.quality),
            FIELD("dlss", "model_preset", dlss.model_preset),
            FIELD("dlss", "hdr_input", dlss.hdr_input),
            FIELD("dlss", "automatic_mip_bias", dlss.automatic_mip_bias),
            FIELD("frame_generation", "enabled", fg.enabled),
            FIELD("frame_generation", "backend", fg.backend),
            FIELD("frame_generation", "factor", fg.factor),
            FIELD("frame_generation", "dynamic", fg.dynamic),
            FIELD("frame_generation", "target_fps", fg.target_fps),
            FIELD("frame_generation", "model_preset", fg.model_preset),
            FIELD("frame_generation", "hdr_input", fg.hdr_input),
            FIELD("frame_generation", "experimental_xefg_5x_6x", fg.experimental_xefg_5x_6x),
            FIELD("frame_generation", "reflex", fg.reflex),
            FIELD("frame_generation", "latency", fg.latency),
            FIELD("frame_generation", "xefg_unlock", fg.xefg_unlock),
            FIELD("frame_generation", "fsr_mfg", fg.fsr_mfg),
            FIELD("neural_rendering", "runtime_id", nr.runtime_id),
            FIELD("neural_rendering", "enabled", nr.enabled),
            FIELD("neural_rendering", "passes", nr.passes),
            FIELD("neural_rendering", "feedback", nr.feedback),
            FIELD("neural_rendering", "detail_strength", nr.detail_strength),
            FIELD("neural_rendering", "colour_strength", nr.colour_strength),
            FIELD("neural_rendering", "apply_model", nr.apply_model),
            FIELD("neural_rendering", "separate_skin", nr.separate_skin),
            FIELD("neural_rendering", "skin_colour", nr.skin_colour),
            FIELD("neural_rendering", "skin_detail", nr.skin_detail),
            FIELD("neural_rendering", "skin_colour_strength", nr.skin_colour_strength),
            FIELD("neural_rendering", "environment_detail", nr.environment_detail),
            FIELD("neural_rendering", "environment_colour", nr.environment_colour),
            FIELD("neural_rendering", "model_scale", nr.model_scale),
            FIELD("neural_rendering", "pause_on_focus_loss", nr.pause_on_focus_loss),
            FIELD("neural_rendering", "foveated", nr.foveated),
            FIELD("neural_rendering", "central_fraction", nr.central_fraction),
            FIELD("neural_rendering", "work_fraction", nr.work_fraction),
            FIELD("neural_rendering", "precision", nr.precision),
            FIELD("neural_rendering", "bottleneck_interval", nr.bottleneck_interval),
            FIELD("neural_rendering", "first_pass_reuse_only", nr.first_pass_reuse_only),
            FIELD("neural_rendering", "unverified_kernels", nr.unverified_kernels),
            FIELD("neural_rendering", "generate_before_sr_apply_after", nr.generate_before_sr_apply_after),
            FIELD("neural_rendering", "residual_history", nr.residual_history),
            FIELD("neural_rendering", "hdr_input", nr.hdr_input),
            FIELD("hdr", "enabled", hdr.enabled),
            FIELD("hdr", "output", hdr.output),
            FIELD("hdr", "peak_nits", hdr.peak_nits),
            FIELD("hdr", "paper_white_nits", hdr.paper_white_nits),
            FIELD("hdr", "black_nits", hdr.black_nits),
            FIELD("hdr", "saturation", hdr.saturation),
            FIELD("hdr", "vibrance", hdr.vibrance),
            FIELD("hdr", "contrast", hdr.contrast),
            FIELD("hdr", "preserve_reshade_highlights", hdr.preserve_reshade_highlights),
            FIELD("hdr", "screenshot_enabled", hdr.screenshot_enabled),
            FIELD("hdr", "screenshot_profile", hdr.screenshot_profile),
            FIELD("hdr", "screenshot_key", hdr.screenshot_key),
            FIELD("hdr", "upgrade_game_screenshots", hdr.upgrade_game_screenshots),
            FIELD("reshade", "automatic_depth", reshade.automatic_depth),
            FIELD("reshade", "rest_hud_isolation", reshade.rest_hud_isolation),
            FIELD("preview", "dlss", preview.dlss),
            FIELD("preview", "nr", preview.nr),
            FIELD("preview", "nr_instance_limit", preview.nr_instance_limit),
        };
        const auto add_profile = [&](const std::string &section, int index) {
            const auto add = [&](const char *key, auto member) {
                result.push_back(
                    {section, key,
                     [index, member](const Settings &s) {
                         return format((index < 0 ? s.nr.profile : s.nr.later_passes[index].profile).*member);
                     },
                     [index, member](Settings &s, std::string_view text) {
                         auto &p = index < 0 ? s.nr.profile : s.nr.later_passes[index].profile;
                         auto value = p.*member;
                         if (!parse(text, value))
                             return false;
                         p.*member = value;
                         return true;
                     }});
            };
            add("preset", &NrPassProfile::preset);
            add("style", &NrPassProfile::style);
            add("intensity", &NrPassProfile::intensity);
            add("structure", &NrPassProfile::structure);
            add("tone", &NrPassProfile::tone);
            add("skin", &NrPassProfile::skin);
            add("auto_mask", &NrPassProfile::auto_mask);
        };
        add_profile("nr_pass_1", -1);
        for (int i = 0; i < 7; ++i) {
            const auto section = "nr_pass_" + std::to_string(i + 2);
            add_profile(section, i);
            result.push_back({section, "inherit",
                              [i](const Settings &s) { return format(s.nr.later_passes[i].inherit); },
                              [i](Settings &s, std::string_view text) {
                                  return parse(text, s.nr.later_passes[i].inherit);
                              }});
        }
        return result;
    }();
#undef FIELD
    return list;
}

} // namespace

std::vector<std::string> normalize_settings(Settings &settings) {
    std::vector<std::string> warnings;
    const Settings defaults;
    const auto range = [&](auto &value, auto low, auto high, auto fallback, std::string_view name) {
        bool valid = value >= low && value <= high;
        if constexpr (std::is_floating_point_v<std::remove_reference_t<decltype(value)>>)
            valid = valid && std::isfinite(value);
        if (!valid) {
            value = fallback;
            warnings.emplace_back(std::string(name) + ": invalid value; default restored");
        }
    };
    const auto enumeration = [&](auto &value, int maximum, auto fallback, std::string_view name) {
        const auto number = static_cast<int>(value);
        if (number < 0 || number > maximum) {
            value = fallback;
            warnings.emplace_back(std::string(name) + ": unknown selection; default restored");
        }
    };
    enumeration(settings.dlss.quality, 6, defaults.dlss.quality, "dlss.quality");
    enumeration(settings.dlss.model_preset, 13, defaults.dlss.model_preset, "dlss.model_preset");
    enumeration(settings.fg.backend, 3, defaults.fg.backend, "frame_generation.backend");
    enumeration(settings.fg.latency, 4, defaults.fg.latency, "frame_generation.latency");
    range(settings.fg.factor, 2u, 6u, defaults.fg.factor, "frame_generation.factor");
    range(settings.fg.target_fps, 30u, 1000u, defaults.fg.target_fps, "frame_generation.target_fps");
    enumeration(settings.fg.model_preset, 3, defaults.fg.model_preset, "frame_generation.model_preset");
    if (settings.nr.runtime_id.empty() || settings.nr.runtime_id.size() > 64 ||
        settings.nr.runtime_id == "." || settings.nr.runtime_id == ".." ||
        !std::all_of(settings.nr.runtime_id.begin(), settings.nr.runtime_id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                   c == '.' || c == '_';
        })) {
        settings.nr.runtime_id = defaults.nr.runtime_id;
        warnings.emplace_back("neural_rendering.runtime_id: invalid runtime identifier; default restored");
    }
    range(settings.nr.passes, 1u, 8u, defaults.nr.passes, "neural_rendering.passes");
    const auto validate_profile = [&](NrPassProfile &p, const NrPassProfile &fallback,
                                      const std::string &name) {
        range(p.preset, 0u, 3u, fallback.preset, name + ".preset");
        range(p.style, 0u, 2u, fallback.style, name + ".style");
        range(p.intensity, 0.f, 2.f, fallback.intensity, name + ".intensity");
        range(p.structure, 0.f, 2.f, fallback.structure, name + ".structure");
        range(p.tone, 0.f, 2.f, fallback.tone, name + ".tone");
        range(p.skin, -1.f, 2.f, fallback.skin, name + ".skin");
    };
    validate_profile(settings.nr.profile, defaults.nr.profile, "nr_pass_1");
    for (unsigned i = 0; i < settings.nr.later_passes.size(); ++i)
        validate_profile(settings.nr.later_passes[i].profile, defaults.nr.later_passes[i].profile,
                         "nr_pass_" + std::to_string(i + 2));
    range(settings.nr.skin_detail, 0.f, 1.f, 1.f, "neural_rendering.skin_detail");
    range(settings.nr.skin_colour_strength, 0.f, 1.f, 1.f, "neural_rendering.skin_colour_strength");
    range(settings.nr.environment_detail, 0.f, 1.f, 1.f, "neural_rendering.environment_detail");
    range(settings.nr.environment_colour, 0.f, 1.f, 1.f, "neural_rendering.environment_colour");
    range(settings.nr.feedback, 0.f, 1.f, 1.f, "neural_rendering.feedback");
    range(settings.nr.detail_strength, 0.f, 2.f, 1.f, "neural_rendering.detail_strength");
    range(settings.nr.colour_strength, 0.f, 4.f, 1.f, "neural_rendering.colour_strength");
    range(settings.nr.model_scale, .25f, 2.0f, defaults.nr.model_scale, "neural_rendering.model_scale");
    range(settings.nr.central_fraction, .1f, .95f, defaults.nr.central_fraction,
          "neural_rendering.central_fraction");
    range(settings.nr.work_fraction, .25f, 1.0f, defaults.nr.work_fraction, "neural_rendering.work_fraction");
    if (settings.nr.central_fraction >= settings.nr.work_fraction) {
        settings.nr.central_fraction = defaults.nr.central_fraction;
        settings.nr.work_fraction = defaults.nr.work_fraction;
        warnings.emplace_back(
            "neural_rendering: work fraction must exceed the central region; defaults restored");
    }
    enumeration(settings.nr.precision, 0, defaults.nr.precision, "neural_rendering.precision");
    range(settings.nr.bottleneck_interval, 1u, 4u, defaults.nr.bottleneck_interval,
          "neural_rendering.bottleneck_interval");
    range(settings.nr.residual_history, 0.0f, .95f, defaults.nr.residual_history,
          "neural_rendering.residual_history");
    enumeration(settings.hdr.output, 3, defaults.hdr.output, "hdr.output");
    range(settings.hdr.peak_nits, 80.0f, 10000.0f, defaults.hdr.peak_nits, "hdr.peak_nits");
    range(settings.hdr.paper_white_nits, 40.0f, 1000.0f, defaults.hdr.paper_white_nits,
          "hdr.paper_white_nits");
    if (settings.hdr.paper_white_nits > settings.hdr.peak_nits) {
        settings.hdr.paper_white_nits = std::min(defaults.hdr.paper_white_nits, settings.hdr.peak_nits);
        warnings.emplace_back("hdr.paper_white_nits: exceeds peak; restored within peak");
    }
    range(settings.hdr.black_nits, 0.0f, 10.0f, defaults.hdr.black_nits, "hdr.black_nits");
    range(settings.hdr.saturation, 0.0f, 2.0f, defaults.hdr.saturation, "hdr.saturation");
    range(settings.hdr.vibrance, -1.0f, 1.0f, defaults.hdr.vibrance, "hdr.vibrance");
    range(settings.hdr.contrast, .1f, 2.0f, defaults.hdr.contrast, "hdr.contrast");
    enumeration(settings.hdr.screenshot_profile, 1, defaults.hdr.screenshot_profile,
                "hdr.screenshot_profile");
    range(settings.hdr.screenshot_key, 1u, 255u, defaults.hdr.screenshot_key, "hdr.screenshot_key");
    range(settings.preview.nr_instance_limit, 1u, 4u, defaults.preview.nr_instance_limit,
          "preview.nr_instance_limit");
    return warnings;
}

LoadResult load_settings(const std::filesystem::path &path) {
    LoadResult result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        std::error_code error;
        if (std::filesystem::exists(path, error))
            result.warnings.emplace_back("Settings file could not be read; defaults retained");
        return result;
    }
    result.found = true;
    std::string section, line;
    unsigned line_number = 0;
    const Settings defaults;
    while (std::getline(stream, line)) {
        ++line_number;
        if (line_number == 1 && line.starts_with("\xef\xbb\xbf"))
            line.erase(0, 3);
        const auto clean = trim(line);
        if (clean.empty() || clean[0] == ';' || clean[0] == '#')
            continue;
        if (clean[0] == '[' && clean.back() == ']') {
            section = lower(trim(std::string_view(clean).substr(1, clean.size() - 2)));
            continue;
        }
        const auto equal = clean.find('=');
        if (equal == std::string::npos) {
            result.warnings.emplace_back("Malformed settings line " + std::to_string(line_number));
            continue;
        }
        const auto key = lower(trim(std::string_view(clean).substr(0, equal)));
        const auto raw_value = std::string_view(clean).substr(equal + 1);
        const auto value = trim(raw_value.substr(0, raw_value.find_first_of(";#")));
        if (section == "xivrender" && key == "schema") {
            unsigned schema = 0;
            if (!parse(value, schema) || schema != 1) {
                result.settings = defaults;
                result.warnings.emplace_back("Unsupported settings schema; defaults retained");
                return result;
            }
            continue;
        }
        for (const auto &field : fields())
            if (field.section == section && field.key == key) {
                if (!field.set(result.settings, value)) {
                    field.set(result.settings, field.get(defaults));
                    result.warnings.emplace_back(section + "." + key + ": malformed value; default restored");
                }
                break;
            }
    }
    if (stream.bad()) {
        result.settings = defaults;
        result.warnings.emplace_back("Settings read failed; partial settings discarded");
        return result;
    }
    auto validation = normalize_settings(result.settings);
    result.warnings.insert(result.warnings.end(), validation.begin(), validation.end());
    return result;
}

static bool write_settings(const std::filesystem::path &path, const Settings &settings, std::string &error,
                           bool nr_only) {
    error.clear();
    auto validated = settings;
    const auto warnings = normalize_settings(validated);
    if (!warnings.empty()) {
        error = warnings.front();
        return false;
    }
    static std::atomic<std::uint64_t> serial{0};
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(++serial);
    try {
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Could not create temporary settings file";
            return false;
        }
        stream << "; XIVRender settings. Runtime support is checked independently.\n[xivrender]\nschema=1\n";
        std::string_view section;
        if (nr_only)
            stream << "\n[nr_profile]\nschema=1\n";
        for (const auto &field : fields()) {
            if (nr_only && !(field.section.starts_with("nr_pass_") ||
                             (field.section == "neural_rendering" && field.key != "runtime_id" &&
                              field.key != "enabled" && field.key != "pause_on_focus_loss")))
                continue;
            if (section != field.section) {
                section = field.section;
                stream << "\n[" << section << "]\n";
            }
            stream << field.key << '=' << field.get(settings) << '\n';
        }
        stream.flush();
        if (!stream) {
            stream.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            error = "Settings write failed";
            return false;
        }
        stream.close();
        if (!stream) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            error = "Settings close failed";
            return false;
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(int(GetLastError()), std::system_category(),
                                    "Could not replace settings file");
#else
        std::filesystem::rename(temporary, path);
#endif
        return true;
    } catch (const std::exception &exception) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = exception.what();
        return false;
    }
}

bool save_settings(const std::filesystem::path &path, const Settings &s, std::string &error) {
    return write_settings(path, s, error, false);
}
bool save_nr_profile(const std::filesystem::path &path, const NeuralRenderingSettings &nr,
                     std::string &error) {
    Settings s;
    s.nr = nr;
    return write_settings(path, s, error, true);
}
bool load_nr_profile(const std::filesystem::path &path, NeuralRenderingSettings &nr, std::string &error) {
    error.clear();
    try {
        if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 1024 * 1024) {
            error = "NR profile is missing or exceeds 1 MB";
            return false;
        }
        // A full settings file or unrelated INI must never reset NR by accident.
        std::ifstream input(path, std::ios::binary);
        bool profile_header = false, supported = false;
        std::string line;
        while (std::getline(input, line)) {
            const auto clean = trim(line);
            if (!clean.empty() && clean.front() == '[' && clean.back() == ']')
                profile_header = lower(clean) == "[nr_profile]";
            else if (profile_header && clean.starts_with("schema=")) {
                unsigned schema = 0;
                if (!parse(std::string_view(clean).substr(7), schema) || schema != 1) {
                    error = "Unsupported NR profile file";
                    return false;
                }
                supported = true;
            }
        }
        if (input.bad() || !supported) {
            error = "Unsupported NR profile file";
            return false;
        }
        auto result = load_settings(path);
        if (!result.found || !result.warnings.empty()) {
            error = result.warnings.empty() ? "Cannot read NR profile" : result.warnings.front();
            return false;
        }
        result.settings.nr.enabled = nr.enabled;
        result.settings.nr.runtime_id = nr.runtime_id;
        result.settings.nr.pause_on_focus_loss = nr.pause_on_focus_loss;
        nr = std::move(result.settings.nr);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

} // namespace xivrender
