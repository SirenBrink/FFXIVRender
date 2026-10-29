#include "xivrender/fg_backend.hpp"
#include "xivrender/ngx_lifecycle.hpp"
#include <nvsdk_ngx_defs.h>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc != 2) return 2;
        ComPtr<ID3D12Device> d12;
        ComPtr<ID3D11Device> d11;
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12))) ||
            FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                     D3D11_SDK_VERSION, &d11, nullptr, nullptr))) return 3;
        std::string error;
        auto fg = xivrender::fg::make_backend(xivrender::FrameGeneration::DlssG, argv[1], false, error);
        if (!fg || !fg->prepare_device(d12.Get(), factory.Get(), error)) throw std::runtime_error(error);
        const auto driver = GetModuleHandleW(L"_nvngx.dll");
        using Init = NVSDK_NGX_Result(__cdecl *)(const char *, NVSDK_NGX_EngineType, const char *,
            const wchar_t *, ID3D11Device *, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *);
        using Stop = NVSDK_NGX_Result(__cdecl *)();
        const auto init = reinterpret_cast<Init>(GetProcAddress(driver, "NVSDK_NGX_D3D11_Init_ProjectID"));
        const auto stop = reinterpret_cast<Stop>(GetProcAddress(driver, "NVSDK_NGX_D3D11_Shutdown"));
        if (!init || !stop) throw std::runtime_error("Native NGX lifecycle exports missing");
        const auto path = std::filesystem::absolute(L"build/native-ngx-shutdown-data");
        std::filesystem::create_directories(path);
        NVSDK_NGX_FeatureCommonInfo info{};
        const auto started = init("be4ee394-a407-4b26-82c1-74250a367f68", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                  "XIVRender lifecycle test", path.c_str(), d11.Get(), NVSDK_NGX_Version_API, &info);
        if (started != NVSDK_NGX_Result_Success) throw std::runtime_error("Native DX11 NGX init failed: " + std::to_string(unsigned(started)));
        if (!xivrender::ngx::guard_native_shutdown(d11.Get(), error)) throw std::runtime_error(error);
        if (stop() != NVSDK_NGX_Result_Success || stop() != NVSDK_NGX_Result_Success)
            throw std::runtime_error("Guarded/repeated native shutdown failed");
        if (!fg->start_latency(d12.Get(), factory.Get(), error)) throw std::runtime_error(error);
        fg->sleep(1); fg->marker(0, 1); fg->marker(1, 1);
        if (!fg->close(error)) throw std::runtime_error(error);
        std::cout << "PASS: real driver DX11 shutdown preserves the private DX12 Streamline/Reflex instance\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
