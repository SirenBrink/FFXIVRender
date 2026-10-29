// Isolated native model capability probe. No feature, swapchain or game hooks.
#include "xivrender/ngx_parameters.hpp"
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
using Microsoft::WRL::ComPtr;
extern "C" __declspec(dllexport) int ProbeOptimal(const wchar_t *model, int(__cdecl *install)()) {
    const auto library = LoadLibraryExW(model, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) return 2;
    using Init = unsigned(__cdecl *)(unsigned long long, const wchar_t *, ID3D11Device *, unsigned, const void *);
    using Populate = unsigned(__cdecl *)(NVSDK_NGX_Parameter *);
    using Shutdown = unsigned(__cdecl *)(ID3D11Device *);
    using Callback = unsigned(__cdecl *)(NVSDK_NGX_Parameter *);
    const auto init = reinterpret_cast<Init>(GetProcAddress(library, "NVSDK_NGX_D3D11_Init_Ext"));
    const auto populate = reinterpret_cast<Populate>(GetProcAddress(library, "NVSDK_NGX_D3D11_PopulateParameters_Impl"));
    const auto shutdown = reinterpret_cast<Shutdown>(GetProcAddress(library, "NVSDK_NGX_D3D11_Shutdown1"));
    if (!init || !populate || !shutdown) return 3;
    ComPtr<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr))) return 4;
    const auto data = std::filesystem::absolute(L"build/sr-capability-data");
    std::filesystem::create_directories(data);
    volatile auto result = init(0, data.c_str(), device.Get(), 0x15, nullptr);
    std::printf("Direct SR Init: 0x%x\n", result);
    if (result != 1) return 5;
    if (install && install() != 1) { shutdown(device.Get()); return 7; }
    xivrender::ngx::Parameters parameters;
    result = populate(&parameters);
    void *callback = nullptr;
    parameters.Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
    std::printf("Populate: 0x%x, optimal callback=%p\n", result, callback);
    if (result == 1 && callback) {
        for (int quality = 0; quality <= 5; ++quality) {
            parameters.Set(NVSDK_NGX_Parameter_Width, 3840u);
            parameters.Set(NVSDK_NGX_Parameter_Height, 2160u);
            parameters.Set(NVSDK_NGX_Parameter_PerfQualityValue, quality);
            parameters.Set(NVSDK_NGX_Parameter_RTXValue, 0);
            result = reinterpret_cast<Callback>(callback)(&parameters);
            unsigned width = 0, height = 0;
            parameters.Get(NVSDK_NGX_Parameter_OutWidth, &width);
            parameters.Get(NVSDK_NGX_Parameter_OutHeight, &height);
            std::printf("NGX quality=%d result=0x%x optimal=%ux%u\n", quality, result, width, height);
        }
    }
    volatile auto stopped = shutdown(device.Get());
    std::printf("Direct SR shutdown: 0x%x\n", stopped);
    device.Reset(); FreeLibrary(library);
    return stopped == 1 ? 0 : 6;
}
