#include "xivrender/hdr.hpp"
#include "xivrender/hdr_scene.hpp"
#include "xivrender/hdr_composition.hpp"
#include <d3dcompiler.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cassert>
#include <array>
#include <cstring>
#include <cmath>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("HDR test D3D11 failure " + std::to_string(hr));
}
ComPtr<ID3D11Texture2D> image(ID3D11Device *d, unsigned w, unsigned h, DXGI_FORMAT fmt, unsigned flags,
                              const void *data = nullptr, unsigned pitch = 0) {
    D3D11_TEXTURE2D_DESC t{};
    t.Width = w;
    t.Height = h;
    t.Format = fmt;
    t.MipLevels = t.ArraySize = t.SampleDesc.Count = 1;
    t.BindFlags = flags;
    D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
    ComPtr<ID3D11Texture2D> out;
    check(d->CreateTexture2D(&t, data ? &init : nullptr, &out));
    return out;
}
std::vector<unsigned char> read(ID3D11Device *d, ID3D11DeviceContext *c, ID3D11Texture2D *t, unsigned bpp) {
    D3D11_TEXTURE2D_DESC desc{};
    t->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> s;
    check(d->CreateTexture2D(&desc, nullptr, &s));
    c->CopyResource(s.Get(), t);
    D3D11_MAPPED_SUBRESOURCE map{};
    check(c->Map(s.Get(), 0, D3D11_MAP_READ, 0, &map));
    std::vector<unsigned char> bytes(desc.Width * desc.Height * bpp);
    for (unsigned y = 0; y < desc.Height; ++y)
        std::memcpy(bytes.data() + y * desc.Width * bpp,
                    static_cast<unsigned char *>(map.pData) + y * map.RowPitch, desc.Width * bpp);
    c->Unmap(s.Get(), 0);
    return bytes;
}
float half(std::uint16_t h) {
    int e = (h >> 10) & 31;
    float f = float(h & 1023) / 1024;
    return std::copysign(e ? std::ldexp(1 + f, e - 15) : std::ldexp(f, -14), h & 0x8000 ? -1.f : 1.f);
}
} // namespace
int main() {
    try {
        using namespace xivrender;
        ComPtr<ID3D11Device> d;
        ComPtr<ID3D11DeviceContext> c;
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d,
                                nullptr, &c));
        {
            constexpr char source[] =
                XIVRENDER_HDR_COMPOSITION_HLSL "RWTexture2D<float4> Out:register(u0);[numthreads(3,1,1)]void "
                                               "Main(uint3 p:SV_DispatchThreadID){"
                                               "float before=p.x==2?.25:.5,after=p.x==2?.5:.25;"
                                               "Out[p.xy]=float4(hdr_compose(after.xxx,float3(8,8,8),before."
                                               "xxx,1,before.xxx,p.x!=1,200,1000),1);}";
            ComPtr<ID3DBlob> code;
            check(D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr, "Main", "cs_5_0", 0, 0, &code,
                             nullptr));
            ComPtr<ID3D11ComputeShader> cs;
            check(d->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));
            auto output = image(d.Get(), 3, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
            ComPtr<ID3D11UnorderedAccessView> uav;
            check(d->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
            ID3D11UnorderedAccessView *views[]{uav.Get()};
            c->CSSetShader(cs.Get(), nullptr, 0);
            c->CSSetUnorderedAccessViews(0, 1, views, nullptr);
            c->Dispatch(1, 1, 1);
            c->ClearState();
            auto bytes = read(d.Get(), c.Get(), output.Get(), 16);
            std::array<float, 12> samples{};
            std::memcpy(samples.data(), bytes.data(), bytes.size());
            // Regression: peak-compressing after the gain would yield 542.86,
            // rather than 354.55 nits for the first darkening case.
            assert(std::abs(samples[0] - 354.54545f) < .01f);
            assert(std::abs(samples[4] - 659.0909f) < .01f);
            assert(std::abs(samples[8] - 1418.1818f) < .01f);
        }
        Settings cfg;
        cfg.hdr.enabled = true;
        cfg.hdr.output = HdrOutput::ScRgb16Float;
        hdr::configure(cfg, true);
        std::array<std::array<float, 4>, 64> source{};
        for (auto &p : source)
            p = {1, 1, 1, 1};
        auto native = image(d.Get(), 8, 8, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE,
                            source.data(), 8 * 16);
        // Both samples have identical clipped SDR (white). The real scene still
        // contains distinct 2x/8x lighting. A finished-SDR expansion cannot recover it.
        auto slot = std::make_shared<hdr::scene::Slot>();
        slot->width = slot->height = 8;
        slot->crop = {0, 0, 8, 8};
        slot->reference = native;
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x) {
                const float h = x < 4 ? 2.f : 8.f;
                source[y * 8 + x] = {std::pow(h, 1.f / 2.2f), std::pow(h, 1.f / 2.2f),
                                     std::pow(h, 1.f / 2.2f), 1};
            }
        slot->hdr = image(d.Get(), 8, 8, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE,
                          source.data(), 8 * 16);
        check(d->CreateShaderResourceView(slot->hdr.Get(), nullptr, &slot->srv));
        hdr::scene::State().aliases[native.Get()] = {native, slot};
        hdr::DlssInput input;
        if (!hdr::prepare_dlss(c.Get(), native.Get(), 8, 8, 0, 0, 8, 8, input))
            throw std::runtime_error(hdr::status());
        auto data = read(d.Get(), c.Get(), input.color.Get(), 8);
        std::uint16_t left = 0, right = 0;
        std::memcpy(&left, data.data(), 2);
        std::memcpy(&right, data.data() + 7 * 8, 2);
        assert(std::abs(half(left) - 2) < .01f && std::abs(half(right) - 8) < .03f);
        assert(input.output.Get() != input.working.Get() && input.output.Get() != native.Get());
        // Model the native DLSS result, retaining its immutable output for history.
        c->CopyResource(input.output.Get(), input.color.Get());
        const auto original = read(d.Get(), c.Get(), input.output.Get(), 8);
        assert(hdr::begin_nr(c.Get()));
        auto sdr = image(d.Get(), 8, 8, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        assert(hdr::finish_dlss(c.Get(), sdr.Get(), 8, 8, 0, 0));
        assert(read(d.Get(), c.Get(), input.output.Get(), 8) == original);
        auto target = image(d.Get(), 8, 8, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11RenderTargetView> rtv;
        check(d->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        assert(hdr::present(c.Get(), sdr.Get(), rtv.Get(), 8, 8, false));
        auto answer = read(d.Get(), c.Get(), target.Get(), 16);
        std::array<float, 4> a{}, b{};
        std::memcpy(a.data(), answer.data(), 16);
        std::memcpy(b.data(), answer.data() + 7 * 16, 16);
        assert(std::isfinite(a[0]) && a[0] > cfg.hdr.paper_white_nits / 80 && b[0] > a[0] &&
               b[0] < cfg.hdr.peak_nits / 80);
        // An opaque UI pixel must entirely reject world highlights. Native UI stays
        // at its own paper-white range, even over a very bright background.
        std::array<std::uint8_t, 64> mask{};
        auto ui_slot = std::make_shared<hdr::ui::Slot>();
        ui_slot->width = ui_slot->height = 8;
        ui_slot->texture =
            image(d.Get(), 8, 8, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, mask.data(), 8);
        hdr::ui::State().current = ui_slot;
        const std::array<std::uint16_t, 4> gray{0x3800, 0x3800, 0x3800, 0x3c00};
        std::array<std::array<std::uint16_t, 4>, 64> ui_pixels;
        ui_pixels.fill(gray);
        c->UpdateSubresource(sdr.Get(), 0, nullptr, ui_pixels.data(), 8 * 8, 0);
        assert(hdr::present(c.Get(), sdr.Get(), rtv.Get(), 8, 8, false));
        answer = read(d.Get(), c.Get(), target.Get(), 16);
        std::memcpy(a.data(), answer.data(), 16);
        std::memcpy(b.data(), answer.data() + 7 * 16, 16);
        const float gray_nits = std::pow((.5f + .055f) / 1.055f, 2.4f) * cfg.hdr.paper_white_nits;
        assert(std::abs(a[0] - gray_nits / 80) < .001f && std::abs(b[0] - a[0]) < .000001f);
        // Live controls must affect actual GPU output without startup/recreation.
        const float original_gray = a[0];
        cfg.hdr.paper_white_nits = 300;
        hdr::configure(cfg);
        assert(hdr::present(c.Get(), sdr.Get(), rtv.Get(), 8, 8, false));
        answer = read(d.Get(), c.Get(), target.Get(), 16);
        std::memcpy(a.data(), answer.data(), 16);
        assert(std::abs(a[0] / original_gray - 300.f / 203.f) < .002f);
        ui_pixels.fill({0x3a00, 0x3400, 0x3000, 0x3c00});
        c->UpdateSubresource(sdr.Get(), 0, nullptr, ui_pixels.data(), 8 * 8, 0);
        cfg.hdr.saturation = 0;
        hdr::configure(cfg);
        assert(hdr::present(c.Get(), sdr.Get(), rtv.Get(), 8, 8, false));
        answer = read(d.Get(), c.Get(), target.Get(), 16);
        std::memcpy(a.data(), answer.data(), 16);
        assert(std::abs(a[0] - a[1]) < 1e-5f && std::abs(a[1] - a[2]) < 1e-5f);
        cfg.hdr.paper_white_nits = 203;
        cfg.hdr.saturation = 1;
        hdr::configure(cfg);
        // Effects see that bounded proxy. An identity effect must preserve HDR.
        hdr::ui::State().current.reset();
        hdr::finish_dlss(c.Get(), sdr.Get(), 8, 8, 0, 0);
        hdr::begin_effects(sdr.Get());
        assert(hdr::present(c.Get(), sdr.Get(), rtv.Get(), 8, 8, false));
        answer = read(d.Get(), c.Get(), target.Get(), 16);
        std::memcpy(a.data(), answer.data(), 16);
        std::memcpy(b.data(), answer.data() + 7 * 16, 16);
        assert(b[0] > a[0] && a[0] > 2);
        // Simultaneously live native DLSS handles never share output history.
        hdr::DlssInput second;
        assert(hdr::prepare_dlss(c.Get(), native.Get(), 8, 8, 0, 0, 8, 8, second));
        assert(second.output.Get() != input.output.Get());
        auto *retained_output = input.output.Get();
        assert(hdr::prepare_dlss(c.Get(), native.Get(), 8, 8, 0, 0, 8, 8, input));
        assert(input.output.Get() == retained_output);
        assert(read(d.Get(), c.Get(), input.output.Get(), 8) == original);
        // Canonicalizing wrappers must still reject a genuinely different
        // device, even when both devices use the same WARP adapter.
        ComPtr<ID3D11Device> other_device;
        ComPtr<ID3D11DeviceContext> other_context;
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                &other_device, nullptr, &other_context));
        hdr::DlssInput wrong_device;
        assert(!hdr::prepare_dlss(other_context.Get(), native.Get(), 8, 8, 0, 0, 8, 8, wrong_device));
        assert(hdr::prepare_dlss(c.Get(), native.Get(), 8, 8, 0, 0, 8, 8, input));
        hdr::finish_frame();
        hdr::shutdown();
        std::cout << "PASS: distinct unclipped scene values survive identical SDR references; native DLSS "
                     "history isolation, SDR proxy, HDR output, opaque UI and identity ReShade effects\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
