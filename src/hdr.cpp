#include "xivrender/hdr.hpp"
#include "xivrender/hdr_composition.hpp"
#include "xivrender/d3d11_identity.hpp"
#include "xivrender/hdr_scene.hpp"
#include "xivrender/app.hpp"
#include <MinHook.h>
#include <d3dcompiler.h>
#include <d3d11_1.h>
#include <atomic>
#include <array>
#include <mutex>
#include <sstream>
#include <map>
namespace xivrender::hdr {
using Microsoft::WRL::ComPtr;
namespace {
std::atomic<bool> enabled{false};
std::mutex options_mutex;
HdrSettings requested;
std::string message = "HDR is disabled";
thread_local bool inside = false;
struct Entry {
    bool prior = inside;
    Entry() {
        inside = true;
    }
    ~Entry() {
        inside = prior;
    }
};
using Draw = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
using DrawIndexed = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, INT);
using DrawInstanced = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
using DrawIndexedInstanced = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
using Copy = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
using CopyRegion = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT,
                                             ID3D11Resource *, UINT, const D3D11_BOX *);
using Finish = HRESULT(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, BOOL, ID3D11CommandList **);
using Execute = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11CommandList *, BOOL);
using Clear = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11RenderTargetView *, const FLOAT *);
using Indirect = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
using Auto = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *);
struct Functions {
    Draw draw = nullptr;
    DrawIndexed indexed = nullptr;
    DrawInstanced instanced = nullptr;
    DrawIndexedInstanced indexed_instanced = nullptr;
    Copy copy = nullptr;
    CopyRegion region = nullptr;
    Finish finish = nullptr;
    Execute execute = nullptr;
    Clear clear = nullptr;
    Auto automatic = nullptr;
    Indirect indirect = nullptr, indexed_indirect = nullptr;
};
Functions functions[2];
std::atomic<bool> kinds_ready[2]{};
ComPtr<ID3D11DeviceContext> hooked;
ComPtr<ID3D11Device> hook_device;
std::mutex hook_mutex;
std::vector<void *> native_hook_targets;
std::map<void *, void *> trampolines;
std::atomic<unsigned> calls{0};
struct Call {
    Call() {
        ++calls;
    }
    ~Call() {
        --calls;
    }
};
inline constexpr GUID record_id = {
    0x79e9bc48, 0x2330, 0x4139, {0xb8, 0x1c, 0x82, 0xc8, 0xb3, 0x43, 0x2a, 0xb6}};
