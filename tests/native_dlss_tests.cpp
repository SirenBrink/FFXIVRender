#include <xivrender/native_dlss.hpp>
#include <xivrender/hdr.hpp>
#include <xivrender/app.hpp>
#include <xivrender/ffxiv_hooks.hpp>
#include <MinHook.h>
#include <nvsdk_ngx.h>
#include <wrl/client.h>
#include <windows.h>
#include <array>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

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
xivrender::Settings requested;
xivrender::Status reported;
std::vector<std::string> messages;
unsigned measured_calls = 0;
unsigned forced_refreshes = 0;
std::array<unsigned, 4> measured{};

class Parameters final : public NVSDK_NGX_Parameter {
    using Value = std::variant<unsigned long long, float, double, unsigned, int, ID3D11Resource *,
                               ID3D12Resource *, void *>;
    std::unordered_map<std::string, Value> values;
    template <class T> void write(const char *name, T value) {
        values.insert_or_assign(name, value);
    }
    template <class T> NVSDK_NGX_Result read(const char *name, T *output) const {
        if (!name || !output)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        const auto found = values.find(name);
        if (found == values.end())
            return NVSDK_NGX_Result_FAIL_MissingInput;
        if (const auto *value = std::get_if<T>(&found->second)) {
            *output = *value;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }

  public:
    void Set(const char *n, unsigned long long v) override {
        write(n, v);
    }
    void Set(const char *n, float v) override {
        write(n, v);
    }
    void Set(const char *n, double v) override {
        write(n, v);
    }
    void Set(const char *n, unsigned v) override {
        write(n, v);
    }
    void Set(const char *n, int v) override {
        write(n, v);
    }
    void Set(const char *n, ID3D11Resource *v) override {
        write(n, v);
    }
    void Set(const char *n, ID3D12Resource *v) override {
        write(n, v);
    }
    void Set(const char *n, void *v) override {
        write(n, v);
    }
    NVSDK_NGX_Result Get(const char *n, unsigned long long *v) const override {
        return read(n, v);
    }
    NVSDK_NGX_Result Get(const char *n, float *v) const override {
        return read(n, v);
    }
    NVSDK_NGX_Result Get(const char *n, double *v) const override {
        return read(n, v);
    }
    NVSDK_NGX_Result Get(const char *n, unsigned *v) const override {
        return read(n, v);
    }
    NVSDK_NGX_Result Get(const char *n, int *v) const override {
        return read(n, v);
    }
    NVSDK_NGX_Result Get(const char *n, ID3D11Resource **v) const override {
        return read(n, v);
    }
    NVSDK_NGX_Result Get(const char *n, ID3D12Resource **v) const override {
        return read(n, v);
    }
    NVSDK_NGX_Result Get(const char *n, void **v) const override {
        return read(n, v);
    }
    void Reset() override {
        values.clear();
    }
};
constexpr std::array<const char *, 6> keys{NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};
void dimensions(Parameters &p, unsigned w = 2560, unsigned h = 1440, unsigned outw = 3840,
                unsigned outh = 2160) {
    p.Set(NVSDK_NGX_Parameter_Width, w);
    p.Set(NVSDK_NGX_Parameter_Height, h);
    p.Set(NVSDK_NGX_Parameter_OutWidth, outw);
    p.Set(NVSDK_NGX_Parameter_OutHeight, outh);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, w);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, h);
    for (unsigned i = 0; i < keys.size(); ++i)
        p.Set(keys[i], 20u + i);
}
void originals_restored(const Parameters &p) {
    for (unsigned i = 0; i < keys.size(); ++i) {
        unsigned value = 0;
        assert(p.Get(keys[i], &value) == NVSDK_NGX_Result_Success);
        assert(value == 20u + i);
    }
}
void NVSDK_CONV progress(float, bool &) {}
} // namespace

namespace xivrender {
std::uint64_t trace_epoch() {
    return 1;
}
Settings settings_snapshot() {
    return requested;
}
Status status_snapshot() {
    return reported;
}
void update_status(const Status &s) {
    reported = s;
}
Status modify_status(const std::function<void(Status &)> &change) {
    change(reported);
    return reported;
}
void log_message(const std::string &text) {
    messages.push_back(text);
}
} // namespace xivrender
namespace xivrender::ffxiv {
void settings_changed() {}
void request_feature_refresh() {
    ++forced_refreshes;
}
void measured_size(unsigned w, unsigned h, unsigned ow, unsigned oh) {
    ++measured_calls;
    measured = {w, h, ow, oh};
}
} // namespace xivrender::ffxiv

