// Test executable only: observe the SDK's native output, never the desktop.
// The window is deliberately never shown or activated. Readback is batched,
// so this checks ordering without serializing every frame on the CPU.
#include "xivrender/frame_generation.hpp"
#include "xivrender/hdr.hpp"
#include <MinHook.h>
#include <DirectXMath.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
constexpr unsigned foreground_origin = 32, foreground_step = 4, foreground_width = 16;
constexpr double foreground_half_width = (foreground_width - 1) * .5;
constexpr double position_tolerance = .25;
thread_local unsigned source_frame_hint = 0;
std::atomic<unsigned> latest_source_frame{0};
std::int64_t qpc_now() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
double qpc_ms(std::int64_t ticks) {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return double(value.QuadPart);
    }();
    return double(ticks) * 1000.0 / frequency;
}
void timing_statistics(const char *label, std::vector<double> values) {
    if (values.empty()) {
        std::cout << label << ": no observations\n";
        return;
    }
    constexpr std::array<double, 9> upper{1, 4, 8, 12, 16.7, 24, 33.4, 50, 100};
    std::array<unsigned, upper.size() + 1> histogram{};
    double total = 0;
    for (const auto value : values) {
        ++histogram[std::size_t(std::upper_bound(upper.begin(), upper.end(), value) - upper.begin())];
        total += value;
    }
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double fraction) {
        return values[std::size_t(std::ceil(fraction * double(values.size() - 1)))];
    };
    std::cout << label << " ms: n=" << values.size() << ", min/mean/p50/p95/p99/max=" << values.front() << "/"
              << total / double(values.size()) << "/" << percentile(.5) << "/" << percentile(.95) << "/"
              << percentile(.99) << "/" << values.back() << "\n  interval bins";
    for (std::size_t index = 0; index < upper.size(); ++index)
        std::cout << " <" << upper[index] << "=" << histogram[index];
    std::cout << " >=100=" << histogram.back() << '\n';
}
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
void checked(HRESULT result, const char *operation) {
    require(SUCCEEDED(result), std::string(operation) + ": " + std::to_string(unsigned(result)));
}
struct Event {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Event() {
        if (handle)
            CloseHandle(handle);
    }
};
struct Sample {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Resource> pixels;
    std::uint64_t fence = 0, ordinal = 0;
    unsigned width = 0, row_pitch = 0, second_row = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    HRESULT present_result = E_PENDING;
    std::int64_t entered = 0, returned = 0;
    unsigned source_hint = 0;
    DWORD thread = 0;
    bool direct_source_hint = false;
};
struct SourceTiming {
    unsigned frame = 0;
    std::int64_t begin = 0, markers = 0, capture = 0, captured = 0, present = 0, end = 0;
};
struct Audit {
    HWND window = nullptr;
    std::mutex mutex;
    bool enabled = true;
    bool timing_only = false;
    std::string error;
    ComPtr<IDXGISwapChain4> chain;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    std::vector<std::unique_ptr<Sample>> samples;
    std::vector<SourceTiming> sources;
    std::uint64_t serial = 0;
    std::atomic<unsigned> in_flight{0};
    unsigned expected_width = 0, expected_height = 0;
    static constexpr unsigned maximum_samples = 2048;
    unsigned sample_limit = maximum_samples;

