// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <xivrender/mip_bias.hpp>
#include <MinHook.h>
#include <cassert>
#include <cmath>
#include <cstdio>

using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT result) {
    assert(SUCCEEDED(result));
}
ComPtr<ID3D11SamplerState> sampler(ID3D11Device *device, float bias, float maxLod = D3D11_FLOAT32_MAX,
                                   D3D11_FILTER filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR) {
    D3D11_SAMPLER_DESC desc{};
    desc.Filter = filter;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.MipLODBias = bias;
    desc.MinLOD = 0;
    desc.MaxLOD = maxLod;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D11_COMPARISON_LESS;
    ComPtr<ID3D11SamplerState> result;
    check(device->CreateSamplerState(&desc, &result));
    return result;
}
float bias(ID3D11SamplerState *sampler) {
    D3D11_SAMPLER_DESC desc{};
    sampler->GetDesc(&desc);
    return desc.MipLODBias;
}
xivrender::mip::detail::Get other_owner_get = nullptr;
void STDMETHODCALLTYPE other_owner_hook(ID3D11DeviceContext *context, UINT start, UINT count,
                                        ID3D11SamplerState **output) {
    other_owner_get(context, start, count, output);
}
} // namespace

int main() {
    using namespace xivrender::mip;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                            nullptr, &context));
    auto **table = *reinterpret_cast<void ***>(context.Get());
    constexpr unsigned sets[]{10, 26, 32, 61, 65, 70}, gets[]{75, 85, 88, 99, 103, 108};
    std::array<detail::Set, detail::Stages> setters{};
    std::array<detail::Get, detail::Stages> getters{};
    for (unsigned i = 0; i < detail::Stages; ++i) {
        setters[i] = reinterpret_cast<detail::Set>(table[sets[i]]);
        getters[i] = reinterpret_cast<detail::Get>(table[gets[i]]);
    }
    detail::Binder binder;
    binder.Attach(context.Get(), setters, getters);
    auto world = sampler(device.Get(), -1), positive = sampler(device.Get(), .5f),
         zero = sampler(device.Get(), 0), ui = sampler(device.Get(), -1, 0),
         comparison =
             sampler(device.Get(), -1, D3D11_FLOAT32_MAX, D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR);
    std::array<ID3D11SamplerState *, 5> logical{world.Get(), positive.Get(), zero.Get(), ui.Get(),
                                                comparison.Get()};
    for (unsigned stage = 0; stage < detail::Stages; ++stage)
        setters[stage](context.Get(), 0, UINT(logical.size()), logical.data());
    assert(binder.Update(context.Get(), true, 2560, 3840));
    const float expected = std::log2(2560.0f / 3840.0f);
    assert(std::abs(binder.Target() - expected) < .0001f);
    for (unsigned stage = 0; stage < detail::Stages; ++stage) {
        std::array<ID3D11SamplerState *, 5> physical{}, observed{};
        getters[stage](context.Get(), 0, 5, physical.data());
        assert(physical[0] != logical[0] && std::abs(bias(physical[0]) - expected) < .0001f);
        for (unsigned i = 1; i < 5; ++i)
            assert(physical[i] == logical[i]);
        binder.Read(stage, context.Get(), 0, 5, observed.data());
        for (unsigned i = 0; i < 5; ++i) {
            assert(observed[i] == logical[i]);
            observed[i]->Release();
            physical[i]->Release();
        }
    }
    const auto created = binder.Creations();
    assert(!binder.Update(context.Get(), true, 2560, 3840));
    assert(binder.Creations() == created);
    for (unsigned pause = 0; pause < 20; ++pause) {
        binder.Update(context.Get(), false, 0, 0);
        binder.Update(context.Get(), true, 2560, 3840);
    }
    assert(binder.Creations() == created); // ReShade effects must not rebuild world variants every frame
    assert(binder.Update(context.Get(), true, 1920, 3840));
    assert(std::abs(binder.Target() + 1) < .0001f);
    binder.Clear(context.Get());
    for (unsigned stage = 0; stage < detail::Stages; ++stage) {
        ComPtr<ID3D11SamplerState> restored;
        getters[stage](context.Get(), 0, 1, &restored);
        assert(restored == world);
    }
    // Native context-state changes must be observed, not replaced by stale shadows.
    binder.Update(context.Get(), true, 2560, 3840);
    context->ClearState();
    binder.Update(context.Get(), true, 1920, 3840);
    ComPtr<ID3D11SamplerState> empty;
    context->PSGetSamplers(0, 1, &empty);
    assert(!empty);
    binder.Clear(context.Get());
    assert(policy::Apply(-1, 0, D3D11_FLOAT32_MAX, 0x100u, .5f, true, false, false) ==
           -1); // min/max filter exclusion
    assert(policy::Recommended(3840, 3840) == 0 && policy::Recommended(0, 3840) == 0);

    // Exercise the real twelve MinHook routes on WARP. The module must not
    // initialize/uninitialize MinHook or touch an unrelated owner's hooks.
    assert(MH_Initialize() == MH_OK);
    std::string error;
    assert(attach(context.Get(), error));
    assert(error.empty());
    auto *original = world.Get();
    context->PSSetSamplers(0, 1, &original);
    update(true, 2560, 3840);
    ComPtr<ID3D11SamplerState> observed;
    context->PSGetSamplers(0, 1, &observed);
    assert(observed == world);
    assert(std::abs(current() - expected) < .0001f);
    ComPtr<ID3D11Device> other;
    ComPtr<ID3D11DeviceContext> otherContext;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &other,
                            nullptr, &otherContext));
    assert(!attach(otherContext.Get(), error) && !error.empty());
    detach();
    assert(current() == 0);
    observed.Reset();
    context->PSGetSamplers(0, 1, &observed);
    assert(observed == world);
    assert(attach(context.Get(), error));
    detach();
    // Fail halfway through the first stage because another owner hooked Get.
    // Rollback must remove only our new Set hook and preserve that owner's Get.
    auto *owned_target = table[gets[0]];
    assert(MH_CreateHook(owned_target, reinterpret_cast<void *>(&other_owner_hook),
                         reinterpret_cast<void **>(&other_owner_get)) == MH_OK);
    assert(MH_EnableHook(owned_target) == MH_OK);
    assert(!attach(context.Get(), error) && !error.empty());
    assert(MH_DisableHook(owned_target) == MH_OK); // still existed and was enabled
    assert(MH_EnableHook(owned_target) == MH_OK);
    observed.Reset();
    context->PSGetSamplers(0, 1, &observed);
    assert(observed == world);
    assert(MH_RemoveHook(owned_target) == MH_OK);
    assert(attach(context.Get(), error));
    detach();
    assert(MH_Uninitialize() == MH_OK);
    puts("PASS: WARP six-stage logical/physical sampler binding, live quality updates, UI/comparison/minmax "
         "exclusions, context clear and targeted MinHook lifecycle");
}
