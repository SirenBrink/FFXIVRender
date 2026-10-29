// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from OptiScaler tests/reshade_depth_capture.cpp (SirenBrink fork).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <xivrender/depth.hpp>
#include <xivrender/app.hpp>
#include <xivrender/depth_effect_cache.hpp>
#include <vector>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <string>
#include <unordered_map>
namespace xivrender {
Settings settings_snapshot() {
    return {};
}
} // namespace xivrender
using Microsoft::WRL::ComPtr;
ID3D11Device *proxyOwner = nullptr;
void STDMETHODCALLTYPE ProxyGetDevice(ID3D11Texture2D *, ID3D11Device **out) {
    *out = proxyOwner;
    proxyOwner->AddRef();
}
void check(HRESULT h) {
    assert(SUCCEEDED(h));
}
namespace {
struct EffectHandle {
    std::uint64_t handle = 0;
    bool operator==(const EffectHandle &) const = default;
};
struct EffectRuntimeMock {
    struct Counters {
        unsigned texture_lookups = 0, techniques = 0, effect_names = 0, global_definitions = 0,
                 effect_definitions = 0, uniforms = 0, annotations = 0;
        bool operator==(const Counters &) const = default;
    } calls;
    std::vector<std::string> effects{"A.fx", "A.fx", "B.fx", "A.fx", "B.fx"};
    std::vector<std::pair<EffectHandle, std::string>> uniforms{
        {{1}, "bufready_depth"}, {{2}, "frametime"}, {{3}, "bufready_depth"}, {{3}, "bufready_depth"}};
    std::unordered_map<std::string, std::string> global_definitions, effect_definitions;
    std::uint64_t texture = 100;
    bool have_texture = true, fallback_name = false;

