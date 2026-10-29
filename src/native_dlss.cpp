// Native FFXIV NGX interception. No proxy DLL and no OptiScaler runtime.
#include "xivrender/native_dlss.hpp"
#include "xivrender/app.hpp"
#include "xivrender/ffxiv_hooks.hpp"
#include "xivrender/policies.hpp"
#include "xivrender/depth.hpp"
#include "xivrender/mip_bias.hpp"
#include "xivrender/trace.hpp"
#include "xivrender/ngx_parameters.hpp"
#include "xivrender/native_nr_api.hpp"
#include "xivrender/native_nr.hpp"
#include "xivrender/hdr.hpp"
#include "xivrender/frame_generation.hpp"
#include "xivrender/fg_diagnostics.hpp"
#include "xivrender/ngx_lifecycle.hpp"
#include "xivrender/dlss_sizes.hpp"
#include "xivrender/d3d11_work.hpp"
#include <MinHook.h>
#include <nvsdk_ngx.h>
#include <wrl/client.h>
#include <windows.h>
#include <psapi.h>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <cmath>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
namespace xivrender::native_dlss {
namespace {
using Microsoft::WRL::ComPtr;
using Create = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D11DeviceContext *, NVSDK_NGX_Feature, NVSDK_NGX_Parameter *,
                                              NVSDK_NGX_Handle **);
using Evaluate = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D11DeviceContext *, const NVSDK_NGX_Handle *,
                                                const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);
