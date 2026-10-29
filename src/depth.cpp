// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from OptiScaler misc/FfxivReShadeDepth.cpp (SirenBrink fork).
#include <xivrender/depth.hpp>
#include <xivrender/app.hpp>
#include <xivrender/depth_effect_cache.hpp>

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xivrender::depth {
namespace {
namespace api = reshade::api;
using Microsoft::WRL::ComPtr;

struct Binding {
    api::resource_view linear{}, srgb{};
    ComPtr<ID3D11ShaderResourceView> keep_linear, keep_srgb, keep_capture;
    std::vector<std::pair<api::effect_uniform_variable, bool>> ready;
    bool active = false;
};
struct Data {
    std::recursive_mutex mutex;
    CaptureImage image;
    ComPtr<ID3D11Device> native_device;
    std::unordered_map<api::effect_runtime *, Binding> bindings;
    std::unordered_map<api::effect_runtime *,
                       EffectMetadata<api::effect_texture_variable, api::effect_uniform_variable>>
        effects;
    unsigned out_width = 0, out_height = 0;
    ULONGLONG produced = 0;
    bool consumer = false;
    std::string message = "Waiting for scene depth";
};
Data &state() {
    static auto *value = new Data;
    return *value;
}

void restore(api::effect_runtime *runtime, Binding &binding) {
    if (!binding.active)
        return;
    runtime->update_texture_bindings("DEPTH", binding.linear, binding.srgb);
    for (const auto &[variable, value] : binding.ready)
        runtime->set_uniform_value_bool(variable, &value, 1);
    binding = Binding{};
}

} // namespace

void capture(ID3D11DeviceContext *context, ID3D11Texture2D *source, unsigned width, unsigned height,
             unsigned x, unsigned y, unsigned outputWidth, unsigned outputHeight, bool variable_resolution) {
    auto &data = state();
    std::lock_guard lock(data.mutex);
    data.produced = 0;
    if (!settings_snapshot().reshade.automatic_depth) {
        data.message = "Automatic ReShade depth disabled";
        return;
    }
    if (!context || !source || !outputWidth || !outputHeight ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        data.message = "Scene depth unavailable; generic depth retained";
        return;
    }
    data.out_width = outputWidth;
    data.out_height = outputHeight;
    // A concurrent/interleaved effect group still owns this image. Do not
    // overwrite it or allocate another full-size image until the group ends.
    for (const auto &[runtime, binding] : data.bindings) {
        (void)runtime;
        if (binding.active) {
            data.message = "Scene depth capture deferred until effects finish";
            return;
        }
    }
    if (!data.consumer || !data.native_device) {
        data.message = "Waiting for a ReShade depth consumer";
        return;
    }
    // Native device identity comes from the supported ReShade runtime API.
    // Resource GetDevice may have been redirected to ReShade's proxy.
    if (!data.image.Capture(source, width, height, x, y, outputWidth, outputHeight, data.native_device.Get(),
                            variable_resolution)) {
        data.message = "Scene depth capture unavailable; generic depth retained";
        return;
    }
    data.produced = GetTickCount64();
    data.message = "Automatic scene depth: " + std::to_string(width) + "x" + std::to_string(height) + " -> " +
                   std::to_string(data.image.Width()) + "x" + std::to_string(data.image.Height());
}

void begin_effects(api::effect_runtime *runtime, api::command_list *, api::resource_view,
                   api::resource_view) {
    if (!runtime || !runtime->get_device() || runtime->get_device()->get_api() != api::device_api::d3d11)
        return;
    auto &data = state();
    std::lock_guard lock(data.mutex);
    if (!settings_snapshot().reshade.automatic_depth)
        return;
    auto &effects = data.effects[runtime];
    effects.load(runtime);
    if (!effects.texture.handle)
        return;
    if (effects.manual) {
        data.message = "Manual ReShade depth transform retained";
        return;
    }
    uint32_t width = 0, height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);
    const auto native = reinterpret_cast<ID3D11Device *>(runtime->get_device()->get_native());
    data.consumer = true;
    if (!data.out_width || (width == data.out_width && height == data.out_height))
        data.native_device = native;
    // Both compact fixed-ratio depth and output-sized DRS depth expose the
    // complete active crop through normalized UVs. Match the capture's intended
    // output window, not the depth resource's storage dimensions.
    if (!data.image.View() || data.image.Device() != native || !data.produced || width != data.out_width ||
        height != data.out_height || GetTickCount64() - data.produced >= 250)
        return;
    auto &binding = data.bindings[runtime];
    if (binding.active)
        return;
    runtime->get_texture_binding(effects.texture, &binding.linear, &binding.srgb);
    binding.keep_linear = reinterpret_cast<ID3D11ShaderResourceView *>(binding.linear.handle);
    binding.keep_srgb = reinterpret_cast<ID3D11ShaderResourceView *>(binding.srgb.handle);
    binding.keep_capture = data.image.View();
    const api::resource_view compact{reinterpret_cast<uint64_t>(data.image.View())};
    binding.active = true;
    runtime->update_texture_bindings("DEPTH", compact, compact);
    for (const auto uniform : effects.ready_uniforms) {
        bool previous = false;
        runtime->get_uniform_value_bool(uniform, &previous, 1);
        binding.ready.emplace_back(uniform, previous);
        const bool available = true;
        runtime->set_uniform_value_bool(uniform, &available, 1);
    }
}

void end_effects(api::effect_runtime *runtime, api::command_list *, api::resource_view, api::resource_view) {
    if (!runtime)
        return;
    auto &data = state();
    std::lock_guard lock(data.mutex);
    if (auto found = data.bindings.find(runtime); found != data.bindings.end())
        restore(runtime, found->second);
}

void forget_runtime(api::effect_runtime *runtime) {
    if (!runtime)
        return;
    auto &data = state();
    std::lock_guard lock(data.mutex);
    // ReShade has invalidated handles at reload/destroy time. Never call them.
    data.bindings.erase(runtime);
    data.effects.erase(runtime);
    if (data.bindings.empty()) {
        data.image.Reset();
        data.native_device.Reset();
        data.produced = 0;
        data.consumer = false;
        data.message = "Waiting for a ReShade depth consumer";
    }
}

void reset() {
    auto &data = state();
    std::lock_guard lock(data.mutex);
    // Called on the render/lifecycle thread, after runtime-destroy callbacks
    // have removed dead runtimes. Restore any still-valid active effect group.
    for (auto &[runtime, binding] : data.bindings)
        restore(runtime, binding);
    data.bindings.clear();
    data.effects.clear();
    data.image.Reset();
    data.native_device.Reset();
    data.consumer = false;
    data.produced = 0;
    data.out_width = data.out_height = 0;
    data.message = "Waiting for scene depth";
}

std::string status() {
    auto &data = state();
    std::lock_guard lock(data.mutex);
    return data.message;
}

} // namespace xivrender::depth
