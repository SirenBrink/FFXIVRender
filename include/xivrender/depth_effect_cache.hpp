#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace xivrender::depth {
// ReShade's texture/uniform handles and preprocessor values belong to an
// effect-load epoch. Macro edits reload effects; the runtime's reload/destroy
// callback discards this whole cache before any old handle is used again.
template <class Texture, class Uniform> struct EffectMetadata {
    Texture texture{};
    std::vector<Uniform> ready_uniforms;
    bool manual = false, loaded = false;

    template <class Runtime> void load(Runtime *runtime) {
        if (loaded)
            return;
        loaded = true;
        texture = runtime->find_texture_variable(nullptr, "ReShade::DepthBufferTex");
        if (!texture.handle)
            texture = runtime->find_texture_variable(nullptr, "DepthBufferTex");
        if (!texture.handle)
            return;
        const auto changed = [&](const char *effect) {
            for (const char *name : {"RESHADE_DEPTH_INPUT_X_SCALE", "RESHADE_DEPTH_INPUT_Y_SCALE"})
                if (!default_transform(runtime, effect, name, 1))
                    return true;
            for (const char *name :
                 {"RESHADE_DEPTH_INPUT_X_OFFSET", "RESHADE_DEPTH_INPUT_Y_OFFSET",
                  "RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET", "RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET"})
                if (!default_transform(runtime, effect, name, 0))
                    return true;
            return false;
        };
        manual = changed(nullptr);
        if (manual)
            return;
        std::unordered_set<std::string> effects;
        runtime->enumerate_techniques(nullptr, [&](auto *current, auto technique) {
            if (manual)
                return;
            char effect[1024]{};
            current->get_technique_effect_name(technique, effect);
            if (*effect && effects.emplace(effect).second)
                manual = changed(effect);
        });
        if (manual)
            return;
        std::unordered_set<decltype(Uniform{}.handle)> seen;
        runtime->enumerate_uniform_variables(nullptr, [&](auto *current, Uniform uniform) {
            char source[64]{};
            if (current->get_annotation_string_from_uniform_variable(uniform, "source", source) &&
                !std::strcmp(source, "bufready_depth") && seen.emplace(uniform.handle).second)
                ready_uniforms.push_back(uniform);
        });
    }

  private:
    template <class Runtime>
    static bool default_transform(Runtime *runtime, const char *effect, const char *name, double expected) {
        char value[128]{};
        const bool defined = effect ? runtime->get_preprocessor_definition_for_effect(effect, name, value)
                                    : runtime->get_preprocessor_definition(name, value);
        if (!defined)
            return true;
        char *end = nullptr;
        const double number = std::strtod(value, &end);
        while (end && (*end == ' ' || *end == 'f' || *end == 'F'))
            ++end;
        return end != value && end && !*end && std::isfinite(number) && number == expected;
    }
};
} // namespace xivrender::depth
