// Test-only ReShade addon. Never included in the production install whitelist.
#include <windows.h>
#include <unknwn.h>
#include <reshade.hpp>

namespace {
void init_device(reshade::api::device *device) {
    if (device->get_api() == reshade::api::device_api::d3d11)
        reshade::log::message(reshade::log::level::info, "NR device observer: D3D11 game device initialized");
    if (device->get_api() == reshade::api::device_api::d3d12)
        reshade::log::message(reshade::log::level::error, "NR device observer: private D3D12 device exposed");
}
void init_effect_runtime(reshade::api::effect_runtime *) {
    reshade::log::message(reshade::log::level::info, "HDR observer: SDR effect runtime initialized");
}
void init_command_list(reshade::api::command_list *list) {
    if (list->get_device()->get_api() != reshade::api::device_api::d3d11)
        return;
    using Attach = BOOL (*)(IUnknown *);
    const auto attach =
        reinterpret_cast<Attach>(GetProcAddress(GetModuleHandleW(nullptr), "XIVRenderProbeAttachHdrContext"));
    if (attach && attach(reinterpret_cast<IUnknown *>(list->get_native())))
        reshade::log::message(reshade::log::level::info, "HDR observer: native context event attached");
}
void destroy_device(reshade::api::device *device) {
    if (device->get_api() == reshade::api::device_api::d3d12)
        reshade::log::message(reshade::log::level::error,
                              "NR device observer: private D3D12 device destroyed");
}
} // namespace
extern "C" __declspec(dllexport) const char *NAME = "NR Device Observer (test only)";
extern "C" __declspec(dllexport) BOOL AddonInit(HMODULE module, HMODULE reshade_module) {
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, 32768))
        return FALSE;
    const auto slash = wcsrchr(path, L'\\');
    if (_wcsicmp(slash ? slash + 1 : path, L"nr_pipeline_probe.exe") != 0 &&
        _wcsicmp(slash ? slash + 1 : path, L"hdr_output_probe.exe") != 0 &&
        _wcsicmp(slash ? slash + 1 : path, L"output_switch_probe.exe") != 0 &&
        _wcsicmp(slash ? slash + 1 : path, L"fg_probe.exe") != 0 &&
        _wcsicmp(slash ? slash + 1 : path, L"fg_desktop_probe.exe") != 0)
        return FALSE;
    if (!reshade::register_addon(module, reshade_module))
        return FALSE;
    reshade::register_event<reshade::addon_event::init_device>(&init_device);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(&init_effect_runtime);
    reshade::register_event<reshade::addon_event::init_command_list>(&init_command_list);
    reshade::register_event<reshade::addon_event::destroy_device>(&destroy_device);
    return TRUE;
}
extern "C" __declspec(dllexport) void AddonUninit(HMODULE module, HMODULE) {
    reshade::unregister_event<reshade::addon_event::init_device>(&init_device);
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&init_effect_runtime);
    reshade::unregister_event<reshade::addon_event::init_command_list>(&init_command_list);
    reshade::unregister_event<reshade::addon_event::destroy_device>(&destroy_device);
    reshade::unregister_addon(module);
}
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}
