// Adapted from the SirenBrink OptiScaler fork, GPL-3.0-or-later.
#pragma once
#include <d3d11_1.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <functional>
#include "hdr.hpp"
#include "renodx/SceneShaders.h"
#include "hdr_ui.hpp"
#include <cmath>
#include <algorithm>
#include <sstream>
#include <string_view>

// RenoDX FFXIV shaders run in an independent FP16 branch. This deliberately
// leaves native SDR and ReShade's SDR effects unchanged. DLSS/NR may opt into
// private HDR inputs without changing the game's colour textures.
namespace xivrender::hdr::scene {
template <class... T> inline void log(const char *message, T &&...values) {
    std::ostringstream line;
    std::string_view rest(message);
    const auto append = [&](const auto &value) {
        const auto at = rest.find("{}");
        if (at != std::string_view::npos) {
            line << rest.substr(0, at) << value;
            rest.remove_prefix(at + 2);
        }
    };
    (append(values), ...);
    line << rest;
    hdr::report(line.str().c_str());
}
#define HDR_LOG(...) log(__VA_ARGS__)

using Microsoft::WRL::ComPtr;
inline constexpr GUID shaderTag{0xe3e16e11, 0x412b, 0x458b, {0x9d, 0x2c, 0x3a, 0x81, 0xb5, 0x29, 0xaa, 0x01}};
inline bool Requested() {
    return hdr::active();
}
inline UINT Stage(UINT crc) {
    switch (crc) {
    case 0x85e777ef:
        return 1;
    case 0xf8f57f0a:
        return 2;
    case 0x27ebc404:
        return 3;
    case 0x1f264d17:
        return 4;
    case 0xf6e81a1b:
        return 5;
    default:
        return 0;
    }
}
struct Slot {
    ComPtr<ID3D11Texture2D> hdr, reference;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;

    std::shared_ptr<Slot> owner; // Pins stable GPU storage for command-list metadata.
    UINT width = 0, height = 0, stage = 0;
    ULONGLONG lastUse = 0;
    D3D11_VIEWPORT viewport{};
    std::array<float, 4> crop{};
    DXGI_FORMAT referenceFormat = DXGI_FORMAT_UNKNOWN;
};
struct Alias {
    ComPtr<ID3D11Resource> native;
    std::shared_ptr<Slot> image;
};
struct Operation {
    enum class Kind { Invalidate, Draw, Copy } kind;
    ComPtr<ID3D11Resource> target, source;
    std::shared_ptr<Slot> image;
};
struct Graph {
    std::unordered_map<ID3D11Resource *, Alias> aliases;
    std::vector<Operation> operations;
    std::vector<std::shared_ptr<ui::Slot>> masks;
};
struct Data {
    std::mutex mutex;
    ComPtr<ID3D11Device> device;
    std::array<ComPtr<ID3D11PixelShader>, 5> shaders;
    std::unordered_map<ID3D11Resource *, Alias> shadows, aliases;
    std::shared_ptr<Slot> latest;