using Release = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);
using Optimal = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter *);
Create original_create = nullptr;
Evaluate original_evaluate = nullptr;
Release original_release = nullptr;
Optimal original_optimal = nullptr;
decltype(&XIVSR_InvokeCallback) invoke_callback = nullptr;
HMODULE caller_bridge = nullptr;
std::recursive_mutex mutex;
std::atomic<bool> installed{false}, stopping{false};
std::atomic<unsigned> inside{0};
struct Call {
    Call() {
        inside.fetch_add(1, std::memory_order_acquire);
    }
    ~Call() {
        inside.fetch_sub(1, std::memory_order_release);
    }
};
HMODULE pinned_addon = nullptr, pinned_runtime = nullptr;
ULONGLONG last_probe = 0;
std::vector<void *> targets;
struct Feature {
    unsigned width = 0, height = 0, out_width = 0, out_height = 0;
    bool main = false;
    unsigned allocation_width = 0, allocation_height = 0;
    const char *source = "creation contract";
    unsigned flags = 0;
    bool flags_known = false;
    hdr::DlssInput hdr_buffers;
};
// NGX owns live feature handles. Keep their private inputs/history until its
// successful ReleaseFeature, including addon teardown before game teardown.
// No COM destruction under the loader lock if the game exits with live handles.
auto &features = *new std::unordered_map<const NVSDK_NGX_Handle *, Feature>;
unsigned value(const NVSDK_NGX_Parameter *p, const char *name, unsigned fallback = 0) {
    unsigned v = fallback;
    return p && p->Get(name, &v) == NVSDK_NGX_Result_Success ? v : fallback;
}
int integer_value(const NVSDK_NGX_Parameter *p, const char *name, int fallback = -1) {
    int v = fallback;
    if (p && p->Get(name, &v) == NVSDK_NGX_Result_Success)
        return v;
    unsigned u = 0;
    return p && p->Get(name, &u) == NVSDK_NGX_Result_Success && u <= INT_MAX ? int(u) : fallback;
}
NVSDK_NGX_Result NVSDK_CONV optimal(NVSDK_NGX_Parameter *p) {
    Call call;
    std::lock_guard lock(mutex);
    const auto cfg = settings_snapshot();
    const auto status = status_snapshot();
    const int quality = integer_value(p, NVSDK_NGX_Parameter_PerfQualityValue);
    const auto width = value(p, NVSDK_NGX_Parameter_Width), height = value(p, NVSDK_NGX_Parameter_Height);
    const auto scale = ngx_scale(quality);
    const bool control = !stopping && p && fixed_dlss_resolution(cfg.dlss) && scale && width && height &&
                         width == status.display_width && height == status.display_height && width <= 16384 &&
                         height <= 16384;
    if (control && quality == 4)
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, 2);
    const auto result =
        static_cast<NVSDK_NGX_Result>(invoke_callback(reinterpret_cast<void *>(original_optimal), p));
    if (control && quality == 4)
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, quality);
    if (control && result == NVSDK_NGX_Result_Success) {
        const auto w = dlss_extent(width, *scale), h = dlss_extent(height, *scale);
        p->Set(NVSDK_NGX_Parameter_OutWidth, w);
        p->Set(NVSDK_NGX_Parameter_OutHeight, h);
        p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, w);
        p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, h);
        p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, w);
        p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, h);
    }
    static TraceBudget trace;
    if (trace.take(trace_epoch(), 24)) {
        log_message("NGX optimal contract request#" + std::to_string(trace_epoch()) +
                    ": perf_quality=" + std::to_string(quality) + ", controlled=" + std::to_string(control) +
                    ", display=" + std::to_string(width) + "x" + std::to_string(height) +
                    ", returned=" + std::to_string(value(p, NVSDK_NGX_Parameter_OutWidth)) + "x" +
                    std::to_string(value(p, NVSDK_NGX_Parameter_OutHeight)) +
                    ", result=" + std::to_string(unsigned(result)));
    }
    return result;
}
Feature dimensions(const NVSDK_NGX_Parameter *p, bool evaluation = false) {
    Feature f;
    f.flags_known =
        p && p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &f.flags) == NVSDK_NGX_Result_Success;
    f.width = value(p, NVSDK_NGX_Parameter_Width);
    f.height = value(p, NVSDK_NGX_Parameter_Height);
    f.out_width = value(p, NVSDK_NGX_Parameter_OutWidth);
    f.out_height = value(p, NVSDK_NGX_Parameter_OutHeight);
    auto extent = [&](const char *key, unsigned &w, unsigned &h) {
        ID3D11Resource *raw = nullptr;
        p->Get(key, &raw);
        ComPtr<ID3D11Texture2D> texture;
        if (raw && SUCCEEDED(raw->QueryInterface(IID_PPV_ARGS(&texture)))) {
            D3D11_TEXTURE2D_DESC d{};
            texture->GetDesc(&d);
            if (!w)
                w = d.Width;
            if (!h)
                h = d.Height;
        }
    };
    extent(NVSDK_NGX_Parameter_Output, f.out_width, f.out_height);
    extent(NVSDK_NGX_Parameter_Color, f.width, f.height);
    if (evaluation) {
        f.width = value(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, f.width);
        f.height = value(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, f.height);
    }
    const auto status = status_snapshot();
    f.main = f.out_width >= 640 && f.out_height >= 360 && status.display_width &&
             f.out_width == status.display_width && f.out_height == status.display_height;
    return f;
}
void texture_extent(const NVSDK_NGX_Parameter *p, const char *key, unsigned &w, unsigned &h,
                    unsigned &format) {
    ID3D11Resource *raw = nullptr;
    ComPtr<ID3D11Texture2D> texture;
    if (p->Get(key, &raw) == NVSDK_NGX_Result_Success && raw &&
        SUCCEEDED(raw->QueryInterface(IID_PPV_ARGS(&texture)))) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        w = desc.Width;
        h = desc.Height;
        format = unsigned(desc.Format);
    }
}
std::string float_parameter(const NVSDK_NGX_Parameter *p, const char *key) {
    float v = 0;
    if (p->Get(key, &v) != NVSDK_NGX_Result_Success)
        return "missing";
    std::ostringstream out;
    out << std::setprecision(9) << v;
    return out.str();
}
constexpr std::array<const char *, 6> preset_keys{
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};
NVSDK_NGX_Result NVSDK_CONV create(ID3D11DeviceContext *context, NVSDK_NGX_Feature id, NVSDK_NGX_Parameter *p,
                                   NVSDK_NGX_Handle **handle) {
    Call call;
    d3d11_work::Guard context_owner;
    std::lock_guard lock(mutex);
    if (stopping || !p || id != NVSDK_NGX_Feature_SuperSampling)
        return original_create(context, id, p, handle);
    auto f = dimensions(p);
    const auto settings = settings_snapshot();
    std::array<unsigned, 6> previous{};
    const bool override =
        f.main && settings.dlss.enabled && settings.dlss.model_preset != DlssModelPreset::Automatic;
    if (override)
        for (size_t i = 0; i < preset_keys.size(); ++i) {
            previous[i] = value(p, preset_keys[i]);
            p->Set(preset_keys[i], unsigned(settings.dlss.model_preset));
        }
    // Let FFXIV and NGX own the handle and all GPU model history. A live preset
    // request uses the native resize/recreate path, never a second private model.
    const int previous_quality = integer_value(p, NVSDK_NGX_Parameter_PerfQualityValue);
    const bool custom_ultra_quality = f.main && fixed_dlss_resolution(settings.dlss) && previous_quality == 4;
    if (custom_ultra_quality)
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, 2);
    const unsigned original_flags = value(p, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags);
    const bool hdr_feature = f.main && f.flags_known && hdr::active();
    if (hdr_feature) {
        f.flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
        p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, f.flags);
    }
    const auto result = original_create(context, id, p, handle);
    if (hdr_feature)
        p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, original_flags);
    if (custom_ultra_quality)
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, previous_quality);
    if (override)
        for (size_t i = 0; i < preset_keys.size(); ++i)
            p->Set(preset_keys[i], previous[i]);
    if (result == NVSDK_NGX_Result_Success && handle && *handle) {
        features[*handle] = f;
        if (f.main) {
            modify_status([&](Status &s) {
                s.last_created_model =
                    override ? int(settings.dlss.model_preset) : int(DlssModelPreset::Automatic);
                ++s.creates;
                s.message = "Native DLSS feature created";
            });
            log_message("Native DLSS feature created: " + std::to_string(f.width) + "x" +
                        std::to_string(f.height) + " -> " + std::to_string(f.out_width) + "x" +
                        std::to_string(f.out_height) + ", requested model " +
                        std::to_string(unsigned(settings.dlss.model_preset)));
        }
    } else if (f.main) {
        modify_status([&](Status &s) {
            ++s.failures;
            s.message = "Native DLSS creation failed; see XIVRender.log";
        });
    }
    std::ostringstream created;
    created << "NGX Create request#" << trace_epoch() << " context=" << context
            << " handle=" << (handle ? *handle : nullptr) << " main=" << f.main
            << " quality=" << quality_name(settings.dlss.quality)
            << " requested_model=" << model_name(settings.dlss.model_preset) << " hint_override=" << override
            << " custom_ultra_quality=" << custom_ultra_quality << " input_contract=" << f.width << 'x'
            << f.height << " output_contract=" << f.out_width << 'x' << f.out_height
            << " perf_quality=" << value(p, NVSDK_NGX_Parameter_PerfQualityValue, 999) << " flags=" << f.flags
            << " hdr=" << hdr_feature << " result=0x" << std::hex << unsigned(result);
    log_message(created.str());
    return result;
}
NVSDK_NGX_Result NVSDK_CONV evaluate(ID3D11DeviceContext *context, const NVSDK_NGX_Handle *handle,
                                     const NVSDK_NGX_Parameter *p, PFN_NVSDK_NGX_ProgressCallback callback) {
    const auto lock_started = fg::TimingClock::now();
    Call call;
    d3d11_work::Guard context_owner;
    std::lock_guard lock(mutex);
    const bool profile_timing = fg::output_requested();
    if (profile_timing)
        fg::note_timing(fg::TimingPhase::NativeSrLock, fg::elapsed_ms(lock_started));
    if (stopping || !p)
        return original_evaluate(context, handle, p, callback);
    auto found = features.find(handle);
    auto f = found == features.end() ? dimensions(p, true) : found->second;
    // Late addon attachment may miss CreateFeature. Recover only from explicit
    // native evaluation metadata; do not assume that a preview is the world.
    unsigned colour_format = 0, out_format = 0, output_allocation_width = 0, output_allocation_height = 0;
    texture_extent(p, NVSDK_NGX_Parameter_Color, f.allocation_width, f.allocation_height, colour_format);
    texture_extent(p, NVSDK_NGX_Parameter_Output, output_allocation_width, output_allocation_height,
                   out_format);
    const auto active_width = value(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width);
    const auto active_height = value(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height);
    if (active_width && active_height && (!f.allocation_width || active_width <= f.allocation_width) &&
        (!f.allocation_height || active_height <= f.allocation_height)) {
        f.width = active_width;
        f.height = active_height;
        f.source = "evaluation subrect";
    }
    if (!f.out_width)
        f.out_width = value(p, NVSDK_NGX_Parameter_OutWidth);
    if (!f.out_height)
        f.out_height = value(p, NVSDK_NGX_Parameter_OutHeight);
    if (!f.main) {
        const auto s = status_snapshot();
        f.main = f.out_width >= 640 && f.out_height >= 360 && f.out_width == s.display_width &&
                 f.out_height == s.display_height;
    }
    const auto settings = settings_snapshot();
    if (f.main && f.width && f.height && f.out_width && f.out_height) {
        ffxiv::measured_size(f.width, f.height, f.out_width, f.out_height);
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (!mip::is_attached())
            attach_device(device.Get());
        mip::update(settings.dlss.enabled && settings.dlss.automatic_mip_bias, f.width, f.out_width);
        ID3D11Resource *raw = nullptr;
        p->Get(NVSDK_NGX_Parameter_Depth, &raw);
        ComPtr<ID3D11Texture2D> depth_texture;
        if (raw)
            raw->QueryInterface(IID_PPV_ARGS(&depth_texture));
        if (depth_texture && settings.reshade.automatic_depth)
            depth::capture(context, depth_texture.Get(), f.width, f.height,
                           value(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X),
                           value(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y), f.out_width,
                           f.out_height, !fixed_dlss_resolution(settings.dlss));
    }
    hdr::DlssInput hdr_input = f.hdr_buffers;
    NgxOverlay hdr_parameters(p);
    const bool hdr_feature =
        f.main && f.flags_known && (f.flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) && hdr::active();
    if (hdr_feature) {
        ID3D11Resource *raw = nullptr;
        ComPtr<ID3D11Texture2D> color;
        p->Get(NVSDK_NGX_Parameter_Color, &raw);
        if (raw)
            raw->QueryInterface(IID_PPV_ARGS(&color));
        if (!color || !hdr::prepare_dlss(context, color.Get(), f.width, f.height,
                                         value(p, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X),
                                         value(p, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y),
                                         f.out_width, f.out_height, hdr_input)) {
            const auto failed = modify_status([](Status &s) {
                ++s.failures;
                s.message = "HDR input preparation failed; see log";
            });
            if (failed.failures <= 8 || failed.failures % 256 == 0)
                log_message("HDR input preparation rejected before native DLSS: " + hdr::status());
            // The native camera producer already queued this world frame.
            // Consume its cohort even though no output can be interpolated.
            if (fg::output_requested())
                fg::capture({});
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        }
        if (found != features.end())
            found->second.hdr_buffers = hdr_input;
        hdr_parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource *>(hdr_input.color.Get()));
        hdr_parameters.Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource *>(hdr_input.output.Get()));
        hdr_parameters.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
        hdr_parameters.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
        hdr_parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
        hdr_parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
    }
    const auto evaluate_started = fg::TimingClock::now();
    const auto result = original_evaluate(context, handle, hdr_feature ? &hdr_parameters : p, callback);
    if (profile_timing)
        fg::note_timing(fg::TimingPhase::NativeSrEvaluate, fg::elapsed_ms(evaluate_started));
    if (f.main && fg::output_requested() &&
        (result != NVSDK_NGX_Result_Success || !f.flags_known ||
         context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE))
        fg::capture({});
    if (result == NVSDK_NGX_Result_Success && hdr_feature)
        hdr::begin_nr(context);
    if (result == NVSDK_NGX_Result_Success && f.main && f.flags_known &&
        context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE &&
        (settings.nr.enabled || native_nr::needs_service() || fg::output_requested())) {
        const auto texture = [&](const char *key) {
            ID3D11Resource *raw = nullptr;
            ComPtr<ID3D11Texture2D> image;
            if (p->Get(key, &raw) == NVSDK_NGX_Result_Success && raw)
                raw->QueryInterface(IID_PPV_ARGS(&image));
            return image;
        };
        auto color = hdr_feature ? hdr_input.working : texture(NVSDK_NGX_Parameter_Output);
        auto depth = texture(NVSDK_NGX_Parameter_Depth);
        auto motion = texture(NVSDK_NGX_Parameter_MotionVectors);
        nr::FrameInput frame;
        auto &images = frame.images;
        images.color = color.Get();
        images.depth = depth.Get();
        images.motion = motion.Get();
        images.output_width = f.out_width;
        images.output_height = f.out_height;
        images.output_x = hdr_feature ? 0 : value(p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X);
        images.output_y = hdr_feature ? 0 : value(p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y);
        images.guide_width = f.width;
        images.guide_height = f.height;
        images.depth_x = value(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X);
        images.depth_y = value(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
        const bool low_motion = (f.flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
        images.motion_width = low_motion ? f.width : f.out_width;
        images.motion_height = low_motion ? f.height : f.out_height;
        images.motion_x = value(p, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X);
        images.motion_y = value(p, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y);
        p->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &images.jitter_x);
        p->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &images.jitter_y);
        p->Get(NVSDK_NGX_Parameter_MV_Scale_X, &images.motion_scale_x);
        p->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &images.motion_scale_y);
        images.depth_inverted = (f.flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
        frame.hdr = (f.flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
        images.linear_hdr = frame.hdr;
        frame.motion_jittered = (f.flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
        frame.reset = value(p, NVSDK_NGX_Parameter_Reset) != 0;
        frame.sr_feature = handle;
        static std::uint64_t real_frame = 0;
        frame.frame_id = ++real_frame;
        if (settings.nr.enabled || native_nr::needs_service()) {
            const auto nr_started = fg::TimingClock::now();
            native_nr::after_upscale(frame);
            if (profile_timing)
                fg::note_timing(fg::TimingPhase::NeuralRendering, fg::elapsed_ms(nr_started));
        }
        fg::capture(frame);
    }
    if (result == NVSDK_NGX_Result_Success && hdr_feature) {
        ID3D11Resource *raw = nullptr;
        ComPtr<ID3D11Texture2D> color;
        p->Get(NVSDK_NGX_Parameter_Output, &raw);
        if (raw)
            raw->QueryInterface(IID_PPV_ARGS(&color));
        if (!hdr::finish_dlss(context, color.Get(), f.out_width, f.out_height,
                              value(p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X),
                              value(p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y)))
            hdr::report("HDR output conversion failed");
    }
    if (f.main) {
        const auto s = modify_status([&](Status &s) {
            s.jitter_phases = jitter_phase_count(f.width, f.out_width);
            s.mip_bias = mip::current();
            if (result == NVSDK_NGX_Result_Success) {
                s.render_width = f.width;
                s.render_height = f.height;
                s.output_width = f.out_width;
                s.output_height = f.out_height;
                s.input_allocation_width = f.allocation_width;
                s.input_allocation_height = f.allocation_height;
                s.resolution_source = f.source;
                s.evaluated_resolution = f.width && f.height && f.out_width && f.out_height;
                s.last_evaluation_tick = GetTickCount64();
                ++s.evaluations;
            } else
                ++s.failures;
        });
        static TraceBudget trace;
        static Feature last_layout;
        const bool layout_changed = f.width != last_layout.width || f.height != last_layout.height ||
                                    f.out_width != last_layout.out_width ||
                                    f.out_height != last_layout.out_height;
        const bool report_failure =
            result != NVSDK_NGX_Result_Success && (s.failures <= 8 || s.failures % 256 == 0);
        if (trace.take(trace_epoch()) || layout_changed || report_failure) {
            std::ostringstream line;
            line << "NGX Evaluate request#" << trace_epoch() << " eval=" << s.evaluations
                 << " handle=" << handle << " context=" << context
                 << " context_type=" << unsigned(context->GetType())
                 << " quality_request=" << quality_name(settings.dlss.quality)
                 << " model_request=" << model_name(settings.dlss.model_preset) << " input=" << f.width << 'x'
                 << f.height << " source=" << f.source << " output=" << f.out_width << 'x' << f.out_height
                 << " colour_allocation=" << f.allocation_width << 'x' << f.allocation_height
                 << " colour_format=" << colour_format << " output_allocation=" << output_allocation_width
                 << 'x' << output_allocation_height << " output_format=" << out_format
                 << " hdr=" << hdr_feature << " hdr_status=" << hdr::status()
                 << " jitter=" << float_parameter(p, NVSDK_NGX_Parameter_Jitter_Offset_X) << ','
                 << float_parameter(p, NVSDK_NGX_Parameter_Jitter_Offset_Y)
                 << " mv_scale=" << float_parameter(p, NVSDK_NGX_Parameter_MV_Scale_X) << ','
                 << float_parameter(p, NVSDK_NGX_Parameter_MV_Scale_Y)
                 << " frame_ms=" << float_parameter(p, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec)
                 << " pre_exposure=" << float_parameter(p, NVSDK_NGX_Parameter_DLSS_Pre_Exposure)
                 << " reset=" << value(p, NVSDK_NGX_Parameter_Reset, 999) << " mip_bias=" << s.mip_bias
                 << " sampler_variants=" << mip::sampler_creations() << " result=0x" << std::hex
                 << unsigned(result);
            log_message(line.str());
        }
        last_layout = f;
    }
    return result;
}
NVSDK_NGX_Result NVSDK_CONV release(NVSDK_NGX_Handle *handle) {
    fg::diagnostics::Scope phase("native.dlss_release");
    Call call;
    auto presentation = fg::lock_native_source();
    d3d11_work::Guard context_owner;
    std::lock_guard lock(mutex);
    std::string retirement_error;
    if (!fg::retire_native_source(handle, retirement_error)) {
        log_message("Native SR release retained while FG is busy: " + retirement_error);
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
    const auto result = original_release(handle);
    if (result == NVSDK_NGX_Result_Success)
        features.erase(handle);
    return result;
}
HMODULE find_runtime() {
    HMODULE modules[1024]{};
    DWORD size = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &size))
        return nullptr;
    // Streamline can load an additional SR snippet for its own dependencies.
    // Those private copies must not suppress interception of FFXIV's native
    // snippet, or receive the game's quality/HDR/NR hooks themselves. Do not
    // prefer arbitrary filenames outside this explicitly owned subtree: NVIDIA
    // cache replacements can have hashed names, and true ambiguity remains an
    // error rather than guessing which caller owns the model.
    std::filesystem::path private_root;
    HMODULE self = nullptr;
    wchar_t own_path[32768]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&poll), &self) &&
        GetModuleFileNameW(self, own_path, 32768))
        private_root =
            (std::filesystem::path(own_path).parent_path() / L"XIVRender/runtime").lexically_normal();
    auto private_prefix = private_root.native();
    if (!private_prefix.empty() && private_prefix.back() != L'\\')
        private_prefix += L'\\';
    HMODULE found = nullptr;
    std::filesystem::path found_path;
    unsigned ignored_private = 0;
    for (unsigned i = 0; i < std::min<unsigned>(size / sizeof(HMODULE), 1024); ++i) {
        auto module = modules[i];
        if (!GetProcAddress(module, "NVSDK_NGX_D3D11_CreateFeature") ||
            !GetProcAddress(module, "NVSDK_NGX_D3D11_EvaluateFeature") ||
            !GetProcAddress(module, "NVSDK_NGX_D3D11_ReleaseFeature"))
            continue;
        wchar_t path[32768]{};
        if (!GetModuleFileNameW(module, path, 32768))
            continue;
        const auto candidate = std::filesystem::path(path).lexically_normal();
        const auto &native = candidate.native();
        if (!private_prefix.empty() && native.size() > private_prefix.size() &&
            _wcsnicmp(native.c_str(), private_prefix.c_str(), private_prefix.size()) == 0) {
            ++ignored_private;
            continue;
        }
        bool identity = _wcsicmp(std::filesystem::path(path).filename().c_str(), L"nvngx_dlss.dll") == 0;
        if (!identity && GetProcAddress(module, "NGX_SNIPPETS_GetRequiredDriverSupport")) {
            DWORD ignored = 0;
            const auto length = GetFileVersionInfoSizeW(path, &ignored);
            std::vector<std::uint8_t> info(length);
            if (length && GetFileVersionInfoW(path, 0, length, info.data())) {
                struct Translation {
                    WORD language, codepage;
                };
                Translation *translations = nullptr;
                UINT bytes = 0;
                if (VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation",
                                   reinterpret_cast<void **>(&translations), &bytes))
                    for (unsigned j = 0; j < bytes / sizeof(Translation); ++j) {
                        wchar_t key[128]{};
                        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\OriginalFilename",
                                   translations[j].language, translations[j].codepage);
                        wchar_t *original = nullptr;
                        UINT chars = 0;
                        if (VerQueryValueW(info.data(), key, reinterpret_cast<void **>(&original), &chars) &&
                            chars && _wcsicmp(original, L"nvngx_dlss.dll") == 0) {
                            identity = true;
                            break;
                        }
                    }
            }
        }
        if (identity) {
            if (found && found != module) {
                static ULONGLONG last_ambiguity = 0;
                const auto now = GetTickCount64();
                if (!last_ambiguity || now - last_ambiguity >= 15000) {
                    last_ambiguity = now;
                    log_message(
                        "Multiple non-private native DLSS SR runtimes are loaded; interception withheld: " +
                        found_path.string() + " and " + candidate.string());
                }
                return nullptr;
            }
            found = module;
            found_path = candidate;
        }
    }
    static std::filesystem::path reported_selection;
    static unsigned reported_private = 0;
    if (found && (found_path != reported_selection || ignored_private != reported_private)) {
        reported_selection = found_path;
        reported_private = ignored_private;
        log_message("Native DLSS SR selected: " + found_path.string() +
                    "; private provider/helper modules excluded=" + std::to_string(ignored_private));
    }
    return found;
}
bool pristine(HMODULE module, void *function) {
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(module, path, 32768))
        return false;
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
        return false;
    // Model DLLs may contain hundreds of megabytes of weights. Only PE headers
    // and the target prologue are needed; never load the model image into RAM.
    IMAGE_DOS_HEADER dos{};
    if (!file.read(reinterpret_cast<char *>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 0 || dos.e_lfanew > 16 * 1024 * 1024)
        return false;
    file.seekg(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    if (!file.read(reinterpret_cast<char *>(&signature), sizeof(signature)) ||
        signature != IMAGE_NT_SIGNATURE || !file.read(reinterpret_cast<char *>(&header), sizeof(header)) ||
        header.Machine != IMAGE_FILE_MACHINE_AMD64 || header.NumberOfSections > 96)
        return false;
    file.seekg(header.SizeOfOptionalHeader, std::ios::cur);
    std::vector<IMAGE_SECTION_HEADER> sections(header.NumberOfSections);
    if (!file.read(reinterpret_cast<char *>(sections.data()),
                   std::streamsize(sections.size() * sizeof(IMAGE_SECTION_HEADER))))
        return false;
    const auto offset = reinterpret_cast<std::uintptr_t>(function) - reinterpret_cast<std::uintptr_t>(module);
    std::array<std::uint8_t, 16> disk{}, live{};
    bool located = false;
    for (const auto &section : sections)
        if (offset >= section.VirtualAddress && offset - section.VirtualAddress < section.SizeOfRawData &&
            section.SizeOfRawData - (offset - section.VirtualAddress) >= disk.size() &&
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
            file.seekg(std::streamoff(section.PointerToRawData) +
                       std::streamoff(offset - section.VirtualAddress));
            located = bool(file.read(reinterpret_cast<char *>(disk.data()), disk.size()));
            break;
        }
    SIZE_T copied = 0;
    return located && ReadProcessMemory(GetCurrentProcess(), function, live.data(), live.size(), &copied) &&
           copied == live.size() && live == disk;
}
} // namespace
void attach_device(ID3D11Device *device) {
    if (!device || stopping || mip::is_attached())
        return;
    std::string shutdown_error;
    if (!ngx::guard_native_shutdown(device, shutdown_error) && !shutdown_error.empty())
        log_message(shutdown_error);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    std::string error;
    if (!mip::attach(context.Get(), error) && !error.empty()) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            log_message(error);
        }
    }
}
void poll() {
    std::string shutdown_error;
    ngx::guard_native_shutdown(nullptr, shutdown_error);
    if (installed || stopping)
        return;
    std::lock_guard lock(mutex);
    if (installed || stopping || GetTickCount64() - last_probe < 500)
        return;
    last_probe = GetTickCount64();
    HMODULE module = find_runtime();
    if (!module)
        return;
    if (!caller_bridge) {
        HMODULE self = nullptr;
        wchar_t own_path[32768]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&poll), &self) &&
            GetModuleFileNameW(self, own_path, 32768)) {
            const auto path = std::filesystem::path(own_path).parent_path() /
                              L"XIVRender/runtime/XIVRender_nvngx.dll_nr.dll";
            caller_bridge = LoadLibraryExW(
                path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }
    }
    const auto populate = caller_bridge ? reinterpret_cast<decltype(&XIVSR_PopulateCallbacks)>(
                                              GetProcAddress(caller_bridge, "XIVSR_PopulateCallbacks"))
                                        : nullptr;
    invoke_callback = caller_bridge ? reinterpret_cast<decltype(&XIVSR_InvokeCallback)>(
                                          GetProcAddress(caller_bridge, "XIVSR_InvokeCallback"))
                                    : nullptr;
    ngx::Parameters capabilities;
    void *optimal_target = nullptr;
    if (!populate || !invoke_callback || populate(module, &capabilities) != 1 ||
        capabilities.Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &optimal_target) !=
            NVSDK_NGX_Result_Success ||
        !optimal_target || !pristine(module, optimal_target)) {
        static ULONGLONG last_callback_report = 0;
        const auto now = GetTickCount64();
        if (!last_callback_report || now - last_callback_report >= 15000) {
            last_callback_report = now;
            log_message("Native SR optimal-settings callback unavailable; quality changes remain inactive");
        }
        return;
    }
    const char *names[]{"NVSDK_NGX_D3D11_CreateFeature", "NVSDK_NGX_D3D11_EvaluateFeature",
                        "NVSDK_NGX_D3D11_ReleaseFeature"};
    void *hooks[]{reinterpret_cast<void *>(&create), reinterpret_cast<void *>(&evaluate),
                  reinterpret_cast<void *>(&release)};
    void **originals[]{reinterpret_cast<void **>(&original_create),
                       reinterpret_cast<void **>(&original_evaluate),
                       reinterpret_cast<void **>(&original_release)};
    for (unsigned i = 0; i < 3; ++i) {
        auto *target = reinterpret_cast<void *>(GetProcAddress(module, names[i]));
        const bool unchanged = target && pristine(module, target);
        const auto hook_result = unchanged ? MH_CreateHook(target, hooks[i], originals[i]) : MH_ERROR_NOT_EXECUTABLE;
        if (!unchanged || hook_result != MH_OK) {
            log_message(std::string("Native DLSS hook rejected: ") + names[i] +
                        "; export=" + std::to_string(reinterpret_cast<std::uintptr_t>(target)) +
                        "; unchanged=" + std::to_string(unchanged) +
                        "; hook=" + MH_StatusToString(hook_result));
            for (auto *p : targets)
                MH_RemoveHook(p);
            targets.clear();
            auto s = status_snapshot();
            s.conflict = true;
            s.message = "DLSS hooks unavailable or already modified by another addon";
            update_status(s);
            // No hooks were enabled. Retry a later stable load instead of
            // permanently disabling controls after a transient startup failure.
            return;
        }
        targets.push_back(target);
    }
    if (MH_CreateHook(optimal_target, reinterpret_cast<void *>(&optimal),
                      reinterpret_cast<void **>(&original_optimal)) != MH_OK) {
        for (auto *p : targets)
            MH_RemoveHook(p);
        targets.clear();
        stopping = true;
        log_message(
            "Native SR optimal-settings callback is already controlled; no duplicate hooks installed");
        return;
    }
    targets.push_back(optimal_target);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&poll), &pinned_addon) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(targets.front()), &pinned_runtime)) {
        for (auto *p : targets)
            MH_RemoveHook(p);
        targets.clear();
        stopping = true;
        return;
    }
    for (auto *p : targets)
        if (MH_EnableHook(p) != MH_OK) {
            for (auto *q : targets)
                MH_DisableHook(q);
            // An enabled wrapper may already be waiting for this mutex. Keep
            // its trampoline alive until shutdown can drain outside the lock.
            if (!inside.load(std::memory_order_acquire)) {
                for (auto *q : targets)
                    MH_RemoveHook(q);
                targets.clear();
            }
            stopping = true;
            return;
        }

    installed = true;
    auto s = status_snapshot();
    s.native_dlss_hooks = true;
    s.conflict = false;
    s.phase = "Native DLSS adapter";
    s.message = "DLSS interception ready; enable native DLSS in FFXIV";
    update_status(s);
    ffxiv::request_feature_refresh();
    log_message("Native DLSS hooks installed; native model ownership retained");
}
bool active() {
    return installed;
}
void shutdown() {
    stopping = true;
    for (auto *p : targets)
        MH_DisableHook(p);
    const auto deadline = GetTickCount64() + 1000;
    while (inside.load(std::memory_order_acquire) && GetTickCount64() < deadline)
        Sleep(1);
    if (inside.load(std::memory_order_acquire)) {
        installed = false;
        log_message("Native DLSS hooks disabled; busy trampolines retained until process exit");
        return;
    }
    std::lock_guard lock(mutex);
    for (auto *p : targets)
        MH_RemoveHook(p);
    targets.clear();
    installed = false;
    // Live game-owned handles may still reference their HDR textures.
    {
        fg::diagnostics::Scope phase("shutdown.native_nr");
        native_nr::shutdown();
    }
    mip::detach();
    depth::reset();
    // Both addon code and original runtime remain pinned for process teardown.
}
} // namespace xivrender::native_dlss
