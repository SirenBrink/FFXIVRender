#pragma once
#include <cstdint>
#include <limits>

namespace xivrender::fg {
// DXGI's native Present counter is asynchronous to the application's Present.
// Individual deltas can be 0/1/3 for continuous 2X output. Keep all native
// increments and estimate a multiplier over several complete source frames.
class PresentationCount {
    std::uint32_t previous_ = 0;
    std::uint64_t total_ = 0, start_ = 0, real_ = 0, native_ = 0;
    double ratio_ = 0;
    bool ready_ = false, resync_ = false;

  public:
    void reset(std::uint32_t baseline, std::uint64_t now) {
        previous_ = baseline;
        total_ = 0;
        reset_window(now);
    }
    void reset_window(std::uint64_t now) {
        start_ = now;
        real_ = native_ = 0;
        ratio_ = 0;
        ready_ = false;
        resync_ = true;
    }
    // Rebase the ratio after a failed query. The next successful native
    // counter still recovers the full displayed total, including the gap.
    void query_failed(std::uint64_t now) {
        reset_window(now);
    }
    bool observe(std::uint32_t current, std::uint64_t now) {
        const std::uint32_t difference = current - previous_;
        previous_ = current;
        // Small unsigned deltas naturally handle UINT rollover. A backwards
        // counter/reset is a discontinuity, not billions of displayed frames.
        if (difference > std::numeric_limits<std::uint32_t>::max() / 2) {
            reset_window(now);
            return false;
        }
        total_ += difference;
        if (resync_ || now < start_) {
            start_ = now;
            real_ = native_ = 0;
            resync_ = false;
            return true;
        }
        ++real_;
        native_ += difference;
        if (real_ >= 8 && now - start_ >= 250) {
            ratio_ = double(native_) / double(real_);
            ready_ = true;
            start_ = now;
            real_ = native_ = 0;
        }
        return true;
    }
    std::uint64_t total() const {
        return total_;
    }
    bool ready() const {
        return ready_;
    }
    double ratio() const {
        return ratio_;
    }
};
} // namespace xivrender::fg
