// Compatibility bootstrap follows the FFXIV OptiScaler fork's
// AmpereMfgLoader: load before Streamline, explicitly initialize renamed ASI,
// and leave frame count/pacing ownership with the host via FollowGame.
#include "xivrender/fg_compatibility.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>
namespace xivrender::fg {
namespace {
bool compute_capability(const LUID &adapter, int &major, int &minor) {
    static HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda) return false;
    const auto init = reinterpret_cast<int(WINAPI *)(unsigned)>(GetProcAddress(cuda, "cuInit"));
    const auto count = reinterpret_cast<int(WINAPI *)(int *)>(GetProcAddress(cuda, "cuDeviceGetCount"));
    const auto get = reinterpret_cast<int(WINAPI *)(int *, int)>(GetProcAddress(cuda, "cuDeviceGet"));
    const auto luid = reinterpret_cast<int(WINAPI *)(char *, unsigned *, int)>(GetProcAddress(cuda, "cuDeviceGetLuid"));
    const auto attribute = reinterpret_cast<int(WINAPI *)(int *, int, int)>(GetProcAddress(cuda, "cuDeviceGetAttribute"));
    int devices = 0;
    if (!init || !count || !get || !luid || !attribute || init(0) || count(&devices)) return false;
    for (int index = 0; index < devices; ++index) {
        int device = 0;
        unsigned mask = 0;
        std::array<char, sizeof(LUID)> id{};
        if (!get(&device, index) && !luid(id.data(), &mask, device) &&
            std::memcmp(id.data(), &adapter, sizeof(adapter)) == 0)
            return !attribute(&major, 75, device) && !attribute(&minor, 76, device);
    }
    return false;
}
bool verified_runtime(const std::filesystem::path &path) {
    // User-supplied SilyNoMeta 0.3.5-5 package, SHA256SUMS.txt. The ABI and
    // embedded kernels are version-specific; arbitrary DLLs are not equivalent.
    constexpr char expected[] = "7489a89bf593cda243d95c62e6d8b75905d9774e27b7b2d53b91625a79c05ca4";
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != 33231360) return false;
    input.seekg(0);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    bool okay = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<unsigned char, 65536> block{};
    while (okay && input) {
        input.read(reinterpret_cast<char *>(block.data()), block.size());
        const auto size = input.gcount();
        if (size) okay = BCryptHashData(hash, block.data(), ULONG(size), 0) >= 0;
    }
    std::array<unsigned char, 32> digest{};
    okay = okay && input.eof() && BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; okay && i < digest.size(); ++i)
        okay = hex[digest[i] >> 4] == expected[i * 2] && hex[digest[i] & 15] == expected[i * 2 + 1];
    return okay;
}
}
bool prepare_compatibility(IDXGIAdapter *adapter, const std::filesystem::path &root, std::string &error) {
    DXGI_ADAPTER_DESC desc{};
    if (!adapter || FAILED(adapter->GetDesc(&desc))) {
        error = "DLSS-G rendering adapter identity unavailable";
        return false;
    }
    if (desc.VendorId != 0x10de) {
        error = "SM75/SM86 compatibility is not needed for this adapter";
        return true;
    }
    int major = 0, minor = 0;
    if (!compute_capability(desc.AdapterLuid, major, minor)) {
        error = "Cannot match the NVIDIA rendering adapter to its CUDA architecture";
        return false;
    }
    if (!requires_sm_compatibility(major, minor)) {
        error = "NVIDIA SM" + std::to_string(major) + std::to_string(minor) + ": native provider path";
        return true;
    }
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    static HMODULE compatibility = nullptr;
    static bool initialized = false;
    if (initialized) {
        error = "SM75/SM86 compatibility initialized; host controls frame generation";
        return true;
    }
    const auto path = root / L"fg/dlssg_sm86/dlssg_sm86.dll";
    if (!verified_runtime(path)) {
        error = "RTX 20/30 DLSS-G needs the pinned compatibility runtime in XIVRender/runtime/fg/dlssg_sm86; run prepare-sm86-runtime.ps1";
        return false;
    }
    if (!compatibility)
        compatibility = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!compatibility) {
        error = "Cannot load the SM75/SM86 compatibility runtime: " + std::to_string(GetLastError());
        return false;
    }
    using Init = void(*)();
    auto init = reinterpret_cast<Init>(GetProcAddress(compatibility, "InitializeASI"));
    if (!init) init = reinterpret_cast<Init>(GetProcAddress(compatibility, "DLSSG_UniversalProxy"));
    struct Control { std::uint32_t version = 1, mode = 0, multiplier = 0, target = 0, flags = 0; } request;
    static_assert(sizeof(Control) == 20);
    auto control = reinterpret_cast<bool(*)(const Control *)>(GetProcAddress(compatibility, "DLSSG_RequestControl"));
    const auto ui = reinterpret_cast<bool(*)(std::uint32_t)>(GetProcAddress(compatibility, "DLSSG_RequestUI"));
    if (!init || !control || !ui) {
        error = "SM75/SM86 runtime initialization/control exports are unavailable";
        return false;
    }
    if (!control(&request) || !ui(0)) {
        error = "SM75/SM86 runtime rejected host-owned frame generation controls";
        return false;
    }
    // Set ownership before initialization starts the companion's workers.
    // The pinned proxy accepts this export before InitializeASI, as in the
    // OptiScaler host. Subsequent provider changes keep the same ownership.
    init();
    // Its hooks and embedded model retain process lifetime. Never unload them
    // during an FG toggle or leave calls pointing into an unloaded DLL.
    initialized = true;
    error = "SM75/SM86 compatibility initialized; actual MFG capability will be queried from Streamline";
    return true;
}
}
