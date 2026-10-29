#include "xivrender/ngx_logging.hpp"
#include "xivrender/fg_diagnostics.hpp"
#include <nvsdk_ngx_defs.h>
#include <d3d12.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
namespace xivrender::fg {
namespace {
using Init = NVSDK_NGX_Result(NVSDK_CONV *)(const char *, NVSDK_NGX_EngineType, const char *,
    const wchar_t *, ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *);
constexpr char project[] = "be4ee394-a407-4b26-82c1-74250a367f68";
struct State {
    std::mutex mutex;
    Init original = nullptr;
    void *target = nullptr;
    std::atomic<unsigned> rewritten{0};
};
State &state() { static auto *s = new State; return *s; }
thread_local bool binding = false;
void NVSDK_CONV durable_log(const char *message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature feature) {
    diagnostics::event("sdk.ngx", 0, unsigned(level), unsigned(feature), 0, 0, message);
}
NVSDK_NGX_Result NVSDK_CONV initialize(const char *id, NVSDK_NGX_EngineType engine, const char *version,
    const wchar_t *path, ID3D12Device *device, NVSDK_NGX_Version sdk, const NVSDK_NGX_FeatureCommonInfo *info) {
    auto &s = state();
    if (!binding || !id || std::strcmp(id, project) || !info)
        return s.original(id, engine, version, path, device, sdk, info);
    auto owned = *info;
    owned.LoggingInfo.LoggingCallback = &durable_log;
    ++s.rewritten;
    diagnostics::event("ngx.logging.redirected");
    return s.original(id, engine, version, path, device, sdk, &owned);
}
HMODULE driver_runtime() {
    if (auto loaded = GetModuleHandleW(L"_nvngx.dll")) return loaded;
    constexpr const wchar_t *keys[]{L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                                  L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore"};
    constexpr const wchar_t *values[]{L"FullPath", L"NGXPath"};
    for (unsigned i = 0; i < 2; ++i) {
        wchar_t path[32768]{};
        DWORD bytes = sizeof(path);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, keys[i], values[i], RRF_RT_REG_SZ, nullptr, path, &bytes) != ERROR_SUCCESS)
            continue;
        auto file = std::filesystem::path(path);
        if (!file.is_absolute()) continue;
        if (file.filename() != L"_nvngx.dll") file /= L"_nvngx.dll";
        if (auto loaded = LoadLibraryExW(file.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return loaded;
    }
    return nullptr;
}
}
bool bind_streamline_ngx(const std::function<int()> &bind, int &result, std::string &error, HMODULE test_runtime) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.target) {
        const auto module = test_runtime ? test_runtime : driver_runtime();
        const auto target = module ? reinterpret_cast<void *>(GetProcAddress(module, "NVSDK_NGX_D3D12_Init_ProjectID")) : nullptr;
        if (!target) { error = "NGX init export unavailable for durable logging"; return false; }
        HMODULE callback_owner = nullptr, runtime_owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&durable_log), &callback_owner) ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(target), &runtime_owner)) {
            error = "NGX logging callback lifetime could not be secured"; return false;
        }
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
            error = "NGX logging interceptor initialization failed"; return false;
        }
        if (MH_CreateHook(target, reinterpret_cast<void *>(&initialize), reinterpret_cast<void **>(&s.original)) != MH_OK) {
            error = "NGX init export already controlled or cannot be intercepted"; return false;
        }
        // Leave the DISABLED trampoline allocated for process lifetime, so a
        // concurrent foreign init already forwarding through it cannot race
        // its destruction. No init detour remains enabled between binds.
        s.target = target;
    }
    if (MH_EnableHook(s.target) != MH_OK) { error = "NGX logging interceptor enable failed"; return false; }
    const auto before = s.rewritten.load();
    binding = true;
    try { result = bind(); }
    catch (...) {
        binding = false;
        MH_DisableHook(s.target);
        throw;
    }
    binding = false;
    const auto restored = MH_DisableHook(s.target);
    diagnostics::event("ngx.logging.interceptor_disabled", 0, int(restored), s.rewritten.load() - before);
    if (restored != MH_OK) { error = "NGX init interceptor could not be disabled"; return false; }
    if (result == 0 && s.rewritten.load() == before) {
        error = "Streamline bound without installing the durable NGX log callback";
        return false;
    }
    return true;
}
}
