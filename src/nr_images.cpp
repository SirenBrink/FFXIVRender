#include "xivrender/nr_images.hpp"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>

namespace xivrender::nr {
namespace {
using Microsoft::WRL::ComPtr;
bool check(HRESULT hr, const char *operation, std::string &error) {
    if (SUCCEEDED(hr))
        return true;
    std::ostringstream out;
    out << operation << " failed (0x" << std::hex << unsigned(hr) << ')';
    error = out.str();
    return false;
}
DXGI_FORMAT depth_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
bool region(ID3D11Texture2D *texture, unsigned x, unsigned y, unsigned w, unsigned h) {
    if (!texture || !w || !h || w > 16384 || h > 16384)
        return false;
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    return d.SampleDesc.Count == 1 && d.ArraySize == 1 && d.MipLevels >= 1 &&
           (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) && x <= d.Width && y <= d.Height && w <= d.Width - x &&
           h <= d.Height - y;
}
struct Constants {
    unsigned output[2], model[2], output_origin[2], depth_origin[2];
    unsigned guide[2], motion_origin[2], motion_size[2], depth_inverted, padding;
    float jitter_uv[2], motion_normalize[2];
    float center, work, detail, colour;
    float skin_detail, skin_colour, environment_detail, environment_colour;
    unsigned separate_skin, linear_hdr, hdr_input, padding2;
};
static_assert(sizeof(Constants) == 128);
constexpr char shader[] = R"(
cbuffer Grid:register(b0) {
 uint2 OutputSize; uint2 ModelSize; uint2 OutputOrigin; uint2 DepthOrigin;
 uint2 GuideSize; uint2 MotionOrigin; uint2 MotionSize; uint DepthInverted; uint Padding;
 float2 JitterUV; float2 MotionNormalize;
 float Center; float Work; float Detail; float Colour;
 float SkinDetail; float SkinColour; float EnvironmentDetail; float EnvironmentColour;
 uint SeparateSkin; uint LinearHdr; uint HdrInput; uint Padding2;
};
Texture2D<float4> Clean:register(t0);
Texture2D<float4> Aux1:register(t1);
Texture2D<float4> Aux2:register(t2);
RWTexture2D<float4> PreparedColor:register(u0);
RWTexture2D<float> PreparedDepth:register(u1);
RWTexture2D<float2> PreparedMotion:register(u2);
// Symmetric rational peripheral compression with a continuous derivative at
// the central region. Extend outside the image for motion-vector endpoints.
float pack_axis(float uv) {
 if(Work>=1) return uv;
 float d=2*uv-1, r=abs(d), a=(Work-Center)/(1-Center), p=r;
 if(r>1) p=Work+(r-1)*a*a;
 else if(r>Center) { float t=(r-Center)/(1-Center); p=Center+(Work-Center)*t/(a+(1-a)*t); }
 return .5+.5*sign(d)*p/Work;
}
float unpack_axis(float uv) {
 if(Work>=1) return uv;
 float d=(2*uv-1)*Work, r=abs(d), a=(Work-Center)/(1-Center), p=r;
 if(r>Work) p=1+(r-Work)/(a*a);
 else if(r>Center) { float y=(r-Center)/(Work-Center); p=Center+(1-Center)*a*y/(1-(1-a)*y); }
 return .5+.5*sign(d)*p;
}
float2 pack_uv(float2 uv) { return float2(pack_axis(uv.x),pack_axis(uv.y)); }
float2 unpack_uv(float2 uv) { return float2(unpack_axis(uv.x),unpack_axis(uv.y)); }
float3 hdr_linear(float3 v){v=saturate(v);return float3(v.r<=.04045?v.r/12.92:pow((v.r+.055)/1.055,2.4),v.g<=.04045?v.g/12.92:pow((v.g+.055)/1.055,2.4),v.b<=.04045?v.b/12.92:pow((v.b+.055)/1.055,2.4));}
float3 hdr_encode(float3 v){v=max(v,0);return float3(v.r<=.0031308?v.r*12.92:1.055*pow(v.r,1/2.4)-.055,v.g<=.0031308?v.g*12.92:1.055*pow(v.g,1/2.4)-.055,v.b<=.0031308?v.b*12.92:1.055*pow(v.b,1/2.4)-.055);}
float3 hdr_pack(float3 v){v=max(v,0);float peak=max(v.r,max(v.g,v.b));if(peak>.75){float e=peak-.75;v*=(.75+.25*e/(.25+e))/peak;}return saturate(hdr_encode(v));}
float3 hdr_expand(float3 v){v=hdr_linear(v);float peak=max(v.r,max(v.g,v.b));if(peak>.75){float e=peak-.75;v*=min(.75+.25*e/max(1-peak,1e-6),60000)/peak;}return v;}
float4 sample_clean(float2 uv) {
 float2 p=uv*OutputSize-.5;
 int2 a=int2(floor(p)), b=a+1;
 float2 f=frac(p);
 a=clamp(a,0,int2(OutputSize)-1); b=clamp(b,0,int2(OutputSize)-1);
 float4 c00=Clean.Load(int3(OutputOrigin+uint2(a),0));
 float4 c10=Clean.Load(int3(OutputOrigin+uint2(b.x,a.y),0));
 float4 c01=Clean.Load(int3(OutputOrigin+uint2(a.x,b.y),0));
 float4 c11=Clean.Load(int3(OutputOrigin+uint2(b),0));
 return lerp(lerp(c00,c10,f.x),lerp(c01,c11,f.x),f.y);
}
[numthreads(8,8,1)] void Prepare(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=ModelSize)) return;
 float2 uv=unpack_uv((id.xy+.5)/ModelSize);
 float4 c=sample_clean(uv);
 PreparedColor[id.xy]=float4(all(isfinite(c.rgb))?(LinearHdr?(HdrInput?hdr_pack(c.rgb):hdr_encode(saturate(c.rgb))):max(c.rgb,0)):0,1);
 float2 guide_uv=uv+JitterUV;
 uint2 d=uint2(clamp(int2(floor(guide_uv*GuideSize)),0,int2(GuideSize)-1));
 uint2 m=uint2(clamp(int2(floor(guide_uv*MotionSize)),0,int2(MotionSize)-1));
 float z=Aux1.Load(int3(DepthOrigin+d,0)).r;
 float2 v=Aux2.Load(int3(MotionOrigin+m,0)).rg*MotionNormalize;
 PreparedDepth[id.xy]=isfinite(z)?z:(DepthInverted?0:1);
 PreparedMotion[id.xy]=all(isfinite(v))?pack_uv(uv+v)-pack_uv(uv):0;
}
RWTexture2D<float4> Composed:register(u0);
float3 sample_model(Texture2D<float4> image,float2 uv) {
 float2 p=uv*ModelSize-.5; int2 a=int2(floor(p)), b=a+1; float2 f=frac(p);
 a=clamp(a,0,int2(ModelSize)-1); b=clamp(b,0,int2(ModelSize)-1);
 return lerp(lerp(image.Load(int3(a,0)).rgb,image.Load(int3(b.x,a.y,0)).rgb,f.x),
             lerp(image.Load(int3(a.x,b.y,0)).rgb,image.Load(int3(b,0)).rgb,f.x),f.y);
}
[numthreads(8,8,1)] void Compose(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=OutputSize)) return;
 float2 uv=(id.xy+.5)/OutputSize;
 float4 clean=Clean.Load(int3(OutputOrigin+id.xy,0));
 float2 modelUV=pack_uv(uv);
 float3 edited=sample_model(Aux2,modelUV), reference=sample_model(Aux1,modelUV);
 float3 delta=edited-reference;
 if(LinearHdr) {
   // The runtime receives a bounded proxy. Restore only its edit, using the
   // exact input as reference; preserve bright scene energy and identity.
   float3 before=HdrInput?hdr_expand(reference):hdr_linear(reference);
   float3 after=HdrInput?hdr_expand(edited):hdr_linear(edited);
   float3 ratio=clamp(after/max(before,1e-4),.25,4);
   ratio=(abs(ratio-1)<.002 || abs(edited-reference)<1e-6)?1:ratio;
   delta=clean.rgb*(ratio-1);
 }
 float luma=dot(delta,float3(.2126,.7152,.0722));
 float detail=Detail, colour=Colour;
 if(SeparateSkin) {
   float3 c=saturate(clean.rgb);
   float y=dot(c,float3(.299,.587,.114));
   float2 cbcr=float2((c.b-y)*.564+.5,(c.r-y)*.713+.5);
   float2 distance=(cbcr-float2(.405,.600))/float2(.090,.110);
   float chroma=max(c.r,max(c.g,c.b))-min(c.r,min(c.g,c.b));
   float skin=(1-smoothstep(.55,1.35,length(distance)))*smoothstep(.02,.10,chroma);
   detail*=lerp(EnvironmentDetail,SkinDetail,skin);
   colour*=lerp(EnvironmentColour,SkinColour,skin);
 }
 float3 result=clean.rgb+detail*(luma.xxx+colour*(delta-luma.xxx));
 Composed[id.xy]=float4(all(isfinite(result))?result:clean.rgb,clean.a);
}
)";
struct StateGuard {
    ID3D11DeviceContext1 *context;
    ComPtr<ID3DDeviceContextState> previous;
    StateGuard(ID3D11DeviceContext1 *c, ID3DDeviceContextState *own) : context(c) {
        c->SwapDeviceContextState(own, &previous);
    }
    ~StateGuard() {
        ID3D11ShaderResourceView *no_srvs[3]{};
        ID3D11UnorderedAccessView *no_uavs[3]{};
        context->CSSetShaderResources(0, 3, no_srvs);
        context->CSSetUnorderedAccessViews(0, 3, no_uavs, nullptr);
        context->SwapDeviceContextState(previous.Get(), nullptr);
    }
};
} // namespace
struct Images::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3DDeviceContextState> own;
    ComPtr<ID3D11ComputeShader> prepare, compose;
    ComPtr<ID3D11Buffer> constants;
    struct View {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        DXGI_FORMAT format{};
    };
    std::array<View, 3> views;
    ComPtr<ID3D11Texture2D> composite;
    ComPtr<ID3D11UnorderedAccessView> composite_view;
    unsigned width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool initialize(ID3D11Device *d, std::string &error) {
        if (device.Get() == d && own && prepare && compose && constants)
            return true;
        if (!d) {
            error = "Native NR image device is unavailable";
            return false;
        }
        *this = Impl{};
        device = d;
        ComPtr<ID3D11Device1> d1;
        ComPtr<ID3D11DeviceContext> c;
        d->GetImmediateContext(&c);
        if (!check(d->QueryInterface(IID_PPV_ARGS(&d1)), "NR image device1", error) ||
            !check(c.As(&context), "NR image context1", error))
            return false;
        const auto level = d->GetFeatureLevel();
        D3D_FEATURE_LEVEL selected;
        if (!check(d1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                &selected, &own),
                   "NR context state", error))
            return false;
        for (const auto &[entry, target] :
             std::array<std::pair<const char *, ComPtr<ID3D11ComputeShader> *>, 2>{
                 {{"Prepare", &prepare}, {"Compose", &compose}}}) {
            ComPtr<ID3DBlob> code, diagnostics;
            if (!check(D3DCompile(shader, sizeof(shader), "XIVRender NR images", nullptr, nullptr, entry,
                                  "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &diagnostics),
                       "NR image shader", error)) {
                if (diagnostics)
                    error += ": " + std::string(static_cast<const char *>(diagnostics->GetBufferPointer()));
                return false;
            }
            if (!check(d->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                              target->ReleaseAndGetAddressOf()),
                       "NR compute shader", error))
                return false;
        }
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(Constants);
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return check(d->CreateBuffer(&desc, nullptr, &constants), "NR image constants", error);
    }
    bool view(unsigned slot, ID3D11Texture2D *texture, DXGI_FORMAT typed, std::string &error) {
        auto &v = views[slot];
        if (v.texture.Get() == texture && v.view && v.format == typed)
            return true;
        v = {};
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = typed;
        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels = 1;
        if (!check(device->CreateShaderResourceView(texture, &desc, &v.view), "NR input SRV", error))
            return false;
        v.texture = texture;
        v.format = typed;
        return true;
    }
    bool setup(const SourceFrame &f, unsigned mw, unsigned mh, Constants &c, std::string &error, bool guides,
               const NeuralRenderingSettings *settings) {
        if (!mw || !mh || mw > 16384 || mh > 16384 ||
            !region(f.color, f.output_x, f.output_y, f.output_width, f.output_height)) {
            error = "NR color subrect or model dimensions are invalid";
            return false;
        }
        D3D11_TEXTURE2D_DESC color{};
        f.color->GetDesc(&color);
        if (color.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
            color.Format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
            error = "NR requires a supported floating-point native SR output";
            return false;
        }
        if (!view(0, f.color, color.Format, error))
            return false;
        c = {};
        c = {{f.output_width, f.output_height},
             {mw, mh},
             {f.output_x, f.output_y},
             {f.depth_x, f.depth_y},
             {f.guide_width, f.guide_height},
             {f.motion_x, f.motion_y},
             {f.motion_width, f.motion_height},
             f.depth_inverted ? 1u : 0u,
             0,
             {0, 0},
             {0, 0},
             0,
             0,
             0,
             0,
             0,
             0,
             0,
             0,
             0,
             0,
             0,
             0};
        const NeuralRenderingSettings defaults;
        const auto &cfg = settings ? *settings : defaults;
        c.center = cfg.foveated ? cfg.central_fraction : 0;
        c.work = cfg.foveated ? cfg.work_fraction : 1;
        c.detail = cfg.apply_model ? cfg.detail_strength : 0;
        c.colour = cfg.colour_strength;
        c.skin_detail = cfg.skin_detail;
        c.skin_colour = cfg.skin_colour ? cfg.skin_colour_strength : 0;
        c.environment_detail = cfg.environment_detail;
        c.environment_colour = cfg.environment_colour;
        c.separate_skin = cfg.separate_skin ? 1u : 0u;
        c.linear_hdr = f.linear_hdr ? 1u : 0u;
        c.hdr_input = cfg.hdr_input ? 1u : 0u;
        if (guides) {
            if (!region(f.depth, f.depth_x, f.depth_y, f.guide_width, f.guide_height) ||
                !region(f.motion, f.motion_x, f.motion_y, f.motion_width, f.motion_height) ||
                !std::isfinite(f.jitter_x) || !std::isfinite(f.jitter_y) ||
                !std::isfinite(f.motion_scale_x) || !std::isfinite(f.motion_scale_y)) {
                error = "NR native guide metadata is unavailable or invalid";
                return false;
            }
            D3D11_TEXTURE2D_DESC depth{}, motion{};
            f.depth->GetDesc(&depth);
            f.motion->GetDesc(&motion);
            const auto df = depth_format(depth.Format);
            if (df == DXGI_FORMAT_UNKNOWN ||
                (motion.Format != DXGI_FORMAT_R16G16_FLOAT && motion.Format != DXGI_FORMAT_R32G32_FLOAT &&
                 motion.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
                 motion.Format != DXGI_FORMAT_R32G32B32A32_FLOAT)) {
                error = "NR native depth/motion format is unsupported";
                return false;
            }
            if (!view(1, f.depth, df, error) || !view(2, f.motion, motion.Format, error))
                return false;
            c.jitter_uv[0] = f.jitter_x / f.guide_width;
            c.jitter_uv[1] = f.jitter_y / f.guide_height;
            c.motion_normalize[0] = f.motion_scale_x / f.output_width;
            c.motion_normalize[1] = f.motion_scale_y / f.output_height;
        }
        context->UpdateSubresource(constants.Get(), 0, nullptr, &c, 0, 0);
        return true;
    }
};
Images::Images() : impl_(std::make_unique<Impl>()) {}
Images::~Images() = default;
void Images::Reset() {
    *impl_ = Impl{};
}
bool Images::Prepare(ID3D11Device *device, const SourceFrame &f, unsigned mw, unsigned mh,
                     ID3D11UnorderedAccessView *color, ID3D11UnorderedAccessView *depth,
                     ID3D11UnorderedAccessView *motion, std::string &error,
                     const NeuralRenderingSettings *settings) {
    error.clear();
    auto &s = *impl_;
    if (!color || !depth || !motion || !s.initialize(device, error))
        return false;
    Constants constants{};
    if (!s.setup(f, mw, mh, constants, error, true, settings))
        return false;
    StateGuard guard(s.context.Get(), s.own.Get());
    ID3D11Buffer *buffers[]{s.constants.Get()};
    ID3D11ShaderResourceView *inputs[]{s.views[0].view.Get(), s.views[1].view.Get(), s.views[2].view.Get()};
    ID3D11UnorderedAccessView *outputs[]{color, depth, motion};
    s.context->CSSetShader(s.prepare.Get(), nullptr, 0);
    s.context->CSSetConstantBuffers(0, 1, buffers);
    s.context->CSSetShaderResources(0, 3, inputs);
    s.context->CSSetUnorderedAccessViews(0, 3, outputs, nullptr);
    s.context->Dispatch((mw + 7) / 8, (mh + 7) / 8, 1);
    return true;
}
bool Images::Composite(ID3D11Device *device, const SourceFrame &f, unsigned mw, unsigned mh,
                       ID3D11ShaderResourceView *input, ID3D11ShaderResourceView *output, std::string &error,
                       const NeuralRenderingSettings *settings) {
    error.clear();
    auto &s = *impl_;
    if (!input || !output || !s.initialize(device, error))
        return false;
    Constants constants{};
    if (!s.setup(f, mw, mh, constants, error, false, settings))
        return false;
    D3D11_TEXTURE2D_DESC source{};
    f.color->GetDesc(&source);
    if (!s.composite || s.width != f.output_width || s.height != f.output_height ||
        s.format != source.Format) {
        s.composite.Reset();
        s.composite_view.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = f.output_width;
        d.Height = f.output_height;
        d.ArraySize = d.MipLevels = d.SampleDesc.Count = 1;
        d.Format = source.Format;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (!check(device->CreateTexture2D(&d, nullptr, &s.composite), "NR composition texture", error) ||
            !check(device->CreateUnorderedAccessView(s.composite.Get(), nullptr, &s.composite_view),
                   "NR composition UAV", error))
            return false;
        s.width = f.output_width;
        s.height = f.output_height;
        s.format = source.Format;
    }
    StateGuard guard(s.context.Get(), s.own.Get());
    ID3D11Buffer *buffers[]{s.constants.Get()};
    ID3D11ShaderResourceView *inputs[]{s.views[0].view.Get(), input, output};
    ID3D11UnorderedAccessView *outputs[]{s.composite_view.Get()};
    s.context->CSSetShader(s.compose.Get(), nullptr, 0);
    s.context->CSSetConstantBuffers(0, 1, buffers);
    s.context->CSSetShaderResources(0, 3, inputs);
    s.context->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
    s.context->Dispatch((f.output_width + 7) / 8, (f.output_height + 7) / 8, 1);
    ID3D11UnorderedAccessView *none[]{nullptr};
    ID3D11ShaderResourceView *no_inputs[3]{};
    s.context->CSSetUnorderedAccessViews(0, 1, none, nullptr);
    s.context->CSSetShaderResources(0, 3, no_inputs);
    const D3D11_BOX box{0, 0, 0, f.output_width, f.output_height, 1};
    s.context->CopySubresourceRegion(f.color, 0, f.output_x, f.output_y, 0, s.composite.Get(), 0, &box);
    return true;
}
} // namespace xivrender::nr
