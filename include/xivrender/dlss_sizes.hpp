#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
namespace xivrender {
// NGX enum values, not FFXIV's one-based selector. Ultra Quality is a custom
// ratio: current NVIDIA runtimes can return success with a zero size for enum 4.
inline std::optional<double> ngx_scale(int quality) {
    switch (quality) {
    case 0: return .5;
    case 1: return 1.0 / 1.7;
    case 2: return 2.0 / 3.0;
    case 3: return 1.0 / 3.0;
    case 4: return .77;
    case 5: return 1.0;
    default: return {};
    }
}
inline unsigned dlss_extent(unsigned display, double scale) {
    if (scale >= 1) return display;
    const auto result = unsigned(std::floor(display * scale));
    return std::clamp(result > 1 ? result & ~1u : 1u, 1u, display);
}
} // namespace xivrender