struct Recording final : IUnknown {
    std::atomic<ULONG> references{1};
    scene::Graph graph;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown))
            return E_NOINTERFACE;
        *out = static_cast<IUnknown *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --references;
        if (!n)
            delete this;
        return n;
    }
};
ComPtr<Recording> recording(ID3D11DeviceChild *object) {
    IUnknown *data = nullptr;
    UINT size = sizeof(data);
    ComPtr<Recording> result;
    if (object && SUCCEEDED(object->GetPrivateData(record_id, &size, &data)) && data)
        result.Attach(static_cast<Recording *>(data));
    return result;
}
unsigned kind(ID3D11DeviceContext *c) {
    return c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? 1u : 0u;
}
Functions &originals(ID3D11DeviceContext *c) {
    const auto k = kind(c);
    return functions[kinds_ready[k] ? k : (kinds_ready[0] ? 0u : 1u)];
}
bool tracked(ID3D11DeviceContext *c, const ComPtr<Recording> &record) {
    return c == hooked.Get() || bool(record);
}
template <class F> void route(ID3D11DeviceContext *c, F &&original) {
    Call call;
    if (inside || !active()) {
        original();
        return;
    }
    auto record = recording(c);
    if (!tracked(c, record)) {
        original();
        return;
    }
    Entry guard;
    ui::BeforeDraw(c, original, record ? &record->graph.masks : nullptr);
    original();
    scene::AfterDraw(c, original, record ? &record->graph : nullptr);
}
void STDMETHODCALLTYPE on_draw(ID3D11DeviceContext *c, UINT a, UINT b) {
    route(c, [&] { originals(c).draw(c, a, b); });
}
void STDMETHODCALLTYPE on_indexed(ID3D11DeviceContext *c, UINT a, UINT b, INT d) {
    route(c, [&] { originals(c).indexed(c, a, b, d); });
}
void STDMETHODCALLTYPE on_instanced(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, UINT e) {
    route(c, [&] { originals(c).instanced(c, a, b, d, e); });
}
void STDMETHODCALLTYPE on_indexed_instanced(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, INT e, UINT f) {
    route(c, [&] { originals(c).indexed_instanced(c, a, b, d, e, f); });
}
void STDMETHODCALLTYPE on_auto(ID3D11DeviceContext *c) {
    route(c, [&] { originals(c).automatic(c); });
}
void STDMETHODCALLTYPE on_indirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT n) {
    route(c, [&] { originals(c).indirect(c, b, n); });
}
void STDMETHODCALLTYPE on_indexed_indirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT n) {
    route(c, [&] { originals(c).indexed_indirect(c, b, n); });
}
void STDMETHODCALLTYPE on_copy(ID3D11DeviceContext *c, ID3D11Resource *d, ID3D11Resource *s) {
    Call call;
    originals(c).copy(c, d, s);
    if (inside || !active())
        return;
    auto record = recording(c);
    if (!tracked(c, record))
        return;
    Entry guard;
    scene::Copy(d, s, true, c, record ? &record->graph : nullptr);
}
void STDMETHODCALLTYPE on_region(ID3D11DeviceContext *c, ID3D11Resource *d, UINT sub, UINT x, UINT y, UINT z,
                                 ID3D11Resource *s, UINT from, const D3D11_BOX *b) {
    Call call;
    originals(c).region(c, d, sub, x, y, z, s, from, b);
    if (inside || !active())
        return;
    auto record = recording(c);
    if (!tracked(c, record))
        return;
    Entry guard;
    scene::Copy(d, s, !sub && !from && !x && !y && !z && !b, c, record ? &record->graph : nullptr);
}
void STDMETHODCALLTYPE on_clear(ID3D11DeviceContext *c, ID3D11RenderTargetView *v, const FLOAT *color) {
    Call call;
    originals(c).clear(c, v, color);
    if (inside || !active() || !v)
        return;
    auto record = recording(c);
    if (!tracked(c, record))
        return;
    ComPtr<ID3D11Resource> target;
    v->GetResource(&target);
    auto &s = scene::State();
    std::lock_guard lock(s.mutex);
    scene::Invalidate(s, record ? &record->graph : nullptr, target.Get());
}
HRESULT STDMETHODCALLTYPE on_finish(ID3D11DeviceContext *c, BOOL restore, ID3D11CommandList **out) {
    Call call;
    const auto hr = originals(c).finish(c, restore, out);
    if (FAILED(hr) || !out || !*out || !active())
        return hr;
    auto source = recording(c);
    if (!source)
        return hr;
    auto data = new Recording;
    data->graph = std::move(source->graph);
    source->graph = {};
    (*out)->SetPrivateDataInterface(record_id, data);
    data->Release();
    auto &s = scene::State();
    std::lock_guard lock(s.mutex);
    ++s.recorded_lists;
    return hr;
}
void STDMETHODCALLTYPE on_execute(ID3D11DeviceContext *c, ID3D11CommandList *commands, BOOL restore) {
    Call call;
    auto data = active() && !inside ? recording(commands) : ComPtr<Recording>{};
    if (!data || c != hooked.Get()) {
        originals(c).execute(c, commands, restore);
        return;
    }
    Entry guard;
    ui::BeforeExecute(c, data->graph.masks);
    originals(c).execute(c, commands, restore);
    // Publish only after the real command list was queued. Recorded but never
    // executed work must never masquerade as the current scene HDR input.
    scene::Publish(data->graph);
}
constexpr char hdr_source[] = XIVRENDER_HDR_COMPOSITION_HLSL R"(
cbuffer Settings:register(b0){ uint Width;uint Height;uint Mode;uint HasScene;uint2 Origin;uint HasMask;uint HasEffects;float4 Rect;float Paper;float Peak;float Contrast;float Saturation;float Vibrance;float Black;uint Pq;uint Padding; };
Texture2D<float4> Source:register(t0);Texture2D<float4> Scene:register(t1);Texture2D<float4> Reference:register(t2);Texture2D<float4> Mask:register(t3);Texture2D<float4> Effects:register(t4);
RWTexture2D<float4> Target:register(u0);
float3 Decode(float3 v){return float3(v.r<=.04045?v.r/12.92:pow((v.r+.055)/1.055,2.4),v.g<=.04045?v.g/12.92:pow((v.g+.055)/1.055,2.4),v.b<=.04045?v.b/12.92:pow((v.b+.055)/1.055,2.4));}
float3 encode(float3 v){v=max(v,0);return float3(v.r<=.0031308?v.r*12.92:1.055*pow(v.r,1/2.4)-.055,v.g<=.0031308?v.g*12.92:1.055*pow(v.g,1/2.4)-.055,v.b<=.0031308?v.b*12.92:1.055*pow(v.b,1/2.4)-.055);}
float3 compress(float3 v){v=max(v,0);float p=max(v.r,max(v.g,v.b));if(p>.75){float e=p-.75;v*=(.75+.25*e/(.25+e))/p;}return saturate(encode(v));}
float3 guide(Texture2D<float4> t,float2 uv){uint w,h;t.GetDimensions(w,h);float2 p=(Rect.xy+uv*Rect.zw)*float2(w,h)-.5;int2 a=int2(floor(p));float2 f=frac(p);int2 lo=int2(round(Rect.xy*float2(w,h))),hi=int2(round((Rect.xy+Rect.zw)*float2(w,h)))-1;return lerp(lerp(t.Load(int3(clamp(a,lo,hi),0)).rgb,t.Load(int3(clamp(a+int2(1,0),lo,hi),0)).rgb,f.x),lerp(t.Load(int3(clamp(a+int2(0,1),lo,hi),0)).rgb,t.Load(int3(clamp(a+1,lo,hi),0)).rgb,f.x),f.y);}
[numthreads(8,8,1)]void Compute(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=uint2(Width,Height)))return;
 float4 s=Source.Load(int3(Origin+id.xy,0));
 if(Mode==0){float3 base=Decode(saturate(s.rgb));if(HasScene){float2 uv=(id.xy+.5)/float2(Width,Height);float3 ref=Decode(saturate(guide(Reference,uv))),hdr=sign(guide(Scene,uv))*pow(abs(guide(Scene,uv)),2.2);float3 relative=base/max(ref,1e-6);base=ref>1e-6?relative*hdr:hdr;}Target[id.xy]=float4(clamp(base,-60000,60000),1);}
 else Target[id.xy]=float4(compress(s.rgb),1);
}
void Vertex(uint id:SV_VertexID,out float4 p:SV_Position){p=float4(id==2?3:-1,id==1?3:-1,0,1);}
float3 pq(float3 n){float3 p=pow(max(n,0)/10000,2610.0/16384);return pow((3424.0/4096+(2413.0/128)*p)/(1+(2392.0/128)*p),2523.0/32);}
float4 Pixel(float4 position:SV_Position):SV_Target{
 uint2 p=uint2(position.xy);float3 s=Source.Load(int3(p,0)).rgb;
 float3 rgb=Decode(saturate(s))*Paper;
 if(HasScene){float3 h=Scene.Load(int3(p,0)).rgb,r=Decode(saturate(Reference.Load(int3(p,0)).rgb));float trans=HasMask?saturate(Mask.Load(int3(p,0)).r):1;float3 before=HasEffects?Decode(saturate(Effects.Load(int3(p,0)).rgb)):0;
 rgb=hdr_compose(rgb/Paper,h,r,trans,before,HasEffects,Paper,Peak);}
 rgb=max(rgb,0);float y=dot(rgb,float3(.2126,.7152,.0722));
 if(y>0 && Contrast!=1)rgb*=pow(max(y/Paper,1e-6),Contrast-1);
 y=dot(rgb,float3(.2126,.7152,.0722));float high=max(rgb.r,max(rgb.g,rgb.b)),low=min(rgb.r,min(rgb.g,rgb.b));
 float amount=Saturation*(1+Vibrance*(1-saturate((high-low)/max(high,1e-6))));rgb=y+(rgb-y)*amount;rgb=max(rgb,0)+Black;
 high=max(rgb.r,max(rgb.g,rgb.b));if(high>Peak)rgb*=Peak/high;
 if(Pq){float3 wide=float3(dot(rgb,float3(.627404,.329282,.043314)),dot(rgb,float3(.069097,.919540,.011362)),dot(rgb,float3(.016391,.088013,.895595)));return float4(pq(min(wide,Peak)),1);}
 return float4(rgb/80,1);
})";
struct Constants {
    unsigned w, h, mode, has_scene, origin[2], has_mask, has_effects;
    float rect[4], paper, peak, contrast, saturation, vibrance, black;
    unsigned pq, padding;
};
static_assert(sizeof(Constants) == 80);
struct Image {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
struct Core {
    std::recursive_mutex mutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3DDeviceContextState> state;
    ComPtr<ID3D11ComputeShader> compute;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Buffer> constants;
    Image input, sr_output, scene, reference, effects;
    bool ready = false, scene_valid = false, effects_valid = false, history_claimed = false;
    unsigned scene_w = 0, scene_h = 0;
};
Core &core() {
    static auto *s = new Core;
    return *s;
}
bool ok(HRESULT hr, const char *what) {
    if (SUCCEEDED(hr))
        return true;
    report(what);
    return false;
}
bool allocate(Core &s, Image &i, unsigned w, unsigned h, DXGI_FORMAT format, bool writable) {
    D3D11_TEXTURE2D_DESC old{};
    if (i.texture)
        i.texture->GetDesc(&old);
    if (old.Width == w && old.Height == h && old.Format == format && i.srv && (!writable || i.uav))
        return true;
    Image next;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.ArraySize = d.MipLevels = d.SampleDesc.Count = 1;
    d.Format = format;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (writable ? D3D11_BIND_UNORDERED_ACCESS : 0);
    if (!ok(s.device->CreateTexture2D(&d, nullptr, &next.texture), "HDR texture allocation failed") ||
        !ok(s.device->CreateShaderResourceView(next.texture.Get(), nullptr, &next.srv),
            "HDR texture SRV failed") ||
        (writable && !ok(s.device->CreateUnorderedAccessView(next.texture.Get(), nullptr, &next.uav),
                         "HDR texture UAV failed")))
        return false;
    i = std::move(next);
    return true;
}
bool initialize(ID3D11DeviceContext *c) {
    auto &s = core();
    ComPtr<ID3D11Device> d;
    c->GetDevice(&d);
    d = d3d11_identity::native_device(d.Get());
    auto native_context = d3d11_identity::native_context(c);
    if (!d || !native_context) {
        report("HDR requires a verified native immediate D3D11 context");
        return false;
    }
    if (s.device == d && s.ready)
        return true;
    if (s.device) {
        report(s.device == d ? "HDR device initialization is incomplete"
                             : "HDR received a different native D3D11 device");
        return false;
    }
    s.device = d;
    ComPtr<ID3D11Device1> d1;
    if (FAILED(d.As(&d1)) || FAILED(native_context.As(&s.context)))
        return false;
    D3D_FEATURE_LEVEL level = d->GetFeatureLevel(), selected;
    if (FAILED(d1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                            &selected, &s.state)))
        return false;
    for (auto entry : {"Compute", "Vertex", "Pixel"}) {
        ComPtr<ID3DBlob> code, errors;
        const char *target = entry[0] == 'C' ? "cs_5_0" : entry[0] == 'V' ? "vs_5_0" : "ps_5_0";
        if (FAILED(D3DCompile(hdr_source, sizeof(hdr_source), "XIVRender HDR", nullptr, nullptr, entry,
                              target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
            if (errors)
                report(static_cast<const char *>(errors->GetBufferPointer()));
            return false;
        }
        HRESULT hr =
            entry[0] == 'C'
                ? d->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s.compute)
            : entry[0] == 'V'
                ? d->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s.vertex)
                : d->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s.pixel);
        if (FAILED(hr))
            return false;
    }
    D3D11_BUFFER_DESC b{};
    b.ByteWidth = sizeof(Constants);
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    s.ready = SUCCEEDED(d->CreateBuffer(&b, nullptr, &s.constants));
    return s.ready;
}
struct Guard {
    Core &s;
    ComPtr<ID3DDeviceContextState> prior;
    Entry entry;
    Guard(Core &v) : s(v) {
        s.context->SwapDeviceContextState(s.state.Get(), &prior);
    }
    ~Guard() {
        s.context->ClearState();
        s.context->SwapDeviceContextState(prior.Get(), nullptr);
    }
};
ComPtr<ID3D11ShaderResourceView> view(Core &s, ID3D11Texture2D *t) {
    ComPtr<ID3D11ShaderResourceView> v;
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = scene::Typed(d.Format);
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    s.device->CreateShaderResourceView(t, &desc, &v);
    return v;
}
Constants constants(unsigned w, unsigned h) {
    const auto cfg = options();
    Constants c{};
    c.w = w;
    c.h = h;
    c.rect[2] = c.rect[3] = 1;
    c.paper = cfg.paper_white_nits;
    c.peak = cfg.peak_nits;
    c.contrast = cfg.contrast;
    c.saturation = cfg.saturation;
    c.vibrance = cfg.vibrance;
    c.black = cfg.black_nits;
    return c;
}
void dispatch(Core &s, const Constants &b, ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *c,
              ID3D11ShaderResourceView *d, ID3D11UnorderedAccessView *out) {
    s.context->UpdateSubresource(s.constants.Get(), 0, nullptr, &b, 0, 0);
    ID3D11Buffer *buf = s.constants.Get();
    ID3D11ShaderResourceView *inputs[]{a, c, d};
    s.context->CSSetShader(s.compute.Get(), nullptr, 0);
    s.context->CSSetConstantBuffers(0, 1, &buf);
    s.context->CSSetShaderResources(0, 3, inputs);
    s.context->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
    s.context->Dispatch((b.w + 7) / 8, (b.h + 7) / 8, 1);
    ID3D11UnorderedAccessView *none = nullptr;
    ID3D11ShaderResourceView *empty[3]{};
    s.context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    s.context->CSSetShaderResources(0, 3, empty);
}
} // namespace
void configure(const Settings &s, bool startup) {
    std::lock_guard lock(options_mutex);
    requested = s.hdr;
    if (startup) {
        enabled = s.hdr.enabled;
        message = enabled ? "HDR enabled; waiting for scene and HDR presentation" : "HDR is disabled";
    }
}
bool active() {
    return enabled.load();
}
void commit_output(bool hdr) {
    if (enabled.exchange(hdr) != hdr) {
        finish_frame();
        report(hdr ? "Scene HDR enabled live" : "SDR output enabled live");
    }
}
void disable(const char *reason) {
    enabled = false;
    report(reason);
}
bool warmup(ID3D11DeviceContext *c, unsigned w, unsigned h) {
    auto &s = core();
    std::lock_guard lock(s.mutex);
    return initialize(c) && allocate(s, s.sr_output, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true) &&
           allocate(s, s.scene, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false) &&
           allocate(s, s.reference, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
}
HdrSettings options() {
    std::lock_guard lock(options_mutex);
    return requested;
}
void report(const char *text) {
    std::lock_guard lock(options_mutex);
    message = text;
}
std::string status() {
    std::string current;
    {
        std::lock_guard lock(options_mutex);
        current = message;
    }
    if (active())
        current += "; " + scene::Summary();
    return current;
}
void tag(ID3D11PixelShader *ps, const void *data, size_t size) {
    if (!ps || !data)
        return;
    unsigned crc = ~0u;
    auto *b = static_cast<const unsigned char *>(data);
    while (size--) {
        crc ^= *b++;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    scene::Tag(ps, ~crc);
}
bool attach(ID3D11DeviceContext *c, std::string &error) {
    auto native = d3d11_identity::unwrap_context(c);
    if (!native) {
        error = "HDR context identity unavailable";
        return false;
    }
    c = native.Get();
    ComPtr<ID3D11Device> device;
    c->GetDevice(&device);
    device = d3d11_identity::native_device(device.Get());
    std::lock_guard lock(hook_mutex);
    if (!device || (hook_device && hook_device != device)) {
        error = "HDR context belongs to another native device";
        return false;
    }
    hook_device = device;
    const auto k = kind(c);
    if (!k) {
        if (hooked && hooked.Get() != c) {
            error = "HDR already owns another immediate context";
            return false;
        }
        if (!hooked)
            hooked = c;
    } else if (!recording(c)) {
        auto *data = new Recording;
        const auto hr = c->SetPrivateDataInterface(record_id, data);
        data->Release();
        if (FAILED(hr)) {
            error = "HDR deferred metadata attachment failed";
            return false;
        }
    }
    if (kinds_ready[k])
        return true;
    auto &f = functions[k];
    auto **v = *reinterpret_cast<void ***>(c);
    std::vector<void *> added;
    const auto add = [&](unsigned index, void *hook, void **saved) {
        void *target = v[index];
        if (auto found = trampolines.find(target); found != trampolines.end()) {
            *saved = found->second;
            return true;
        }
        if (MH_CreateHook(target, hook, saved) != MH_OK)
            return false;
        trampolines[target] = *saved;
        added.push_back(target);
        return true;
    };
    const bool created =
        add(13, reinterpret_cast<void *>(on_draw), reinterpret_cast<void **>(&f.draw)) &&
        add(12, reinterpret_cast<void *>(on_indexed), reinterpret_cast<void **>(&f.indexed)) &&
        add(21, reinterpret_cast<void *>(on_instanced), reinterpret_cast<void **>(&f.instanced)) &&
        add(20, reinterpret_cast<void *>(on_indexed_instanced),
            reinterpret_cast<void **>(&f.indexed_instanced)) &&
        add(38, reinterpret_cast<void *>(on_auto), reinterpret_cast<void **>(&f.automatic)) &&
        add(39, reinterpret_cast<void *>(on_indexed_indirect),
            reinterpret_cast<void **>(&f.indexed_indirect)) &&
        add(40, reinterpret_cast<void *>(on_indirect), reinterpret_cast<void **>(&f.indirect)) &&
        add(47, reinterpret_cast<void *>(on_copy), reinterpret_cast<void **>(&f.copy)) &&
        add(46, reinterpret_cast<void *>(on_region), reinterpret_cast<void **>(&f.region)) &&
        add(50, reinterpret_cast<void *>(on_clear), reinterpret_cast<void **>(&f.clear)) &&
        add(114, reinterpret_cast<void *>(on_finish), reinterpret_cast<void **>(&f.finish)) &&
        add(58, reinterpret_cast<void *>(on_execute), reinterpret_cast<void **>(&f.execute));
    if (!created) {
        for (auto *target : added) {
            MH_RemoveHook(target);
            trampolines.erase(target);
        }
        error = "HDR native context hook unavailable";
        return false;
    }
    kinds_ready[k] = true;
    for (auto *target : added)
        if (MH_EnableHook(target) != MH_OK) {
            for (auto *undo : added) {
                MH_DisableHook(undo);
                MH_RemoveHook(undo);
                trampolines.erase(undo);
            }
            kinds_ready[k] = false;
            error = "HDR context hook activation failed";
            return false;
        }
    native_hook_targets.insert(native_hook_targets.end(), added.begin(), added.end());
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&attach), &pinned);
    return true;
}
bool prepare_dlss(ID3D11DeviceContext *c, ID3D11Texture2D *n, unsigned w, unsigned h, unsigned x, unsigned y,
                  unsigned ow, unsigned oh, DlssInput &out) {
    if (!active() || !n || !w || !h || !ow || !oh)
        return false;
    D3D11_TEXTURE2D_DESC native{};
    n->GetDesc(&native);
    if (native.SampleDesc.Count != 1 || x > native.Width || y > native.Height || w > native.Width - x ||
        h > native.Height - y) {
        report("HDR DLSS input subrect exceeds its native texture");
        return false;
    }
    auto &s = core();
    std::lock_guard lock(s.mutex);
    if (!initialize(c))
        return false;
    // Each native feature retains its own immutable DLSS output. Switching a
    // quality/model handle cannot recycle an older handle's history buffer.
    if (out.output) {
        if (s.sr_output.texture != out.output) {
            s.sr_output = {};
            s.sr_output.texture = out.output;
            if (!ok(s.device->CreateShaderResourceView(out.output.Get(), nullptr, &s.sr_output.srv),
                    "HDR history view creation failed") ||
                !ok(s.device->CreateUnorderedAccessView(out.output.Get(), nullptr, &s.sr_output.uav),
                    "HDR history write view creation failed"))
                return false;
        }
    } else if (s.sr_output.texture && s.sr_output.texture.Get() != out.output.Get()) {
        // The startup warmup buffer can be claimed by the first feature only.
        if (s.history_claimed)
            s.sr_output = {};
        s.history_claimed = true;
    }
    if (!initialize(c) || !allocate(s, s.input, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true) ||
        !allocate(s, s.sr_output, ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, true) ||
        !allocate(s, s.scene, ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, false))
        return false;
    auto src = view(s, n);
    if (!src)
        return false;
    auto captured = scene::Find(n);
    auto b = constants(w, h);
    b.origin[0] = x;
    b.origin[1] = y;
    if (captured &&
        (!captured->srv || captured->crop[2] <= 0 || captured->crop[3] <= 0 ||
         std::abs(double(captured->crop[2]) * h - double(captured->crop[3]) * w) > 2.0 * std::max(w, h)))
        captured.reset();
    if (captured) {
        b.has_scene = 1;
        for (unsigned i = 0; i < 4; ++i)
            b.rect[i] = captured->crop[i] / float(i % 2 ? captured->height : captured->width);
    }
    Guard guard(s);
    ComPtr<ID3D11ShaderResourceView> ref;
    if (captured)
        ref = view(s, captured->reference.Get());
    dispatch(s, b, src.Get(), captured ? captured->srv.Get() : nullptr, ref.Get(), s.input.uav.Get());
    s.history_claimed = true;
    out = {s.input.texture, s.sr_output.texture, s.scene.texture, w, h};
    report(captured ? "RenoDX scene HDR feeding native DLSS"
                    : "HDR DLSS active; waiting for recognized RenoDX scene input");
    return true;
}
bool begin_nr(ID3D11DeviceContext *c) {
    auto &s = core();
    std::lock_guard lock(s.mutex);
    if (!s.scene.texture || !s.sr_output.texture || !initialize(c))
        return false;
    Entry guard;
    s.context->CopyResource(s.scene.texture.Get(), s.sr_output.texture.Get());
    return true;
}
bool finish_dlss(ID3D11DeviceContext *c, ID3D11Texture2D *n, unsigned w, unsigned h, unsigned x, unsigned y) {
    if (!active() || !n)
        return false;
    auto &s = core();
    std::lock_guard lock(s.mutex);
    if (!initialize(c) || !s.scene.texture ||
        !allocate(s, s.reference, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true))
        return false;
    D3D11_TEXTURE2D_DESC target{};
    n->GetDesc(&target);
    if (target.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || x > target.Width || y > target.Height ||
        w > target.Width - x || h > target.Height - y)
        return false;
    Guard guard(s);
    auto b = constants(w, h);
    b.mode = 1;
    dispatch(s, b, s.scene.srv.Get(), nullptr, nullptr, s.reference.uav.Get());
    D3D11_BOX full{0, 0, 0, w, h, 1};
    s.context->CopySubresourceRegion(n, 0, x, y, 0, s.reference.texture.Get(), 0, &full);
    s.scene_valid = true;
    s.scene_w = w;
    s.scene_h = h;
    return true;
}

