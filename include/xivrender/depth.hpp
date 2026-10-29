#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from OptiScaler misc/FfxivReShadeDepthCapture.h (SirenBrink fork).
#include <reshade_events.hpp>
#include <string>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace xivrender::depth {
// Normalized UVs span only the active native scene crop. Fixed-ratio DLSS keeps
// a compact raw depth texture. Native DRS can use a stable output-sized texture
// with point sampling, avoiding allocation churn without filtering depth edges.
class CaptureImage {
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext1> context;
    Ptr<ID3DDeviceContextState> state;
    Ptr<ID3D11VertexShader> vs;
    Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11Buffer> constants;
    Ptr<ID3D11Texture2D> image;
    Ptr<ID3D11RenderTargetView> target;
    Ptr<ID3D11ShaderResourceView> view;
    unsigned width = 0, height = 0;

  public:
    ID3D11ShaderResourceView *View() const {
        return view.Get();
    }
    ID3D11Device *Device() const {
        return device.Get();
    }
    unsigned Width() const {
        return width;
    }
    unsigned Height() const {
        return height;
    }
    void Reset() {
        *this = CaptureImage{};
    }
    bool Capture(ID3D11Texture2D *source, unsigned w, unsigned h, unsigned x, unsigned y,
                 unsigned outputWidth, unsigned outputHeight, ID3D11Device *nativeDevice = nullptr,
                 bool full_resolution = false) {
        if (!source || !w || !h || w > 16384 || h > 16384 || !outputWidth || !outputHeight ||
            outputWidth > 16384 || outputHeight > 16384)
            return false;
        D3D11_TEXTURE2D_DESC td{};
        source->GetDesc(&td);
        if (td.SampleDesc.Count != 1 || td.ArraySize != 1 || !(td.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
            x > td.Width || y > td.Height || w > td.Width - x || h > td.Height - y)
            return false;
        DXGI_FORMAT format = td.Format;
        switch (format) {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
            format = DXGI_FORMAT_R32_FLOAT;
            break;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            break;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            format = DXGI_FORMAT_R16_UNORM;
            break;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            break;
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            break;
        default:
            return false;
        }
        // ReShade patches resource GetDevice to return its proxy. Its supported
        // runtime API supplies the native device instead. Creating the SRV on
        // that device also validates that this resource belongs to it.
        Ptr<ID3D11Device> owner;
        if (nativeDevice)
            owner = nativeDevice;
        else
            source->GetDevice(&owner);
        if (!owner)
            return false;
        if (owner != device) {
            Reset();
            device = owner;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        Ptr<ID3D11ShaderResourceView> input;
        if (FAILED(device->CreateShaderResourceView(source, &sd, &input)))
            return false;
        if (!state) {
            Ptr<ID3D11Device1> d1;
            Ptr<ID3D11DeviceContext> c;
            if (FAILED(device.As(&d1)))
                return false;
            device->GetImmediateContext(&c);
            if (FAILED(c.As(&context)))
                return false;
            const auto level = device->GetFeatureLevel();
            D3D_FEATURE_LEVEL chosen;
            if (FAILED(d1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                    &chosen, &state)))
                return false;
            constexpr char code[] = R"(
Texture2D<float> Depth:register(t0);
cbuffer Settings:register(b0){uint2 Origin;uint2 SourceSize;uint2 OutputSize;uint2 Padding;};
float4 VS(uint id:SV_VertexID):SV_Position {float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);}
float PS(float4 pos:SV_Position):SV_Target {
    // Sample the native texel containing each output pixel's normalized centre.
    // Maximum dimensions keep these products within uint32, avoiding FP rounding
    // at crop edges and never sampling allocation padding outside SourceSize.
    uint2 pixel=uint2(pos.xy);
    // Fixed-ratio compact capture keeps the original direct-load path.
    [branch] if(any(SourceSize!=OutputSize))
        pixel=min(((pixel*2+1)*SourceSize)/(OutputSize*2),SourceSize-1);
    return Depth.Load(int3(Origin+pixel,0));
}
)";
            Ptr<ID3DBlob> blob, error;
            if (FAILED(D3DCompile(code, sizeof(code), nullptr, nullptr, nullptr, "VS", "vs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error)) ||
                FAILED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                                  &vs)))
                return false;
            blob.Reset();
            if (FAILED(D3DCompile(code, sizeof(code), nullptr, nullptr, nullptr, "PS", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error)) ||
                FAILED(
                    device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps)))
                return false;
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = 32;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device->CreateBuffer(&cb, nullptr, &constants)))
                return false;
        }
        if (!vs || !ps || !constants)
            return false;
        const auto targetWidth = full_resolution ? outputWidth : w;
        const auto targetHeight = full_resolution ? outputHeight : h;
        if (!image || width != targetWidth || height != targetHeight) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = targetWidth;
            d.Height = targetHeight;
            d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_R32_FLOAT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            Ptr<ID3D11Texture2D> next;
            Ptr<ID3D11RenderTargetView> nextTarget;
            Ptr<ID3D11ShaderResourceView> nextView;
            if (FAILED(device->CreateTexture2D(&d, nullptr, &next)) ||
                FAILED(device->CreateRenderTargetView(next.Get(), nullptr, &nextTarget)) ||
                FAILED(device->CreateShaderResourceView(next.Get(), nullptr, &nextView)))
                return false;
            image = next;
            target = nextTarget;
            view = nextView;
            width = targetWidth;
            height = targetHeight;
        }
        Ptr<ID3DDeviceContextState> saved;
        context->SwapDeviceContextState(state.Get(), &saved);
        context->ClearState();
        unsigned values[]{x, y, w, h, targetWidth, targetHeight, 0, 0};
        context->UpdateSubresource(constants.Get(), 0, nullptr, values, 0, 0);
        D3D11_VIEWPORT vp{0, 0, float(targetWidth), float(targetHeight), 0, 1};
        context->RSSetViewports(1, &vp);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->PSSetShaderResources(0, 1, input.GetAddressOf());
        context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
        context->Draw(3, 0);
        context->ClearState();
        context->SwapDeviceContextState(saved.Get(), nullptr);
        return true;
    }
};
} // namespace xivrender::depth

namespace xivrender::depth {
void capture(ID3D11DeviceContext *, ID3D11Texture2D *, unsigned width, unsigned height, unsigned x,
             unsigned y, unsigned outputWidth, unsigned outputHeight, bool variable_resolution = false);
void begin_effects(reshade::api::effect_runtime *, reshade::api::command_list *, reshade::api::resource_view,
                   reshade::api::resource_view);
void end_effects(reshade::api::effect_runtime *, reshade::api::command_list *, reshade::api::resource_view,
                 reshade::api::resource_view);
// Call after effect reload or before runtime destruction. Existing effect handles
// are already invalid at these boundaries and must never be called again.
void forget_runtime(reshade::api::effect_runtime *);
void reset();
std::string status();
} // namespace xivrender::depth