    EffectHandle find_texture_variable(const char *, const char *name) {
        ++calls.texture_lookups;
        return {have_texture && (!fallback_name || !std::strcmp(name, "DepthBufferTex")) ? texture : 0};
    }
    template <class F> void enumerate_techniques(const char *, F callback) {
        ++calls.techniques;
        for (std::size_t i = 0; i < effects.size(); ++i)
            callback(this, EffectHandle{i + 1});
    }
    template <std::size_t N> void get_technique_effect_name(EffectHandle technique, char (&out)[N]) {
        ++calls.effect_names;
        std::snprintf(out, N, "%s", effects.at(technique.handle - 1).c_str());
    }
    template <std::size_t N> bool get_preprocessor_definition(const char *name, char (&out)[N]) {
        ++calls.global_definitions;
        const auto found = global_definitions.find(name);
        if (found == global_definitions.end())
            return false;
        std::snprintf(out, N, "%s", found->second.c_str());
        return true;
    }
    template <std::size_t N>
    bool get_preprocessor_definition_for_effect(const char *effect, const char *name, char (&out)[N]) {
        ++calls.effect_definitions;
        const auto found = effect_definitions.find(std::string(effect) + ':' + name);
        if (found == effect_definitions.end())
            return false;
        std::snprintf(out, N, "%s", found->second.c_str());
        return true;
    }
    template <class F> void enumerate_uniform_variables(const char *, F callback) {
        ++calls.uniforms;
        for (const auto &uniform : uniforms)
            callback(this, uniform.first);
    }
    template <std::size_t N>
    bool get_annotation_string_from_uniform_variable(EffectHandle variable, const char *name,
                                                     char (&out)[N]) {
        ++calls.annotations;
        assert(!std::strcmp(name, "source"));
        for (const auto &uniform : uniforms)
            if (uniform.first == variable) {
                std::snprintf(out, N, "%s", uniform.second.c_str());
                return true;
            }
        return false;
    }
};
void cached_effect_metadata() {
    using Cache = xivrender::depth::EffectMetadata<EffectHandle, EffectHandle>;
    EffectRuntimeMock runtime;
    Cache cache;
    cache.load(&runtime);
    assert(cache.loaded && !cache.manual && cache.texture.handle == 100);
    assert(cache.ready_uniforms.size() == 2 && cache.ready_uniforms[0].handle == 1 &&
           cache.ready_uniforms[1].handle == 3);
    assert(runtime.calls.texture_lookups == 1 && runtime.calls.techniques == 1 &&
           runtime.calls.effect_names == 5 && runtime.calls.global_definitions == 6 &&
           runtime.calls.effect_definitions == 12 && runtime.calls.uniforms == 1 &&
           runtime.calls.annotations == 4);
    const auto first = runtime.calls;
    for (unsigned frame = 0; frame < 10000; ++frame)
        cache.load(&runtime);
    assert(runtime.calls == first); // No periodic scans or per-frame uniform enumeration.

    // A macro edit reloads ReShade effects. Production forget_runtime erases
    // this entry, so fresh texture/uniform handles and definitions are read.
    runtime.texture = 200;
    runtime.uniforms = {{{7}, "bufready_depth"}, {{8}, "frametime"}};
    runtime.effect_definitions["B.fx:RESHADE_DEPTH_INPUT_X_SCALE"] = "0.5";
    cache = {};
    cache.load(&runtime);
    assert(cache.manual && cache.texture.handle == 200 && cache.ready_uniforms.empty());
    assert(runtime.calls.uniforms == first.uniforms); // Manual transforms retain generic depth.
    runtime.effect_definitions.clear();
    runtime.global_definitions["RESHADE_DEPTH_INPUT_X_SCALE"] = "1.0f";
    runtime.global_definitions["RESHADE_DEPTH_INPUT_Y_SCALE"] = "1 ";
    runtime.global_definitions["RESHADE_DEPTH_INPUT_X_OFFSET"] = "0.0F";
    cache = {};
    cache.load(&runtime);
    assert(!cache.manual && cache.ready_uniforms.size() == 1 && cache.ready_uniforms[0].handle == 7);
    const auto reloaded = runtime.calls;
    for (unsigned frame = 0; frame < 1000; ++frame)
        cache.load(&runtime);
    assert(runtime.calls == reloaded);

    runtime.global_definitions["RESHADE_DEPTH_INPUT_Y_OFFSET"] = "0.125";
    cache = {};
    cache.load(&runtime);
    assert(cache.manual && cache.ready_uniforms.empty() && runtime.calls.uniforms == reloaded.uniforms);
    runtime.global_definitions.clear();
    runtime.global_definitions["RESHADE_DEPTH_INPUT_X_SCALE"] = "not a number";
    cache = {};
    cache.load(&runtime);
    assert(cache.manual);

    // A runtime without a depth effect is also cached until effect reload.
    EffectRuntimeMock absent;
    absent.have_texture = false;
    Cache missing;
    for (unsigned frame = 0; frame < 1000; ++frame)
        missing.load(&absent);
    assert(!missing.texture.handle && absent.calls.texture_lookups == 2 && !absent.calls.techniques &&
           !absent.calls.uniforms);
    absent.have_texture = true;
    absent.fallback_name = true;
    missing = {};
    missing.load(&absent);
    assert(missing.texture.handle == 100 && absent.calls.texture_lookups == 4);

    // Each runtime owns independent handles and manual-transform policy.
    assert(cache.manual && !missing.manual && missing.ready_uniforms.size() == 2);
}
} // namespace
int main() {
    cached_effect_metadata();
    ComPtr<ID3D11Device> d;
    ComPtr<ID3D11DeviceContext> c;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d,
                            nullptr, &c));
    for (auto format : {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R16_UNORM,
                        DXGI_FORMAT_R24G8_TYPELESS}) {
        constexpr unsigned W = 12, H = 8;
        std::vector<float> pixels(W * H);
        std::vector<unsigned> packed(W * H);
        std::vector<unsigned short> packed16(W * H);
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x) {
                auto i = y * W + x;
                // Distinct foreground/background steps and a nonuniform pattern
                // expose padding, shifted crops and accidental depth filtering.
                pixels[i] =
                    (x < W / 2 ? .2f : .8f) + (y < H / 2 ? .01f : .07f) + float((x * 7 + y * 3) % 5) * .003f;
                packed[i] = unsigned(pixels[i] * 16777215);
                packed16[i] = static_cast<unsigned short>(pixels[i] * 65535);
            }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = W;
        td.Height = H;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = format;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (format == DXGI_FORMAT_R32_TYPELESS || format == DXGI_FORMAT_R24G8_TYPELESS)
            td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
        D3D11_SUBRESOURCE_DATA data{format == DXGI_FORMAT_R16_UNORM        ? (void *)packed16.data()
                                    : format == DXGI_FORMAT_R24G8_TYPELESS ? (void *)packed.data()
                                                                           : (void *)pixels.data(),
                                    format == DXGI_FORMAT_R16_UNORM ? W * 2 : W * 4, 0};
        ComPtr<ID3D11Texture2D> source;
        check(d->CreateTexture2D(&td, &data, &source));
        xivrender::depth::CaptureImage image;
        D3D11_VIEWPORT original{1, 2, 8, 5, 0, 1};
        c->RSSetViewports(1, &original);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        struct CaptureCase {
            unsigned w, h, x, y, out_w, out_h;
            std::vector<unsigned> columns, rows;
            bool full_resolution;
        };
        // Expected source texels are written explicitly from normalized pixel
        // centres. In particular, 7 -> 16 has uneven repeats, and downsampling
        // 7 -> 3 selects interior texels 1/3/5 rather than the crop endpoints.
        const CaptureCase cases[]{
            {4,
             3,
             2,
             1,
             16,
             12,
             {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3},
             {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2},
             true},
            {7,
             4,
             2,
             1,
             16,
             12,
             {0, 0, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 5, 6, 6},
             {0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3},
             true},
            {12,
             8,
             0,
             0,
             16,
             12,
             {0, 1, 1, 2, 3, 4, 4, 5, 6, 7, 7, 8, 9, 10, 10, 11},
             {0, 1, 1, 2, 3, 3, 4, 5, 5, 6, 7, 7},
             true},
            {4,
             3,
             7,
             4,
             16,
             12,
             {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3},
             {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2},
             true},
            {4, 3, 1, 2, 9, 7, {0, 0, 1, 1, 2, 2, 2, 3, 3}, {0, 0, 1, 1, 1, 2, 2}, true},
            {7, 4, 2, 1, 3, 2, {1, 3, 5}, {1, 3}, true},
            // Fixed ratios retain the smaller texture and exact active crop.
            // Its normalized UVs cover the same scene at either window size.
            {4, 3, 2, 1, 16, 12, {0, 1, 2, 3}, {0, 1, 2}, false},
            {4, 3, 7, 4, 9, 7, {0, 1, 2, 3}, {0, 1, 2}, false},
            {6, 4, 3, 2, 16, 12, {0, 1, 2, 3, 4, 5}, {0, 1, 2, 3}, false},
        };
        ComPtr<ID3D11Resource> previous_resource;
        ComPtr<ID3D11ShaderResourceView> previous_view;
        unsigned previous_width = 0, previous_height = 0;
        for (const auto &test : cases) {
            const auto target_w = test.full_resolution ? test.out_w : test.w;
            const auto target_h = test.full_resolution ? test.out_h : test.h;
            assert(test.columns.size() == target_w && test.rows.size() == target_h);
            assert(image.Capture(source.Get(), test.w, test.h, test.x, test.y, test.out_w, test.out_h,
                                 nullptr, test.full_resolution));
            assert(image.Width() == target_w && image.Height() == target_h);
            D3D11_PRIMITIVE_TOPOLOGY topology;
            c->IAGetPrimitiveTopology(&topology);
            assert(topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            D3D11_VIEWPORT restored{};
            UINT count = 1;
            c->RSGetViewports(&count, &restored);
            assert(restored.TopLeftX == original.TopLeftX && restored.Width == original.Width);
            ComPtr<ID3D11Resource> resource;
            image.View()->GetResource(&resource);
            if (previous_resource) {
                const bool same_output = previous_width == target_w && previous_height == target_h;
                assert((resource == previous_resource) == same_output);
                assert((image.View() == previous_view.Get()) == same_output);
            }
            previous_resource = resource;
            previous_view = image.View();
            previous_width = target_w;
            previous_height = target_h;
            D3D11_TEXTURE2D_DESC readDesc{};
            readDesc.Width = target_w;
            readDesc.Height = target_h;
            readDesc.MipLevels = readDesc.ArraySize = readDesc.SampleDesc.Count = 1;
            readDesc.Format = DXGI_FORMAT_R32_FLOAT;
            readDesc.Usage = D3D11_USAGE_STAGING;
            readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> read;
            check(d->CreateTexture2D(&readDesc, nullptr, &read));
            c->CopyResource(read.Get(), resource.Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            check(c->Map(read.Get(), 0, D3D11_MAP_READ, 0, &map));
            for (unsigned row = 0; row < target_h; ++row)
                for (unsigned col = 0; col < target_w; ++col) {
                    float got = ((float *)((char *)map.pData + row * map.RowPitch))[col];
                    const auto expected = pixels[(test.rows[row] + test.y) * W + test.columns[col] + test.x];
                    assert(std::abs(got - expected) < .00004f);
                }
            c->Unmap(read.Get(), 0);
        }
        if (format == DXGI_FORMAT_R32_FLOAT) {
            // ReShade replaces GetDevice on native resource vtables with its proxy.
            // The runtime's native device must remain the capture/binding authority.
            ComPtr<ID3D11Device> other;
            check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                    &other, nullptr, nullptr));
            proxyOwner = other.Get();
            auto **object = reinterpret_cast<void ***>(source.Get());
            auto *originalTable = *object;
            void *table[11];
            memcpy(table, originalTable, sizeof(table));
            table[3] = reinterpret_cast<void *>(&ProxyGetDevice);
            *object = table;
            ComPtr<ID3D11Device> observed;
            source->GetDevice(&observed);
            assert(observed == other && observed != d);
            xivrender::depth::CaptureImage proxyImage;
            const bool captured = proxyImage.Capture(source.Get(), 4, 3, 0, 0, 8, 6, d.Get());
            *object = originalTable; // Restore before any resource teardown.
            assert(captured && proxyImage.Device() == d.Get() && proxyImage.Width() == 4 &&
                   proxyImage.Height() == 3); // Compact storage is the default.
        }
        assert(!image.Capture(source.Get(), 13, 1, 0, 0, 16, 12));
        assert(!image.Capture(source.Get(), 1, 1, ~0u, 0, 16, 12));
        assert(!image.Capture(source.Get(), 0, 1, 0, 0, 16, 12));
        assert(!image.Capture(source.Get(), 4, 3, 0, 0, 0, 12));
        assert(!image.Capture(source.Get(), 4, 3, 0, 0, 16, 0));
        assert(!image.Capture(source.Get(), 4, 3, 0, 0, 16385, 12));
        image.Reset();
        assert(!image.View() && !image.Device());
    }
    puts("PASS: native DRS depth point-samples changing crops into reusable output-sized storage; fixed "
         "ratios retain compact storage and normalized crop UVs; pipeline state, R32/typeless/R16/D24 "
         "formats and rectangle/dimension validation are preserved");
}
