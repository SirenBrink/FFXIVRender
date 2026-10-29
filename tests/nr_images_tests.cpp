#include "xivrender/nr_images.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("D3D11 test failed: " + std::to_string(hr));
}
struct Image {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
Image image(ID3D11Device *device, unsigned w, unsigned h, DXGI_FORMAT format, unsigned pitch,
            const void *data, bool writable) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = format;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (writable ? D3D11_BIND_UNORDERED_ACCESS : 0);
    const D3D11_SUBRESOURCE_DATA initial{data, pitch, 0};
    Image result;
    check(device->CreateTexture2D(&d, data ? &initial : nullptr, &result.texture));
    check(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv));
    if (writable)
        check(device->CreateUnorderedAccessView(result.texture.Get(), nullptr, &result.uav));
    return result;
}
std::vector<unsigned char> read(ID3D11Device *d, ID3D11DeviceContext *c, ID3D11Texture2D *image,
                                unsigned bytes_per_pixel) {
    D3D11_TEXTURE2D_DESC desc{};
    image->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(d->CreateTexture2D(&desc, nullptr, &staging));
    c->CopyResource(staging.Get(), image);
    D3D11_MAPPED_SUBRESOURCE map{};
    check(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
    const auto row = desc.Width * bytes_per_pixel;
    std::vector<unsigned char> result(size_t(row) * desc.Height);
    for (unsigned y = 0; y < desc.Height; ++y)
        std::memcpy(result.data() + size_t(y) * row,
                    static_cast<unsigned char *>(map.pData) + size_t(y) * map.RowPitch, row);
    c->Unmap(staging.Get(), 0);
    return result;
}
float half_value(std::uint16_t h) {
    const float mantissa = float(h & 1023) / 1024;
    const int exponent = (h >> 10) & 31;
    const float value = exponent ? std::ldexp(1 + mantissa, exponent - 15) : std::ldexp(mantissa, -14);
    return h & 32768 ? -value : value;
}
void compressed_guides_and_strength() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                            nullptr, &context));
    std::vector<std::array<float, 4>> pixels(16 * 16);
    std::vector<float> depth(16 * 16);
    std::vector<std::array<float, 2>> mv(16 * 16, {.03125f, -.015625f});
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x) {
            pixels[y * 16 + x] = {float(x) / 16, float(y) / 16, .4f, .37f};
            depth[y * 16 + x] = float(x + 16 * y) / 256;
        }
    auto source = image(device.Get(), 16, 16, DXGI_FORMAT_R32G32B32A32_FLOAT, 16 * 16, pixels.data(), false);
    auto z = image(device.Get(), 16, 16, DXGI_FORMAT_R32_FLOAT, 16 * 4, depth.data(), false);
    auto motion = image(device.Get(), 16, 16, DXGI_FORMAT_R32G32_FLOAT, 16 * 8, mv.data(), false);
    auto color = image(device.Get(), 12, 12, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, nullptr, true);
    auto pd = image(device.Get(), 12, 12, DXGI_FORMAT_R32_FLOAT, 0, nullptr, true);
    auto pm = image(device.Get(), 12, 12, DXGI_FORMAT_R16G16_FLOAT, 0, nullptr, true);
    xivrender::nr::SourceFrame f;
    f.color = source.texture.Get();
    f.depth = z.texture.Get();
    f.motion = motion.texture.Get();
    f.output_width = f.output_height = f.guide_width = f.guide_height = f.motion_width = f.motion_height = 16;
    f.motion_scale_x = f.motion_scale_y = 16;
    xivrender::NeuralRenderingSettings cfg;
    cfg.foveated = true;
    cfg.central_fraction = .5f;
    cfg.work_fraction = .75f;
    xivrender::nr::Images images;
    std::string error;
    if (!images.Prepare(device.Get(), f, 12, 12, color.uav.Get(), pd.uav.Get(), pm.uav.Get(), error, &cfg))
        throw std::runtime_error(error);
    const auto c = read(device.Get(), context.Get(), color.texture.Get(), 8);
    const auto d = read(device.Get(), context.Get(), pd.texture.Get(), 4);
    const auto m = read(device.Get(), context.Get(), pm.texture.Get(), 4);
    // Reference inverse is obtained by bisection rather than reproducing the
    // shader's analytic inverse. Check centre, periphery and MV endpoints.
    const auto pack = [](double uv) {
        const double delta = 2 * uv - 1, r = std::abs(delta);
        double p = r;
        if (r > 1)
            p = .75 + (r - 1) * .25;
        else if (r > .5) {
            const double t = 2 * r - 1;
            p = .5 + .25 * t / (.5 + .5 * t);
        }
        return .5 + std::copysign(p, delta) / 1.5;
    };
    const auto inverse = [&](double packed) {
        double lo = 0, hi = 1;
        for (unsigned i = 0; i < 60; ++i) {
            const double mid = (lo + hi) * .5;
            if (pack(mid) < packed)
                lo = mid;
            else
                hi = mid;
        }
        return (lo + hi) * .5;
    };
    for (unsigned y : {0u, 5u, 11u})
        for (unsigned x : {0u, 5u, 11u}) {
            const auto at = y * 12 + x;
            const double u = inverse((x + .5) / 12), v = inverse((y + .5) / 12);
            std::uint16_t red = 0, green = 0, vx = 0, vy = 0;
            float actual_depth = 0;
            std::memcpy(&red, c.data() + at * 8, 2);
            std::memcpy(&green, c.data() + at * 8 + 2, 2);
            std::memcpy(&vx, m.data() + at * 4, 2);
            std::memcpy(&vy, m.data() + at * 4 + 2, 2);
            std::memcpy(&actual_depth, d.data() + at * 4, 4);
            assert(std::abs(half_value(red) - std::clamp(u - .03125, 0., .9375)) < .001);
            assert(std::abs(half_value(green) - std::clamp(v - .03125, 0., .9375)) < .001);
            const auto ix = std::min(15u, unsigned(u * 16)), iy = std::min(15u, unsigned(v * 16));
            assert(std::abs(actual_depth - depth[iy * 16 + ix]) < .00001);
            assert(std::abs(half_value(vx) - (pack(u + .03125) - pack(u))) < .00015);
            assert(std::abs(half_value(vy) - (pack(v - .015625) - pack(v))) < .00015);
        }
    // An identity correction must retain all native detail and alpha despite
    // compression. Turning the effect strength off must also preserve bytes.
    const auto original = read(device.Get(), context.Get(), source.texture.Get(), 16);
    if (!images.Composite(device.Get(), f, 12, 12, color.srv.Get(), color.srv.Get(), error, &cfg))
        throw std::runtime_error(error);
    assert(read(device.Get(), context.Get(), source.texture.Get(), 16) == original);
    auto edited = image(device.Get(), 12, 12, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, nullptr, true);
    const float clear[]{.8f, .7f, .6f, 0};
    context->ClearUnorderedAccessViewFloat(edited.uav.Get(), clear);
    cfg.detail_strength = 0;
    if (!images.Composite(device.Get(), f, 12, 12, color.srv.Get(), edited.srv.Get(), error, &cfg))
        throw std::runtime_error(error);
    assert(read(device.Get(), context.Get(), source.texture.Get(), 16) == original);
    cfg.detail_strength = 1;
    cfg.colour_strength = 0;
    if (!images.Composite(device.Get(), f, 12, 12, color.srv.Get(), edited.srv.Get(), error, &cfg))
        throw std::runtime_error(error);
    const auto result = read(device.Get(), context.Get(), source.texture.Get(), 16);
    for (unsigned i = 0; i < 256; ++i) {
        std::array<float, 4> out{};
        std::memcpy(out.data(), result.data() + i * 16, 16);
        assert(out[3] == pixels[i][3]);
        assert(std::abs((out[0] - out[1]) - (pixels[i][0] - pixels[i][1])) < .00001f);
        assert(std::abs((out[1] - out[2]) - (pixels[i][1] - pixels[i][2])) < .00001f);
    }
}