    void attach(IUnknown *creating_device, IDXGISwapChain *created);
    Sample *before(IDXGISwapChain *presented, UINT flags) noexcept {
        const auto entered = timing_only ? qpc_now() : 0;
        std::lock_guard lock(mutex);
        if (!enabled || !chain || presented != static_cast<IDXGISwapChain *>(chain.Get()) ||
            (flags & DXGI_PRESENT_TEST))
            return nullptr;
        try {
            require(samples.size() < sample_limit, "Native Present audit exceeded its bounded sample cap");
            auto sample = std::make_unique<Sample>();
            sample->ordinal = samples.size() + 1;
            if (timing_only) {
                sample->entered = entered;
                sample->direct_source_hint = source_frame_hint != 0;
                sample->source_hint = source_frame_hint ? source_frame_hint : latest_source_frame.load();
                sample->thread = GetCurrentThreadId();
                auto *result = sample.get();
                samples.push_back(std::move(sample));
                ++in_flight;
                return result;
            }
            ComPtr<ID3D12Resource> source;
            checked(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&source)),
                    "Native output GetBuffer");
            const auto desc = source->GetDesc();
            require(desc.Width == expected_width && desc.Height == expected_height,
                    "The SDK native output dimensions differ from the test output");
            require(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                    "This sequence test requires a native 8-bit SDR output");
            sample->format = desc.Format;
            sample->width = static_cast<unsigned>(desc.Width);
            sample->row_pitch = (sample->width * 4u + 255u) & ~255u;
            sample->second_row = (sample->row_pitch + 511u) & ~511u;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = std::uint64_t(sample->second_row) + sample->row_pitch;
            buffer.Height = buffer.SampleDesc.Count = 1;
            buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&sample->pixels)),
                    "Native output row readback allocation");
            checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&sample->allocator)),
                    "Native audit allocator");
            checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, sample->allocator.Get(),
                                              nullptr, IID_PPV_ARGS(&sample->commands)),
                    "Native audit command list");
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = source.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            sample->commands->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = source.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            for (unsigned row = 0; row < 2; ++row) {
                D3D12_TEXTURE_COPY_LOCATION to{};
                to.pResource = sample->pixels.Get();
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint.Offset = row ? sample->second_row : 0;
                to.PlacedFootprint.Footprint.Format = desc.Format;
                to.PlacedFootprint.Footprint.Width = sample->width;
                to.PlacedFootprint.Footprint.Height = to.PlacedFootprint.Footprint.Depth = 1;
                to.PlacedFootprint.Footprint.RowPitch = sample->row_pitch;
                const unsigned y = row ? expected_height / 2 : 8;
                D3D12_BOX box{0, y, 0, sample->width, y + 1, 1};
                sample->commands->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            }
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            sample->commands->ResourceBarrier(1, &barrier);
            checked(sample->commands->Close(), "Native audit close");
            ID3D12CommandList *commands = sample->commands.Get();
            queue->ExecuteCommandLists(1, &commands);
            sample->fence = ++serial;
            checked(queue->Signal(fence.Get(), sample->fence), "Native audit signal");
            // Retain every allocator, command list and readback until the last
            // queue fence retires; never reset an in-flight audit allocation.
            auto *result = sample.get();
            samples.push_back(std::move(sample));
            ++in_flight;
            return result;
        } catch (const std::exception &failure) {
            error = failure.what();
            enabled = false;
            return nullptr;
        }
    }
    void after(Sample *sample, HRESULT result) noexcept {
        if (sample) {
            const auto returned = timing_only ? qpc_now() : 0;
            std::lock_guard lock(mutex);
            sample->present_result = result;
            sample->returned = returned;
            --in_flight;
        }
    }
    void finish_timing(unsigned submitted, const xivrender::fg::Status &status) {
        // Entries from different SDK pacer threads can overlap. Sort by their
        // QPC entry time instead of assuming mutex acquisition was timestamp order.
        std::vector<const Sample *> ordered;
        for (const auto &sample : samples)
            ordered.push_back(sample.get());
        std::sort(ordered.begin(), ordered.end(),
                  [](const Sample *a, const Sample *b) { return a->entered < b->entered; });
        std::vector<double> native_intervals, native_durations, long_gap_spacing;
        unsigned direct = 0, asynchronous = 0, long_gaps = 0, failed = 0, occluded = 0, changing_threads = 0;
        DWORD previous_thread = 0;
        std::int64_t previous = 0, previous_long = 0;
        for (const auto *sample : ordered) {
            changing_threads += previous_thread && previous_thread != sample->thread;
            previous_thread = sample->thread;
            direct += sample->direct_source_hint;
            asynchronous += !sample->direct_source_hint;
            failed += FAILED(sample->present_result);
            occluded += sample->present_result == DXGI_STATUS_OCCLUDED;
            const auto duration = qpc_ms(sample->returned - sample->entered);
            if (sample->source_hint > 10)
                native_durations.push_back(duration);
            if (previous && sample->source_hint > 10) {
                const auto interval = qpc_ms(sample->entered - previous);
                native_intervals.push_back(interval);
                if (interval >= 100) {
                    ++long_gaps;
                    if (previous_long)
                        long_gap_spacing.push_back(qpc_ms(sample->entered - previous_long));
                    previous_long = sample->entered;
                    if (long_gaps <= 32)
                        std::cout << "Native >=100ms gap: ordinal=" << sample->ordinal
                                  << ", interval=" << interval << ", Present CPU=" << duration
                                  << ", source hint=" << sample->source_hint
                                  << (sample->direct_source_hint ? " (app-thread)" : " (latest submitted)")
                                  << ", thread=" << sample->thread << '\n';
                }
            }
            previous = sample->entered;
        }
        std::vector<double> source_intervals, preparation, markers, capture, composition, present, whole;
        unsigned source_hitches = 0;
        previous = 0;
        for (const auto &source : sources) {
            if (source.frame > 10) {
                if (previous)
                    source_intervals.push_back(qpc_ms(source.begin - previous));
                preparation.push_back(qpc_ms(source.markers - source.begin));
                markers.push_back(qpc_ms(source.capture - source.markers));
                capture.push_back(qpc_ms(source.captured - source.capture));
                composition.push_back(qpc_ms(source.present - source.captured));
                present.push_back(qpc_ms(source.end - source.present));
                const auto duration = qpc_ms(source.end - source.begin);
                whole.push_back(duration);
                if (duration >= 100 && ++source_hitches <= 32)
                    std::cout << "Source >=100ms CPU: id=" << source.frame << ", total=" << duration
                              << ", prepare/markers/capture/compose/Present=" << preparation.back() << "/"
                              << markers.back() << "/" << capture.back() << "/" << composition.back() << "/"
                              << present.back() << '\n';
            }
            previous = source.begin;
        }
        std::cout << "Timing-only native audit: source frames=" << submitted
                  << ", native Presents=" << ordered.size() << ", app-thread source hints=" << direct
                  << ", asynchronous/latest-submitted hints=" << asynchronous
                  << ", >=100ms gaps=" << long_gaps << ", >=100ms source CPU=" << source_hitches
                  << ", occluded=" << occluded << ", failed Presents=" << failed
                  << ", submitting-thread changes=" << changing_threads << '\n';
        timing_statistics("Native Present submission interval (after source10)", native_intervals);
        timing_statistics("Native Present CPU duration (after source10)", native_durations);
        timing_statistics("Spacing between >=100ms native gaps", long_gap_spacing);
        timing_statistics("Source start interval (includes intentional16ms sleep)", source_intervals);
        timing_statistics("Source prepare CPU", preparation);
        timing_statistics("Simulation/render markers CPU", markers);
        timing_statistics("Source capture CPU", capture);
        timing_statistics("Source final colour upload CPU", composition);
        timing_statistics("Source Presenter::present CPU", present);
        timing_statistics("Whole source CPU (excludes intentional sleep)", whole);
        auto slowest = sources;
        std::sort(slowest.begin(), slowest.end(), [](const SourceTiming &a, const SourceTiming &b) {
            return a.end - a.begin > b.end - b.begin;
        });
        std::cout << "Slowest source CPU by ID (prepare/markers/capture/compose/Present/total ms):\n";
        for (std::size_t index = 0; index < std::min<std::size_t>(8, slowest.size()); ++index) {
            const auto &source = slowest[index];
            std::cout << "  " << source.frame << ": " << qpc_ms(source.markers - source.begin) << "/"
                      << qpc_ms(source.capture - source.markers) << "/"
                      << qpc_ms(source.captured - source.capture) << "/"
                      << qpc_ms(source.present - source.captured) << "/"
                      << qpc_ms(source.end - source.present) << "/" << qpc_ms(source.end - source.begin)
                      << '\n';
        }
        std::cout << "SDK status: real=" << status.real_frames << ", displayed=" << status.displayed
                  << ", factor=" << status.factor << ", failures=" << status.failures << '\n';
        require(!failed && !status.failures, "The timing run encountered a native/provider failure");
        std::cout << "PASS: bounded timing audit completed without GPU audit copies. These are native "
                     "Present submissions/CPU timings, not scanout times or interpolation-quality proof. "
                     "Async source IDs are thread hints only; cold source1-10 is excluded from summaries. "
                     "No target framerate or gap threshold is asserted.\n";
    }
    bool stop_and_wait() {
        {
            std::lock_guard lock(mutex);
            enabled = false;
        }
        const auto deadline = GetTickCount64() + 5000;
        while (in_flight && GetTickCount64() < deadline)
            Sleep(1);
        return in_flight == 0;
    }
    void finish(unsigned submitted, unsigned backend, const xivrender::fg::Status &status) {
        require(stop_and_wait(), "An intercepted native Present did not return within five seconds");
        std::lock_guard lock(mutex);
        require(error.empty(), error);
        require(chain && queue && (timing_only || fence),
                "The native DXGI swapchain was not intercepted; no output proof");
        require(!samples.empty(), "No actual native Presents were observed");
        if (timing_only) {
            finish_timing(submitted, status);
            return;
        }
        Event event;
        require(event.handle != nullptr, "Native audit completion event creation failed");
        checked(fence->SetEventOnCompletion(serial, event.handle), "Native audit completion fence");
        require(WaitForSingleObject(event.handle, 5000) == WAIT_OBJECT_0 &&
                    fence->GetCompletedValue() >= serial,
                "Native output audit did not retire within five seconds");
        unsigned observations = 0, decoded = 0, ambiguous = 0, regressions = 0, motion_regressions = 0;
        unsigned previous_code = 0, largest_code = 0, first_code = 0;
        unsigned motion_samples = 0;
        unsigned motion_repeats = 0, real_grid_positions = 0, intermediate_positions = 0;
        unsigned motion_advances = 0;
        double minimum_advance = std::numeric_limits<double>::infinity(), maximum_advance = 0;
        double previous_centroid = -1;
        std::vector<double> centroid_positions;
        std::vector<bool> seen(submitted + 1);
        for (auto &sample : samples) {
            require(SUCCEEDED(sample->present_result), "An intercepted native Present failed");
            void *mapped = nullptr;
            D3D12_RANGE read{0, std::size_t(sample->second_row) + sample->row_pitch};
            checked(sample->pixels->Map(0, &read, &mapped), "Native output audit map");
            const auto *pixels = static_cast<const unsigned char *>(mapped);
            const bool bgra = sample->format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                              sample->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            const auto rgb = [&](unsigned x, unsigned row) {
                const auto *p = pixels + (row ? sample->second_row : 0) + std::size_t(x) * 4;
                return std::array<unsigned, 3>{p[bgra ? 2 : 0], p[1], p[bgra ? 0 : 2]};
            };
            unsigned code = 0;
            bool exact_code = true;
            for (unsigned bit = 0; bit < 16; ++bit) {
                const auto pixel = rgb(8 + bit * 8 + 4, 0);
                const int difference = int(pixel[0]) - int(pixel[2]);
                // A blended/generated barcode is deliberately not mistaken
                // for an exact real-frame identifier.
                if (std::abs(difference) < 160 || pixel[1] > 50)
                    exact_code = false;
                if (difference > 0)
                    code |= 1u << bit;
            }
            double weight = 0, centroid = 0;
            for (unsigned x = 0; x < sample->width; ++x) {
                const auto pixel = rgb(x, 1);
                const auto value = std::max(0, int(pixel[1]) - 40);
                weight += value;
                centroid += double(x) * value;
            }
            D3D12_RANGE written{0, 0};
            sample->pixels->Unmap(0, &written);
            ++observations;
            if (exact_code && code && code <= submitted) {
                if (!first_code)
                    first_code = code;
                if (previous_code && code < previous_code) {
                    ++regressions;
                    std::cout << "Barcode regression at native Present " << sample->ordinal << ": "
                              << previous_code << " -> " << code << '\n';
                }
                previous_code = code;
                largest_code = std::max(largest_code, code);
                seen[code] = true;
                ++decoded;
            } else {
                ++ambiguous;
            }
            if (weight > 100) {
                ++motion_samples;
                centroid /= weight;
                centroid_positions.push_back(centroid);
                const double frame_position =
                    (centroid - double(foreground_origin) - foreground_half_width) / double(foreground_step);
                const double real_grid_distance =
                    std::abs(frame_position - std::round(frame_position)) * double(foreground_step);
                // Source real frames are exactly on this grid. Count only
                // clearly separated intermediate positions; centroid motion
                // alone does not distinguish interpolation from crossfading.
                if (real_grid_distance <= position_tolerance)
                    ++real_grid_positions;
                else if (frame_position >= 1 && frame_position <= submitted && real_grid_distance >= .5)
                    ++intermediate_positions;
                if (previous_centroid >= 0 && centroid < previous_centroid - 1.0) {
                    ++motion_regressions;
                    std::cout << "Foreground regression at native Present " << sample->ordinal << ": "
                              << previous_centroid << " -> " << centroid << '\n';
                }
                if (previous_centroid >= 0) {
                    const double advance = centroid - previous_centroid;
                    if (std::abs(advance) <= position_tolerance)
                        ++motion_repeats;
                    else if (advance > position_tolerance) {
                        ++motion_advances;
                        minimum_advance = std::min(minimum_advance, advance);
                        maximum_advance = std::max(maximum_advance, advance);
                    }
                }
                previous_centroid = centroid;
            }
        }
        auto distinct_positions = centroid_positions;
        std::sort(distinct_positions.begin(), distinct_positions.end());
        unsigned distinct = 0;
        double cluster_start = -1;
        for (const auto position : distinct_positions) {
            if (!distinct || position - cluster_start > position_tolerance) {
                ++distinct;
                cluster_start = position;
            }
        }
        const auto unique = std::count(seen.begin(), seen.end(), true);
        std::cout << "Native output audit: Presents=" << observations
                  << ", exact barcode observations=" << decoded << ", unique source stamps=" << unique << "/"
                  << submitted << ", first/last=" << first_code << "/" << largest_code
                  << ", ambiguous/interpolated=" << ambiguous << ", barcode regressions=" << regressions
                  << ", foreground observations=" << motion_samples
                  << ", foreground regressions=" << motion_regressions << '\n';
        std::cout << "Motion diagnostics (" << foreground_step << "px source step, " << position_tolerance
                  << "px repeat/grid tolerance): distinct positions=" << distinct
                  << ", consecutive repeats=" << motion_repeats
                  << ", real-grid observations=" << real_grid_positions
                  << ", clearly intermediate observations=" << intermediate_positions
                  << ", forward advances=" << motion_advances
                  << ", min/max forward step=" << (motion_advances ? minimum_advance : 0) << "/"
                  << maximum_advance << '\n';
        std::cout << "First foreground centroids:";
        for (std::size_t index = 0; index < std::min<std::size_t>(16, centroid_positions.size()); ++index)
            std::cout << " " << centroid_positions[index];
        std::cout << "\nMotion interpolation metrics are diagnostic only: repeated real frames can satisfy "
                     "SDK/native Present counts, and intermediate centroids can also result from blending.\n";
        std::cout << "SDK status: real=" << status.real_frames << ", displayed=" << status.displayed
                  << ", factor=" << status.factor << ", native dynamic=" << status.native_dynamic
                  << ", failures=" << status.failures << '\n';
        require(decoded >= submitted / 2 && largest_code >= submitted - 3,
                "Actual native output did not contain enough advancing source frames");
        require(motion_samples >= submitted / 2 &&
                    previous_centroid >=
                        double(foreground_origin + foreground_step * submitted) + foreground_half_width - 6,
                "Actual native output did not contain the advancing foreground");
        require(!regressions && !motion_regressions, "Actual native output reverted to an older frame");
        require(!status.failures, "The provider reported a frame-generation failure");
        if (backend == unsigned(xivrender::FrameGeneration::DlssG)) {
            std::cout << "DLSS-G is intentionally unfocused: this proves native real-output sequence only, "
                         "not visible interpolation or generated-frame throughput.\n";
        } else {
            require(status.displayed > status.real_frames + 3,
                    "No sustained SDK-generated frames; output ordering alone is insufficient");
            require(observations > submitted + 3,
                    "No additional native output Presents were observed; generated output is unproven");
        }
        std::cout << "PASS: actual native Present rows advance without old-frame reversal. This is a "
                     "correctness test with extra copy work, not a performance benchmark.\n";
    }
    void clear_owners() {
        if (!stop_and_wait()) {
            // Never release command allocators/resources while a stuck native
            // Present can still reference its sample. The executable exits
            // with failure instead of trying an unsafe SDK teardown.
            std::cerr << "FAIL: Native Present audit teardown timed out\n";
            TerminateProcess(GetCurrentProcess(), 1);
            return;
        }
        std::lock_guard lock(mutex);
        enabled = false;
        // The presenter's private D3D12 runtime must outlive all these COM
        // objects, including the per-Present readback resources.
        samples.clear();
        chain.Reset();
        fence.Reset();
        queue.Reset();
        device.Reset();
    }
};
Audit *audit = nullptr;
using CreateHwnd = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, HWND,
                                                const DXGI_SWAP_CHAIN_DESC1 *,
                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *,
                                                IDXGISwapChain1 **);
