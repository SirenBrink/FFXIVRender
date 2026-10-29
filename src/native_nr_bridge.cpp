// SPDX-License-Identifier: GPL-3.0-or-later
// Direct snippet/caller boundary follows the verified native route in the
// SirenBrink OptiScaler fork. No NVIDIA binary or signature verification is patched.
#include "xivrender/native_nr_api.hpp"
#include "xivrender/d3d12_identity.hpp"
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#include <algorithm>
#include <memory>
#include <utility>

namespace xivrender::nr {
using Init = std::uint32_t(__cdecl *)(std::uint64_t, const wchar_t *, ID3D12Device *, unsigned,
                                      NVSDK_NGX_Parameter *);
using Populate = std::uint32_t(__cdecl *)(NVSDK_NGX_Parameter *);
using Create = std::uint32_t(__cdecl *)(ID3D12GraphicsCommandList *, unsigned, NVSDK_NGX_Parameter *,
                                        void **);
using Evaluate = std::uint32_t(__cdecl *)(ID3D12GraphicsCommandList *, const void *, NVSDK_NGX_Parameter *,
                                          void *);
using Release = std::uint32_t(__cdecl *)(void *);
using Shutdown = std::uint32_t(__cdecl *)(ID3D12Device *);
struct Session {
    struct Feature {
        const void *handle;
        bool ready;
    };
    HMODULE library = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::filesystem::path path;
    Create create = nullptr;
    Evaluate evaluate = nullptr;
    Release release = nullptr;
    Shutdown shutdown = nullptr;
    bool ready = false;
    std::vector<Feature> features;
    ~Session() {
        if (library)
            FreeLibrary(library);
    }
};
namespace {
std::recursive_mutex mutex;
std::vector<Session *> sessions;
bool pe64(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    if (!file.read(reinterpret_cast<char *>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < sizeof(dos))
        return false;
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (std::uint64_t(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > std::uint64_t(size))
        return false;
    file.seekg(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    return bool(file.read(reinterpret_cast<char *>(&nt), sizeof(nt))) && nt.Signature == IMAGE_NT_SIGNATURE &&
           nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
           nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
           (nt.FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;
}
template <class T> T proc(HMODULE library, const char *name) {
    return reinterpret_cast<T>(GetProcAddress(library, name));
}
bool same_device(Session *session, ID3D12GraphicsCommandList *commands) {
    return d3d12_identity::same_device(session->device.Get(), commands);
}
} // namespace
} // namespace xivrender::nr

// The model checks the module owning its caller. This library intentionally has
// "nvngx.dll" in its distinct addon-private name. Keep a real return frame here:
// a tail jump would give the snippet the outer addon's caller instead.
extern "C" std::uint32_t __cdecl XIVSR_PopulateCallbacks(void *module, NVSDK_NGX_Parameter *parameters) {
    if (!module || !parameters) return xivrender::nr::invalid_argument;
    const auto populate = reinterpret_cast<xivrender::nr::Populate>(GetProcAddress(static_cast<HMODULE>(module), "NVSDK_NGX_D3D11_PopulateParameters_Impl"));
    if (!populate) return xivrender::nr::missing_export;
    volatile auto result = populate(parameters);
    return result;
}
extern "C" std::uint32_t __cdecl XIVSR_InvokeCallback(void *callback, NVSDK_NGX_Parameter *parameters) {
    if (!callback || !parameters) return xivrender::nr::invalid_argument;
    volatile auto result = reinterpret_cast<xivrender::nr::Populate>(callback)(parameters);
    return result;
}
extern "C" std::uint32_t __cdecl XIVNR_Open(const wchar_t *model, const wchar_t *data, ID3D12Device *device,
                                            std::uint64_t application_id, NVSDK_NGX_Parameter *parameters,
                                            xivrender::nr::Session **output) {
    using namespace xivrender::nr;
    if (output)
        *output = nullptr;
    if (!model || !*model || !data || !*data || !device || !parameters || !output)
        return invalid_argument;
    try {
        std::lock_guard lock(mutex);
        const std::filesystem::path requested(model);
        if (!requested.is_absolute() || !std::filesystem::is_regular_file(requested))
            return invalid_model;
        const auto path = std::filesystem::canonical(requested);
        if (!pe64(path))
            return invalid_model;
        // A snippet has global initialization state; do not let one owner shut
        // down another. Multiple features belong to the same explicit session.
        for (const auto *session : sessions)
            if (std::filesystem::equivalent(session->path, path))
                return busy;
        // Do not initialize/shut down a model already owned by another addon.
        if (GetModuleHandleW(path.c_str()))
            return busy;
        auto session = std::make_unique<Session>();
        session->path = path;
        session->library = LoadLibraryExW(
            path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!session->library)
            return load_failed;
        for (const auto *existing : sessions)
            if (existing->library == session->library)
                return busy;
        const auto init = proc<Init>(session->library, "NVSDK_NGX_D3D12_Init_Ext");
        const auto populate = proc<Populate>(session->library, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
        session->create = proc<Create>(session->library, "NVSDK_NGX_D3D12_CreateFeature");
        session->evaluate = proc<Evaluate>(session->library, "NVSDK_NGX_D3D12_EvaluateFeature");
        session->release = proc<Release>(session->library, "NVSDK_NGX_D3D12_ReleaseFeature");
        session->shutdown = proc<Shutdown>(session->library, "NVSDK_NGX_D3D12_Shutdown1");
        if (!init || !populate || !session->create || !session->evaluate || !session->release ||
            !session->shutdown)
            return missing_export;
        session->device = device;
        // Reserve ownership storage before initialization can allocate model resources.
        sessions.reserve(sessions.size() + 1);
        volatile std::uint32_t result = init(application_id, data, device, 0x15, nullptr);
        if (result != success)
            return result;
        try {
            result = populate(parameters);
        } catch (...) {
            result = internal_error;
        }
        if (result != success) {
            volatile auto stopped = session->shutdown(device);
            if (stopped != success) {
                // Partial initialization still owns the module/device until a
                // later successful Close; report the original populate error.
                sessions.push_back(session.get());
                *output = session.release();
            }
            return result;
        }
        session->ready = true;
        sessions.push_back(session.get());
        *output = session.release();
        return success;
    } catch (...) {
        return internal_error;
    }
}

extern "C" std::uint32_t __cdecl XIVNR_Create(xivrender::nr::Session *session,
                                              ID3D12GraphicsCommandList *commands,
                                              NVSDK_NGX_Parameter *parameters, void **feature) {
    using namespace xivrender::nr;
    if (feature)
        *feature = nullptr;
    std::lock_guard lock(mutex);
    if (!(std::find(sessions.begin(), sessions.end(), session) != sessions.end()) || !commands ||
        !parameters || !feature || !session->ready)
        return invalid_argument;
    if (!same_device(session, commands))
        return invalid_argument;
    // Allocation failure happens before the model can record commands or return a handle.
    try {
        session->features.reserve(session->features.size() + 1);
    } catch (...) {
        return internal_error;
    }
    volatile auto result = session->create(commands, 18, parameters, feature);
    if (*feature)
        session->features.push_back({*feature, result == success}); // partial handles are cleanup-only
    return result;
}
extern "C" std::uint32_t __cdecl XIVNR_Evaluate(xivrender::nr::Session *session,
                                                ID3D12GraphicsCommandList *commands, const void *feature,
                                                NVSDK_NGX_Parameter *parameters) {
    using namespace xivrender::nr;
    std::lock_guard lock(mutex);
    if (!(std::find(sessions.begin(), sessions.end(), session) != sessions.end()) || !commands ||
        !parameters || !session->ready)
        return invalid_argument;
    if (!same_device(session, commands))
        return invalid_argument;
    const auto owned = std::find_if(session->features.begin(), session->features.end(),
                                    [feature](const auto &candidate) { return candidate.handle == feature; });
    if (owned == session->features.end())
        return foreign_feature;
    if (!owned->ready)
        return invalid_argument;
    volatile auto result = session->evaluate(commands, feature, parameters, nullptr);
    return result;
}
extern "C" std::uint32_t __cdecl XIVNR_Release(xivrender::nr::Session *session, void *feature) {
    using namespace xivrender::nr;
    std::lock_guard lock(mutex);
    if (!(std::find(sessions.begin(), sessions.end(), session) != sessions.end()))
        return invalid_argument;
    const auto owned = std::find_if(session->features.begin(), session->features.end(),
                                    [feature](const auto &candidate) { return candidate.handle == feature; });
    if (owned == session->features.end())
        return foreign_feature;
    volatile auto result = session->release(feature);
    if (result == success)
        session->features.erase(owned);
    return result;
}
extern "C" std::uint32_t __cdecl XIVNR_Close(xivrender::nr::Session *session) {
    using namespace xivrender::nr;
    std::lock_guard lock(mutex);
    if (!(std::find(sessions.begin(), sessions.end(), session) != sessions.end()))
        return invalid_argument;
    if (!session->features.empty())
        return busy;
    volatile auto result = session->shutdown(session->device.Get());
    if (result != success)
        return result;
    std::erase(sessions, session);
    delete session;
    return success;
}
