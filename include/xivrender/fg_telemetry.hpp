#pragma once
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace xivrender::fg {
using TimingClock = std::chrono::steady_clock;
inline double elapsed_ms(TimingClock::time_point begin) {
    return std::chrono::duration<double, std::milli>(TimingClock::now() - begin).count();
}
enum class TimingPhase : unsigned {
    OutputPreparation,
    CaptureLock,
    PresentLock,
    Transport,
    SdkPresent,
    InputRetirement,
    SimulationSleep,
    NativeSrLock,
    NativeSrEvaluate,
    NeuralRendering,
    Count
};
// ETW diagnostics only. QPC timestamps and thread IDs come from each ETW
// event header. These values must remain stable for offline trace decoding.
enum class TraceStage : unsigned {
    SimulationTickEntry = 1,
    SleepBegin = 2,
    SleepEnd = 3,
    SimulationBodyBegin = 4,
    SimulationEndRequested = 5,
    RenderBegin = 6,
    NativeCapture = 7,
    D11FlushBegin = 8,
    D11FlushEnd = 9,
    SdkPresentEntry = 10,
    SdkPresentExit = 11,
    ProviderRequest = 12,
    FrameworkBodyBegin = 13,
    FrameworkBodyEnd = 14
};
// Enabled only while an ETW collector subscribes to XIVRender.FrameTiming
// {88e925ad-5dbd-4b7c-9b60-70c87545a1d1}. With no collector this returns before
// reading shared state. A zero frame resolves to the current simulation ID;
// call only outside the FG global mutex. No file writes or GPU waits occur.
void note_trace(TraceStage, std::uint64_t frame = 0, std::uint64_t source = 0, std::uint64_t detail = 0);
// Preserve existing low-rate SDK/pacer diagnostics in the same ETW timeline.
// The caller retains the string for this synchronous call; no copy is stored.
void note_trace_text(const char *message);
struct TimingTotal {
    std::uint64_t samples = 0, over_100_ms = 0;
    double total_ms = 0, maximum_ms = 0;
};
struct PresentRequests {
    // Index 5 collects intervals outside DXGI's ordinary 0..4 range.
    std::array<std::uint64_t, 6> intervals{};
    std::uint32_t flags_any = 0, flags_all = 0;
    std::uint64_t copy_ready = 0, copy_pending = 0, slow = 0, slow_copy_ready = 0;
    static constexpr double slow_threshold_ms = 25;
    void record(unsigned interval, unsigned flags, bool ready, double milliseconds) {
        if (!std::isfinite(milliseconds) || milliseconds < 0)
            return;
        flags_all = copy_ready + copy_pending ? flags_all & flags : flags;
        flags_any |= flags;
        ++intervals[interval < 5 ? interval : 5];
        ++(ready ? copy_ready : copy_pending);
        if (milliseconds >= slow_threshold_ms) {
            ++slow;
            slow_copy_ready += ready;
        }
    }
};
struct PerformanceWindow {
    std::array<TimingTotal, unsigned(TimingPhase::Count)> stages{};
    std::array<std::uint64_t, 5> presentation_queries{};
    PresentRequests present_requests;
};
struct MemoryUsage {
    bool gpu_available = false, process_available = false;
    std::uint64_t local_bytes = 0, local_budget = 0, nonlocal_bytes = 0, nonlocal_budget = 0;
    std::uint64_t process_private = 0, working_set = 0;
    unsigned handles = 0;
};
// CPU wall-clock aggregation only. No per-frame file writes, GPU queries or
// readbacks. The periodic summary consumes a window; ordinary UI reads do not.
void note_timing(TimingPhase, double milliseconds);
void note_presentation_query(unsigned kind); // SetMax/GetMax/Waitable/Statistics/PresentCount.
// The caller samples its existing copy fence immediately before Present.
// This records whether a slow Present began after our submitted copy completed;
// it does not wait for the GPU or establish unrelated queue completion.
void note_present(unsigned interval, unsigned forwarded_flags, bool copy_ready_at_entry, double milliseconds);
PerformanceWindow take_performance();
MemoryUsage memory_usage();
} // namespace xivrender::fg