int main(int argc, char **argv) {
    requested.dlss.quality = xivrender::DlssQuality::Quality;
    using Microsoft::WRL::ComPtr;
    assert(argc == 2);
    const auto fixture = std::filesystem::absolute(argv[1]);
    assert(fixture.filename() == L"nvngx_dlss.dll" || fixture.filename() == L"hashed_sr_runtime.dll");
    auto module = LoadLibraryExW(fixture.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    assert(module);
    wchar_t executable[32768]{};
    assert(GetModuleFileNameW(nullptr, executable, 32768));
    const auto host_root = std::filesystem::path(executable).parent_path();
    const auto unique = std::to_wstring(GetCurrentProcessId());
    const auto private_folder = host_root / L"XIVRender/runtime" / unique;
    const auto ambiguous_folder = host_root / L"XIVRender/runtime-other" / unique;
    std::filesystem::create_directories(private_folder);
    std::filesystem::create_directories(ambiguous_folder);
    const auto private_path = private_folder / L"nvngx_dlss.dll";
    const auto ambiguous_path = ambiguous_folder / L"nvngx_dlss.dll";
    std::filesystem::copy_file(fixture, private_path, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(fixture, ambiguous_path, std::filesystem::copy_options::overwrite_existing);
    const auto helper = LoadLibraryExW(private_path.c_str(), nullptr,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto ambiguous = LoadLibraryExW(
        ambiguous_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    assert(helper && helper != module && ambiguous && ambiguous != module && ambiguous != helper);
    using Create = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D11DeviceContext *, NVSDK_NGX_Feature,
                                                  NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
    using Evaluate = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D11DeviceContext *, const NVSDK_NGX_Handle *,
                                                    NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);
    using Release = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);
    const auto create = reinterpret_cast<Create>(GetProcAddress(module, "NVSDK_NGX_D3D11_CreateFeature"));
    const auto evaluate =
        reinterpret_cast<Evaluate>(GetProcAddress(module, "NVSDK_NGX_D3D11_EvaluateFeature"));
    const auto release = reinterpret_cast<Release>(GetProcAddress(module, "NVSDK_NGX_D3D11_ReleaseFeature"));
    const auto reset = reinterpret_cast<void(__cdecl *)()>(GetProcAddress(module, "MockReset"));
    const auto snapshot =
        reinterpret_cast<void(__cdecl *)(MockMetrics *)>(GetProcAddress(module, "MockMetricsSnapshot"));
    const auto results = reinterpret_cast<void(__cdecl *)(unsigned, unsigned, unsigned)>(
        GetProcAddress(module, "MockResults"));
    assert(create && evaluate && release && reset && snapshot && results);
    reset();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, nullptr, &context)));
    reported.display_width = 3840;
    reported.display_height = 2160;
    requested.dlss.model_preset = xivrender::DlssModelPreset::L;
    requested.reshade.automatic_depth = false;
    assert(MH_Initialize() == MH_OK);
    // A lookalike sibling folder is not provider-owned. Genuine ambiguity
    // still withholds hooks instead of preferring the conventional filename.
    xivrender::native_dlss::poll();
    assert(!xivrender::native_dlss::active());
    assert(std::any_of(messages.begin(), messages.end(), [](const auto &m) {
        return m.find("Multiple non-private native DLSS") != std::string::npos;
    }));
    FreeLibrary(ambiguous);
    std::filesystem::remove(ambiguous_path);
    std::filesystem::remove(ambiguous_folder);
    Sleep(550);
    void *foreign_original = nullptr;
    assert(MH_CreateHook(reinterpret_cast<void *>(create), reinterpret_cast<void *>(release), &foreign_original) == MH_OK);
    xivrender::native_dlss::poll();
    assert(!xivrender::native_dlss::active());
    // Never remove another owner's hook. Recover only after that owner retires it.
    assert(MH_RemoveHook(reinterpret_cast<void *>(create)) == MH_OK);
    Sleep(550);
    xivrender::native_dlss::poll();
    assert(xivrender::native_dlss::active());
    assert(forced_refreshes == 1);
    xivrender::native_dlss::poll();
    assert(forced_refreshes == 1); // ordinary polling must not recreate every frame
    // The private Streamline dependency remains loaded but is never hooked.
    // Its own evaluations must not receive game presets or affect telemetry.
    const auto helper_create =
        reinterpret_cast<Create>(GetProcAddress(helper, "NVSDK_NGX_D3D11_CreateFeature"));
    const auto helper_release =
        reinterpret_cast<Release>(GetProcAddress(helper, "NVSDK_NGX_D3D11_ReleaseFeature"));
    const auto helper_evaluate =
        reinterpret_cast<Evaluate>(GetProcAddress(helper, "NVSDK_NGX_D3D11_EvaluateFeature"));
    const auto helper_snapshot =
        reinterpret_cast<void(__cdecl *)(MockMetrics *)>(GetProcAddress(helper, "MockMetricsSnapshot"));
    assert(helper_create && helper_release && helper_evaluate && helper_snapshot);
    Parameters helper_parameters;
    dimensions(helper_parameters, 1920, 1080, 3840, 2160);
    NVSDK_NGX_Handle *helper_handle = nullptr;
    assert(helper_create(context.Get(), NVSDK_NGX_Feature_SuperSampling, &helper_parameters,
                         &helper_handle) == NVSDK_NGX_Result_Success);
    assert(helper_evaluate(context.Get(), helper_handle, &helper_parameters, nullptr) ==
           NVSDK_NGX_Result_Success);
    MockMetrics helper_seen{};
    helper_snapshot(&helper_seen);
    assert(helper_seen.creates == 1 && helper_seen.evaluations == 1 && reported.creates == 0 &&
           reported.evaluations == 0);
    for (unsigned i = 0; i < 6; ++i)
        assert(helper_seen.presets[i] == 20u + i);
    assert(helper_release(helper_handle) == NVSDK_NGX_Result_Success);
    const auto optimal = reinterpret_cast<NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter *)>(
        GetProcAddress(module, "MockOptimal"));
    assert(optimal);
    Parameters sizing;
    sizing.Set(NVSDK_NGX_Parameter_Width, 3840u);
    sizing.Set(NVSDK_NGX_Parameter_Height, 2160u);
    for (int q : {5, 4, 2, 1, 0, 5, 4}) {
        sizing.Set(NVSDK_NGX_Parameter_PerfQualityValue, q);
        assert(optimal(&sizing) == NVSDK_NGX_Result_Success);
        unsigned w = 0, h = 0;
        sizing.Get(NVSDK_NGX_Parameter_OutWidth, &w);
        sizing.Get(NVSDK_NGX_Parameter_OutHeight, &h);
        const unsigned widths[]{1920, 2258, 2560, 1280, 2956, 3840},
            heights[]{1080, 1270, 1440, 720, 1662, 2160};
        assert(w == widths[q] && h == heights[q]);
        int restored = -1;
        sizing.Get(NVSDK_NGX_Parameter_PerfQualityValue, &restored);
        assert(restored == q);
    }
    requested.dlss.enabled = false;
    sizing.Set(NVSDK_NGX_Parameter_PerfQualityValue, 4);
    assert(optimal(&sizing) == NVSDK_NGX_Result_Success);
    unsigned disabled_width = 1;
    sizing.Get(NVSDK_NGX_Parameter_OutWidth, &disabled_width);
    assert(disabled_width == 0);
    requested.dlss.enabled = true;
    requested.dlss.quality = xivrender::DlssQuality::Game;
    sizing.Set(NVSDK_NGX_Parameter_PerfQualityValue, 1);
    sizing.Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, 1001u);
    sizing.Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, 3840u);
    assert(optimal(&sizing) == NVSDK_NGX_Result_Success);
    unsigned native_width = 0, native_min = 0, native_max = 0;
    sizing.Get(NVSDK_NGX_Parameter_OutWidth, &native_width);
    sizing.Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &native_min);
    sizing.Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &native_max);
    assert(native_width == unsigned(3840 * .58) && native_min == 1001 && native_max == 3840);
    requested.dlss.quality = xivrender::DlssQuality::Quality;
    Parameters p;
    dimensions(p);
    // A reusable creation block can still carry the previous frame's subrect.
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 1920u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 1080u);
    NVSDK_NGX_Handle *world = nullptr;
    assert(create(context.Get(), NVSDK_NGX_Feature_SuperSampling, &p, &world) == NVSDK_NGX_Result_Success &&
           world);
    MockMetrics seen;
    snapshot(&seen);
    assert(seen.creates == 1 && seen.live_handles == 1);
    assert(seen.width == 2560 && !reported.evaluated_resolution);
    for (unsigned value : seen.presets)
        assert(value == unsigned(xivrender::DlssModelPreset::L));
    originals_restored(p);
    assert(reported.creates == 1);
    // A second, non-world viewport must retain its own native model hints.
    Parameters preview_params;
    dimensions(preview_params, 576, 960, 1152, 1920);
    NVSDK_NGX_Handle *preview = nullptr;
    assert(create(context.Get(), NVSDK_NGX_Feature_SuperSampling, &preview_params, &preview) ==
           NVSDK_NGX_Result_Success);
    snapshot(&seen);
    for (unsigned i = 0; i < keys.size(); ++i)
        assert(seen.presets[i] == 20u + i);
    originals_restored(preview_params);
    assert(reported.creates == 1);
    p.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, -.3125f);
    p.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, .2777778f);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 2560u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 1440u);
    assert(evaluate(context.Get(), world, &p, &progress) == NVSDK_NGX_Result_Success);
    snapshot(&seen);
    assert(seen.evaluations == 1 && seen.creates == 2 && seen.jitter_x == -.3125f &&
           seen.jitter_y == .2777778f);
    assert(seen.callback == reinterpret_cast<std::uintptr_t>(&progress) &&
           seen.context == reinterpret_cast<std::uintptr_t>(context.Get()));
    assert((measured_calls == 1 && measured == std::array<unsigned, 4>{2560, 1440, 3840, 2160}));
    assert(reported.evaluations == 1 && reported.jitter_phases == 18);
    assert(reported.last_evaluation_tick && GetTickCount64() - reported.last_evaluation_tick < 2000);
    assert(reported.evaluated_resolution && reported.render_width == 2560 && reported.render_height == 1440 &&
           reported.output_width == 3840 && reported.output_height == 2160 &&
           reported.resolution_source == "evaluation subrect");
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 1920u);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 1080u);
    requested.dlss.model_preset = xivrender::DlssModelPreset::M;
    assert(evaluate(context.Get(), world, &p, nullptr) == NVSDK_NGX_Result_Success);
    snapshot(&seen);
    assert(seen.creates == 2 && seen.evaluations == 2 && seen.live_handles == 2);
    assert(reported.render_width == 1920 && reported.render_height == 1080 && reported.jitter_phases == 32);
    originals_restored(p);
    // Disabled controls preserve hints on subsequent native creation as well.
    requested.dlss.enabled = false;
    NVSDK_NGX_Handle *native = nullptr;
    assert(create(context.Get(), NVSDK_NGX_Feature_SuperSampling, &p, &native) == NVSDK_NGX_Result_Success);
    snapshot(&seen);
    for (unsigned i = 0; i < keys.size(); ++i)
        assert(seen.presets[i] == 20u + i);
    originals_restored(p);
    const auto failures = reported.failures;
    results(unsigned(NVSDK_NGX_Result_Success), unsigned(NVSDK_NGX_Result_FAIL_PlatformError),
            unsigned(NVSDK_NGX_Result_Success));
    assert(evaluate(context.Get(), world, &p, nullptr) == NVSDK_NGX_Result_FAIL_PlatformError);
    assert(reported.failures == failures + 1);
    results(unsigned(NVSDK_NGX_Result_FAIL_OutOfGPUMemory), unsigned(NVSDK_NGX_Result_Success),
            unsigned(NVSDK_NGX_Result_Success));
    requested.dlss.enabled = true; // temporary preset hints must restore even on SDK failure
    NVSDK_NGX_Handle *failed = nullptr;
    assert(create(context.Get(), NVSDK_NGX_Feature_SuperSampling, &p, &failed) ==
               NVSDK_NGX_Result_FAIL_OutOfGPUMemory &&
           !failed);
    originals_restored(p);
    assert(reported.failures == failures + 2);
    results(unsigned(NVSDK_NGX_Result_Success), unsigned(NVSDK_NGX_Result_Success),
            unsigned(NVSDK_NGX_Result_Success));
    Parameters ultra;
    dimensions(ultra, 2956, 1662, 3840, 2160);
    ultra.Set(NVSDK_NGX_Parameter_PerfQualityValue, 4);
    NVSDK_NGX_Handle *ultra_handle = nullptr;
    assert(create(context.Get(), NVSDK_NGX_Feature_SuperSampling, &ultra, &ultra_handle) ==
           NVSDK_NGX_Result_Success);
    snapshot(&seen);
    assert(seen.create_quality == 2);
    int restored_quality = -1;
    ultra.Get(NVSDK_NGX_Parameter_PerfQualityValue, &restored_quality);
    assert(restored_quality == 4);
    assert(release(ultra_handle) == NVSDK_NGX_Result_Success);
    snapshot(&seen);
    const auto releases = seen.releases;
    assert(release(world) == NVSDK_NGX_Result_Success);
    assert(release(preview) == NVSDK_NGX_Result_Success);
    assert(release(native) == NVSDK_NGX_Result_Success);
    snapshot(&seen);
    assert(seen.releases == releases + 3 && seen.live_handles == 0);
    // HDR changes the native feature's color domain once, while evaluation
    // preserves the game's reusable parameter block and native jitter.
    reported.display_width = 640;
    reported.display_height = 360;
    requested.hdr.enabled = true;
    xivrender::hdr::configure(requested, true);
    Parameters hdr_parameters;
    dimensions(hdr_parameters, 320, 180, 640, 360);
    hdr_parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 10u);
    unsigned native_flags = 0;
    hdr_parameters.Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &native_flags);
    NVSDK_NGX_Handle *hdr_handle = nullptr;
    assert(create(context.Get(), NVSDK_NGX_Feature_SuperSampling, &hdr_parameters, &hdr_handle) ==
           NVSDK_NGX_Result_Success);
    snapshot(&seen);
    assert(seen.create_flags == (native_flags | NVSDK_NGX_DLSS_Feature_Flags_IsHDR));
    unsigned restored_flags = 999;
    hdr_parameters.Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &restored_flags);
    assert(restored_flags == native_flags);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 320;
    td.Height = 180;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> native_color, native_output;
    assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &native_color)));
    td.Width = 640;
    td.Height = 360;
    assert(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &native_output)));
    hdr_parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource *>(native_color.Get()));
    hdr_parameters.Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource *>(native_output.Get()));
    hdr_parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, -.3125f);
    hdr_parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, .2777778f);
    const auto before_hdr_eval = seen.evaluations;
    assert(evaluate(context.Get(), hdr_handle, &hdr_parameters, nullptr) == NVSDK_NGX_Result_Success);
    snapshot(&seen);
    assert(seen.evaluations == before_hdr_eval + 1);
    assert(seen.color && seen.color != reinterpret_cast<std::uintptr_t>(native_color.Get()));
    assert(seen.output && seen.output != reinterpret_cast<std::uintptr_t>(native_output.Get()));
    assert(seen.jitter_x == -.3125f && seen.jitter_y == .2777778f);
    ID3D11Resource *preserved = nullptr;
    hdr_parameters.Get(NVSDK_NGX_Parameter_Color, &preserved);
    assert(preserved == native_color.Get());
    hdr_parameters.Get(NVSDK_NGX_Parameter_Output, &preserved);
    assert(preserved == native_output.Get());
    assert(release(hdr_handle) == NVSDK_NGX_Result_Success);
    xivrender::native_dlss::shutdown();
    xivrender::hdr::shutdown();
    assert(!xivrender::native_dlss::active());
    // Shutdown removes only these hooks; MinHook remains owned by this host.
    assert(MH_Initialize() == MH_ERROR_ALREADY_INITIALIZED);
    assert(MH_Uninitialize() == MH_OK);
    FreeLibrary(module);
    FreeLibrary(helper);
    std::filesystem::remove(private_path);
    std::filesystem::remove(private_folder);
    puts("PASS: actual native NGX hooks preserve engine ownership, main-only hints/restoration, raster "
         "jitter, live requests, failures and exactly-once release");
}
