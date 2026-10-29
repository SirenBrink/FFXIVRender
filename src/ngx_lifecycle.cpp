#include "xivrender/ngx_lifecycle.hpp"
#include "xivrender/fg_diagnostics.hpp"
#include <nvsdk_ngx_defs.h>
#include <MinHook.h>
#include <wrl/client.h>
#include <mutex>
namespace xivrender::ngx {
namespace {
using Legacy = NVSDK_NGX_Result(NVSDK_CONV *)();
// Driver export ABI (also used by the local OptiScaler NVNGXProxy): the
// static SDK's public one-argument wrapper supplies this optional counter.
using Specific = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D11Device *, unsigned *);
struct State {
    std::recursive_mutex mutex;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Legacy legacy = nullptr;
    Specific specific = nullptr;
    void *legacy_target = nullptr, *specific_target = nullptr;
    bool installed = false, managed = false, completed = false;
    unsigned remaining = 0;
    ID3D11Device *identity = nullptr;
};
State &state() { static auto *s = new State; return *s; }
NVSDK_NGX_Result finish(ID3D11Device *requested, unsigned *remaining) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (requested && requested != s.identity)
        return s.specific(requested, remaining);
    if (s.completed) {
        if (remaining) *remaining = s.remaining;
        return NVSDK_NGX_Result_Success;
    }
    if (!s.device) return NVSDK_NGX_Result_FAIL_PlatformError;
    unsigned count = 0;
    const auto result = s.specific(s.device.Get(), &count);
    fg::diagnostics::event("ngx.native_device_shutdown", 0, unsigned(result), count);
    if (remaining) *remaining = count;
    if (result == NVSDK_NGX_Result_Success) {
        s.remaining = count;
        s.completed = true;
        s.device.Reset();
    }
    return result;
}
NVSDK_NGX_Result NVSDK_CONV legacy_shutdown() {
    return finish(nullptr, nullptr);
}
NVSDK_NGX_Result NVSDK_CONV specific_shutdown(ID3D11Device *device, unsigned *remaining) {
    return finish(device, remaining);
}
}
bool guard_native_shutdown(ID3D11Device *device, std::string &error, HMODULE runtime) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (device && device != s.device.Get()) {
        s.device = device;
        s.identity = device;
        s.managed = true;
        s.completed = false;
    }
    if (s.installed) return true;
    if (!s.managed) return false;
    if (!runtime) runtime = GetModuleHandleW(L"_nvngx.dll");
    if (!runtime) return false; // Native NGX has not loaded yet; retry later.
    auto *legacy = reinterpret_cast<void *>(GetProcAddress(runtime, "NVSDK_NGX_D3D11_Shutdown"));
    auto *specific = reinterpret_cast<void *>(GetProcAddress(runtime, "NVSDK_NGX_D3D11_Shutdown1"));
    if (!legacy || !specific) { error = "NGX device-specific shutdown export unavailable"; return false; }
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;
    HMODULE owner = nullptr, driver = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&legacy_shutdown), &owner) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(legacy), &driver)) return false;
    if (s.legacy_target && (s.legacy_target != legacy || s.specific_target != specific)) {
        error = "NGX shutdown runtime changed after hook creation"; return false;
    }
    if (!s.legacy_target) {
    auto result = MH_CreateHook(legacy, reinterpret_cast<void *>(&legacy_shutdown), reinterpret_cast<void **>(&s.legacy));
    if (result != MH_OK) { error = std::string("NGX shutdown hook: ") + MH_StatusToString(result); return false; }
    result = MH_CreateHook(specific, reinterpret_cast<void *>(&specific_shutdown), reinterpret_cast<void **>(&s.specific));
    if (result != MH_OK) {
        MH_RemoveHook(legacy);
        error = std::string("NGX device shutdown hook: ") + MH_StatusToString(result); return false;
    }
    s.legacy_target = legacy;
    s.specific_target = specific;
    }
    if (MH_EnableHook(specific) != MH_OK || MH_EnableHook(legacy) != MH_OK) {
        MH_DisableHook(legacy); MH_DisableHook(specific);
        // Disabled trampolines remain valid for any caller already inside.
        error = "NGX shutdown guard could not be enabled"; return false;
    }
    s.installed = true;
    fg::diagnostics::event("ngx.native_shutdown_guard_ready");
    return true;
}
}