void begin_effects(ID3D11Texture2D *t) {
    if (!active())
        return;
    ui::SealFrame();
    auto &s = core();
    std::lock_guard lock(s.mutex);
    s.effects_valid = false;
    if (!t || !s.context || !options().preserve_reshade_highlights)
        return;
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    if (!allocate(s, s.effects, d.Width, d.Height, scene::Typed(d.Format), false))
        return;
    Entry guard;
    s.context->CopyResource(s.effects.texture.Get(), t);
    s.effects_valid = true;
}
bool present(ID3D11DeviceContext *c, ID3D11Texture2D *source, ID3D11RenderTargetView *target, unsigned w,
             unsigned h, bool pq) {
    if (!source || !target)
        return false;
    auto &s = core();
    std::lock_guard lock(s.mutex);
    if (!initialize(c))
        return false;
    auto src = view(s, source);
    if (!src)
        return false;
    auto mask = ui::Mask(s.device.Get(), w, h);
    auto b = constants(w, h);
    b.pq = pq ? 1u : 0u;
    b.has_scene = s.scene_valid && s.scene_w == w && s.scene_h == h;
    b.has_mask = mask ? 1u : 0u;
    b.has_effects = s.effects_valid ? 1u : 0u;
    Guard guard(s);
    s.context->UpdateSubresource(s.constants.Get(), 0, nullptr, &b, 0, 0);
    ID3D11Buffer *buf = s.constants.Get();
    ID3D11ShaderResourceView *inputs[]{src.Get(), b.has_scene ? s.scene.srv.Get() : nullptr,
                                       b.has_scene ? s.reference.srv.Get() : nullptr, mask.Get(),
                                       b.has_effects ? s.effects.srv.Get() : nullptr};
    s.context->PSSetShaderResources(0, 5, inputs);
    s.context->PSSetConstantBuffers(0, 1, &buf);
    s.context->VSSetShader(s.vertex.Get(), nullptr, 0);
    s.context->PSSetShader(s.pixel.Get(), nullptr, 0);
    s.context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
    s.context->RSSetViewports(1, &viewport);
    s.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s.context->Draw(3, 0);
    return true;
}
void finish_frame() {
    scene::EndFrame();
    ui::EndFrame();
    auto &s = core();
    std::lock_guard lock(s.mutex);
    s.scene_valid = false;
    s.effects_valid = false;
}
void shutdown() {
    enabled = false;
    std::lock_guard hooks_lock(hook_mutex);
    for (auto *t : native_hook_targets)
        MH_DisableHook(t);
    const auto deadline = GetTickCount64() + 500;
    while (calls && GetTickCount64() < deadline)
        Sleep(1);
    if (calls)
        return;
    for (auto *t : native_hook_targets)
        MH_RemoveHook(t);
    native_hook_targets.clear();
    trampolines.clear();
    kinds_ready[0] = kinds_ready[1] = false;
    hooked.Reset();
    hook_device.Reset();
    scene::Reset();
    ui::Reset();
    auto &s = core();
    std::lock_guard lock(s.mutex);
    s.input = {};
    s.sr_output = {};
    s.scene = {};
    s.reference = {};
    s.effects = {};
    s.compute.Reset();
    s.vertex.Reset();
    s.pixel.Reset();
    s.constants.Reset();
    s.state.Reset();
    s.context.Reset();
    s.device.Reset();
    s.ready = s.scene_valid = s.effects_valid = s.history_claimed = false;
}
} // namespace xivrender::hdr
