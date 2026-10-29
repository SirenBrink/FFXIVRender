#include "xivrender/frame_generation.hpp"
#include "xivrender/fg_compatibility.hpp"
#include "xivrender/fg_diagnostics.hpp"
#include "xivrender/hdr_composition.hpp"
#include "xivrender/fg_backend.hpp"
#include "xivrender/fg_policy.hpp"
#include "xivrender/fg_cohort.hpp"
#include "xivrender/d3d11_identity.hpp"
#include "xivrender/d3d11_work.hpp"
#include <d3d11_4.h>
#include <d3d12sdklayers.h>
#include <vector>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <mutex>
#include <deque>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <exception>
#include <condition_variable>
#include <limits>
#include <sstream>
#include <psapi.h>
#include <TraceLoggingProvider.h>

namespace xivrender::fg {
static bool probe_debug = false;
void enable_probe_debug() { probe_debug = true; }
bool probe_debug_enabled() { return probe_debug; }
using Microsoft::WRL::ComPtr;
namespace {
TRACELOGGING_DEFINE_PROVIDER(timing_provider, "XIVRender.FrameTiming",
                             (0x88e925ad, 0x5dbd, 0x4b7c, 0x9b, 0x60, 0x70, 0xc8, 0x75, 0x45, 0xa1, 0xd1));
void register_timing_provider() {
    static std::once_flag once;
    std::call_once(once, [] {
        // An ETW enable callback can arrive after addon shutdown. Keep the
        // provider's code/metadata alive for the process, like the native hooks.
        // Registration failure leaves all event writes as cheap safe no-ops.
        HMODULE owner = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&register_timing_provider), &owner))
            (void)TraceLoggingRegister(timing_provider);
    });
}
bool trace_enabled() {
    return TraceLoggingProviderEnabled(timing_provider, 5, 0);
}
void trace_stage(TraceStage stage, std::uint64_t frame, std::uint64_t source = 0, std::uint64_t detail = 0) {
    TraceLoggingWrite(timing_provider, "Stage", TraceLoggingLevel(5),
                      TraceLoggingUInt32(static_cast<unsigned>(stage), "Stage"),
                      TraceLoggingUInt64(frame, "Frame"), TraceLoggingUInt64(source, "Source"),
                      TraceLoggingUInt64(detail, "Detail"));
}
struct Global {
    std::recursive_mutex presentation_mutex;
    std::mutex mutex, engine_mutex;
    Settings settings;
    FrameGenerationSettings startup;
    std::filesystem::path root;
    bool output = false;
    Status status;
    PerformanceWindow performance;
    ComPtr<IDXGIAdapter3> adapter;
    CameraCohorts<Camera> cameras;
    std::uint64_t simulation = 0, simulation_ended = 0;
    std::weak_ptr<Backend> backend;
    std::condition_variable latency_idle;
    unsigned latency_calls = 0;
    struct Sink {
        virtual void capture_frame(const nr::FrameInput &) = 0;
        virtual void effects(ID3D11Texture2D *) = 0;
        virtual bool retire_source(const void *, std::string &) = 0;
    };
    Sink *sink = nullptr;
};
Global &global() {
    static auto *s = new Global;
    return *s;
}
// Track SDK calls that may sleep without holding the presentation lock. A
// handoff detaches new callers, stops the old latency controller, drains GPU
// work, then waits for its existing calls before destroying that controller.
struct LatencyCall {
    std::shared_ptr<Backend> backend;
    std::uint64_t id = 0;
    LatencyCall(unsigned marker) {
        auto &g = global();
        std::lock_guard lock(g.mutex);
        if (marker == 0)
            ++g.simulation;
        id = g.simulation;
        g.status.simulation_frames = g.simulation;
        if (marker == 1) {
            if (g.simulation_ended == id)
                return;
            g.simulation_ended = id;
        }
        backend = g.backend.lock();
        if (backend)
            ++g.latency_calls;
    }
    ~LatencyCall() {
        if (backend) {
            backend.reset();
            auto &g = global();
            std::lock_guard lock(g.mutex);
            --g.latency_calls;
            g.latency_idle.notify_all();
        }
    }
};
void update(const std::function<void(Status &)> &f) {
    auto &s = global();
    std::lock_guard lock(s.mutex);
    f(s.status);
}
bool ok(HRESULT result, const char *what, std::string &error) {
    if (SUCCEEDED(result))
        return true;
    error = std::string(what) + ": " + std::to_string(unsigned(result));
    return false;
}
struct Shared {
    ComPtr<ID3D11Texture2D> image11;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D12Resource> image12;
};
constexpr char shader[] = XIVRENDER_HDR_COMPOSITION_HLSL R"(
cbuffer Constants:register(b0){uint2 OutputSize;uint2 RenderSize;uint2 DepthOrigin;uint2 MotionOrigin;uint2 MotionSize;uint2 ColorOrigin;float2 MotionScale;float2 Jitter;float2 PrevJitter;uint MotionJittered;uint Mode;uint LinearInput;uint OutputMode;float Paper;float Peak;float Contrast;float Saturation;float Vibrance;float Black;uint HasEffects;uint PreserveEffects;};
Texture2D<float4> Colour:register(t0);Texture2D<float> Depth:register(t1);Texture2D<float2> Motion:register(t2);Texture2D<float4> Before:register(t3);Texture2D<float4> After:register(t4);
RWTexture2D<float> OutDepth:register(u0);RWTexture2D<float2> OutMotion:register(u1);RWTexture2D<float4> Hudless:register(u2);RWTexture2D<float4> Final:register(u3);
float3 decode(float3 x){x=max(x,0);return float3(x.r<=.04045?x.r/12.92:pow((x.r+.055)/1.055,2.4),x.g<=.04045?x.g/12.92:pow((x.g+.055)/1.055,2.4),x.b<=.04045?x.b/12.92:pow((x.b+.055)/1.055,2.4));}
float3 encode(float3 x){x=max(x,0);return float3(x.r<=.0031308?12.92*x.r:1.055*pow(x.r,1/2.4)-.055,x.g<=.0031308?12.92*x.g:1.055*pow(x.g,1/2.4)-.055,x.b<=.0031308?12.92*x.b:1.055*pow(x.b,1/2.4)-.055);}
float3 pq(float3 n){float3 p=pow(max(n,0)/10000,2610.0/16384);return pow((3424.0/4096+(2413.0/128)*p)/(1+(2392.0/128)*p),2523.0/32);}
[numthreads(8,8,1)]void Main(uint3 p:SV_DispatchThreadID){
 if(Mode==0){if(any(p.xy>=RenderSize))return;float d=Depth.Load(int3(DepthOrigin+p.xy,0));float2 uv=(p.xy+.5)/RenderSize;uint2 mp=min(uint2(uv*MotionSize),MotionSize-1);float2 mv=Motion.Load(int3(MotionOrigin+mp,0))*MotionScale*RenderSize/OutputSize;if(MotionJittered)mv-=PrevJitter-Jitter;OutDepth[p.xy]=isfinite(d)?d:0;OutMotion[p.xy]=all(isfinite(mv))?mv:0;return;}
 if(any(p.xy>=OutputSize))return;if(Mode==2){Final[p.xy]=After.Load(int3(p.xy,0));return;}float4 sample=Colour.Load(int3(ColorOrigin+p.xy,0));float3 rgb=LinearInput?sample.rgb:decode(sample.rgb);
 float3 gain=1,b=0,a=0;if(HasEffects){b=decode(saturate(Before.Load(int3(p.xy,0)).rgb));a=decode(saturate(After.Load(int3(p.xy,0)).rgb));gain=b>1e-5?clamp(a/max(b,1e-5),0,8):0;}
 if(OutputMode){rgb=max(hdr_compose(a,rgb,b,1,b,HasEffects&&PreserveEffects,Paper,Peak),0);float y=dot(rgb,float3(.2126,.7152,.0722));if(y>0&&Contrast!=1)rgb*=pow(max(y/Paper,1e-6),Contrast-1);y=dot(rgb,float3(.2126,.7152,.0722));float high=max(rgb.r,max(rgb.g,rgb.b)),low=min(rgb.r,min(rgb.g,rgb.b));float amount=Saturation*(1+Vibrance*(1-saturate((high-low)/max(high,1e-6))));rgb=max(y+(rgb-y)*amount,0)+Black;high=max(rgb.r,max(rgb.g,rgb.b));if(high>Peak)rgb*=Peak/high;if(OutputMode==1){float3 wide=float3(dot(rgb,float3(.627404,.329282,.043314)),dot(rgb,float3(.069097,.919540,.011362)),dot(rgb,float3(.016391,.088013,.895595)));rgb=pq(wide);}else rgb/=80;
 }else rgb=saturate(encode(rgb*gain));Hudless[p.xy]=float4(rgb,1);
})";
struct Constants {
    unsigned output[2], render[2], depth[2], motion[2], motion_size[2], color[2];
    float motion_scale[2], jitter[2], previous_jitter[2];
    unsigned motion_jittered, mode, linear_input, output_mode;
    float paper, peak, contrast, saturation, vibrance, black;
    unsigned has_effects, preserve_effects, padding[2];
};
static_assert(sizeof(Constants) % 16 == 0);
DXGI_FORMAT typed(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return f;
    }
}
} // namespace
struct Presenter::Impl final : Global::Sink {
    HMODULE runtime = nullptr;
    ComPtr<ID3D11Device> device11;
    ComPtr<IDXGIFactory> factory;
    DXGI_SWAP_CHAIN_DESC description{};
    FrameGenerationSettings creation;
    FrameGeneration provider = FrameGeneration::Off;
    std::uint64_t displayed_before = 0;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11DeviceContext1> state_context;
    ComPtr<ID3DDeviceContextState> state;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain4> swapchain;
    ComPtr<ID3D12Fence> producer, consumer;
    ComPtr<ID3D11Fence> producer11, consumer11;
    ComPtr<ID3D11ComputeShader> compute;
    ComPtr<ID3D11Buffer> constants;
    std::shared_ptr<Backend> backend;
    Shared final;
    struct InputSet {
        Shared hudless, depth, motion;
        unsigned rw = 0, rh = 0;
        std::uint64_t done = 0;
        bool in_present = false;
    };
    std::array<InputSet, 2> inputs;
    unsigned pending_input = 0;
    ComPtr<ID3D11Texture2D> scene, before;
    struct Slot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        std::uint64_t done = 0;
    };
    std::array<Slot, 3> slots;
    unsigned cursor = 0, width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    HWND window = nullptr;
    HANDLE event = nullptr;
    std::uint64_t serial = 0, submitted = 0;
    bool source_retired = false;
    bool valid = false, reset = true, have_before = false, dead = false, colour_ready = false,
         retirement_unknown = false;
    Packet packet;
    std::uint64_t latency_frame = 0;
    std::uint64_t last_latency_present = 0;
    bool have_latency_capture = false;
    bool guide_mode_known = false, variable_guides = false;
    FrameCohort cohort;
    FrameCohort::Counters reported_cohort;
    Constants values{};
    DynamicFactor dynamic;
    std::chrono::steady_clock::time_point last_time{};
    ULONGLONG rate_started = 0;
    std::uint64_t rate_real = 0, rate_displayed = 0;
    bool rate_request = false;
    bool fence(ComPtr<ID3D12Fence> &out, ComPtr<ID3D11Fence> &opened, std::string &error) {
        if (!ok(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&out)), "FG shared fence",
                error))
            return false;
        HANDLE h = nullptr;
        auto hr = device12->CreateSharedHandle(out.Get(), nullptr, GENERIC_ALL, nullptr, &h);
        if (FAILED(hr))
            return ok(hr, "FG fence handle", error);
        hr = device5->OpenSharedFence(h, IID_PPV_ARGS(&opened));
        CloseHandle(h);
        return ok(hr, "FG D3D11 fence", error);
    }
    bool shared(Shared &out, unsigned w, unsigned h, DXGI_FORMAT fmt, std::string &error) {
        Shared next;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = fmt;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (!ok(device11->CreateTexture2D(&d, nullptr, &next.image11), "FG shared image", error))
            return false;
        ComPtr<IDXGIResource1> r;
        HANDLE handle = nullptr;
        auto hr = next.image11.As(&r);
        if (SUCCEEDED(hr))
            hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                       nullptr, &handle);
        if (FAILED(hr))
            return ok(hr, "FG image handle", error);
        hr = device12->OpenSharedHandle(handle, IID_PPV_ARGS(&next.image12));
        CloseHandle(handle);
        if (!ok(hr, "FG D3D12 image", error) ||
            !ok(device11->CreateShaderResourceView(next.image11.Get(), nullptr, &next.srv), "FG image SRV",
                error) ||
            !ok(device11->CreateUnorderedAccessView(next.image11.Get(), nullptr, &next.uav), "FG image UAV",
                error) ||
            !ok(device11->CreateRenderTargetView(next.image11.Get(), nullptr, &next.rtv), "FG image RTV",
                error))
            return false;
        out = std::move(next);
        return true;
    }
    bool wait(std::uint64_t value, unsigned timeout, std::string &error) {
        const auto completed = consumer->GetCompletedValue();
        if (completed == std::numeric_limits<std::uint64_t>::max()) {
            error = "FG completion fence reports device removal";
            return false;
        }
        if (!value || completed >= value)
            return true;
        if (!ok(consumer->SetEventOnCompletion(value, event), "FG fence event", error))
            return false;
        if (WaitForSingleObject(event, timeout) != WAIT_OBJECT_0) {
            error = "FG GPU retirement is still pending";
            return false;
        }
        const auto retired = consumer->GetCompletedValue();
        if (retired == std::numeric_limits<std::uint64_t>::max() || retired < value) {
            error = "FG fence woke without successful GPU retirement";
            return false;
        }
        return true;
    }
    void quarantine_inputs() {
        retirement_unknown = dead = true;
        valid = have_before = colour_ready = false;
        packet = {};
        cohort.invalidate();
        reset = true;
        for (auto &input : inputs)
            input.in_present = true;
        auto &g = global();
        std::lock_guard lock(g.mutex);
        g.backend.reset();
        g.status.active = g.status.latency = g.status.native_dynamic = false;
        g.status.factor = 1;
        g.status.latency_name = "Disabled";
        g.status.input_status = "FG inputs quarantined after SDK retirement failure";
    }
    bool drain(unsigned timeout, std::string &error) {
        if (retirement_unknown) {
            error = "FG SDK input retirement is unknown; output generation retained until exit";
            return false;
        }
        if (!queue || !consumer)
            return true;
        // Initial allocation has no native writes to retire. Keep the freshly
        // enabled latency controller ready for the first simulation boundary.
        if (!submitted && !final.image11)
            return true;
        if (backend)
            backend->stop();
        // A native capture may have prepared the other input set during the
        // preceding SDK Present. Include all of its D3D11 writes in retirement.
        if (backend && !backend->drain_output(timeout, error)) return false;
        if (backend && backend->asynchronous_presentation())
            update([&](Status &s) { s.displayed = std::max(s.displayed, displayed_before + backend->displayed()); });
        const auto produced = ++serial;
        if (!ok(context11->Signal(producer11.Get(), produced), "FG drain producer signal", error))
            return false;
        context11->Flush();
        if (!ok(queue->Wait(producer.Get(), produced), "FG drain producer wait", error))
            return false;
        const auto done = ++serial;
        if (!ok(queue->Signal(consumer.Get(), done), "FG drain signal", error))
            return false;
        submitted = done;
        return wait(done, timeout, error);
    }
    bool retire(unsigned timeout, std::string &error) {
        diagnostics::event("retire.begin", 0, timeout, double(submitted));
        auto &g = global();
        {
            std::lock_guard lock(g.mutex);
            g.backend.reset();
        }
        if (backend)
            backend->stop();
        if (!drain(timeout, error))
            return false;
        std::unique_lock lock(g.mutex);
        if (!g.latency_idle.wait_for(lock, std::chrono::milliseconds(timeout),
                                     [&] { return g.latency_calls == 0; })) {
            error = "Previous latency controller is still retiring";
            return false;
        }
        diagnostics::event("retire.complete", 0, double(g.latency_calls), double(submitted));
        return true;
    }
    bool retire_source(const void *source, std::string &error) override {
        if (!source || packet.source_feature != source) return true;
        if (!retire(3000, error)) return false;
        valid = false;
        packet.valid = false;
        reset = true;
        source_retired = provider == FrameGeneration::XeFG;
        diagnostics::event("native_source.retired", packet.frame);
        return true;
    }
    bool close(std::string &error) {
        if (!backend) return true;
        backend->release_output();
        // No retired command list, context state, image or view may keep an
        // old presentation resource alive while its SDK shuts down.
        slots = {};
        final = {}; inputs = {}; scene.Reset(); before.Reset();
        state.Reset(); compute.Reset(); constants.Reset();
        swapchain.Reset();
        if (!backend->close(error)) {
            diagnostics::event("handoff.close_failed", 0, 0, 0, 0, 0, error.c_str());
            return false;
        }
        backend.reset(); queue.Reset();
        producer.Reset(); consumer.Reset(); producer11.Reset(); consumer11.Reset();
        device12.Reset();
        return true;
    }
    bool allocate(unsigned w, unsigned h, std::string &error) {
        if (!drain(1000, error))
            return false;
        final = {};
        inputs = {};
        pending_input = 0;
        scene.Reset();
        before.Reset();
        have_before = false;
        valid = false;
        reset = true;
        guide_mode_known = false;
        cohort.invalidate();
        width = w;
        height = h;
        return shared(final, w, h, format, error) &&
               (provider == FrameGeneration::Off || (shared(inputs[0].hudless, w, h, format, error) &&
                                                     shared(inputs[1].hudless, w, h, format, error)));
    }
    bool private_copy(ID3D11Texture2D *src, ComPtr<ID3D11Texture2D> &out, unsigned w, unsigned h, unsigned x,
                      unsigned y, std::string &error) {
        D3D11_TEXTURE2D_DESC d{}, old{};
        src->GetDesc(&d);
        if (out)
            out->GetDesc(&old);
        if (x > d.Width || y > d.Height || w > d.Width - x || h > d.Height - y || d.SampleDesc.Count != 1) {
            error = "FG native subrect is invalid";
            return false;
        }
        if (!out || old.Width != w || old.Height != h || old.Format != d.Format) {
            d.Width = w;
            d.Height = h;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = 0;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.CPUAccessFlags = 0;
            d.MipLevels = d.ArraySize = 1;
            ComPtr<ID3D11Texture2D> next;
            if (!ok(device11->CreateTexture2D(&d, nullptr, &next), "FG scene snapshot", error))
                return false;
            out = next;
        }
        D3D11_BOX box{x, y, 0, x + w, y + h, 1};
        context11->CopySubresourceRegion(out.Get(), 0, 0, 0, 0, src, 0, &box);
        return true;
    }
    ComPtr<ID3D11ShaderResourceView> view(ID3D11Texture2D *t) {
        ComPtr<ID3D11ShaderResourceView> out;
        if (!t)
            return out;
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = typed(d.Format);
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        device11->CreateShaderResourceView(t, &v, &out);
        return out;
    }
    void dispatch(unsigned mode, ID3D11ShaderResourceView *color, ID3D11ShaderResourceView *d,
                  ID3D11ShaderResourceView *m, ID3D11ShaderResourceView *b = nullptr,
                  ID3D11ShaderResourceView *a = nullptr) {
        values.mode = mode;
        ComPtr<ID3DDeviceContextState> prior;
        state_context->SwapDeviceContextState(state.Get(), &prior);
        context11->UpdateSubresource(constants.Get(), 0, nullptr, &values, 0, 0);
        ID3D11Buffer *cb = constants.Get();
        ID3D11ShaderResourceView *srvs[]{color, d, m, b, a};
        auto &input = inputs[pending_input];
        ID3D11UnorderedAccessView *uavs[]{retirement_unknown ? nullptr : input.depth.uav.Get(),
                                          retirement_unknown ? nullptr : input.motion.uav.Get(),
                                          retirement_unknown ? nullptr : input.hudless.uav.Get(),
                                          final.uav.Get()};
        context11->CSSetShader(compute.Get(), nullptr, 0);
        context11->CSSetConstantBuffers(0, 1, &cb);
        context11->CSSetShaderResources(0, 5, srvs);
        context11->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
        context11->Dispatch(((mode ? width : values.render[0]) + 7) / 8,
                            ((mode ? height : values.render[1]) + 7) / 8, 1);
        context11->ClearState();
        state_context->SwapDeviceContextState(prior.Get(), nullptr);
    }
    void capture_frame(const nr::FrameInput &f) override {
        std::string error;
        valid = false;
        packet.valid = false;
        cohort.begin_capture();
        CameraCohorts<Camera>::Entry native;
        std::uint64_t simulation = 0;
        bool variable_resolution = false;
        {
            auto &g = global();
            std::lock_guard lock(g.mutex);
            // Every queued world evaluation consumes one producer entry,
            // even if its images or camera are rejected.
            native = g.cameras.take();
            simulation = g.simulation;
            variable_resolution = !fixed_dlss_resolution(g.settings.dlss);
            ++g.status.captured_frames;
            g.status.input_status = "Native frame received";
        }
        trace_stage(TraceStage::NativeCapture, native.frame, f.frame_id, native.valid ? 1 : 0);
        diagnostics::event("capture", native.frame, double(f.frame_id), f.images.guide_width,
                           f.images.guide_height, native.valid ? 1 : 0);
        // Ordinary presentation still needs the native render frame's latency
        // markers, without allocating any interpolation guides or history.
        latency_frame = native.frame;
        have_latency_capture = true;
        const auto input_status = [&](const char *message) {
            diagnostics::event("input", native.frame, 0, 0, 0, 0, message);
            auto &g = global();
            std::lock_guard lock(g.mutex);
            auto &s = g.status;
            s.input_status = message;
            s.missing_cameras = g.cameras.counters.missing;
            s.invalid_cameras = g.cameras.counters.invalid;
            s.camera_overflows = g.cameras.counters.overflows;
            s.stale_inputs += cohort.counters.stale - reported_cohort.stale;
            s.overwritten_inputs += cohort.counters.overwritten - reported_cohort.overwritten;
            s.history_resets += cohort.counters.resets - reported_cohort.resets;
            s.input_age = cohort.counters.age;
            reported_cohort = cohort.counters;
        };
        const auto input_reason = [&](const char *message) {
            cohort.invalidate();
            input_status(message);
        };
        const auto &i = f.images;
        if (retirement_unknown) {
            input_reason("FG inputs quarantined after SDK retirement failure");
            return;
        }
        if (!requested()) {
            input_reason("FG input paused");
            auto &g = global();
            std::lock_guard lock(g.mutex);
            g.cameras.clear();
            reset = true;
            return;
        }
        if (!native.valid || !native.frame) {
            input_reason(native.issue == CohortIssue::InvalidCamera
                             ? "Native camera validation rejected the queued camera"
                         : native.issue == CohortIssue::DiscardedCamera
                             ? "Native camera cohort was discarded; waiting for matched input"
                             : "No matching queued native camera; clean frame retained");
            return;
        }
        if (dead || !i.color || !i.depth || !i.motion || i.output_width != width ||
            i.output_height != height || !i.guide_width || !i.guide_height) {
            input_reason("Native scene image, guides or dimensions are unavailable");
            return;
        }
        const auto contains = [](ID3D11Texture2D *t, unsigned x, unsigned y, unsigned w, unsigned h) {
            D3D11_TEXTURE2D_DESC d{};
            t->GetDesc(&d);
            return w && h && d.SampleDesc.Count == 1 && x <= d.Width && y <= d.Height && w <= d.Width - x &&
                   h <= d.Height - y;
        };
        if (!contains(i.depth, i.depth_x, i.depth_y, i.guide_width, i.guide_height) ||
            !contains(i.motion, i.motion_x, i.motion_y, i.motion_width, i.motion_height)) {
            report("FG skipped: native guide region is outside its texture");
            input_reason("Native guide region exceeds its allocation");
            reset = true;
            return;
        }
        ComPtr<ID3D11Device> owner;
        i.color->GetDevice(&owner);
        if (d3d11_identity::native_device(owner.Get()) != device11) {
            input_reason("Native scene belongs to another D3D11 device");
            return;
        }
        auto &input = inputs[pending_input];
        if (input.in_present) {
            input_reason("FG input set is still owned by presentation");
            reset = true;
            return;
        }
        // FFXIV's native DRS changes the active subrect, not the output size.
        // Keep enough capacity for its range instead of reallocating guides
        // and resetting interpolation history on every resolution adjustment.
        // Only this retired set can grow; the other may still be in Present.
        const auto capacity_width =
            variable_resolution ? std::max({width, i.guide_width, input.rw}) : i.guide_width;
        const auto capacity_height =
            variable_resolution ? std::max({height, i.guide_height, input.rh}) : i.guide_height;
        if (input.rw != capacity_width || input.rh != capacity_height) {
            Shared next_depth, next_motion;
            if (!wait(input.done, 1000, error) ||
                !shared(next_depth, capacity_width, capacity_height, DXGI_FORMAT_R32_FLOAT, error) ||
                !shared(next_motion, capacity_width, capacity_height, DXGI_FORMAT_R16G16_FLOAT, error)) {
                report(error);
                input_reason("Native FG guide allocation/retirement is pending");
                return;
            }
            input.depth = std::move(next_depth);
            input.motion = std::move(next_motion);
            input.rw = capacity_width;
            input.rh = capacity_height;
            update([](Status &s) { ++s.guide_allocations; });
            reset = true;
        } else if (input.done &&
                   !ok(context11->Wait(consumer11.Get(), input.done), "FG input reuse wait", error)) {
            report(error);
            input_reason("FG input reuse synchronization failed");
            dead = true;
            return;
        }
        auto d = view(i.depth), m = view(i.motion);
        if (!d || !m) {
            report("FG guide view creation failed");
            input_reason("Native guide views could not be created");
            return;
        }
        const FrameCohort::Source source{f.sr_feature, variable_resolution ? width : i.guide_width,
                                         variable_resolution ? height : i.guide_height};
        const bool mode_changed = guide_mode_known && variable_guides != variable_resolution;
        if (!cohort.accept(native.frame, f.frame_id, native.epoch, source, simulation,
                           reset || f.reset || mode_changed)) {
            input_reason("Stale or repeated native frame; clean frame retained");
            return;
        }
        values.output[0] = width;
        values.output[1] = height;
        // NGX jitter offsets are in each frame's input/render pixel space
        // (nvsdk_ngx_helpers_d3d.h). The guide shader expresses motion in the
        // current render pixels, so convert the previous offset to that same
        // space before removing jitter from a native jittered motion vector.
        // Reset frames have no corresponding previous history to compare.
        values.previous_jitter[0] = !cohort.reset() && values.render[0]
                                        ? values.jitter[0] * float(i.guide_width) / float(values.render[0])
                                        : i.jitter_x;
        values.previous_jitter[1] = !cohort.reset() && values.render[1]
                                        ? values.jitter[1] * float(i.guide_height) / float(values.render[1])
                                        : i.jitter_y;
        values.render[0] = i.guide_width;
        values.render[1] = i.guide_height;
        values.depth[0] = i.depth_x;
        values.depth[1] = i.depth_y;
        values.motion[0] = i.motion_x;
        values.motion[1] = i.motion_y;
        values.motion_size[0] = i.motion_width;
        values.motion_size[1] = i.motion_height;
        values.jitter[0] = i.jitter_x;
        values.jitter[1] = i.jitter_y;
        values.motion_jittered = f.motion_jittered;
        values.motion_scale[0] = i.motion_scale_x;
        values.motion_scale[1] = i.motion_scale_y;
        if (!private_copy(i.color, scene, width, height, i.output_x, i.output_y, error)) {
            report(error);
            input_reason("Native scene copy failed");
            return;
        }
        dispatch(0, nullptr, d.Get(), m.Get());
        packet = {};
        packet.width = width;
        packet.height = height;
        packet.render_width = i.guide_width;
        packet.render_height = i.guide_height;
        packet.variable_resolution = variable_resolution;
        packet.source_feature = f.sr_feature;
        packet.depth = input.depth.image12.Get();
        packet.motion = input.motion.image12.Get();
        packet.hudless = input.hudless.image12.Get();
        packet.reset = cohort.reset();
        packet.jitter_x = i.jitter_x;
        packet.jitter_y = i.jitter_y;
        packet.hdr = f.hdr;
        values.linear_input = i.linear_hdr;
        packet.frame = native.frame;
        packet.camera = native.camera;
        const auto now = std::chrono::steady_clock::now();
        packet.milliseconds = last_time.time_since_epoch().count()
                                  ? float(std::chrono::duration<double, std::milli>(now - last_time).count())
                                  : 16.67f;
        last_time = now;
        packet.valid = packet.camera.valid && packet.frame != 0;
        input_status("Native FG inputs ready");
        valid = true;
        reset = false;
        variable_guides = variable_resolution;
        guide_mode_known = true;
    }
    void effects(ID3D11Texture2D *t) override {
        if (!requested())
            return;
        std::string error;
        have_before = t && private_copy(t, before, width, height, 0, 0, error);
        if (!error.empty())
            report(error);
    }
    bool prepare_colour(ID3D11Texture2D *sdr, std::string &error) {
        if (!sdr) {
            error = "Missing SDR presentation image";
            return false;
        }
        Settings settings;
        {
            auto &g = global();
            std::lock_guard lock(g.mutex);
            settings = g.settings;
        }
        auto a = view(sdr);
        if (!a) {
            error = "FG SDR presentation view failed";
            return false;
        }
        values.output[0] = width;
        values.output[1] = height;
        settings.hdr = committed_output_settings(settings.hdr, format);
        if (valid) {
            auto c = view(scene.Get()), b = view(before.Get());
            const auto &h = settings.hdr;
            values.output_mode = !h.enabled ? 0u : format == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u : 2u;
            values.paper = h.paper_white_nits;
            values.peak = h.peak_nits;
            values.contrast = h.contrast;
            values.saturation = h.saturation;
            values.vibrance = h.vibrance;
            values.black = h.black_nits;
            values.has_effects = have_before && b ? 1u : 0u;
            values.preserve_effects = h.preserve_reshade_highlights ? 1u : 0u;
            dispatch(1, c.Get(), nullptr, nullptr, b.Get(), a.Get());
        }
        if (!settings.hdr.enabled)
            dispatch(2, nullptr, nullptr, nullptr, nullptr, a.Get());
        colour_ready = true;
        return true;
    }
    HRESULT present(unsigned interval, unsigned flags, bool focused, std::unique_lock<std::mutex> &engine,
                    d3d11_work::Guard &context_owner, const std::function<void()> &before_sdk_present) {
        if (dead && !retirement_unknown)
            return DXGI_ERROR_DEVICE_REMOVED;
        if (flags & DXGI_PRESENT_TEST)
            return swapchain->Present(interval, flags);
        const auto preparation_started = TimingClock::now();
        // Window focus does not change scene identity. Keep supplying native
        // frames and effects in the background; the selected provider retains
        // its own OS/driver restrictions. NR focus pause is independent.
        auto cfg = options();
        Settings settings;
        std::uint64_t simulation = 0;
        {
            auto &g = global();
            std::lock_guard lock(g.mutex);
            settings = g.settings;
            simulation = g.simulation;
        }
        settings.hdr = committed_output_settings(settings.hdr, format);
        const bool quarantined = retirement_unknown;
        bool enabled = !quarantined && cfg.enabled && cfg.backend == provider &&
                       provider != FrameGeneration::Off && valid && packet.valid &&
                       cohort.ready(packet.frame);
        packet.frame =
            quarantined ? 0 : cohort.output_frame(valid && packet.valid ? packet.frame : 0, simulation);
        if (!packet.frame)
            enabled = false;
        diagnostics::event("frame", packet.frame, valid ? 1 : 0, packet.valid ? 1 : 0,
                           enabled ? 1 : 0, packet.reset ? 1 : 0);
        std::string error;
        auto &slot = slots[cursor++ % slots.size()];
        if (!wait(slot.done, 1000, error)) {
            report(error);
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        if (valid && !colour_ready) {
            // Standalone GPU callers may have filled target() directly. The
            // game path always supplies its SDR pre/post-effect pair above.
            auto c = view(scene.Get());
            values.output_mode = !settings.hdr.enabled                     ? 0u
                                 : format == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u
                                                                           : 2u;
            const auto &h = settings.hdr;
            values.paper = h.paper_white_nits;
            values.peak = h.peak_nits;
            values.contrast = h.contrast;
            values.saturation = h.saturation;
            values.vibrance = h.vibrance;
            values.black = h.black_nits;
            values.has_effects = 0;
            dispatch(1, c.Get(), nullptr, nullptr);
        }
        if (!ok(slot.allocator->Reset(), "FG allocator reset", error) ||
            !ok(slot.commands->Reset(slot.allocator.Get(), nullptr), "FG list reset", error)) {
            report(error);
            return E_FAIL;
        }
        const auto serial_value = ++serial;
        if (!ok(context11->Signal(producer11.Get(), serial_value), "FG producer signal", error)) {
            report(error);
            return E_FAIL;
        }
        trace_stage(TraceStage::D11FlushBegin, packet.frame, serial_value, unsigned(provider));
        context11->Flush();
        trace_stage(TraceStage::D11FlushEnd, packet.frame, serial_value, unsigned(provider));
        if (!ok(queue->Wait(producer.Get(), serial_value), "FG D3D12 wait", error)) {
            report(error);
            return E_FAIL;
        }
        ComPtr<ID3D12Resource> back;
        if (FAILED(backend->backbuffer(swapchain.Get(), back)))
            return E_FAIL;
        const auto barrier = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = r;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
            slot.commands->ResourceBarrier(1, &b);
        };
        barrier(final.image12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        slot.commands->CopyResource(back.Get(), final.image12.Get());
        barrier(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        barrier(final.image12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        packet.backbuffer = back.Get();
        packet.dynamic = cfg.dynamic && backend->native_dynamic_supported();
        packet.target_fps = cfg.target_fps;
        const auto factor = backend->maximum() <= 1 ? 1u
                            : packet.dynamic ? backend->maximum()
                                             : dynamic.update(packet.milliseconds, cfg.target_fps,
                                                              backend->maximum(), cfg.dynamic, cfg.factor, GetTickCount64());
        if (!quarantined && !backend->prepare(slot.commands.Get(), packet, factor, enabled, error)) {
            backend->stop();
            enabled = false;
            update([&](Status &s) {
                ++s.failures;
                s.last_error = error;
            });
            report(error);
        }
        const auto recorded = slot.commands->Close();
        if (FAILED(recorded)) {
            diagnostics::event("transport.close_failed", packet.frame, unsigned(recorded));
            ComPtr<ID3D12InfoQueue> debug;
            if (SUCCEEDED(device12.As(&debug))) {
                const auto count = debug->GetNumStoredMessagesAllowedByRetrievalFilter();
                for (UINT64 i = count > 32 ? count - 32 : 0; i < count; ++i) {
                    SIZE_T size = 0;
                    debug->GetMessage(i, nullptr, &size);
                    std::vector<unsigned char> bytes(size);
                    auto *message = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
                    if (SUCCEEDED(debug->GetMessage(i, message, &size)))
                        diagnostics::event("transport.debug", packet.frame, message->ID, message->Severity, 0, 0, message->pDescription);
                }
            }
            return E_FAIL;
        }
        ID3D12CommandList *commands = slot.commands.Get();
        queue->ExecuteCommandLists(1, &commands);
        const auto copied = ++serial;
        if (FAILED(queue->Signal(consumer.Get(), copied))) {
            quarantine_inputs();
            backend->stop();
            report("FG transport copy completion signal failed");
            return E_FAIL;
        }
        submitted = slot.done = copied;
        // Final is one shared scratch image. Hold later D3D11 writes only
        // until its copy/tag command list finishes, never until the provider's
        // generated-frame pacing. This also protects direct target() callers.
        if (FAILED(context11->Wait(consumer11.Get(), copied))) {
            quarantine_inputs();
            backend->stop();
            report("FG transport copy synchronization failed");
            return E_FAIL;
        }
        const Packet presenting = packet;
        // Plain has no interpolation cohort: use the actual render frame,
        // rather than a newer simulation that may already be running ahead.
        auto presenting_latency_frame = provider == FrameGeneration::Off
                                            ? (have_latency_capture ? latency_frame : simulation)
                                            : presenting.frame;
        if (presenting_latency_frame <= last_latency_present)
            presenting_latency_frame = 0;
        if (presenting_latency_frame)
            last_latency_present = presenting_latency_frame;
        latency_frame = 0;
        const auto input_index = pending_input;
        if (!quarantined) {
            inputs[input_index].in_present = true;
            pending_input = (pending_input + 1) % unsigned(inputs.size());
        }
        // Seal the submitted cohort before letting native capture prepare the
        // other set. Returning from an old paced Present must not clear it.
        cohort.presented(presenting.frame, enabled);
        valid = false;
        have_before = false;
        colour_ready = false;
        if (!enabled)
            reset = true;
        try {
            if (before_sdk_present)
                before_sdk_present();
        } catch (...) {
            const auto done = ++serial;
            if (SUCCEEDED(queue->Signal(consumer.Get(), done))) {
                submitted = slot.done = done;
                if (!retirement_unknown) {
                    inputs[input_index].done = done;
                    inputs[input_index].in_present = false;
                }
            } else
                quarantine_inputs();
            reset = true;
            packet.reset = true;
            backend->stop();
            report("FG presentation metadata callback failed");
            return E_FAIL;
        }
        // The provider may pace/block here. Its lifetime is protected by the
        // independent presentation mutex, not the native render/context locks.
        engine.unlock();
        context_owner.lock.unlock();
        note_timing(TimingPhase::Transport, elapsed_ms(preparation_started));
        if (presenting_latency_frame) {
            backend->marker(3, presenting_latency_frame);
            backend->marker(4, presenting_latency_frame);
        }
        BOOL exclusive = TRUE;
        // All private provider chains are created with ALLOW_TEARING. Native
        // DX11 flags do not describe this flip-model output. In XeFG's windowed
        // unsynchronized mode, pass the matching flag so XeLL does not observe
        // an unintended compositor-paced presentation mode.
        const bool xefg_windowed = provider == FrameGeneration::XeFG && interval == 0 &&
            SUCCEEDED(swapchain->GetFullscreenState(&exclusive, nullptr)) && !exclusive;
        const auto forwarded_flags = presentation_flags(provider, interval, flags, xefg_windowed);
        const auto completed_at_entry = consumer->GetCompletedValue();
        const bool copy_ready_at_entry =
            completed_at_entry != std::numeric_limits<std::uint64_t>::max() && completed_at_entry >= copied;
        const auto present_started = TimingClock::now();
        trace_stage(TraceStage::SdkPresentEntry, presenting_latency_frame, unsigned(provider), factor);
        diagnostics::event("present.begin", presenting_latency_frame, factor, interval, forwarded_flags,
                           copy_ready_at_entry ? 1 : 0);
        const auto result = backend->present(swapchain.Get(), interval, forwarded_flags);
        trace_stage(TraceStage::SdkPresentExit, presenting_latency_frame, unsigned(provider),
                    static_cast<unsigned>(result));
        const auto present_ms = elapsed_ms(present_started);
        diagnostics::event("present.end", presenting_latency_frame, present_ms, unsigned(result), factor,
                           enabled ? 1 : 0);
        note_timing(TimingPhase::SdkPresent, present_ms);
        note_present(interval, forwarded_flags, copy_ready_at_entry, present_ms);
        const auto retirement_started = TimingClock::now();
        if (presenting_latency_frame)
            backend->marker(5, presenting_latency_frame);
        const bool inputs_retired = quarantined || backend->after_present(queue.Get(), error);
        diagnostics::event("retirement", presenting.frame, elapsed_ms(retirement_started),
                           inputs_retired ? 1 : 0, backend->last_presented(), packet.milliseconds,
                           inputs_retired ? nullptr : error.c_str());
        if (!inputs_retired) {
            backend->stop();
            enabled = false;
            update([&](Status &s) {
                ++s.failures;
                s.last_error = error;
            });
            report(error);
        }
        const auto done = ++serial;
        // Queue completion before taking engine_mutex again: capture can be
        // retiring/resizing an older set while holding that mutex.
        const auto completion = queue->Signal(consumer.Get(), done);
        engine.lock();
        note_timing(TimingPhase::InputRetirement, elapsed_ms(retirement_started));
        if (!inputs_retired || FAILED(completion)) {
            // A game-queue fence cannot prove an SDK's private input queue is
            // finished when its retirement query/wait failed. Preserve both
            // sets, including a newer capture prepared during this Present.
            quarantine_inputs();
        }
        if (FAILED(completion)) {
            backend->stop();
            report("FG completion synchronization failed");
            return E_FAIL;
        }
        submitted = slot.done = done;
        if (!retirement_unknown) {
            inputs[input_index].done = done;
            inputs[input_index].in_present = false;
        }
        if (FAILED(result) || !enabled) {
            // A newer capture may already be ready. Preserve it and request a
            // history reset rather than invalidating/freeing its input set.
            reset = true;
            packet.reset = true;
        }
        update([&](Status &s) {
            s.available = true;
            s.native_dynamic = enabled && presenting.dynamic;
            s.active = enabled && backend->last_presented() > 1;
            s.provider = cfg.enabled && cfg.backend != FrameGeneration::Off ? provider : FrameGeneration::Off;
            s.factor = enabled ? std::clamp(backend->last_presented(), 1u,
                                            presenting.dynamic ? backend->maximum() : factor)
                               : 1;
            s.maximum = backend->maximum();
            s.latency = !retirement_unknown && backend->latency_available();
            s.latency_name =
                retirement_unknown ? "Unavailable after SDK retirement failure" : backend->latency_name();
            ++s.real_frames;
            s.displayed = displayed_before + backend->displayed();
            const auto now = GetTickCount64();
            const bool rate_enabled = cfg.enabled && cfg.backend == provider;
            if (!rate_started || rate_request != rate_enabled || s.displayed < rate_displayed) {
                rate_started = now;
                rate_real = s.real_frames;
                rate_displayed = s.displayed;
                rate_request = rate_enabled;
                s.rate_measured = false;
            } else if (now - rate_started >= 500 && s.real_frames > rate_real) {
                const auto real = s.real_frames - rate_real, displayed = s.displayed - rate_displayed;
                const double seconds = double(now - rate_started) / 1000;
                s.observed_ratio = double(displayed) / double(real);
                s.real_fps = double(real) / seconds;
                s.displayed_fps = double(displayed) / seconds;
                s.rate_measured = true;
                rate_started = now;
                rate_real = s.real_frames;
                rate_displayed = s.displayed;
            }
            if (!enabled)
                ++s.skipped;
            s.message =
                retirement_unknown ? "FG paused after an SDK input retirement failure; restart to retry"
                : enabled
                    ? (s.active
                           ? (focused ? "Frame generation active" : "Frame generation active in background")
                           : (focused ? "FG requested; runtime is warming up or skipping interpolation"
                                      : "FG requested in background; runtime is warming up or skipping "
                                        "interpolation"))
                    : (provider == FrameGeneration::Off ? "Frame generation disabled; ordinary presentation"
                       : !presenting.valid              ? s.input_status
                                                        : "FG waiting for a new native DLSS frame");
        });
        return result;
    }
    ~Impl() {
        if (backend)
            backend->stop();
        swapchain.Reset();
        backend.reset();
        queue.Reset();
        if (event)
            CloseHandle(event);
        // Device/resource COM owners below must die before their runtime.
        // d3d12.dll is a system module also held by the game/OS; retain this
        // one process lifetime reference when destroying private GPU owners.
    }
};
void configure(const Settings &s, const std::filesystem::path &root, bool startup) {
    if (startup && !root.empty()) diagnostics::start(root.parent_path() / L"logs");
    if (startup)
        register_timing_provider();
    auto &g = global();
    std::uint64_t frame = 0;
    {
        std::lock_guard lock(g.mutex);
        g.settings = s;
        g.output = presentation_requested(s.fg);
        if (!root.empty())
            g.root = root;
        if (startup) {
            g.startup = s.fg;
            g.output = presentation_requested(s.fg);
        }
        frame = g.simulation;
    }
    // Detail: factor[7:0], target FPS[23:8], enabled[32], dynamic[33],
    // ordinary-frame latency[34]. The source field is the requested provider.
    const auto request = std::uint64_t(s.fg.factor) | (std::uint64_t(s.fg.target_fps) << 8) |
                         (std::uint64_t(s.fg.enabled) << 32) | (std::uint64_t(s.fg.dynamic) << 33) |
                         (std::uint64_t(s.fg.reflex) << 34);
    trace_stage(TraceStage::ProviderRequest, frame, unsigned(s.fg.backend), request);
    diagnostics::event("settings", frame, unsigned(s.fg.backend), double(request),
                       unsigned(requested_output_format(s.hdr)), fixed_dlss_resolution(s.dlss) ? 1 : 0);
    diagnostics::event("settings.dlss", frame, s.dlss.enabled ? 1 : 0, unsigned(s.dlss.quality),
                       unsigned(s.dlss.model_preset), s.dlss.automatic_mip_bias ? 1 : 0);
    diagnostics::event("settings.hdr", frame, s.hdr.enabled ? 1 : 0, unsigned(s.hdr.output),
                       s.hdr.paper_white_nits, s.hdr.peak_nits);
    diagnostics::event("settings.nr", frame, s.nr.enabled ? 1 : 0, s.nr.passes,
                       s.nr.model_scale, s.nr.foveated ? 1 : 0, s.nr.runtime_id.c_str());
    for (unsigned pass = 0; pass < s.nr.passes && pass <= s.nr.later_passes.size(); ++pass) {
        const auto p = nr_pass_profile(s.nr, pass);
        diagnostics::event("settings.nr.profile", frame, pass, p.preset, p.style, p.intensity);
        diagnostics::event("settings.nr.controls", frame, pass, p.structure, p.tone, p.skin);
    }
}
FrameGenerationSettings options() {
    auto &g = global();
    std::lock_guard lock(g.mutex);
    return g.settings.fg;
}
bool requested() {
    const auto o = options();
    return o.enabled && o.backend != FrameGeneration::Off;
}
bool output_requested() {
    auto &g = global();
    std::lock_guard lock(g.mutex);
    return g.output;
}
Status status() {
    auto &g = global();
    std::lock_guard lock(g.mutex);
    return g.status;
}
void note_timing(TimingPhase phase, double milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds < 0)
        return;
    if (milliseconds >= 20)
        diagnostics::event("stage.slow", 0, unsigned(phase), milliseconds);
    auto &g = global();
    std::lock_guard lock(g.mutex);
    auto &total = g.performance.stages[unsigned(phase)];
    ++total.samples;
    total.over_100_ms += milliseconds >= 100;
    total.total_ms += milliseconds;
    total.maximum_ms = std::max(total.maximum_ms, milliseconds);
}
void note_trace(TraceStage stage, std::uint64_t frame, std::uint64_t source, std::uint64_t detail) {
    if (!trace_enabled())
        return;
    if (!frame) {
        auto &g = global();
        std::lock_guard lock(g.mutex);
        frame = g.simulation;
    }
    trace_stage(stage, frame, source, detail);
}
void note_trace_text(const char *message) {
    diagnostics::event("pacer", 0, 0, 0, 0, 0, message);
    TraceLoggingWrite(timing_provider, "Diagnostic", TraceLoggingLevel(5),
                      TraceLoggingString(message ? message : "", "Message"));
}
void note_presentation_query(unsigned kind) {
    auto &g = global();
    std::lock_guard lock(g.mutex);
    if (kind < g.performance.presentation_queries.size())
        ++g.performance.presentation_queries[kind];
}
void note_present(unsigned interval, unsigned flags, bool copy_ready_at_entry, double milliseconds) {
    auto &g = global();
    std::lock_guard lock(g.mutex);
    g.performance.present_requests.record(interval, flags, copy_ready_at_entry, milliseconds);
}
PerformanceWindow take_performance() {
    auto &g = global();
    std::lock_guard lock(g.mutex);
    auto value = g.performance;
    g.performance = {};
    return value;
}
MemoryUsage memory_usage() {
    ComPtr<IDXGIAdapter3> adapter;
    {
        auto &g = global();
        std::lock_guard lock(g.mutex);
        adapter = g.adapter;
    }
    MemoryUsage value;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
    if (adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
        SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal))) {
        value.gpu_available = true;
        value.local_bytes = local.CurrentUsage;
        value.local_budget = local.Budget;
        value.nonlocal_bytes = nonlocal.CurrentUsage;
        value.nonlocal_budget = nonlocal.Budget;
    }
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof(process);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&process),
                             sizeof(process))) {
        value.process_available = true;
        value.process_private = process.PrivateUsage;
        value.working_set = process.WorkingSetSize;
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles))
        value.handles = handles;
    return value;
}
void report(const std::string &text) {
    diagnostics::event("status", 0, 0, 0, 0, 0, text.c_str());
    update([&](Status &s) { s.message = text; });
}
void report_failure(const std::string &text) noexcept {
    diagnostics::event("failure", 0, 0, 0, 0, 0, text.c_str());
    try {
        update([&](Status &s) {
            ++s.failures;
            s.last_error = text;
            s.message = text;
        });
    } catch (...) {
        // Destruction must not throw across COM/SDK ownership boundaries.
    }
}
void camera(const Camera &c) {
    auto &g = global();
    std::lock_guard lock(g.mutex);
    ++g.status.camera_samples;
    if (c.valid)
        ++g.status.valid_cameras;
    g.cameras.push(g.simulation, c, c.valid);
}
void simulation_begin() {
    LatencyCall call(0);
    trace_stage(TraceStage::SimulationTickEntry, call.id);
    // Sleep must never hold the render/Present lock: it may be waiting for the
    // preceding frame's render thread to submit work through capture().
    if (call.backend) {
        const auto started = TimingClock::now();
        trace_stage(TraceStage::SleepBegin, call.id);
        diagnostics::event("sleep.begin", call.id);
        call.backend->sleep(call.id);
        diagnostics::event("sleep.end", call.id, elapsed_ms(started));
        trace_stage(TraceStage::SleepEnd, call.id);
        note_timing(TimingPhase::SimulationSleep, elapsed_ms(started));
        call.backend->marker(0, call.id);
    }
    trace_stage(TraceStage::SimulationBodyBegin, call.id);
}
void simulation_end() {
    diagnostics::SlowScope phase("latency.simulation_end");
    LatencyCall call(1);
    // Renderer update supplies the real end boundary. The Tick-exit fallback
    // calls here too; detail distinguishes sent markers from deduplicated ones.
    trace_stage(TraceStage::SimulationEndRequested, call.id, 0, call.backend ? 1 : 0);
    if (call.backend)
        call.backend->marker(1, call.id);
}
void render_begin() {
    diagnostics::SlowScope phase("latency.render_begin");
    LatencyCall call(2);
    trace_stage(TraceStage::RenderBegin, call.id);
    if (call.backend)
        call.backend->marker(2, call.id);
}
void capture(const nr::FrameInput &f) {
    const auto started = TimingClock::now();
    d3d11_work::Guard context_owner;
    auto &g = global();
    std::lock_guard lock(g.engine_mutex);
    note_timing(TimingPhase::CaptureLock, elapsed_ms(started));
    if (g.sink)
        g.sink->capture_frame(f);
}
void before_effects(ID3D11Texture2D *t) {
    d3d11_work::Guard context_owner;
    auto &g = global();
    std::lock_guard lock(g.engine_mutex);
    if (g.sink)
        g.sink->effects(t);
}
std::unique_lock<std::recursive_mutex> lock_native_source() {
    return std::unique_lock(global().presentation_mutex);
}
bool retire_native_source(const void *source, std::string &error) {
    auto presentation = lock_native_source();
    d3d11_work::Guard context_owner;
    auto &g = global();
    std::lock_guard engine(g.engine_mutex);
    return !g.sink || g.sink->retire_source(source, error);
}
Presenter::Presenter() : presentation_mutex_(global().presentation_mutex) {}
Presenter::~Presenter() {
    diagnostics::Scope phase("shutdown.presenter");
    std::lock_guard presentation(presentation_mutex_);
    d3d11_work::Guard context_owner;
    if (!impl_)
        return;
    auto &g = global();
    std::lock_guard lock(g.engine_mutex);
    g.sink = nullptr;
    {
        std::lock_guard data(g.mutex);
        g.backend.reset();
    }
    std::string error;
    if (!impl_->retire(1000, error)) {
        report(error);
        impl_.release();
        // Preserve the SDK/device too if an outstanding GPU/latency owner
        // could still use them. Process teardown will reclaim this generation.
        if (streamline_)
            new std::shared_ptr<Backend>(std::move(streamline_));
        return;
    }
        if (!impl_->close(error)) {
        report_failure(error);
        impl_.release();
        if (streamline_) new std::shared_ptr<Backend>(std::move(streamline_));
        return;
    }
    impl_.reset();
    streamline_.reset();
    device12_.Reset();
    diagnostics::event("presenter.destroyed");
    // Never wait for the file writer while holding presentation/context
    // locks. The session owner drains diagnostics after renderer teardown.
}
IDXGISwapChain4 *Presenter::chain() const {
    return impl_ ? impl_->swapchain.Get() : nullptr;
}
ID3D11RenderTargetView *Presenter::target() const {
    return impl_ ? impl_->final.rtv.Get() : nullptr;
}
bool Presenter::synchronize(std::string &error) {
    std::lock_guard presentation(presentation_mutex_);
    d3d11_work::Guard context_owner;
    auto &g = global();
    std::lock_guard engine(g.engine_mutex);
    if (!impl_) {
        error = "FG presentation is unavailable";
        return false;
    }
    if (impl_->retirement_unknown) {
        // Continue ordinary color presentation, but do not republish latency,
        // restart interpolation, resize or replace uncertain SDK input owners.
        update([&](Status &s) { s.transition = "FG input retirement failed; restart to retry"; });
        return true;
    }
    Settings cfg;
    {
        std::lock_guard lock(g.mutex);
        cfg = g.settings;
    }
    // A stopped vendor swapchain can still pace ordinary frames. Replace it
    // on Off, and release its SDK/device before creating the next owner.
    const auto requested_provider = cfg.fg.enabled ? cfg.fg.backend : FrameGeneration::Off;
    const auto desired_format = requested_output_format(cfg.hdr);
    const bool format_blocked = !supported_format(requested_provider, desired_format, cfg.hdr.enabled);
    auto target = cfg;
    if (format_blocked)
        target.fg.enabled = false;
    const auto desired = target.fg.enabled ? target.fg.backend : FrameGeneration::Off;
    const auto desired_latency = latency_provider(target.fg);
    const auto key = std::tuple{requested_provider, cfg.fg.xefg_unlock, desired_latency, desired_format, cfg.fg.fsr_mfg};
    const bool same =
        !impl_->source_retired && desired == impl_->provider && desired_format == impl_->format &&
        (desired != FrameGeneration::XeFG || cfg.fg.xefg_unlock == impl_->creation.xefg_unlock) &&
        (desired != FrameGeneration::FsrFG || cfg.fg.fsr_mfg == impl_->creation.fsr_mfg) &&
        desired_latency == latency_provider(impl_->creation);
    if (same) {
        blocked_.reset();
        {
            std::lock_guard lock(g.mutex);
            g.status.transition =
                format_blocked
                    ? "Selected FG needs 10-bit HDR; ordinary presentation and selected latency retained"
                    : "";
            if (desired != FrameGeneration::Off || desired_latency != FrameGeneration::Off)
                g.backend = impl_->backend;
            else
                g.backend.reset();
        }
        return true;
    }
    if (blocked_ == key)
        return true;
    diagnostics::event("handoff.begin", 0, unsigned(impl_->provider), unsigned(desired),
                       unsigned(impl_->format), unsigned(desired_format));
    const auto blocked = [&](const std::string &reason) {
        diagnostics::event("handoff.failed", 0, unsigned(desired), 0, 0, 0, reason.c_str());
        blocked_ = key;
        update([&](Status &s) { s.transition = "Provider change unavailable: " + reason; });
    };
    if (!impl_->retire(3000, error)) {
        blocked(error);
        return true;
    }
    // Keep the native D3D11 owner and factory alive across the old context's
    // destruction. ReShade retains its existing carrier and SDR backbuffers.
    auto device = impl_->device11;
    auto factory = impl_->factory;
    const auto description = impl_->description;
    const auto format = impl_->format;
    const auto window = impl_->window;
    g.sink = nullptr;
    if (!impl_->close(error)) {
        blocked(error);
        return false;
    }
    impl_.reset();
    // A feature unload leaves Streamline's common/Reflex state, proxies and
    // driver timing state alive. A different provider must start without any
    // of that ownership. NR owns a separate D3D12 device and is unaffected.
    streamline_.reset();
    device12_.Reset();
    streamline_attempted_ = false;
    streamline_error_.clear();
    diagnostics::event("handoff.old_owner_released");
    {
        std::lock_guard lock(g.mutex);
        g.cameras.clear();
    }
    // Keep the selected latency provider even when interpolation is disabled.
    // initialize_locked chooses a plain output separately from its controller.
    target.fg.backend = cfg.fg.backend;
    target.fg.enabled = desired != FrameGeneration::Off;
    bool changed = initialize_locked(device.Get(), factory.Get(), window, description, desired_format, target, error);
    if (changed) {
        blocked_.reset();
        update([&](Status &s) {
            ++s.provider_changes;
            s.transition =
                format_blocked
                    ? "Selected FG needs 10-bit HDR; ordinary presentation and selected latency retained"
                : desired == FrameGeneration::Off
                    ? (desired_latency == FrameGeneration::Off
                           ? "FG output released; ordinary presentation active"
                           : "FG output released; ordinary presentation with latency reduction")
                    : "Provider and latency controller changed live";
        });
        return true;
    }
    const auto failure = error;
    auto fallback = cfg;
    // A rejected format request must restore both the previous DXGI format
    // and its matching transfer-function domain, never reinterpret SDR as HDR.
    fallback.hdr = committed_output_settings(cfg.hdr, format);
    // A failed FG request can still use the selected latency controller on an
    // ordinary chain. Keep its native simulation/Present markers published.
    fallback.fg.enabled = false;
    if (!initialize_locked(device.Get(), factory.Get(), window, description, format, fallback, error)) {
        fallback.fg.reflex = false;
        fallback.fg.backend = FrameGeneration::Off;
        if (!initialize_locked(device.Get(), factory.Get(), window, description, format, fallback, error))
            return false;
    }
    // Failed requests retain ordinary rendering. Do not repeatedly tear down
    // a working chain every Present; selecting another provider permits retry.
    blocked(failure);
    return true;
}
bool Presenter::prepare_colour(ID3D11Texture2D *sdr, std::string &error) {
    std::lock_guard presentation(presentation_mutex_);
    d3d11_work::Guard context_owner;
    auto &g = global();
    std::lock_guard lock(g.engine_mutex);
    return impl_ && impl_->prepare_colour(sdr, error);
}
HRESULT Presenter::present(UINT interval, UINT flags, bool focused,
                           const std::function<void()> &before_sdk_present) {
    const auto started = TimingClock::now();
    std::lock_guard presentation(presentation_mutex_);
    d3d11_work::Guard context_owner;
    auto &g = global();
    std::unique_lock engine(g.engine_mutex);
    if (!(flags & DXGI_PRESENT_TEST))
        note_timing(TimingPhase::PresentLock, elapsed_ms(started));
    return impl_ ? impl_->present(interval, flags, focused, engine, context_owner, before_sdk_present)
                 : E_FAIL;
}
bool Presenter::drain(unsigned timeout, std::string &error) {
    std::lock_guard presentation(presentation_mutex_);
    d3d11_work::Guard context_owner;
    auto &g = global();
    std::lock_guard lock(g.engine_mutex);
    return !impl_ || impl_->drain(timeout, error);
}
bool Presenter::resize(unsigned w, unsigned h, std::string &error) {
    std::lock_guard presentation(presentation_mutex_);
    d3d11_work::Guard context_owner;
    auto &g = global();
    std::lock_guard lock(g.engine_mutex);
    if (!impl_ || !impl_->drain(3000, error) || !impl_->backend->resize(w, h, error) ||
        !impl_->allocate(w, h, error))
        return false;
    impl_->description.BufferDesc.Width = w;
    impl_->description.BufferDesc.Height = h;
    return true;
}
bool Presenter::initialize(ID3D11Device *d, IDXGIFactory *f, HWND window, const DXGI_SWAP_CHAIN_DESC &desc,
                           DXGI_FORMAT format, std::string &error) try {
    std::lock_guard presentation(presentation_mutex_);
    d3d11_work::Guard context_owner;
    auto &g = global();
    std::lock_guard lock(g.engine_mutex);
    Settings cfg;
    {
        std::lock_guard data(g.mutex);
        cfg = g.settings;
    }
    if (!supported_format(cfg.fg.enabled ? cfg.fg.backend : FrameGeneration::Off, format, cfg.hdr.enabled))
        cfg.fg.enabled = false;
    return initialize_locked(d, f, window, desc, format, cfg, error);
} catch (const std::exception &e) {
    error = e.what();
    return false;
}
bool Presenter::initialize_locked(ID3D11Device *d, IDXGIFactory *f, HWND window,
                                  const DXGI_SWAP_CHAIN_DESC &desc, DXGI_FORMAT format, const Settings &cfg,
                                  std::string &error) try {
    auto &g = global();
    if (impl_ || g.sink) {
        error = "Another window already owns frame generation";
        return false;
    }
    auto next = std::make_unique<Impl>();
    next->device11 = d3d11_identity::native_device(d);
    next->factory = f;
    next->description = desc;
    next->creation = cfg.fg;
    next->provider = cfg.fg.enabled ? cfg.fg.backend : FrameGeneration::Off;
    next->window = window;
    next->format = format;
    std::filesystem::path root;
    {
        std::lock_guard data(g.mutex);
        root = g.root;
        next->displayed_before = g.status.displayed;
    }
    if (!supported_format(next->provider, format, cfg.hdr.enabled)) {
        error = "Selected FG backend does not support this output format";
        return false;
    }
    if (!ok(next->device11.As(&next->device5), "FG D3D11.5 device", error))
        return false;
    ComPtr<ID3D11DeviceContext> native;
    next->device11->GetImmediateContext(&native);
    if (!ok(native.As(&next->context11), "FG D3D11.4 context", error) ||
        !ok(native.As(&next->state_context), "FG state context", error))
        return false;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(next->device11.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)))
        return false;
    DXGI_ADAPTER_DESC adapter_desc{};
    if (SUCCEEDED(adapter->GetDesc(&adapter_desc))) {
        char name[512]{};
        WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, name, sizeof(name), nullptr, nullptr);
        LARGE_INTEGER driver{};
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
        std::ostringstream identity;
        identity << name << " LUID=" << std::hex << unsigned(adapter_desc.AdapterLuid.HighPart) << ':'
                 << adapter_desc.AdapterLuid.LowPart << " driver=" << driver.QuadPart;
        diagnostics::event("adapter", 0, adapter_desc.VendorId, adapter_desc.DeviceId,
                           double(adapter_desc.DedicatedVideoMemory) / (1024 * 1024), 0, identity.str().c_str());
    }
    {
        std::lock_guard data(g.mutex);
        adapter.As(&g.adapter);
    }
    static HMODULE runtime = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    next->runtime = runtime;
    auto get = next->runtime ? reinterpret_cast<decltype(&D3D12GetInterface)>(
                                   GetProcAddress(next->runtime, "D3D12GetInterface"))
                             : nullptr;
    ComPtr<ID3D12DeviceFactory> factory;
    if (device12_)
        next->device12 = device12_;
    else {
        if (!get ||
             !ok(get(CLSID_D3D12DeviceFactory, IID_PPV_ARGS(&factory)), "FG private device factory", error) ||
             !ok(factory->SetFlags(D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON),
                 "FG private factory isolation", error)) return false;
        if (probe_debug) {
            ComPtr<ID3D12Debug> debug;
            if (!ok(factory->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(&debug)), "Probe debug layer", error)) return false;
            debug->EnableDebugLayer();
        }
        if (!ok(factory->CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&next->device12)), "FG private device", error)) return false;
    }
    device12_ = next->device12;
    // Streamline belongs to this output generation. Create it only when FG
    // or the selected independent latency controller actually requires it.
    if (!streamline_attempted_ && (next->provider == FrameGeneration::DlssG ||
                                 latency_provider(cfg.fg) == FrameGeneration::DlssG)) {
        streamline_attempted_ = true;
        std::string compatibility;
        prepare_compatibility(adapter.Get(), root, compatibility);
        update([&](Status &s) { s.compatibility = compatibility; });
        // Reflex remains useful when the optional compatibility module cannot
        // load. Let Streamline report its actual FG capability independently.
        streamline_ = make_backend(FrameGeneration::DlssG, root, cfg.fg.xefg_unlock, streamline_error_);
        if (streamline_ && !streamline_->prepare_device(next->device12.Get(), f, streamline_error_))
            report("DLSS-G device preparation unavailable: " + streamline_error_);
    }
    // A latency-only Streamline owner must unload interpolation hooks before
    // the ordinary chain is created (DLSS-G guide section 18).
    if (streamline_ &&
        !streamline_->set_interpolation_loaded(next->provider == FrameGeneration::DlssG, error))
        return false;
    if (next->provider == FrameGeneration::DlssG) {
        next->backend = streamline_;
        if (!next->backend) {
            error = streamline_error_;
            return false;
        }
    } else if (next->provider == FrameGeneration::Off) {
        std::shared_ptr<Backend> latency;
        const auto selected = latency_provider(cfg.fg);
        if (selected == FrameGeneration::DlssG)
            latency = streamline_;
        else if (selected != FrameGeneration::Off)
            latency = make_backend(selected, root, false, error);
        if (selected != FrameGeneration::Off && !latency) {
            report("Latency reduction unavailable: " +
                   (selected == FrameGeneration::DlssG ? streamline_error_ : error));
            error.clear();
        }
        next->backend = make_plain_backend(std::move(latency));
    } else {
        next->backend = next->provider == FrameGeneration::FsrFG && cfg.fg.fsr_mfg
                            ? make_enabler_backend(root, error)
                            : make_backend(next->provider, root, cfg.fg.xefg_unlock, error);
        const auto selected = latency_provider(cfg.fg);
        if (next->backend && next->provider == FrameGeneration::FsrFG && (selected != next->provider || cfg.fg.fsr_mfg)) {
            auto latency = selected == FrameGeneration::DlssG ? streamline_
                           : selected == FrameGeneration::Off ? std::shared_ptr<Backend>{}
                           : make_backend(selected, root, false, error);
            if (selected != FrameGeneration::Off && !latency) {
                report("Latency reduction unavailable: " +
                       (selected == FrameGeneration::DlssG ? streamline_error_ : error));
                error.clear();
            }
            next->backend = with_latency(std::move(next->backend), std::move(latency));
        }
    }
    if (!next->backend)
        return false;
    if (!next->backend->create(next->device12.Get(), f, window, desc, format, next->queue, next->swapchain,
                               error))
        return false;
    const auto space = !cfg.hdr.enabled ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                       : format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                                : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    UINT support = 0;
    if (!ok(next->swapchain->CheckColorSpaceSupport(space, &support), "Output color-space query", error) ||
        !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ||
        !ok(next->swapchain->SetColorSpace1(space), "Output color-space activation", error)) {
        if (error.empty())
            error = "Requested output color space is unavailable";
        return false;
    }
    if (!next->fence(next->producer, next->producer11, error) ||
        !next->fence(next->consumer, next->consumer11, error))
        return false;
    next->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!next->event)
        return false;
    for (auto &slot : next->slots) {
        if (!ok(next->device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&slot.allocator)),
                "FG allocator", error) ||
            !ok(next->device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(),
                                                  nullptr, IID_PPV_ARGS(&slot.commands)),
                "FG command list", error) ||
            FAILED(slot.commands->Close()))
            return false;
    }
    ComPtr<ID3DBlob> code, errors;
    if (!ok(D3DCompile(shader, sizeof(shader), "XIVRender FG transport", nullptr, nullptr, "Main", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors),
            "FG guide shader", error)) {
        if (errors)
            error += static_cast<const char *>(errors->GetBufferPointer());
        return false;
    }
    if (!ok(next->device11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                &next->compute),
            "FG compute shader", error))
        return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Constants);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (!ok(next->device11->CreateBuffer(&bd, nullptr, &next->constants), "FG shader constants", error))
        return false;
    ComPtr<ID3D11Device1> d1;
    auto level = next->device11->GetFeatureLevel();
    D3D_FEATURE_LEVEL selected;
    if (FAILED(next->device11.As(&d1)) ||
        !ok(d1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &selected,
                                         &next->state),
            "FG isolated pipeline state", error))
        return false;
    if (!next->allocate(desc.BufferDesc.Width, desc.BufferDesc.Height, error))
        return false;
    next->backend->retain_device(next->device12.Get(), next->queue.Get());
    impl_ = std::move(next);
    ++generation_;
    diagnostics::context(unsigned(impl_->provider), generation_);
    diagnostics::event("handoff.committed", 0, impl_->backend->maximum(), unsigned(format),
                       latency_provider(cfg.fg) == FrameGeneration::Off ? 0 : 1);
    g.sink = impl_.get();
    {
        std::lock_guard data(g.mutex);
        if ((g.settings.fg.enabled && g.settings.fg.backend == impl_->provider &&
             impl_->provider != FrameGeneration::Off) ||
            latency_provider(cfg.fg) != FrameGeneration::Off)
            g.backend = impl_->backend;
        else
            g.backend.reset();
    }
    update([&](Status &s) {
        s.available = true;
        s.active = false;
        s.latency = impl_->backend->latency_available();
        s.factor = 1;
        s.rate_measured = false;
        s.provider = impl_->provider;
        s.maximum = impl_->backend->maximum();
        s.latency_name = impl_->backend->latency_name();
        s.message = "FG presentation initialized; waiting for native DLSS frame";
    });
    return true;
} catch (const std::exception &e) {
    error = e.what();
    return false;
}
} // namespace xivrender::fg
