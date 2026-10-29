// Test-only NGX ABI fixture. This file implements no NVIDIA model or algorithm.
#define NVSDK_NGX
#include <nvsdk_ngx.h>
#include <array>
#include <cstdint>
#include <unordered_set>

struct MockMetrics {
    unsigned creates = 0, evaluations = 0, releases = 0, live_handles = 0;
    unsigned feature = 0, presets[6]{}, width = 0, height = 0, out_width = 0, out_height = 0;
    float jitter_x = 0, jitter_y = 0;
    std::uintptr_t context = 0, handle = 0, parameters = 0, callback = 0;
    int create_quality = -1;
    unsigned create_flags = 0;
    std::uintptr_t color = 0, output = 0;
};
namespace {
MockMetrics metrics;
NVSDK_NGX_Result create_result = NVSDK_NGX_Result_Success, evaluate_result = NVSDK_NGX_Result_Success,
                 release_result = NVSDK_NGX_Result_Success;
unsigned next_id = 1;
std::unordered_set<NVSDK_NGX_Handle *> handles;
constexpr std::array<const char *, 6> preset_keys{
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};
} // namespace

extern "C" __declspec(dllexport) void __cdecl MockReset() {
    for (auto *handle : handles)
        delete reinterpret_cast<unsigned *>(handle);
    handles.clear();
    metrics = {};
    next_id = 1;
    create_result = evaluate_result = release_result = NVSDK_NGX_Result_Success;
}
extern "C" __declspec(dllexport) void __cdecl MockMetricsSnapshot(MockMetrics *output) {
    if (output)
        *output = metrics;
}
extern "C" __declspec(dllexport) void __cdecl MockResults(unsigned creation, unsigned evaluation,
                                                          unsigned release) {
    create_result = static_cast<NVSDK_NGX_Result>(creation);
    evaluate_result = static_cast<NVSDK_NGX_Result>(evaluation);
    release_result = static_cast<NVSDK_NGX_Result>(release);
}

extern "C" __declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result NVSDK_CONV
MockCreateFeature(ID3D11DeviceContext *context, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter *parameters,
                  NVSDK_NGX_Handle **output) {
    ++metrics.creates;
    metrics.context = reinterpret_cast<std::uintptr_t>(context);
    metrics.parameters = reinterpret_cast<std::uintptr_t>(parameters);
    metrics.feature = unsigned(feature);
    metrics.create_quality = -1;
    if (parameters)
        parameters->Get(NVSDK_NGX_Parameter_PerfQualityValue, &metrics.create_quality);
    if (parameters) {
        for (unsigned i = 0; i < preset_keys.size(); ++i) {
            metrics.presets[i] = 0;
            parameters->Get(preset_keys[i], &metrics.presets[i]);
        }
        parameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &metrics.create_flags);
        parameters->Get(NVSDK_NGX_Parameter_Width, &metrics.width);
        parameters->Get(NVSDK_NGX_Parameter_Height, &metrics.height);
        parameters->Get(NVSDK_NGX_Parameter_OutWidth, &metrics.out_width);
        parameters->Get(NVSDK_NGX_Parameter_OutHeight, &metrics.out_height);
    }
    if (!output)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *output = nullptr;
    if (create_result != NVSDK_NGX_Result_Success)
        return create_result;
    *output = reinterpret_cast<NVSDK_NGX_Handle *>(new unsigned(next_id++));
    handles.insert(*output);
    metrics.live_handles = unsigned(handles.size());
    metrics.handle = reinterpret_cast<std::uintptr_t>(*output);
    return create_result;
}

extern "C" __declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result NVSDK_CONV
MockEvaluateFeature(ID3D11DeviceContext *context, const NVSDK_NGX_Handle *handle,
                    NVSDK_NGX_Parameter *parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
    ++metrics.evaluations;
    metrics.context = reinterpret_cast<std::uintptr_t>(context);
    metrics.handle = reinterpret_cast<std::uintptr_t>(handle);
    metrics.parameters = reinterpret_cast<std::uintptr_t>(parameters);
    metrics.callback = reinterpret_cast<std::uintptr_t>(callback);
    if (parameters) {
        ID3D11Resource *color = nullptr, *output = nullptr;
        parameters->Get(NVSDK_NGX_Parameter_Color, &color);
        parameters->Get(NVSDK_NGX_Parameter_Output, &output);
        metrics.color = reinterpret_cast<std::uintptr_t>(color);
        metrics.output = reinterpret_cast<std::uintptr_t>(output);
        parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &metrics.jitter_x);
        parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &metrics.jitter_y);
    }
    return evaluate_result;
}

extern "C" __declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result NVSDK_CONV
MockReleaseFeature(NVSDK_NGX_Handle *handle) {
    ++metrics.releases;
    metrics.handle = reinterpret_cast<std::uintptr_t>(handle);
    if (release_result != NVSDK_NGX_Result_Success)
        return release_result;
    if (!handles.erase(handle))
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    delete reinterpret_cast<unsigned *>(handle);
    metrics.live_handles = unsigned(handles.size());
    return release_result;
}

extern "C" __declspec(dllexport) unsigned NGX_SNIPPETS_GetRequiredDriverSupport() {
    return 0;
}
extern "C" __declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result NVSDK_CONV
MockOptimal(NVSDK_NGX_Parameter *parameters) {
    if (!parameters)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    unsigned w = 0, h = 0;
    int quality = -1;
    parameters->Get(NVSDK_NGX_Parameter_Width, &w);
    parameters->Get(NVSDK_NGX_Parameter_Height, &h);
    parameters->Get(NVSDK_NGX_Parameter_PerfQualityValue, &quality);
    const double scales[]{.5, .58, 2.0 / 3.0, 1.0 / 3.0, 0, 1};
    if (quality < 0 || quality > 5)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    parameters->Set(NVSDK_NGX_Parameter_OutWidth, unsigned(w * scales[quality]));
    parameters->Set(NVSDK_NGX_Parameter_OutHeight, unsigned(h * scales[quality]));
    return NVSDK_NGX_Result_Success;
}
extern "C" NVSDK_NGX_Result NVSDK_CONV MockPopulate(NVSDK_NGX_Parameter *parameters) {
    if (!parameters)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    parameters->Set(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, reinterpret_cast<void *>(&MockOptimal));
    return NVSDK_NGX_Result_Success;
}
