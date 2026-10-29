#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <MinHook.h>
#include "xivrender/app.hpp"
#include "xivrender/native_nr.hpp"
#include "xivrender/hdr.hpp"
#include "xivrender/frame_generation.hpp"
#include "xivrender/fg_diagnostics.hpp"
#include "xivrender/overlay.hpp"
#include "xivrender/native_dlss.hpp"
#include "xivrender/ffxiv_signatures.hpp"
#include "xivrender/ffxiv_hooks.hpp"
#include "xivrender/runtime_bundle.hpp"
#include "xivrender/depth.hpp"
#include "xivrender/mip_bias.hpp"
#include "xivrender/trace.hpp"
#include <windows.h>
#include <psapi.h>
#include <wrl/client.h>
#include <mutex>
#include <fstream>
#include <vector>
#include <atomic>
#include <filesystem>
namespace xivrender {
namespace {
struct Application {
    std::mutex mutex, log_mutex;
    Settings settings;
    Status status;
    std::filesystem::path root, config;
    std::atomic<bool> rescan{false}, stopping{false};
    std::atomic<std::uint64_t> epoch{1};
    std::ofstream logfile;
    ULONGLONG heartbeat = 0;
    bool native_allowed = false;
    HWND main_window = nullptr;
    reshade::api::swapchain *main_chain = nullptr;
};
Application &app() {
    static auto *value = new Application;
    return *value;
}
std::filesystem::path module_path(HMODULE module) {
    std::wstring result(32768, L'\0');
    const auto n = GetModuleFileNameW(module, result.data(), DWORD(result.size()));
    if (!n || n >= result.size())
        return {};
    result.resize(n);
    return result;
}
bool conflicting_addon() {
    HMODULE modules[1024]{};
    DWORD size = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &size))
        return true;
    for (unsigned i = 0; i < std::min<unsigned>(size / sizeof(HMODULE), 1024); ++i) {
        if (GetProcAddress(modules[i], "OptiScaler_ExternalHudlessVersion") ||
            GetProcAddress(modules[i], "OptiScaler_GetHudlessAPI"))
            return true;
    }
    return false;
}
void discover() {
    auto &a = app();
    if (a.stopping)
        return;
    const auto exe = module_path(nullptr);
    std::ifstream input(exe, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    const auto report = ffxiv::inspect_executable(bytes);
    auto status = status_snapshot();
    status.signatures = report.quality && report.jitter ? "Quality and jitter signatures verified"
                                                        : "Required native signatures are unavailable";
    for (const auto &line : report.evidence)
        log_message(line);
    for (const auto &line : report.errors)
        log_message(line);
    const auto bundle = runtime::InspectBundle(a.root / L"XIVRender" / L"runtime");
    status.runtime = bundle.summary;
    // Manifest presence does not activate Streamline. Its owned presentation
    // bridge is a separate milestone and cannot be inferred from DLL versions.
    status.phase = "Native DLSS adapter";
    if (conflicting_addon()) {
        status.conflict = true;
        status.message = "Another upscaler controller is active; restart with one controller";
        a.native_allowed = false;
    } else if (report.quality && report.jitter) {
        std::string error;
        if (ffxiv::install_hooks(report, bytes, error)) {
            a.native_allowed = true;
            status.native_quality = true;
            status.message = "FFXIV native quality and jitter adapter ready";
        } else {
            a.native_allowed = false;
            status.conflict = true;
            status.message = error;
        }
    } else {
        a.native_allowed = false;
        status.message = "Native DLSS retained; addon controls disabled for this executable";
    }
    update_status(status);
    log_message(status.message);
}
void on_pipeline(reshade::api::device *device, reshade::api::pipeline_layout, uint32_t count,
                 const reshade::api::pipeline_subobject *objects, reshade::api::pipeline pipeline) {
    if (!app().native_allowed || device->get_api() != reshade::api::device_api::d3d11)
        return;
    for (unsigned i = 0; i < count; ++i)
        if (objects[i].type == reshade::api::pipeline_subobject_type::pixel_shader && objects[i].count) {
            const auto &shader = *static_cast<const reshade::api::shader_desc *>(objects[i].data);
            hdr::tag(reinterpret_cast<ID3D11PixelShader *>(pipeline.handle), shader.code, shader.code_size);
        }
}
void on_command_list(reshade::api::command_list *list) {
    if (app().stopping || !app().native_allowed ||
        list->get_device()->get_api() != reshade::api::device_api::d3d11)
        return;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    auto *native = reinterpret_cast<IUnknown *>(list->get_native());
    if (native && SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&context)))) {
        std::string error;
        if (!hdr::attach(context.Get(), error) && error != "HDR context belongs to another native device")
            log_message(error);
    }
}
void on_device(reshade::api::device *device) {
    if (app().stopping || !app().native_allowed || device->get_api() != reshade::api::device_api::d3d11)
        return;
    native_dlss::poll();
}
void on_swapchain(reshade::api::swapchain *chain, bool) {
    if (chain->get_device()->get_api() != reshade::api::device_api::d3d11)
        return;
    auto &a = app();
    const auto window = static_cast<HWND>(chain->get_hwnd());
    DWORD pid = 0;
    if (a.main_window && !IsWindow(a.main_window))
        a.main_window = nullptr;
    RECT client{};
    if (!GetClientRect(window, &client) || client.right - client.left < 640 ||
        client.bottom - client.top < 360)
        return;
    if (!window || !IsWindow(window) || !GetWindowThreadProcessId(window, &pid) ||
        pid != GetCurrentProcessId() || (a.main_window && a.main_window != window))
        return;
    a.main_window = window;
    a.main_chain = chain;
    if (a.native_allowed) {
        auto *device = reinterpret_cast<ID3D11Device *>(chain->get_device()->get_native());
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        std::string error;
        if (!hdr::attach(context.Get(), error) && error != "HDR context belongs to another native device")
            log_message(error);
    }
    if (a.native_allowed)
        native_nr::attach_device(reinterpret_cast<ID3D11Device *>(chain->get_device()->get_native()));
    if (a.native_allowed && !mip::is_attached())
        native_dlss::attach_device(reinterpret_cast<ID3D11Device *>(chain->get_device()->get_native()));
    const auto desc = chain->get_device()->get_resource_desc(chain->get_back_buffer(0));
    modify_status([&](Status &status) {
        status.display_width = desc.texture.width;
        status.display_height = desc.texture.height;
    });
}
void log_summary(const char *reason) {
    const auto s = status_snapshot();
    const auto nr = native_nr::snapshot();
    log_message(std::string("Session summary (") + reason + "): creates=" + std::to_string(s.creates) +
                ", evaluations=" + std::to_string(s.evaluations) +
                ", failures=" + std::to_string(s.failures) + ", presents=" + std::to_string(s.real_presents) +
                ", input=" + std::to_string(s.render_width) + "x" + std::to_string(s.render_height) +
                ", output=" + std::to_string(s.output_width) + "x" + std::to_string(s.output_height) +
                ", target jitter phases=" + std::to_string(s.jitter_phases) +
                ", mip bias=" + std::to_string(s.mip_bias) +
                ", sampler variants=" + std::to_string(mip::sampler_creations()) +
                ", depth=" + depth::status() + ", NR runtime=" + nr.active_runtime +
                ", NR applied=" + std::to_string(nr.applied) + ", NR skipped=" + std::to_string(nr.skipped) +
                ", NR paced=" + std::to_string(nr.paced) + ", NR failures=" + std::to_string(nr.failures));
    if (settings_snapshot().hdr.enabled)
        log_message("HDR summary: " + hdr::status());
    const auto fg_status = fg::status();
    if (fg::output_requested())
        log_message("FG summary: " + fg_status.message + ", active=" + std::to_string(fg_status.active) +
                    ", factor=" + std::to_string(fg_status.factor) +
                    ", real=" + std::to_string(fg_status.real_frames) + ", displayed=" +
                    std::to_string(fg_status.displayed) + ", failures=" + std::to_string(fg_status.failures) +
                    ", latency=" + fg_status.latency_name +
                    ", native dynamic=" + std::to_string(fg_status.native_dynamic) +
                    ", provider=" + std::to_string(unsigned(fg_status.provider)) +
                    ", live changes=" + std::to_string(fg_status.provider_changes) +
                    ", transition=" + fg_status.transition + ", last error=" + fg_status.last_error);
    if (fg::output_requested()) {
        const auto memory = fg::memory_usage();
        log_message("FG memory: gpu_available=" + std::to_string(memory.gpu_available) +
                    ", local_bytes=" + std::to_string(memory.local_bytes) +
                    ", local_budget=" + std::to_string(memory.local_budget) +
                    ", nonlocal_bytes=" + std::to_string(memory.nonlocal_bytes) +
                    ", nonlocal_budget=" + std::to_string(memory.nonlocal_budget) +
                    ", process_available=" + std::to_string(memory.process_available) +
                    ", process_private=" + std::to_string(memory.process_private) + ", working_set=" +
                    std::to_string(memory.working_set) + ", handles=" + std::to_string(memory.handles));
        const auto timing = fg::take_performance();
        constexpr const char *names[]{"output preparation", "capture lock",   "present lock",
                                      "transport",          "SDK Present",    "input retirement",
                                      "simulation sleep",   "native SR lock", "native SR evaluate",
                                      "NR transaction"};
        for (unsigned index = 0; index < timing.stages.size(); ++index) {
            const auto &stage = timing.stages[index];
            if (stage.samples)
                log_message(std::string("FG CPU (") + names[index] +
                            "): samples=" + std::to_string(stage.samples) +
                            ", mean_ms=" + std::to_string(stage.total_ms / double(stage.samples)) +
                            ", maximum_ms=" + std::to_string(stage.maximum_ms) +
                            ", over_100_ms=" + std::to_string(stage.over_100_ms));
        }
        log_message(
            "FG application latency queries: set_max=" + std::to_string(timing.presentation_queries[0]) +
            ", get_max=" + std::to_string(timing.presentation_queries[1]) +
            ", waitable_handle=" + std::to_string(timing.presentation_queries[2]) +
            ", frame_statistics=" + std::to_string(timing.presentation_queries[3]) +
            ", present_count=" + std::to_string(timing.presentation_queries[4]));
        const auto &requests = timing.present_requests;
        if (requests.copy_ready + requests.copy_pending)
            log_message("FG Present requests: interval_0=" + std::to_string(requests.intervals[0]) +
                        ", interval_1=" + std::to_string(requests.intervals[1]) +
                        ", interval_2=" + std::to_string(requests.intervals[2]) +
                        ", interval_3=" + std::to_string(requests.intervals[3]) +
                        ", interval_4=" + std::to_string(requests.intervals[4]) +
                        ", interval_other=" + std::to_string(requests.intervals[5]) +
                        ", flags_or=" + std::to_string(requests.flags_any) +
                        ", flags_and=" + std::to_string(requests.flags_all) +
                        ", copy_ready_at_entry=" + std::to_string(requests.copy_ready) +
                        ", copy_pending_at_entry=" + std::to_string(requests.copy_pending) +
                        ", slow_threshold_ms=" + std::to_string(fg::PresentRequests::slow_threshold_ms) +
                        ", slow=" + std::to_string(requests.slow) +
                        ", slow_with_copy_ready=" + std::to_string(requests.slow_copy_ready));
        if (fg_status.rate_measured)
            log_message("FG observed rate: ratio=" + std::to_string(fg_status.observed_ratio) +
                        ", real_fps=" + std::to_string(fg_status.real_fps) +
                        ", displayed_fps=" + std::to_string(fg_status.displayed_fps));
    }
    if (fg::output_requested())
        log_message("FG input summary: " + fg_status.input_status +
                    ", simulation=" + std::to_string(fg_status.simulation_frames) +
                    ", cameras=" + std::to_string(fg_status.camera_samples) +
                    ", valid cameras=" + std::to_string(fg_status.valid_cameras) +
                    ", captures=" + std::to_string(fg_status.captured_frames) +
                    ", missing=" + std::to_string(fg_status.missing_cameras) +
                    ", invalid=" + std::to_string(fg_status.invalid_cameras) +
                    ", overflow=" + std::to_string(fg_status.camera_overflows) +
                    ", stale=" + std::to_string(fg_status.stale_inputs) +
                    ", overwritten=" + std::to_string(fg_status.overwritten_inputs) +
                    ", resets=" + std::to_string(fg_status.history_resets) +
                    ", age=" + std::to_string(fg_status.input_age));
}
void destroy_swapchain(reshade::api::swapchain *chain, bool resize) {
    auto &a = app();
    if (chain != a.main_chain)
        return;
    fg::diagnostics::Scope phase(resize ? "reshade.resize" : "shutdown.reshade_swapchain");
    ffxiv::finish_frame();
    log_summary(resize ? "swapchain resize" : "main swapchain destroyed");
    if (!resize) {
        // Release our retained immediate context before ReShade releases its
        // last device/manager reference. Otherwise unload can be held in a cycle.
        a.main_chain = nullptr;
        {
            fg::diagnostics::Scope phase("shutdown.native_nr");
            native_nr::shutdown();
        }
        mip::detach();
        depth::reset();
    }
}
void on_present(reshade::api::command_queue *, reshade::api::swapchain *chain, const reshade::api::rect *,
                const reshade::api::rect *, uint32_t, const reshade::api::rect *) {
    auto &a = app();
    if (a.stopping)
        return;
    on_swapchain(chain, false);
    if (a.main_chain != chain)
        return;
    fg::diagnostics::Scope phase("reshade.present_callback");
    ffxiv::finish_frame();
    // Redetection does not replace installed hooks while the renderer is live.
    native_nr::service();
    if (a.rescan.exchange(false)) {
        const auto bundle = runtime::InspectBundle(a.root / L"XIVRender" / L"runtime");
        modify_status([&](Status &status) {
            status.runtime = bundle.summary + "; file check complete (no runtime reload)";
        });
        log_message("Runtime file check complete; no DLL reload, quality request or hook change");
    }
    if (a.native_allowed)
        native_dlss::poll();
    modify_status([](Status &status) { ++status.real_presents; });
    const auto now = GetTickCount64();
    if (now - a.heartbeat >= 15000) {
        a.heartbeat = now;
        log_summary("periodic");
    }
}
void begin_effects(reshade::api::effect_runtime *r, reshade::api::command_list *c,
                   reshade::api::resource_view v, reshade::api::resource_view s) {
    mip::update(false, 0, 0);
    if (hdr::active() || fg::output_requested()) {
        const auto resource = r->get_device()->get_resource_from_view(v);
        auto *texture = reinterpret_cast<ID3D11Texture2D *>(resource.handle);
        if (hdr::active())
            hdr::begin_effects(texture);
        fg::before_effects(texture);
    }
    depth::begin_effects(r, c, v, s);
    fg::diagnostics::event("reshade.effects.begin");
}
void end_effects(reshade::api::effect_runtime *r, reshade::api::command_list *c,
                 reshade::api::resource_view v, reshade::api::resource_view s) {
    fg::diagnostics::event("reshade.effects.end");
    fg::diagnostics::SlowScope phase("reshade.effects_restore");
    depth::end_effects(r, c, v, s);
    const auto cfg = settings_snapshot();
    const auto state = status_snapshot();
    mip::update(app().native_allowed && cfg.dlss.enabled && cfg.dlss.automatic_mip_bias, state.render_width,
                state.display_width);
}
} // namespace
Settings settings_snapshot() {
    std::lock_guard lock(app().mutex);
    return app().settings;
}
void request_settings(const Settings &desired) {
    auto normalized = desired;
    normalize_settings(normalized);
    Settings previous;
    {
        std::lock_guard lock(app().mutex);
        previous = app().settings;
        app().settings = normalized;
    }
    hdr::configure(normalized);
    fg::configure(normalized);
    if (previous.dlss != normalized.dlss) {
        ffxiv::finish_frame();
        const auto epoch = ++app().epoch;
        log_message("DLSS request#" + std::to_string(epoch) +
                    ": enabled=" + std::to_string(normalized.dlss.enabled) +
                    ", quality=" + quality_name(normalized.dlss.quality) +
                    ", model=" + model_name(normalized.dlss.model_preset) +
                    ", automatic mip=" + std::to_string(normalized.dlss.automatic_mip_bias));
        ffxiv::settings_changed();
    }
    if (previous.nr != normalized.nr)
        log_message("NR request: enabled=" + std::to_string(normalized.nr.enabled) + ", runtime=" +
                    normalized.nr.runtime_id + ", model_scale=" + std::to_string(normalized.nr.model_scale) +
                    ", passes=" + std::to_string(normalized.nr.passes) +
                    ", profile=" + std::to_string(normalized.nr.profile.style) +
                    ", feedback=" + std::to_string(normalized.nr.feedback) +
                    ", foveated=" + std::to_string(normalized.nr.foveated) +
                    ", pause_unfocused=" + std::to_string(normalized.nr.pause_on_focus_loss));
    if (previous.nr.enabled != normalized.nr.enabled)
        native_nr::enabled_changed(normalized.nr.enabled);
}
std::uint64_t trace_epoch() {
    return app().epoch.load(std::memory_order_acquire);
}
Status status_snapshot() {
    std::lock_guard lock(app().mutex);
    return app().status;
}
void update_status(const Status &value) {
    std::lock_guard lock(app().mutex);
    app().status = value;
}
Status modify_status(const std::function<void(Status &)> &change) {
    std::lock_guard lock(app().mutex);
    change(app().status);
    return app().status;
}
void log_message(const std::string &text) {
    fg::diagnostics::event("application", 0, 0, 0, 0, 0, text.c_str());
    reshade::log::message(reshade::log::level::info, ("XIVRender: " + text).c_str());
    if (!app().root.empty()) {
        std::lock_guard lock(app().log_mutex);
        if (!app().logfile.is_open())
            app().logfile.open(app().root / L"XIVRender.log", std::ios::app);
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        app().logfile << GetTickCount64() << " qpc=" << qpc.QuadPart << " tid=" << GetCurrentThreadId() << " "
                      << text << '\n';
        app().logfile.flush();
    }
}
bool save_nr_slot(unsigned slot, const NeuralRenderingSettings &nr, std::string &error) {
    if (slot < 1 || slot > 8) {
        error = "Invalid NR profile slot";
        return false;
    }
    return save_nr_profile(app().root / L"XIVRender/profiles" / ("NR-" + std::to_string(slot) + ".ini"), nr,
                           error);
}
bool load_nr_slot(unsigned slot, NeuralRenderingSettings &nr, std::string &error) {
    if (slot < 1 || slot > 8) {
        error = "Invalid NR profile slot";
        return false;
    }
    return load_nr_profile(app().root / L"XIVRender/profiles" / ("NR-" + std::to_string(slot) + ".ini"), nr,
                           error);
}
bool save_requested_settings(std::string &error) {
    return save_settings(app().config, settings_snapshot(), error);
}
void request_rescan() {
    app().rescan = true;
}
} // namespace xivrender
extern "C" __declspec(dllexport) const char *NAME = "XIVRender";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "FFXIV rendering addon: native DLSS controls and normalized ReShade depth (development build)";
extern "C" __declspec(dllexport) BOOL AddonInit(HMODULE module, HMODULE reshade_module) {
    using namespace xivrender;
    if (!reshade::register_addon(module, reshade_module))
        return FALSE;
    const auto exe = module_path(nullptr);
    if (_wcsicmp(exe.filename().c_str(), L"ffxiv_dx11.exe") != 0) {
        log_message("Unsupported executable; no hooks installed");
        reshade::unregister_addon(module);
        return FALSE;
    }
    app().root = module_path(module).parent_path();
    app().config = app().root / L"XIVRender.ini";
    native_nr::set_root(app().root);
    auto loaded = load_settings(app().config);
    app().settings = loaded.settings;
    log_message("0.4.16: FSR 3X/4X through DLSS Enabler with asynchronous presentation; "
                "0.4.15 XeFG timing retained; MFG gameplay validation pending");
    for (const auto &warning : loaded.warnings)
        log_message(warning);
    if (MH_Initialize() != MH_OK) {
        log_message("Hook initialization failed; no rendering changes applied");
        reshade::unregister_addon(module);
        return FALSE;
    }
    discover();
    hdr::configure(app().settings, true);
    fg::configure(app().settings, app().root / L"XIVRender" / L"runtime", true);
    if (hdr::active() && !app().native_allowed)
        hdr::disable("HDR unavailable: native FFXIV validation failed");
    if (app().native_allowed) {
        std::string error;
        if (!hdr::install_output(error)) {
            hdr::disable(error.c_str());
            log_message(error);
        }
    }
    reshade::register_event<reshade::addon_event::init_pipeline>(&on_pipeline);
    reshade::register_event<reshade::addon_event::init_command_list>(&on_command_list);
    reshade::register_event<reshade::addon_event::init_device>(&on_device);
    reshade::register_event<reshade::addon_event::init_swapchain>(&on_swapchain);
    reshade::register_event<reshade::addon_event::destroy_swapchain>(&destroy_swapchain);
    reshade::register_event<reshade::addon_event::present>(&on_present);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(&begin_effects);
    reshade::register_event<reshade::addon_event::reshade_finish_effects>(&end_effects);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&depth::forget_runtime);
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&depth::forget_runtime);
    reshade::register_overlay("XIVRender", &draw_overlay);
    return TRUE;
}
extern "C" __declspec(dllexport) void AddonUninit(HMODULE module, HMODULE) {
    using namespace xivrender;
    fg::diagnostics::event("addon.shutdown.begin");
    app().stopping = true;
    log_summary("addon uninit");
    reshade::unregister_overlay("XIVRender", &draw_overlay);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(&on_pipeline);
    reshade::unregister_event<reshade::addon_event::init_command_list>(&on_command_list);
    reshade::unregister_event<reshade::addon_event::init_device>(&on_device);
    reshade::unregister_event<reshade::addon_event::init_swapchain>(&on_swapchain);
    reshade::unregister_event<reshade::addon_event::destroy_swapchain>(&destroy_swapchain);
    reshade::unregister_event<reshade::addon_event::present>(&on_present);
    reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(&begin_effects);
    reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(&end_effects);
    reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&depth::forget_runtime);
    reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(&depth::forget_runtime);
    ffxiv::remove_hooks();
    native_dlss::shutdown();
    hdr::shutdown();
    hdr::remove_output();
    // Do not tear down another addon's hook library or free busy trampolines.
    reshade::unregister_addon(module);
    fg::diagnostics::event("addon.shutdown.end");
    fg::diagnostics::shutdown();
}
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}
