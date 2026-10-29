// Actual SR callback with the addon contract enabled; no feature or game hooks.
#include "xivrender/app.hpp"
#include "xivrender/native_dlss.hpp"
#include "xivrender/ffxiv_hooks.hpp"
#include "xivrender/trace.hpp"
#include "xivrender/fg_backend.hpp"
#include <windows.h>
#include <MinHook.h>
#include <iostream>
namespace {
xivrender::Settings config;
xivrender::Status status;
int __cdecl install() {
    status.display_width = 3840; status.display_height = 2160;
    config.dlss.quality = xivrender::DlssQuality::UltraQuality;
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return 0;
    xivrender::native_dlss::poll();
    return xivrender::native_dlss::active() ? 1 : 0;
}
}
namespace xivrender {
Settings settings_snapshot() { return config; }
Status status_snapshot() { return status; }
void update_status(const Status &value) { status = value; }
Status modify_status(const std::function<void(Status &)> &f) { f(status); return status; }
void log_message(const std::string &value) { std::cout << value << '\n' << std::flush; }
std::uint64_t trace_epoch() { return 1; }
}
namespace xivrender::ffxiv {
void request_feature_refresh() {}
void settings_changed() {}
void measured_size(unsigned,unsigned,unsigned,unsigned) {}
}
int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 3 && argc != 4) return 2;
    std::shared_ptr<xivrender::fg::Backend> fg;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (argc == 4) {
        std::string error;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return 8;
        fg = xivrender::fg::make_backend(xivrender::FrameGeneration::DlssG, argv[3], false, error);
        if (!fg || !fg->prepare_device(device.Get(), factory.Get(), error)) {
            std::cerr << "Streamline preparation failed: " << error << '\n'; return 9;
        }
    }
    const auto library = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) return 3;
    const auto call = reinterpret_cast<int(__cdecl *)(const wchar_t *, int(__cdecl *)())>(GetProcAddress(library, "ProbeOptimal"));
    if (!call) return 4;
    const auto result = call(argv[2], install);
    xivrender::native_dlss::shutdown(); MH_Uninitialize();
    FreeLibrary(library); return result;
}