using CreateLegacy = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *,
                                                  IDXGISwapChain **);
using Present = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using Present1 = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
CreateHwnd create_hwnd = nullptr;
CreateLegacy create_legacy = nullptr;
Present native_present = nullptr;
Present1 native_present1 = nullptr;
void *present_address = nullptr, *present1_address = nullptr;
thread_local bool inside_present = false;
HRESULT STDMETHODCALLTYPE intercepted_present(IDXGISwapChain *chain, UINT interval, UINT flags) {
    if (inside_present)
        return native_present(chain, interval, flags);
    inside_present = true;
    auto *sample = audit ? audit->before(chain, flags) : nullptr;
    const auto result = native_present(chain, interval, flags);
    if (audit)
        audit->after(sample, result);
    inside_present = false;
    return result;
}
HRESULT STDMETHODCALLTYPE intercepted_present1(IDXGISwapChain1 *chain, UINT interval, UINT flags,
                                               const DXGI_PRESENT_PARAMETERS *parameters) {
    if (inside_present)
        return native_present1(chain, interval, flags, parameters);
    inside_present = true;
    auto *sample = audit ? audit->before(chain, flags) : nullptr;
    const auto result = native_present1(chain, interval, flags, parameters);
    if (audit)
        audit->after(sample, result);
    inside_present = false;
    return result;
}
void install_hook(void *address, void *replacement, void **original, const char *name) {
    require(MH_CreateHook(address, replacement, original) == MH_OK, std::string(name) + " hook failed");
    require(MH_EnableHook(address) == MH_OK, std::string(name) + " hook enable failed");
}
void Audit::attach(IUnknown *creating_device, IDXGISwapChain *created) {
    try {
        ComPtr<ID3D12CommandQueue> actual_queue;
        if (FAILED(creating_device->QueryInterface(IID_PPV_ARGS(&actual_queue))))
            return; // Ignore any native D3D11 carrier, if one is created.
        DXGI_SWAP_CHAIN_DESC description{};
        checked(created->GetDesc(&description), "Native output description");
        if (description.OutputWindow != window)
            return;
        std::lock_guard lock(mutex);
        if (chain && created == static_cast<IDXGISwapChain *>(chain.Get()))
            return; // Legacy CreateSwapChain may delegate to CreateSwapChainForHwnd.
        require(!chain, "More than one native output chain was created for the probe HWND");
        checked(created->QueryInterface(IID_PPV_ARGS(&chain)), "Native output IDXGISwapChain4");
        queue = actual_queue;
        checked(queue->GetDevice(IID_PPV_ARGS(&device)), "Native output queue device");
        if (!timing_only)
            checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                    "Native audit fence");
        auto **table = *reinterpret_cast<void ***>(created);
        if (!present_address) {
            present_address = table[8];
            install_hook(present_address, reinterpret_cast<void *>(intercepted_present),
                         reinterpret_cast<void **>(&native_present), "Native Present");
            ComPtr<IDXGISwapChain1> chain1;
            checked(created->QueryInterface(IID_PPV_ARGS(&chain1)), "Native output IDXGISwapChain1");
            present1_address = (*reinterpret_cast<void ***>(chain1.Get()))[22];
            install_hook(present1_address, reinterpret_cast<void *>(intercepted_present1),
                         reinterpret_cast<void **>(&native_present1), "Native Present1");
        }
        std::cout << "Captured native D3D12 chain and creating queue for the hidden probe HWND\n";
    } catch (const std::exception &failure) {
        std::lock_guard lock(mutex);
        error = failure.what();
        enabled = false;
    }
}
HRESULT STDMETHODCALLTYPE intercepted_create_hwnd(IDXGIFactory2 *factory, IUnknown *device, HWND window,
                                                  const DXGI_SWAP_CHAIN_DESC1 *description,
                                                  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen,
                                                  IDXGIOutput *restrict_to, IDXGISwapChain1 **chain) {
    const auto result = create_hwnd(factory, device, window, description, fullscreen, restrict_to, chain);
    if (SUCCEEDED(result) && chain && *chain && audit && window == audit->window)
        audit->attach(device, *chain);
    return result;
}
HRESULT STDMETHODCALLTYPE intercepted_create_legacy(IDXGIFactory *factory, IUnknown *device,
                                                    DXGI_SWAP_CHAIN_DESC *description,
                                                    IDXGISwapChain **chain) {
    const auto result = create_legacy(factory, device, description, chain);
    if (SUCCEEDED(result) && chain && *chain && description && audit &&
        description->OutputWindow == audit->window)
        audit->attach(device, *chain);
    return result;
}
struct Hooks {
    explicit Hooks(IDXGIFactory2 *factory) {
        require(MH_Initialize() == MH_OK, "Test-only DXGI hook initialization failed");
        auto **table = *reinterpret_cast<void ***>(factory);
        install_hook(table[15], reinterpret_cast<void *>(intercepted_create_hwnd),
                     reinterpret_cast<void **>(&create_hwnd), "Native CreateSwapChainForHwnd");
        install_hook(table[10], reinterpret_cast<void *>(intercepted_create_legacy),
                     reinterpret_cast<void **>(&create_legacy), "Native CreateSwapChain");
    }
    ~Hooks() {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
};
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}
ComPtr<ID3D11Texture2D> texture(ID3D11Device *device, unsigned width, unsigned height, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> result;
    checked(device->CreateTexture2D(&desc, nullptr, &result), "Synthetic input texture");
    return result;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::cout << std::unitbuf;
    HWND window = nullptr;
    try {
        require(argc >= 3, "Usage: fg_sequence_probe runtime backend[1=DLSSG/2=XeFG/3=FSR] "
                           "[factor=2] [frames=120] [width=1280] [height=720] "
                           "[--dynamic] [--timing-only]");
        const unsigned backend = unsigned(_wtoi(argv[2]));
        const unsigned factor = argc > 3 ? unsigned(_wtoi(argv[3])) : 2;
        const unsigned frames = argc > 4 ? unsigned(_wtoi(argv[4])) : 120;
        const unsigned width = argc > 5 ? unsigned(_wtoi(argv[5])) : 1280;
        const unsigned height = argc > 6 ? unsigned(_wtoi(argv[6])) : 720;
        bool dynamic = false, timing_only = false;
        for (int index = 7; index < argc; ++index) {
            const std::wstring_view flag(argv[index]);
            require(flag == L"--dynamic" || flag == L"--timing-only", "Unknown sequence probe option");
            dynamic |= flag == L"--dynamic";
            timing_only |= flag == L"--timing-only";
        }
        require(backend >= 1 && backend <= 3 && factor >= 2 && factor <= 6,
                "Invalid provider or requested FG multiplier");
        require(backend != 3 || factor == 2, "FSR FG currently supports 2X only");
        require(frames >= 30 && frames <= (timing_only ? 3600u : 240u) && width >= 640 && width <= 1920 &&
                    height >= 360 && height <= 1080 &&
                    (timing_only || foreground_origin + foreground_step * frames + foreground_width < width),
                "Invalid sequence dimensions/count; the foreground stripe must remain onscreen");
        WNDCLASSW wc{};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"XIVRender hidden native output sequence audit";
        RegisterClassW(&wc);
        window =
            CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, wc.lpszClassName, WS_POPUP,
                            -32000, -32000, int(width), int(height), nullptr, nullptr, wc.hInstance, nullptr);
        require(window != nullptr, "Hidden sequence probe window creation failed");
        require(!IsWindowVisible(window), "The sequence probe must never show its window");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &device, nullptr, &context),
                "Sequence probe D3D11 device");
        ComPtr<IDXGIFactory2> factory;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Sequence probe DXGI factory");
        Audit observer;
        observer.window = window;
        observer.timing_only = timing_only;
        observer.sample_limit = timing_only ? frames * 6 + 64 : Audit::maximum_samples;
        if (timing_only) {
            observer.samples.reserve(observer.sample_limit);
            observer.sources.reserve(frames);
        }
        observer.expected_width = width;
        observer.expected_height = height;
        audit = &observer;
        Hooks hooks(factory.Get());
        xivrender::Settings settings;
        settings.fg.enabled = true;
        settings.fg.backend = static_cast<xivrender::FrameGeneration>(backend);
        settings.fg.factor = factor;
        settings.fg.dynamic = dynamic;
        settings.fg.target_fps = 138;
        settings.hdr.enabled = false;
        xivrender::fg::configure(settings, std::filesystem::absolute(argv[1]), true);
        xivrender::hdr::configure(settings, true);
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = width;
        desc.BufferDesc.Height = height;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        std::string error;
        {
            xivrender::fg::Presenter output;
            struct AuditOwners {
                Audit &value;
                ~AuditOwners() {
                    value.clear_owners();
                }
            } owners{observer};
            require(output.initialize(device.Get(), factory.Get(), window, desc, DXGI_FORMAT_R8G8B8A8_UNORM,
                                      error),
                    error);
            require(xivrender::fg::status().available, "Provider initialization silently fell back");
            constexpr unsigned depth_x = 13, depth_y = 7, motion_x = 19, motion_y = 11;
            const unsigned guide_width = width * 2257u / 3840u, guide_height = height * 1270u / 2160u;
            const unsigned padded_width = (guide_width + motion_x + 63u) & ~63u;
            const unsigned padded_height = (guide_height + motion_y + 63u) & ~63u;
            auto scene = texture(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
            auto depth = texture(device.Get(), padded_width, padded_height, DXGI_FORMAT_R32_FLOAT);
            auto motion = texture(device.Get(), padded_width, padded_height, DXGI_FORMAT_R32G32_FLOAT);
            ComPtr<ID3D11Resource> final_resource;
            output.target()->GetResource(&final_resource);
            ComPtr<ID3D11Texture2D> final;
            checked(final_resource.As(&final), "Private final texture");
            std::vector<std::array<unsigned char, 4>> pixels(std::size_t(width) * height);
            std::vector<float> depths(std::size_t(padded_width) * padded_height);
            std::vector<std::array<float, 2>> vectors(std::size_t(padded_width) * padded_height);
            xivrender::fg::Camera camera;
            using namespace DirectX;
            XMFLOAT4X4 view, projection;
            XMStoreFloat4x4(&view, XMMatrixIdentity());
            camera.near_plane = .1f;
            camera.far_plane = std::numeric_limits<float>::infinity();
            camera.fov = 1.f;
            camera.aspect = float(width) / float(height);
            camera.inverted = camera.infinite = camera.valid = true;
            const float cot = 1.f / std::tan(.5f);
            XMStoreFloat4x4(&projection, XMMatrixSet(cot / camera.aspect, 0, 0, 0, 0, cot, 0, 0, 0, 0, 0, -1,
                                                     0, 0, .1f, 0));
            std::memcpy(camera.view.data(), &view, sizeof(view));
            std::memcpy(camera.projection.data(), &projection, sizeof(projection));
            unsigned previous_position = foreground_origin;
            for (unsigned frame = 1; frame <= frames; ++frame) {
                SourceTiming timing;
                if (timing_only) {
                    timing.frame = frame;
                    timing.begin = qpc_now();
                    source_frame_hint = frame;
                    latest_source_frame = frame;
                }
                require(output.synchronize(error), error);
                unsigned position = foreground_origin + foreground_step * frame;
                if (timing_only) {
                    // Long timing runs smoothly reverse direction at the edge;
                    // no artificial history reset or teleport every screenful.
                    const auto travel = (width - foreground_origin - foreground_width - 1) / foreground_step;
                    const auto phase = (frame - 1) % (travel * 2);
                    position =
                        foreground_origin + foreground_step * (phase <= travel ? phase : 2 * travel - phase);
                }
                std::fill(pixels.begin(), pixels.end(), std::array<unsigned char, 4>{13, 13, 13, 255});
                for (unsigned y = 24; y < height; ++y)
                    std::fill_n(pixels.data() + std::size_t(y) * width + position, foreground_width,
                                std::array<unsigned char, 4>{40, 230, 40, 255});
                std::fill(depths.begin(), depths.end(), .125f);
                std::fill(vectors.begin(), vectors.end(), std::array<float, 2>{0, 0});
                for (unsigned y = 0; y < guide_height; ++y)
                    for (unsigned x = 0; x < guide_width; ++x) {
                        const float output_x = (float(x) + .5f) * float(width) / float(guide_width);
                        const bool foreground =
                            output_x >= float(position) && output_x < float(position + foreground_width);
                        depths[std::size_t(depth_y + y) * padded_width + depth_x + x] =
                            foreground ? .8f : .2f;
                        if (foreground && frame > 1)
                            vectors[std::size_t(motion_y + y) * padded_width + motion_x + x] = {
                                float(int(previous_position) - int(position)) / float(width), 0};
                    }
                previous_position = position;
                context->UpdateSubresource(scene.Get(), 0, nullptr, pixels.data(), width * 4, 0);
                context->UpdateSubresource(depth.Get(), 0, nullptr, depths.data(), padded_width * 4, 0);
                context->UpdateSubresource(motion.Get(), 0, nullptr, vectors.data(), padded_width * 8, 0);
                if (timing_only)
                    timing.markers = qpc_now();
                xivrender::fg::simulation_begin();
                xivrender::fg::camera(camera);
                xivrender::fg::simulation_end();
                xivrender::fg::render_begin();
                xivrender::nr::FrameInput input;
                input.frame_id = frame;
                input.sr_feature = &output;
                input.reset = frame == 1;
                auto &i = input.images;
                i.color = scene.Get();
                i.depth = depth.Get();
                i.motion = motion.Get();
                i.output_width = width;
                i.output_height = height;
                i.guide_width = i.motion_width = guide_width;
                i.guide_height = i.motion_height = guide_height;
                i.depth_x = depth_x;
                i.depth_y = depth_y;
                i.motion_x = motion_x;
                i.motion_y = motion_y;
                i.motion_scale_x = float(width);
                i.motion_scale_y = float(height);
                i.depth_inverted = true;
                if (timing_only)
                    timing.capture = qpc_now();
                xivrender::fg::capture(input);
                if (timing_only)
                    timing.captured = qpc_now();
                for (unsigned bit = 0; bit < 16; ++bit)
                    for (unsigned y = 4; y < 20; ++y)
                        std::fill_n(pixels.data() + std::size_t(y) * width + 8 + bit * 8, 8,
                                    frame & (1u << bit) ? std::array<unsigned char, 4>{230, 13, 13, 255}
                                                        : std::array<unsigned char, 4>{13, 13, 230, 255});
                context->UpdateSubresource(final.Get(), 0, nullptr, pixels.data(), width * 4, 0);
                if (timing_only)
                    timing.present = qpc_now();
                checked(output.present(0, DXGI_PRESENT_ALLOW_TEARING, true), "Provider Present");
                if (timing_only) {
                    timing.end = qpc_now();
                    observer.sources.push_back(timing);
                    source_frame_hint = 0;
                }
                if (!timing_only && frame % 30 == 0) {
                    const auto status = xivrender::fg::status();
                    std::cout << "Submitted " << frame << ", SDK displayed=" << status.displayed
                              << ", factor=" << status.factor << ", failures=" << status.failures << '\n';
                }
                Sleep(16);
            }
            require(output.drain(5000, error), error);
            // Allow an asynchronous SDK pacer to finish its final present. Do
            // not infer display completion merely from the app queue's fence.
            std::size_t previous = 0;
            unsigned stable = 0;
            for (unsigned wait = 0; wait < 30 && stable < 3; ++wait) {
                Sleep(40);
                std::lock_guard lock(observer.mutex);
                const auto count = observer.samples.size();
                stable = count == previous ? stable + 1 : 0;
                previous = count;
            }
            observer.finish(frames, backend, xivrender::fg::status());
            context->ClearState();
            context->Flush();
        }
        audit = nullptr;
        DestroyWindow(window);
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << "FAIL: " << failure.what() << "; provider=" << xivrender::fg::status().message << '\n';
        audit = nullptr;
        if (window)
            DestroyWindow(window);
        return 1;
    }
}
