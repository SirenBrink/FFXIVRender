#pragma once
#include <atomic>
#include <cstdint>
#include "settings.hpp"
namespace xivrender {
// Automatic short capture after startup/each DLSS request; never log every
// rendered frame indefinitely. Old in-flight requests cannot reset a new window.
class TraceBudget {
    std::atomic<std::uint64_t> generation{0};
    std::atomic<unsigned> samples{0};

  public:
    bool take(std::uint64_t epoch, unsigned limit = 48) {
        auto current = generation.load(std::memory_order_acquire);
        if (epoch < current)
            return false;
        if (epoch > current && generation.compare_exchange_strong(current, epoch, std::memory_order_acq_rel))
            samples.store(0, std::memory_order_release);
        if (generation.load(std::memory_order_acquire) != epoch)
            return false;
        if (samples.load(std::memory_order_relaxed) >= limit)
            return false;
        return samples.fetch_add(1, std::memory_order_relaxed) < limit;
    }
};
inline const char *quality_name(DlssQuality value) {
    switch (value) {
    case DlssQuality::Game:
        return "Game/DRS";
    case DlssQuality::Dlaa:
        return "DLAA";
    case DlssQuality::UltraQuality:
        return "UltraQuality";
    case DlssQuality::Quality:
        return "Quality";
    case DlssQuality::Balanced:
        return "Balanced";
    case DlssQuality::Performance:
        return "Performance";
    case DlssQuality::UltraPerformance:
        return "UltraPerformance";
    }
    return "Unknown";
}
inline std::string model_name(DlssModelPreset value) {
    return value == DlssModelPreset::Automatic ? "Automatic"
                                               : std::string(1, char('A' + unsigned(value) - 1));
}
std::uint64_t trace_epoch();
} // namespace xivrender
