#pragma once
#include <dxgi.h>
#include <filesystem>
#include <string>
namespace xivrender::fg {
// Match the actual rendering adapter, never the first NVIDIA GPU enumerated.
inline bool requires_sm_compatibility(int major, int minor) {
    return (major == 7 && minor == 5) || (major == 8 && minor == 6);
}
bool prepare_compatibility(IDXGIAdapter *, const std::filesystem::path &, std::string &);
}