    uint64_t pixel_shaders = 0, tagged_shaders = 0, draws = 0;
    uint64_t captures = 0, recorded_lists = 0, executed_lists = 0, deferred_draws = 0;
    std::string last_reject;
    uint64_t requests = 0, available = 0, missing = 0;
    std::array<bool, 5> observed{};
    std::array<std::array<bool, 24>, 5> rejected{};
};
inline Data &State() {
    static auto *data = new Data;
    return *data;
} // No COM destruction under loader lock.
inline void Tag(ID3D11PixelShader *shader, UINT crc) {
    auto stage = Stage(crc);
    {
        auto &s = State();
        std::lock_guard lock(s.mutex);
        ++s.pixel_shaders;
        if (stage)
            ++s.tagged_shaders;
    }
    if (ui::Tag(shader, crc))
        stage = 6; // UI only; never a scene replay stage.
    if (stage)
        shader->SetPrivateData(shaderTag, sizeof(stage), &stage);
}
inline void Reject(Data &s, UINT stage, UINT reason, const char *description) {
    if (stage && stage <= 5 && reason < s.rejected[0].size() && !s.rejected[stage - 1][reason]) {
        s.rejected[stage - 1][reason] = true;
        s.last_reject = "stage " + std::to_string(stage) + ": " + description;
        hdr::report(
            ("RenoDX scene HDR: stage " + std::to_string(stage) + " rejected: " + description).c_str());
    }
}
inline DXGI_FORMAT Typed(DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_R16G16B16A16_TYPELESS)
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    return f;
}
inline bool Allocate(Slot &s, ID3D11Device *d, const D3D11_TEXTURE2D_DESC &input) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = input.Width;
    td.Height = input.Height;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = 0;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (FAILED(d->CreateTexture2D(&td, nullptr, &s.hdr)) ||
        FAILED(d->CreateRenderTargetView(s.hdr.Get(), nullptr, &s.rtv)) ||
        FAILED(d->CreateShaderResourceView(s.hdr.Get(), nullptr, &s.srv)))
        return false;
    td.Format = Typed(input.Format);
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d->CreateTexture2D(&td, nullptr, &s.reference)))
        return false;
    s.width = td.Width;
    s.height = td.Height;
    s.referenceFormat = td.Format;
    return true;
}
inline void Invalidate(Data &s, Graph *graph, ID3D11Resource *native) {
    auto &aliases = graph ? graph->aliases : s.aliases;
    // Recording order can differ from execution order. A target that has no
    // shadow yet may acquire one before this list executes, so record native
    // writes conservatively. Consecutive writes to one target coalesce.
    if (graph) {
        aliases[native] = {native, {}};
        if (graph->operations.empty() || graph->operations.back().kind != Operation::Kind::Invalidate ||
            graph->operations.back().target.Get() != native)
            graph->operations.push_back({Operation::Kind::Invalidate, native, {}, {}});
    } else
        aliases.erase(native);
}
inline std::shared_ptr<Slot> Shadow(Data &s, ID3D11Resource *native, ID3D11Device *device,
                                    const D3D11_TEXTURE2D_DESC &desc) {
    auto found = s.shadows.find(native);
    if (found != s.shadows.end()) {
        found->second.image->lastUse = GetTickCount64();
        return found->second.image;
    }
    if (s.shadows.size() >= 16)
        return {};
    auto slot = std::make_shared<Slot>();
    if (!Allocate(*slot, device, desc))
        return {};
    slot->lastUse = GetTickCount64();
    s.shadows[native] = {native, slot};
    return slot;
}
inline std::shared_ptr<Slot> Upstream(Data &s, Graph *graph, ID3D11Resource *native) {
    if (graph) {
        if (auto found = graph->aliases.find(native); found != graph->aliases.end())
            return found->second.image;
        // GPU storage is stable across recording contexts. Actual validity and
        // execution order are checked again when the command list executes.
        if (auto found = s.shadows.find(native); found != s.shadows.end())
            return found->second.image;
    }
    auto found = s.aliases.find(native);
    return found == s.aliases.end() ? nullptr : found->second.image;
}
inline void Publish(const Graph &graph) {
    auto &s = State();
    std::lock_guard lock(s.mutex);
    ++s.executed_lists;
    for (const auto &op : graph.operations) {
        if (op.kind == Operation::Kind::Invalidate) {
            s.aliases.erase(op.target.Get());
            continue;
        }
        auto input = s.aliases.find(op.source.Get());
        if (op.source && (input == s.aliases.end() || !input->second.image)) {
            s.aliases.erase(op.target.Get());
            continue;
        }
        auto snapshot = std::make_shared<Slot>(*op.image);
        if (op.kind == Operation::Kind::Copy && input != s.aliases.end()) {
            snapshot->crop = input->second.image->crop;
            snapshot->viewport = input->second.image->viewport;
        }
        s.aliases[op.target.Get()] = {op.target, snapshot};
        s.latest = snapshot;
        if (op.kind == Operation::Kind::Draw)
            ++s.captures;
    }
}
template <class F> inline void AfterDraw(ID3D11DeviceContext *c, F &&draw, Graph *graph = nullptr) {
    if (!Requested())
        return;
    auto &s = State();
    std::lock_guard lock(s.mutex);
    ++s.draws;
    if (graph)
        ++s.deferred_draws;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth;
    c->OMGetRenderTargets(1, &target, &depth);
    if (!target)
        return;
    ComPtr<ID3D11Resource> native;
    target->GetResource(&native);
    ComPtr<ID3D11PixelShader> original;
    UINT instances = 0;
    c->PSGetShader(&original, nullptr, &instances);
    UINT stage = 0, bytes = sizeof(stage);
    if (original)
        original->GetPrivateData(shaderTag, &bytes, &stage);
    // Any unrecognised write breaks forwarding, but keeps the immutable scene
    // snapshot for the final SDR grade bridge (HUD is intentionally not replayed).
    if (!stage || stage > 5) {
        Invalidate(s, graph, native.Get());
        return;
    }
    Invalidate(s, graph, native.Get()); // A rejected overwrite must not forward an earlier snapshot.
    auto reject = [&](UINT reason, const char *description) { Reject(s, stage, reason, description); };
    if (instances) {
        reject(0, "dynamic shader linkage");
        return;
    }
    if (!s.observed[stage - 1]) {
        s.observed[stage - 1] = true;
        hdr::report(("RenoDX scene HDR: recognised native stage " + std::to_string(stage)).c_str());
    }
    if (depth) {
        ComPtr<ID3D11DepthStencilState> ds;
        UINT stencil = 0;
        c->OMGetDepthStencilState(&ds, &stencil);
        if (!ds) {
            reject(1, "default active depth state");
            return;
        }
        D3D11_DEPTH_STENCIL_DESC dd{};
        ds->GetDesc(&dd);
        if (dd.DepthEnable || dd.StencilEnable) {
            reject(2, "active depth/stencil test");
            return;
        }
    }
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicateValue = FALSE;
    c->GetPredication(&predicate, &predicateValue);
    if (predicate) {
        reject(3, "predicated draw");
        return;
    }
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(native.As(&texture))) {
        reject(4, "target is not a 2D texture");
        return;
    }
    D3D11_RENDER_TARGET_VIEW_DESC targetView{};
    target->GetDesc(&targetView);
    if (targetView.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || targetView.Texture2D.MipSlice ||
        targetView.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        targetView.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        reject(5, "unsupported target view or sRGB target");
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Width < 640 || desc.Height < 360 || desc.MipLevels != 1 || desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1 ||
        (Typed(desc.Format) != DXGI_FORMAT_R16G16B16A16_FLOAT &&
         Typed(desc.Format) != DXGI_FORMAT_R8G8B8A8_UNORM &&
         Typed(desc.Format) != DXGI_FORMAT_B8G8R8A8_UNORM)) {
        reject(6, "unsupported target dimensions, format or samples");
        return;
    }
    D3D11_VIEWPORT viewport{};
    UINT count = 1;
    c->RSGetViewports(&count, &viewport);
    // FFXIV uses a 0.5-pixel viewport origin. Keep rasterisation unchanged,
    // but describe the covered texel centres rather than treating that origin
    // as a fractional texture-sampling offset. Right/bottom edges are exclusive.
    auto halfAligned = [](float v) { return std::abs(v * 2 - std::round(v * 2)) < .001f; };
    const bool finiteViewport = std::isfinite(viewport.TopLeftX) && std::isfinite(viewport.TopLeftY) &&
                                std::isfinite(viewport.Width) && std::isfinite(viewport.Height);
    const float left = finiteViewport ? std::ceil(viewport.TopLeftX - .5f) : 0;
    const float top = finiteViewport ? std::ceil(viewport.TopLeftY - .5f) : 0;
    const float right = finiteViewport ? std::ceil(viewport.TopLeftX + viewport.Width - .5f) : 0;
    const float bottom = finiteViewport ? std::ceil(viewport.TopLeftY + viewport.Height - .5f) : 0;
    const bool validViewport =
        count == 1 && finiteViewport && viewport.TopLeftX >= 0 && viewport.TopLeftY >= 0 &&
        halfAligned(viewport.TopLeftX) && halfAligned(viewport.TopLeftY) &&
        std::abs(viewport.Width - std::round(viewport.Width)) < .001f &&
        std::abs(viewport.Height - std::round(viewport.Height)) < .001f && right - left >= 640 &&
        bottom - top >= 360 && left >= 0 && top >= 0 && right <= desc.Width && bottom <= desc.Height;
    if (!validViewport) {
        if (!s.rejected[stage - 1][7])
            HDR_LOG("RenoDX scene HDR: stage {} viewport count {}, origin {}/{}, size {}x{}, target {}x{}",
                    stage, count, viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height,
                    desc.Width, desc.Height);
        reject(7, "viewport cannot be safely cropped");
        return;
    }
    std::array<ComPtr<ID3D11RenderTargetView>, 8> targets;
    ID3D11RenderTargetView *rawTargets[8]{};
    c->OMGetRenderTargets(8, rawTargets, nullptr);
    for (UINT i = 0; i < 8; i++)
        targets[i].Attach(rawTargets[i]);
    for (UINT i = 1; i < 8; i++)
        if (targets[i]) {
            reject(8, "multiple render targets");
            return;
        }
    ComPtr<ID3D11BlendState> blend;
    FLOAT factor[4]{};
    UINT mask = 0;
    c->OMGetBlendState(&blend, factor, &mask);
    if (blend) {
        D3D11_BLEND_DESC b{};
        blend->GetDesc(&b);
        if (b.RenderTarget[0].BlendEnable || (b.RenderTarget[0].RenderTargetWriteMask & 7) != 7) {
            reject(9, "blending or incomplete RGB writes");
            return;
        }
    }
    ComPtr<ID3D11ShaderResourceView> input;
    c->PSGetShaderResources(0, 1, &input);
    if (!input) {
        reject(10, "missing colour input");
        return;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC inputDesc{};
    input->GetDesc(&inputDesc);
    if (inputDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || inputDesc.Texture2D.MostDetailedMip) {
        reject(11, "unsupported colour input view");
        return;
    }
    ComPtr<ID3D11Resource> inputResource;
    input->GetResource(&inputResource);
    if (inputResource.Get() == native.Get()) {
        reject(12, "input aliases target");
        return;
    }
    auto upstream = Upstream(s, graph, inputResource.Get());
    if (stage != 1 && !upstream) {
        reject(13, "no captured upstream scene");
        return;
    }
    if (stage == 1) {
        ComPtr<ID3D11Texture2D> t;
        if (FAILED(inputResource.As(&t))) {
            reject(14, "root input is not a 2D texture");
            return;
        }
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        if (Typed(d.Format) != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            inputDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || d.SampleDesc.Count != 1) {
            reject(15, "root input is not FP16 colour");
            return;
        }
    }
    ComPtr<ID3D11Device> d;
    c->GetDevice(&d);
    if (s.device.Get() != d.Get()) {
        s.device = d;
        s.shaders = {};
        s.aliases.clear();
        s.latest.reset();
        s.shadows.clear();
        upstream.reset();
        if (stage != 1)
            return;
    }
    auto &ps = s.shaders[stage - 1];
    if (!ps) {
        const void *code = nullptr;
        size_t size = 0;
#define SCENE_SHADER(i, n)                                                                                   \
    case i:                                                                                                  \
        code = n;                                                                                            \
        size = sizeof(n);                                                                                    \
        break
        switch (stage) {
            SCENE_SHADER(1, Tonemap_0x85E777EF);
            SCENE_SHADER(2, PostTonemapPreLUT_0xF8F57F0A);
            SCENE_SHADER(3, LUT_0x27EBC404);
            SCENE_SHADER(4, LUT_0x1F264D17);
            SCENE_SHADER(5, FullscreenGammaCorrection_0xF6E81A1B);
        }
#undef SCENE_SHADER
        if (FAILED(d->CreatePixelShader(code, size, nullptr, &ps))) {
            reject(16, "replacement shader creation failed");
            return;
        }
    }
    auto slot = Shadow(s, native.Get(), d.Get(), desc);
    if (!slot) {
        reject(18, "HDR shadow allocation or cache admission failed");
        return;
    }
    // Snapshot the real SDR result, then replay only the verified fullscreen
    // pass into FP16. Every modified binding is restored before the next draw.
    c->CopyResource(slot->reference.Get(), texture.Get());
    c->OMSetRenderTargets(1, slot->rtv.GetAddressOf(), nullptr);
    c->PSSetShader(ps.Get(), nullptr, 0);
    if (upstream)
        c->PSSetShaderResources(0, 1, upstream->srv.GetAddressOf());
    const float clear[4]{};
    c->ClearRenderTargetView(slot->rtv.Get(), clear);
    draw();
    c->OMSetRenderTargets(8, rawTargets, depth.Get());
    c->PSSetShaderResources(0, 1, input.GetAddressOf());
    c->PSSetShader(original.Get(), nullptr, 0);
    slot->lastUse = GetTickCount64();
    slot->stage = stage;
    slot->viewport = viewport;
    slot->crop = {left, top, right - left, bottom - top};
    auto snapshot = std::make_shared<Slot>(*slot);
    snapshot->owner = slot;
    if (graph) {
        graph->aliases[native.Get()] = {native, snapshot};
        graph->operations.push_back(
            {Operation::Kind::Draw, native, stage == 1 ? ComPtr<ID3D11Resource>{} : inputResource, snapshot});
    } else {
        s.aliases[native.Get()] = {native, snapshot};
        s.latest = snapshot;
        ++s.captures;
    }
}

