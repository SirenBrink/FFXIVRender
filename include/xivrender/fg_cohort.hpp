#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <utility>

namespace xivrender::fg {
enum class CohortIssue { None, MissingCamera, InvalidCamera, DiscardedCamera };

// The native producer and evaluator have a FIFO relationship. An invalid or
// discarded camera still occupies one position: deleting it would pair every
// following image with a different camera. Overflow retains a bounded loss
// ledger so an evaluator skips that exact number of missing producer entries.
template <class Camera, unsigned Capacity = 8> class CameraCohorts {
    static_assert(Capacity > 0);

  public:
    struct Entry {
        std::uint64_t frame = 0, epoch = 0;
        Camera camera{};
        bool valid = false;
        CohortIssue issue = CohortIssue::MissingCamera;
    };
    struct Counters {
        std::uint64_t missing = 0, invalid = 0, overflows = 0;
    } counters;

    void push(std::uint64_t frame, const Camera &camera, bool valid) {
        if (entries_.size() >= Capacity) {
            entries_.pop_front();
            ++lost_;
            ++counters.overflows;
            ++epoch_;
        }
        entries_.push_back(
            {frame, epoch_, camera, valid, valid ? CohortIssue::None : CohortIssue::InvalidCamera});
    }
    Entry take() {
        if (lost_) {
            --lost_;
            ++counters.missing;
            return {0, epoch_, {}, false, CohortIssue::DiscardedCamera};
        }
        if (entries_.empty()) {
            ++counters.missing;
            ++epoch_;
            return {0, epoch_, {}, false, CohortIssue::MissingCamera};
        }
        auto entry = std::move(entries_.front());
        entries_.pop_front();
        if (entry.issue == CohortIssue::DiscardedCamera)
            ++counters.missing;
        else if (!entry.valid)
            ++counters.invalid;
        return entry;
    }
    void clear() {
        ++epoch_;
        // Keep each producer position as a tombstone until its evaluator
        // consumes it. Moving entries to loss debt would fill the queue again
        // during continuous production and prevent ordinary focus/provider
        // transitions from catching up.
        for (auto &entry : entries_) {
            entry.valid = false;
            entry.issue = CohortIssue::DiscardedCamera;
            entry.epoch = epoch_;
        }
    }

  private:
    std::deque<Entry> entries_;
    std::uint64_t epoch_ = 1, lost_ = 0;
};

class FrameCohort {
  public:
    struct Source {
        const void *feature = nullptr;
        unsigned width = 0, height = 0;
        bool operator==(const Source &) const = default;
    };
    struct Counters {
        std::uint64_t stale = 0, overwritten = 0, resets = 0;
        unsigned age = 0;
    } counters;

    void begin_capture() {
        if (pending_) {
            ++counters.overwritten;
            reset_next_ = true;
        }
        pending_ = false;
    }
    void invalidate() {
        pending_ = false;
        reset_next_ = true;
    }
    bool accept(std::uint64_t native_frame, std::uint64_t source_frame, std::uint64_t epoch,
                const Source &source, std::uint64_t simulation, bool reset) {
        counters.age = native_frame && simulation >= native_frame
                           ? unsigned(std::min<std::uint64_t>(simulation - native_frame,
                                                              std::numeric_limits<unsigned>::max()))
                           : 0;
        if (!native_frame || !source_frame || native_frame <= last_capture_ ||
            native_frame <= last_presented_ || source_frame <= last_source_) {
            ++counters.stale;
            invalidate();
            return false;
        }
        reset_ = reset || reset_next_ || epoch != epoch_ || !(source == source_) ||
                 (last_capture_ && native_frame != last_capture_ + 1) ||
                 (last_source_ && source_frame != last_source_ + 1) ||
                 (last_presented_ && native_frame != last_presented_ + 1);
        if (reset_)
            ++counters.resets;
        epoch_ = epoch;
        source_ = source;
        last_capture_ = pending_frame_ = native_frame;
        last_source_ = source_frame;
        reset_next_ = false;
        pending_ = true;
        return true;
    }
    bool ready(std::uint64_t frame) const {
        return pending_ && frame == pending_frame_ && frame > last_output_;
    }
    bool reset() const {
        return reset_;
    }
    // An ordinary Present may lack world guides. Only use an observed native
    // boundary for its SDK markers; never invent a new native-frame number.
    std::uint64_t output_frame(std::uint64_t candidate, std::uint64_t simulation) const {
        if (pending_ && candidate == pending_frame_) {
            // An ordinary no-guide Present can be one simulation ahead of its
            // render worker. Do not advance again while that genuine source
            // catches up, or interpolation would remain disabled forever.
            return ready(candidate) ? candidate : 0;
        }
        // Once world rendering starts, an unmatched Present must not chase
        // simulation ahead of its render worker. Only matched image cohorts
        // may advance the SDK timeline from this point onward.
        return !last_capture_ && simulation > last_output_ ? simulation : 0;
    }
    void presented(std::uint64_t frame, bool interpolated) {
        if (ready(frame))
            last_presented_ = frame;
        if (frame > last_output_)
            last_output_ = frame;
        pending_ = false;
        if (!interpolated)
            reset_next_ = true;
    }

  private:
    Source source_;
    std::uint64_t epoch_ = 0, last_capture_ = 0, last_source_ = 0, last_presented_ = 0, last_output_ = 0,
                  pending_frame_ = 0;
    bool pending_ = false, reset_ = true, reset_next_ = true;
};
} // namespace xivrender::fg