void run() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                            nullptr, &context));
    std::vector<std::uint16_t> color(6 * 6 * 4, 0x3b80);
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x) {
            const auto at = ((y + 1) * 6 + x + 1) * 4;
            color[at] = x % 2 ? 0x3800 : 0x3400;
            color[at + 1] = 0x3800;
            color[at + 2] = 0x3a00;
            color[at + 3] = 0x3800;
        }
    std::vector<float> depth(8 * 8);
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
            depth[y * 8 + x] = float(x + 10 * y) / 100;
    std::vector<std::uint16_t> motion(8 * 8 * 2);
    for (unsigned p = 0; p < 64; ++p) {
        motion[p * 2] = 0x3400;
        motion[p * 2 + 1] = 0xb000;
    }
    auto original = image(device.Get(), 6, 6, DXGI_FORMAT_R16G16B16A16_FLOAT, 6 * 8, color.data(), false);
    auto z = image(device.Get(), 8, 8, DXGI_FORMAT_R32_FLOAT, 8 * 4, depth.data(), false);
    auto mv = image(device.Get(), 8, 8, DXGI_FORMAT_R16G16_FLOAT, 8 * 4, motion.data(), false);
    auto prepared = image(device.Get(), 2, 2, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, nullptr, true);
    auto pdepth = image(device.Get(), 2, 2, DXGI_FORMAT_R32_FLOAT, 0, nullptr, true);
    auto pmotion = image(device.Get(), 2, 2, DXGI_FORMAT_R16G16_FLOAT, 0, nullptr, true);
    xivrender::nr::SourceFrame f;
    f.color = original.texture.Get();
    f.depth = z.texture.Get();
    f.motion = mv.texture.Get();
    f.output_width = f.output_height = f.guide_width = f.guide_height = f.motion_width = f.motion_height = 4;
    f.output_x = f.output_y = 1;
    f.depth_x = f.motion_x = 2;
    f.depth_y = f.motion_y = 1;
    f.jitter_x = .5f;
    f.jitter_y = -.5f;
    f.motion_scale_x = f.motion_scale_y = 4;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    check(device->CreateSamplerState(&sd, &sampler));
    auto *binding = sampler.Get();
    context->PSSetSamplers(3, 1, &binding);
    ID3D11ShaderResourceView *source_binding = original.srv.Get();
    context->PSSetShaderResources(6, 1, &source_binding);
    xivrender::nr::Images images;
    std::string error;
    if (!images.Prepare(device.Get(), f, 2, 2, prepared.uav.Get(), pdepth.uav.Get(), pmotion.uav.Get(),
                        error))
        throw std::runtime_error(error);
    const auto rgb_bytes = read(device.Get(), context.Get(), prepared.texture.Get(), 8);
    std::vector<std::uint16_t> rgb(rgb_bytes.size() / 2);
    std::memcpy(rgb.data(), rgb_bytes.data(), rgb_bytes.size());
    for (unsigned p = 0; p < 4; ++p) {
        assert(rgb[p * 4] == 0x3600 && rgb[p * 4 + 1] == 0x3800 && rgb[p * 4 + 2] == 0x3a00 &&
               rgb[p * 4 + 3] == 0x3c00);
    }
    const auto z_bytes = read(device.Get(), context.Get(), pdepth.texture.Get(), 4);
    float first_depth = 0;
    std::memcpy(&first_depth, z_bytes.data(), sizeof(first_depth));
    assert(std::abs(first_depth - .13f) < .00001f);
    const auto mv_bytes = read(device.Get(), context.Get(), pmotion.texture.Get(), 4);
    std::uint16_t vx = 0, vy = 0;
    std::memcpy(&vx, mv_bytes.data(), 2);
    std::memcpy(&vy, mv_bytes.data() + 2, 2);
    assert(vx == 0x3400 && vy == 0xb000);
    std::vector<std::uint16_t> edited(2 * 2 * 4);
    for (unsigned p = 0; p < 4; ++p) {
        edited[p * 4] = 0x3900;
        edited[p * 4 + 1] = 0x3a00;
        edited[p * 4 + 2] = 0x3c00;
        edited[p * 4 + 3] = 0;
    }
    auto model = image(device.Get(), 2, 2, DXGI_FORMAT_R16G16B16A16_FLOAT, 2 * 8, edited.data(), false);
    if (!images.Composite(device.Get(), f, 2, 2, prepared.srv.Get(), model.srv.Get(), error))
        throw std::runtime_error(error);
    const auto composed_bytes = read(device.Get(), context.Get(), original.texture.Get(), 8);
    std::vector<std::uint16_t> composed(composed_bytes.size() / 2);
    std::memcpy(composed.data(), composed_bytes.data(), composed_bytes.size());
    for (unsigned y = 0; y < 6; ++y)
        for (unsigned x = 0; x < 6; ++x) {
            const auto at = (y * 6 + x) * 4;
            if (x >= 1 && x <= 4 && y >= 1 && y <= 4) {
                assert(composed[at] == ((x - 1) % 2 ? 0x3a00 : 0x3800));
                assert(composed[at + 1] == 0x3a00 && composed[at + 2] == 0x3c00 &&
                       composed[at + 3] == 0x3800);
            } else
                for (unsigned channel = 0; channel < 4; ++channel)
                    assert(composed[at + channel] == color[at + channel]);
        }
    ComPtr<ID3D11SamplerState> retained_sampler;
    ComPtr<ID3D11ShaderResourceView> retained_source;
    context->PSGetSamplers(3, 1, &retained_sampler);
    context->PSGetShaderResources(6, 1, &retained_source);
    assert(retained_sampler.Get() == sampler.Get() && retained_source.Get() == original.srv.Get());
    for (unsigned p = 0; p < 4; ++p)
        edited[p * 4] = edited[p * 4 + 1] = edited[p * 4 + 2] = 0x7e00;
    context->UpdateSubresource(model.texture.Get(), 0, nullptr, edited.data(), 2 * 8, 0);
    if (!images.Composite(device.Get(), f, 2, 2, prepared.srv.Get(), model.srv.Get(), error))
        throw std::runtime_error(error);
    assert(read(device.Get(), context.Get(), original.texture.Get(), 8) == composed_bytes);
    f.depth_x = 99;
    assert(!images.Prepare(device.Get(), f, 2, 2, prepared.uav.Get(), pdepth.uav.Get(), pmotion.uav.Get(),
                           error));
    assert(!error.empty());
    // Identity NR must preserve the full original floating-point range,
    // including negative reconstruction overshoot and values above SDR white.
    const std::array<std::array<float, 4>, 4> floating_pixels{{{-.125f, 1.25f, .5f, .25f},
                                                               {-.125f, 1.25f, .5f, .25f},
                                                               {-.125f, 1.25f, .5f, .25f},
                                                               {-.125f, 1.25f, .5f, .25f}}};
    auto floating =
        image(device.Get(), 2, 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 2 * 16, floating_pixels.data(), false);
    f.color = floating.texture.Get();
    f.output_width = f.output_height = 2;
    f.output_x = f.output_y = 0;
    f.depth_x = 2;
    if (!images.Prepare(device.Get(), f, 2, 2, prepared.uav.Get(), pdepth.uav.Get(), pmotion.uav.Get(),
                        error))
        throw std::runtime_error(error);
    if (!images.Composite(device.Get(), f, 2, 2, prepared.srv.Get(), prepared.srv.Get(), error))
        throw std::runtime_error(error);
    const auto identity = read(device.Get(), context.Get(), floating.texture.Get(), 16);
    assert(identity.size() == sizeof(floating_pixels) &&
           std::memcmp(identity.data(), floating_pixels.data(), identity.size()) == 0);
    // Both NR input-domain choices must preserve exact HDR identity, including
    // negative overshoot and unclipped highlights, with independent alpha.
    f.linear_hdr = true;
    for (const bool hdr_input : {false, true}) {
        xivrender::NeuralRenderingSettings cfg;
        cfg.hdr_input = hdr_input;
        if (!images.Prepare(device.Get(), f, 2, 2, prepared.uav.Get(), pdepth.uav.Get(), pmotion.uav.Get(),
                            error, &cfg))
            throw std::runtime_error(error);
        if (!images.Composite(device.Get(), f, 2, 2, prepared.srv.Get(), prepared.srv.Get(), error, &cfg))
            throw std::runtime_error(error);
        const auto hdr_identity = read(device.Get(), context.Get(), floating.texture.Get(), 16);
        assert(hdr_identity == identity);
    }
}
} // namespace
int main() {
    try {
        run();
        compressed_guides_and_strength();
        std::cout << "PASS: NR active subrects, guide unjittering/motion units, residual detail, alpha, NaN "
                     "fallback and caller state\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
