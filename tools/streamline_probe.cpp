// Isolated runtime startup probe. Never injects or presents into the game.
#include "xivrender/runtime_bundle.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <iostream>
#include <filesystem>
#include <vector>
#include <fstream>
using Microsoft::WRL::ComPtr;
void message(sl::LogType, const char *text) {
    std::cout << "SL: " << (text ? text : "") << '\n';
}
int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc < 2) {
        std::cerr << "Usage: streamline_probe <absolute runtime directory> [--nr]\n";
        return 2;
    }
    const auto root = std::filesystem::absolute(argv[1]);
    const bool nr = argc > 2 && std::wstring(argv[2]) == L"--nr";
    const auto bundle = xivrender::runtime::InspectBundle(root);
    std::cout << bundle.summary << '\n';
    for (const auto &module : bundle.modules)
        if (module.exists)
            std::cout << module.name << " " << module.version.ToString() << '\n';
    if (!bundle.core_coherent) {
        std::cerr << "Core bundle is not coherent; no DLLs loaded\n";
        return 3;
    }
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return 4;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND)
            break;
        if (candidate && SUCCEEDED(candidate->GetDesc1(&desc)) &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId == 0x10de) {
            adapter = candidate;
            break;
        }
    }
    if (!adapter) {
        std::cerr << "No NVIDIA hardware adapter available for startup probe\n";
        return 5;
    }
    std::wcout << L"Adapter: " << desc.Description << L'\n';
    const auto library = LoadLibraryExW((root / L"sl.interposer.dll").c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) {
        std::cerr << "Interposer load error " << GetLastError() << '\n';
        return 6;
    }
    auto init = reinterpret_cast<PFun_slInit *>(GetProcAddress(library, "slInit"));
    auto shutdown = reinterpret_cast<PFun_slShutdown *>(GetProcAddress(library, "slShutdown"));
    auto bind = reinterpret_cast<PFun_slSetD3DDevice *>(GetProcAddress(library, "slSetD3DDevice"));
    auto supported =
        reinterpret_cast<PFun_slIsFeatureSupported *>(GetProcAddress(library, "slIsFeatureSupported"));
    auto get = reinterpret_cast<PFun_slGetFeatureFunction *>(GetProcAddress(library, "slGetFeatureFunction"));
    if (!init || !shutdown || !bind || !supported || !get) {
        FreeLibrary(library);
        return 7;
    }
    std::vector<sl::Feature> features{sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    if (nr)
        features.push_back(sl::kFeatureDLSS_NR);
    const auto path = root.wstring();
    const wchar_t *paths[]{path.c_str()};
    sl::Preferences preferences{};
    preferences.pathsToPlugins = paths;
    preferences.numPathsToPlugins = 1;
    preferences.pathToLogsAndData = path.c_str();
    preferences.logMessageCallback = message;
    preferences.featuresToLoad = features.data();
    preferences.numFeaturesToLoad = static_cast<uint32_t>(features.size());
    preferences.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eDisableCLStateTracking;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "XIVRender-0.1.0";
    preferences.projectId = "be4ee394-a407-4b26-82c1-74250a367f68";
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    const auto initialized = init(preferences, sl::kSDKVersion);
    std::cout << "slInit: " << static_cast<int>(initialized) << '\n';
    if (initialized != sl::Result::eOk) {
        FreeLibrary(library);
        return 8;
    }
    ComPtr<ID3D12Device> device;
    const auto made = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    std::cout << "D3D12CreateDevice: " << std::hex << static_cast<unsigned>(made) << std::dec << '\n';
    if (FAILED(made)) {
        shutdown();
        FreeLibrary(library);
        return 9;
    }
    const auto bound = bind(device.Get());
    std::cout << "slSetD3DDevice: " << static_cast<int>(bound) << '\n';
    sl::AdapterInfo info{};
    info.deviceLUID = reinterpret_cast<uint8_t *>(&desc.AdapterLuid);
    info.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);
    bool nr_supported = !nr;
    std::vector<std::pair<sl::Feature, int>> capabilities;
    for (auto feature : features) {
        const auto result = supported(feature, info);
        capabilities.emplace_back(feature, static_cast<int>(result));
        if (feature == sl::kFeatureDLSS_NR)
            nr_supported = result == sl::Result::eOk;
        std::cout << "Feature " << feature << " supported: " << static_cast<int>(result) << '\n';
    }
    void *function = nullptr;
    const auto fg_api = get(sl::kFeatureDLSS_G, "slDLSSGGetState", function);
    std::cout << "DLSSG API: " << static_cast<int>(fg_api) << '\n';
    if (fg_api == sl::Result::eOk && function) {
        sl::DLSSGState state{};
        auto fn = reinterpret_cast<sl::Result (*)(const sl::ViewportHandle &, sl::DLSSGState &,
                                                  const sl::DLSSGOptions *)>(function);
        const auto result = fn(sl::ViewportHandle(0), state, nullptr);
        std::cout << "DLSSG GetState: " << static_cast<int>(result) << ", status "
                  << static_cast<unsigned>(state.status) << '\n';
    }
    function = nullptr;
    const auto reflex_api = get(sl::kFeatureReflex, "slReflexGetState", function);
    std::cout << "Reflex API: " << static_cast<int>(reflex_api) << '\n';
    if (reflex_api == sl::Result::eOk && function) {
        sl::ReflexState state{};
        auto fn = reinterpret_cast<sl::Result (*)(sl::ReflexState &)>(function);
        const auto result = fn(state);
        std::cout << "Reflex GetState: " << static_cast<int>(result) << '\n';
    }
    std::cout << "No swapchain, generated frame, sleep policy or game setting was changed by this probe\n";
    const auto stopped = shutdown();
    std::cout << "slShutdown: " << static_cast<int>(stopped) << '\n';
    std::ofstream report(root / L"xivrender-probe.json");
    report << "{\"sdk\":\"2.14.16\",\"initialization\":" << static_cast<int>(initialized)
           << ",\"device_bind\":" << static_cast<int>(bound) << ",\"shutdown\":" << static_cast<int>(stopped)
           << ",\"nr_requested\":" << (nr ? "true" : "false") << ",\"features\":[";
    for (size_t i = 0; i < capabilities.size(); ++i) {
        if (i)
            report << ',';
        report << "{\"id\":" << capabilities[i].first << ",\"result\":" << capabilities[i].second << '}';
    }
    report << "],\"presentation_tested\":false,\"model_evaluation_tested\":false}";
    device.Reset();
    FreeLibrary(library);
    return bound == sl::Result::eOk && stopped == sl::Result::eOk ? (nr_supported ? 0 : 11) : 10;
}
