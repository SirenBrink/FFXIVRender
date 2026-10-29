#include "xivrender/fg_cohort.hpp"
#include <cassert>
#include <iostream>

namespace {
struct Camera {
    unsigned image = 0;
};
void camera_positions_are_consumed_once() {
    using namespace xivrender::fg;
    CameraCohorts<Camera, 3> queue;
    queue.push(10, {10}, true);
    queue.push(11, {11}, false);
    queue.push(12, {12}, true);
    assert(queue.take().camera.image == 10);
    const auto invalid = queue.take();
    assert(invalid.frame == 11 && invalid.camera.image == 11 && !invalid.valid &&
           invalid.issue == CohortIssue::InvalidCamera);
    const auto after = queue.take();
    assert(after.frame == 12 && after.camera.image == 12 && after.valid);
    const auto absent = queue.take();
    assert(!absent.frame && !absent.valid && absent.issue == CohortIssue::MissingCamera);
    assert(queue.counters.invalid == 1 && queue.counters.missing == 1);

    // An evaluator whose camera overflowed must consume a missing position,
    // not the next image's camera. Memory remains bounded by Capacity.
    queue.push(20, {20}, true);
    queue.push(21, {21}, true);
    queue.push(22, {22}, true);
    queue.push(23, {23}, true);
    queue.push(24, {24}, true);
    assert(queue.take().issue == CohortIssue::DiscardedCamera); // Image 20.
    assert(queue.take().issue == CohortIssue::DiscardedCamera); // Image 21.
    for (unsigned image = 22; image <= 24; ++image) {
        const auto entry = queue.take();
        assert(entry.valid && entry.frame == image && entry.camera.image == image);
    }
    assert(queue.counters.overflows == 2);

    // Focus/provider retirement may discard produced packets that the native
    // rendering thread has not evaluated yet. Preserve their positions too.
    queue.push(30, {30}, true);
    queue.push(31, {31}, true);
    queue.clear();
    queue.push(32, {32}, true);
    assert(queue.take().issue == CohortIssue::DiscardedCamera);
    assert(queue.take().issue == CohortIssue::DiscardedCamera);
    const auto fresh = queue.take();
    assert(fresh.frame == 32 && fresh.camera.image == 32 && fresh.valid);
}
void native_history_requires_a_matching_present() {
    using namespace xivrender::fg;
    FrameCohort frames;
    int feature = 0, replacement = 0;
    FrameCohort::Source source{&feature, 2257, 1270};
    auto capture = [&](std::uint64_t native, std::uint64_t sequence, std::uint64_t epoch = 1) {
        frames.begin_capture();
        return frames.accept(native, sequence, epoch, source, native + 1, false);
    };
    assert(capture(10, 100) && frames.reset() && frames.counters.age == 1);
    assert(frames.ready(10) && frames.output_frame(10, 11) == 10);
    frames.presented(10, true);
    assert(!frames.ready(10));
    assert(capture(11, 101) && !frames.reset());
    frames.presented(11, true);
    assert(capture(14, 102) && frames.reset()); // Missing native frames.
    frames.presented(14, true);
    assert(!capture(14, 103) && !frames.ready(14)); // Repeated native source.
    assert(!capture(13, 104));                      // Regressing source.
    assert(capture(15, 103) && frames.reset());
    frames.presented(15, true);
    assert(capture(16, 105) && frames.reset()); // Missing evaluation sequence.
    frames.presented(16, true);
    assert(capture(17, 106, 2) && frames.reset()); // Camera queue epoch.
    frames.presented(17, true);
    source.feature = &replacement;
    assert(capture(18, 107, 2) && frames.reset());
    frames.presented(18, true);
    source.width = 2560;
    source.height = 1440;
    assert(capture(19, 108, 2) && frames.reset());
    frames.presented(19, true);
    assert(capture(20, 109, 2) && !frames.reset());
    assert(capture(21, 110, 2) && frames.reset()); // Overwrote an unpresented capture.
    assert(!frames.ready(20) && frames.ready(21) && frames.counters.overwritten == 1);
    frames.presented(21, true);

    // A rejected camera/image cannot leave the previous capture eligible.
    assert(capture(22, 111, 2));
    frames.begin_capture();
    frames.invalidate();
    assert(!frames.ready(22));
    assert(capture(24, 113, 2) && frames.reset());
    frames.presented(24, true);

    // Ordinary Presents use only observed native IDs and do not manufacture
    // a new ID when the native simulation boundary has not advanced.
    assert(frames.output_frame(0, 24) == 0);
    assert(frames.output_frame(0, 25) == 0);
    frames.presented(0, false);
    assert(frames.output_frame(0, 25) == 0);
    // The unmatched ordinary Present did not advance ahead of the worker.
    // Its next real render packet can resume immediately on the native ID.
    assert(capture(25, 114, 2) && frames.ready(25));
    assert(frames.output_frame(25, 26) == 25);
    frames.presented(25, true);
    assert(capture(26, 115, 2) && !frames.reset() && frames.ready(26));
    frames.presented(26, true);
    assert(capture(27, 116, 2) && !frames.reset());
    frames.presented(27, true);
    frames.invalidate(); // Focus/resize/provider suspension.
    assert(capture(28, 117, 2) && frames.reset());
    assert(frames.counters.stale == 2);
}
void continuous_production_recovers_after_transitions() {
    using namespace xivrender::fg;
    struct Camera {
        unsigned image;
    };
    CameraCohorts<Camera, 3> queue;
    queue.push(1, {1}, true);
    queue.push(2, {2}, true);
    queue.clear();
    for (unsigned frame = 3; frame < 20; ++frame) {
        queue.push(frame, {frame}, true);
        const auto entry = queue.take();
        if (frame < 5)
            assert(!entry.valid && entry.issue == CohortIssue::DiscardedCamera);
        else
            assert(entry.valid && entry.frame == frame - 2 && entry.camera.image == frame - 2);
    }
    FrameCohort frames;
    int feature = 0;
    const FrameCohort::Source source{&feature, 1920, 1080};
    // One startup Present precedes the first genuine world cohort. Ordinary
    // Presents between every later capture must not keep moving the target.
    frames.presented(frames.output_frame(0, 11), false);
    unsigned interpolated = 0;
    for (std::uint64_t frame = 10; frame < 30; ++frame) {
        frames.begin_capture();
        assert(frames.accept(frame, frame, 1, source, frame + 1, false));
        const auto id = frames.output_frame(frame, frame + 1);
        if (id)
            ++interpolated;
        frames.presented(id, id != 0);
        assert(frames.output_frame(0, frame + 2) == 0);
        frames.presented(0, false);
    }
    assert(interpolated == 18);
}
void age_is_bounded_and_missing_cameras_never_reuse_history() {
    using namespace xivrender::fg;
    FrameCohort frames;
    int feature = 0;
    const FrameCohort::Source source{&feature, 1920, 1080};
    frames.begin_capture();
    assert(frames.accept(1, 1, 1, source, std::numeric_limits<std::uint64_t>::max(), false));
    assert(frames.counters.age == std::numeric_limits<unsigned>::max());
    frames.presented(1, true);
    frames.begin_capture();
    frames.invalidate();
    assert(!frames.ready(1));
    assert(!frames.accept(0, 2, 1, source, 2, false));
    frames.begin_capture();
    assert(frames.accept(3, 3, 1, source, 3, false) && frames.reset());
}
} // namespace
int main() {
    camera_positions_are_consumed_once();
    native_history_requires_a_matching_present();
    age_is_bounded_and_missing_cameras_never_reuse_history();
    continuous_production_recovers_after_transitions();
    std::cout << "FG camera cohorts, source gaps, overwritten captures and native Present IDs passed\n";
}