inline void Copy(ID3D11Resource *dest, ID3D11Resource *source, bool full,
                 ID3D11DeviceContext *context = nullptr, Graph *graph = nullptr) {
    if (!Requested() || !dest || !source || dest == source)
        return;
    auto &s = State();
    std::lock_guard lock(s.mutex);
    auto upstream = Upstream(s, graph, source);
    if (!full || !upstream) {
        Invalidate(s, graph, dest);
        return;
    }
    if (!context) {
        s.aliases[dest] = {dest, upstream};
        return;
    }
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    if (FAILED(dest->QueryInterface(IID_PPV_ARGS(&texture)))) {
        Invalidate(s, graph, dest);
        return;
    }
    texture->GetDesc(&desc);
    if (desc.Width != upstream->width || desc.Height != upstream->height || desc.MipLevels != 1 ||
        desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
        Typed(desc.Format) != upstream->referenceFormat) {
        Invalidate(s, graph, dest);
        return;
    }
    auto storage = Shadow(s, dest, s.device.Get(), desc);
    if (!storage) {
        Invalidate(s, graph, dest);
        return;
    }
    context->CopyResource(storage->hdr.Get(), upstream->hdr.Get());
    context->CopyResource(storage->reference.Get(), upstream->reference.Get());
    auto snapshot = std::make_shared<Slot>(*storage);
    snapshot->owner = storage;
    snapshot->crop = upstream->crop;
    snapshot->viewport = upstream->viewport;
    if (graph) {
        graph->aliases[dest] = {dest, snapshot};
        graph->operations.push_back({Operation::Kind::Copy, dest, source, snapshot});
    } else
        s.aliases[dest] = {dest, snapshot};
}
inline std::shared_ptr<Slot> Find(ID3D11Resource *native) {
    auto &s = State();
    std::lock_guard lock(s.mutex);
    ++s.requests;
    const auto it = s.aliases.find(native);
    if (it == s.aliases.end()) {
        ++s.missing;
        return nullptr;
    }
    ++s.available;
    return it->second.image;
}
inline std::string Summary() {
    auto &s = State();
    std::lock_guard lock(s.mutex);
    std::string stages;
    for (unsigned i = 0; i < s.observed.size(); ++i)
        if (s.observed[i])
            stages += std::to_string(i + 1);
    return "stages=" + (stages.empty() ? std::string("none") : stages) +
           ", pixel shaders=" + std::to_string(s.pixel_shaders) +
           ", matched shaders=" + std::to_string(s.tagged_shaders) +
           ", native draws=" + std::to_string(s.draws) +
           ", deferred draws=" + std::to_string(s.deferred_draws) +
           ", recorded/executed=" + std::to_string(s.recorded_lists) + "/" +
           std::to_string(s.executed_lists) + ", captures=" + std::to_string(s.captures) +
           ", DLSS matches=" + std::to_string(s.available) + ", missing=" + std::to_string(s.missing) +
           (s.captures == 0 && !s.last_reject.empty() ? "; last rejection=" + s.last_reject : std::string());
}
inline void EndFrame() {
    auto &s = State();
    std::lock_guard lock(s.mutex);
    s.aliases.clear();
    s.latest.reset();
    // Snapshots are ordered on the same immediate context. Idle sizes left
    // behind by quality/window changes do not need to occupy VRAM forever.
    const auto now = GetTickCount64();
    std::erase_if(s.shadows, [&](const auto &entry) {
        const auto &slot = entry.second.image;
        return slot.use_count() == 1 && now - slot->lastUse > 2000;
    });
}
inline void Reset() {
    auto &s = State();
    std::lock_guard lock(s.mutex);
    s.aliases.clear();
    s.latest.reset();
    s.shadows.clear();
    s.shaders = {};
    s.device.Reset();
}
#undef HDR_LOG
} // namespace xivrender::hdr::scene
