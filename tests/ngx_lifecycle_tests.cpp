#include "xivrender/ngx_lifecycle.hpp"
#include <nvsdk_ngx_defs.h>
#include <wrl/client.h>
#include <cassert>
#include <iostream>
using Microsoft::WRL::ComPtr;
int wmain(int argc, wchar_t **argv) {
    assert(argc == 2);
    const auto library = LoadLibraryW(argv[1]);
    assert(library);
    using Legacy = unsigned(__cdecl *)();
    using Specific = unsigned(__cdecl *)(ID3D11Device *, unsigned *);
    auto legacy = reinterpret_cast<Legacy>(GetProcAddress(library, "NVSDK_NGX_D3D11_Shutdown"));
    auto specific = reinterpret_cast<Specific>(GetProcAddress(library, "NVSDK_NGX_D3D11_Shutdown1"));
    auto count = reinterpret_cast<unsigned(__cdecl *)(unsigned)>(GetProcAddress(library, "ShutdownCount"));
    auto fail = reinterpret_cast<void(__cdecl *)(bool)>(GetProcAddress(library, "ShutdownFail"));
    assert(legacy && specific && count && fail);
    ComPtr<ID3D11Device> game, foreign;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                     D3D11_SDK_VERSION, &game, nullptr, nullptr)));
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                     D3D11_SDK_VERSION, &foreign, nullptr, nullptr)));
    std::string error;
    assert(!xivrender::ngx::guard_native_shutdown(game.Get(), error, GetModuleHandleW(L"kernel32.dll")));
    assert(!error.empty());
    error.clear();
    assert(xivrender::ngx::guard_native_shutdown(game.Get(), error, library));
    fail(true);
    assert(legacy() == NVSDK_NGX_Result_FAIL_PlatformError && count(0) == 0 && count(1) == 1);
    fail(false);
    unsigned remaining = 0;
    assert(specific(nullptr, &remaining) == NVSDK_NGX_Result_Success && remaining == 7);
    assert(count(0) == 0 && count(1) == 2);
    assert(legacy() == NVSDK_NGX_Result_Success);
    assert(specific(game.Get(), &remaining) == NVSDK_NGX_Result_Success && remaining == 7);
    assert(count(1) == 2); // repeated game shutdown is idempotent
    assert(specific(foreign.Get(), &remaining) == NVSDK_NGX_Result_Success && count(1) == 3);
    assert(xivrender::ngx::guard_native_shutdown(game.Get(), error, library));
    assert(legacy() == NVSDK_NGX_Result_Success && count(1) == 4 && count(0) == 0);
    std::cout << "PASS: device-scoped NGX shutdown, null/legacy routing, failure retry, repeat shutdown, foreign device and reinitialization\n";
}
