#include <nvsdk_ngx_defs.h>
#include <d3d12.h>
#include <d3d11.h>
namespace {
NVSDK_NGX_AppLogCallback cached = nullptr;
}
extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init_ProjectID(
    const char *, NVSDK_NGX_EngineType, const char *, const wchar_t *, ID3D12Device *,
    NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *info) {
    // Driver/model caches can invoke the previous callback during the next
    // initialization, before the new callback is installed.
    if (cached) cached("cached callback during reinitialization", NVSDK_NGX_LOGGING_LEVEL_ON, NVSDK_NGX_Feature_FrameGeneration);
    if (info) cached = info->LoggingInfo.LoggingCallback;
    return NVSDK_NGX_Result_Success;
}
extern "C" __declspec(dllexport) void Emit() {
    if (cached) cached("cached callback after SDK owner retired", NVSDK_NGX_LOGGING_LEVEL_ON, NVSDK_NGX_Feature_FrameGeneration);
}
extern "C" __declspec(dllexport) void Reset() { cached = nullptr; }
namespace { unsigned global_shutdowns = 0, device_shutdowns = 0; bool shutdown_fail = false; }
extern "C" __declspec(dllexport) unsigned __cdecl NVSDK_NGX_D3D11_Shutdown() {
    ++global_shutdowns; return 1;
}
extern "C" __declspec(dllexport) unsigned __cdecl NVSDK_NGX_D3D11_Shutdown1(ID3D11Device *device, unsigned *remaining) {
    if (!device) ++global_shutdowns;
    else ++device_shutdowns;
    if (remaining) *remaining = 7;
    return shutdown_fail ? unsigned(NVSDK_NGX_Result_FAIL_PlatformError) : 1;
}
extern "C" __declspec(dllexport) unsigned __cdecl ShutdownCount(unsigned which) {
    return which ? device_shutdowns : global_shutdowns;
}
extern "C" __declspec(dllexport) void __cdecl ShutdownFail(bool value) { shutdown_fail = value; }
