#include "xivrender/fg_policy.hpp"
#include "xivrender/fg_frame_tokens.hpp"
#include "xivrender/fg_present_count.hpp"
#include "xivrender/pointer_lease.hpp"
#include <cassert>
#include <iostream>
int main() {
    using namespace xivrender;
    using namespace xivrender::fg;
    assert(presentation_flags(FrameGeneration::XeFG, 0, 0, true) == DXGI_PRESENT_ALLOW_TEARING);
    assert(presentation_flags(FrameGeneration::XeFG, 1, DXGI_PRESENT_ALLOW_TEARING, true) == 0);
    assert(presentation_flags(FrameGeneration::XeFG, 0, 0, false) == 0);
    for (auto provider : {FrameGeneration::DlssG, FrameGeneration::FsrFG, FrameGeneration::Off})
        assert(presentation_flags(provider, 0, DXGI_PRESENT_DO_NOT_WAIT, true) == 0);
    FrameGenerationSettings latency;
    for (const auto provider : {FrameGeneration::Off, FrameGeneration::DlssG, FrameGeneration::XeFG, FrameGeneration::FsrFG}) {
        latency.backend = provider;
        for (int choice = 0; choice <= 4; ++choice) {
            latency.latency = static_cast<LatencyReduction>(choice);
            const FrameGeneration selected[]{FrameGeneration::DlssG, FrameGeneration::Off,
                FrameGeneration::DlssG, FrameGeneration::XeFG, FrameGeneration::FsrFG};
            latency.enabled = false; latency.reflex = true;
            assert(latency_provider(latency) == selected[choice]);
            assert(presentation_requested(latency) == (choice != 1));
            latency.reflex = false;
            assert(latency_provider(latency) == FrameGeneration::Off && !presentation_requested(latency));
            latency.enabled = true;
            const auto expected = provider == FrameGeneration::XeFG || provider == FrameGeneration::DlssG
                                  ? provider : provider == FrameGeneration::Off
                                      ? FrameGeneration::Off : selected[choice];
            assert(latency_provider(latency) == expected);
        }
    }
    assert(supported_format(FrameGeneration::DlssG, DXGI_FORMAT_R8G8B8A8_UNORM, false));
    assert(supported_format(FrameGeneration::XeFG, DXGI_FORMAT_R10G10B10A2_UNORM, true));
    assert(!supported_format(FrameGeneration::XeFG, DXGI_FORMAT_R16G16B16A16_FLOAT, true));
    assert(!supported_format(FrameGeneration::DlssG, DXGI_FORMAT_R16G16B16A16_FLOAT, true));
    assert(supported_format(FrameGeneration::FsrFG, DXGI_FORMAT_R16G16B16A16_FLOAT, true));
    assert(maximum_factor(FrameGeneration::FsrFG, 6, false) == 2);
    assert(maximum_factor(FrameGeneration::FsrFG, 5, false, true) == 4);
    assert(maximum_factor(FrameGeneration::FsrFG, 2, false, true) == 2);
    assert(maximum_factor(FrameGeneration::DlssG, 2, false) == 2);
    assert(maximum_factor(FrameGeneration::DlssG, 9, false) == 6);
    assert(maximum_factor(FrameGeneration::XeFG, 4, true) == 4);
    DynamicFactor dynamic;
    assert(dynamic.update(16, 144, 6, false, 6, 0) == 6);
    unsigned prior = 6; std::uint64_t last_change = 0;
    for (std::uint64_t now = 20; now <= 20000; now += 20) {
        const auto factor = dynamic.update(16, 144, 6, true, 6, now);
        if (factor != prior) {
            assert(factor + 1 == prior && now - last_change >= 3000);
            last_change = now; prior = factor;
        }
    }
    assert(prior == 3);
    assert(dynamic.update(4000, 144, 6, true, 6, 21000) == 3);
    for (std::uint64_t now = 21020; now < 24000; now += 20)
        assert(dynamic.update(40, 144, 6, true, 6, now) == 3);
    assert(dynamic.update(0, 144, 2, true, 6, 24001) == 2);
    assert(dynamic.update(16, 144, 6, false, 6, 24002) == 6);
    assert(dynamic.update(0, 144, 4, true, 6, 24003) == 4);
    DynamicFactor boundary;
    for (std::uint64_t now = 0; now < 20000; now += 16)
        assert(boundary.update(now % 32 ? 16 : 17, 120, 4, true, 2, now) == 2);
    HdrSettings output;
    assert(requested_output_format(output) == DXGI_FORMAT_R8G8B8A8_UNORM);
    output.enabled = true;
    assert(requested_output_format(output) == DXGI_FORMAT_R10G10B10A2_UNORM);
    output.output = HdrOutput::ScRgb16Float;
    assert(requested_output_format(output) == DXGI_FORMAT_R16G16B16A16_FLOAT);
    // Pending HDR requests must not change the retained output's color domain.
    output.paper_white_nits = 240;
    auto committed = committed_output_settings(output, DXGI_FORMAT_R8G8B8A8_UNORM);
    assert(!committed.enabled && committed.output == HdrOutput::Sdr8);
    assert(committed.paper_white_nits == 240);
    output.enabled = false;
    committed = committed_output_settings(output, DXGI_FORMAT_R10G10B10A2_UNORM);
    assert(committed.enabled && committed.output == HdrOutput::Pq10);
    committed = committed_output_settings(output, DXGI_FORMAT_R16G16B16A16_FLOAT);
    assert(committed.enabled && committed.output == HdrOutput::ScRgb16Float);
    void *slot = reinterpret_cast<void *>(std::uintptr_t(0x10000));
    PointerLease lease;
    assert(lease.acquire(&slot, reinterpret_cast<void *>(std::uintptr_t(0x10000)),
                         reinterpret_cast<void *>(std::uintptr_t(0x20000))));
    lease.release();
    assert(slot == reinterpret_cast<void *>(std::uintptr_t(0x10000)));
    assert(!lease.acquire(&slot, reinterpret_cast<void *>(std::uintptr_t(0x30000)),
                          reinterpret_cast<void *>(std::uintptr_t(0x20000))));
    assert(lease.acquire(&slot, reinterpret_cast<void *>(std::uintptr_t(0x10000)),
                         reinterpret_cast<void *>(std::uintptr_t(0x20000))));
    slot = reinterpret_cast<void *>(std::uintptr_t(0x40000));
    lease.release();
    assert(slot == reinterpret_cast<void *>(std::uintptr_t(0x40000)));
    struct Token {
        std::uint32_t frame = 0;
        explicit operator std::uint32_t() const {
            return frame;
        }
    };
    std::array<Token, 6> sdk_tokens{};
    unsigned issued = 0;
    FrameTokens<Token, 6> tokens;
    const auto issue = [&](std::uint32_t frame) {
        auto &token = sdk_tokens[issued++ % sdk_tokens.size()];
        token.frame = frame;
        return &token;
    };
    // A provider handoff cannot fabricate a token after simulation has ended.
    assert(!tokens.find(1) && !issued);
    for (std::uint64_t frame = 1; frame <= 64; ++frame) {
        auto *token = tokens.issue(frame, issue);
        assert(token && static_cast<std::uint32_t>(*token) == frame);
        assert(issued == frame);
        for (unsigned marker = 0; marker < 20; ++marker) {
            assert(tokens.find(frame) == token);
            if (frame > 1)
                assert(tokens.find(frame - 1));
            assert(tokens.issue(frame, issue) == token);
        }
        assert(issued == frame); // Render/marker interleaving never churns the SDK pool.
    }
    assert(!tokens.find(58) && !tokens.issue(58, issue) && issued == 64);
    // Fail closed if another SDK caller recycled a borrowed token unexpectedly.
    auto *newest = tokens.find(64);
    assert(newest);
    newest->frame = 999;
    assert(!tokens.find(64) && !tokens.issue(64, issue) && issued == 64);
    tokens.clear();
    assert(!tokens.find(63));
    assert(tokens.issue(100, issue) && issued == 65);
    // A real Present and its generated output are submitted asynchronously:
    // individual native deltas oscillate, but no displayed frame may be lost
    // and the completed window must still measure the correct 2X cadence.
    PresentationCount asynchronous;
    asynchronous.reset(0, 1000);
    assert(asynchronous.observe(1, 1010));
    const unsigned increments[]{1, 3, 0, 4, 1, 3, 2, 2};
    unsigned native_count = 1;
    for (unsigned index = 0; index < std::size(increments); ++index) {
        native_count += increments[index];
        assert(asynchronous.observe(native_count, 1010 + (index + 1) * 40));
        assert(asynchronous.ready() == (index + 1 == std::size(increments)));
    }
    assert(asynchronous.total() == 17 && asynchronous.ratio() == 2);
    // An enable/disable transition resets only the ratio window. Counts stay
    // monotonic and no earlier generated frame is reassigned to the new mode.
    asynchronous.reset_window(1400);
    assert(!asynchronous.ready() && asynchronous.total() == 17);
    assert(asynchronous.observe(++native_count, 1410));
    for (unsigned index = 1; index <= 8; ++index)
        assert(asynchronous.observe(++native_count, 1410 + index * 40));
    assert(asynchronous.ready() && asynchronous.ratio() == 1 && asynchronous.total() == 26);
    asynchronous.query_failed(1800);
    assert(!asynchronous.ready());
    native_count += 6;
    assert(asynchronous.observe(native_count, 1900));
    assert(!asynchronous.ready() && asynchronous.total() == 32);
    for (unsigned index = 1; index <= 8; ++index) {
        native_count += 2;
        assert(asynchronous.observe(native_count, 1900 + index * 40));
    }
    assert(asynchronous.ratio() == 2 && asynchronous.total() == 48);
    // UINT rollover is normal. A backwards/discontinuous counter is not.
    PresentationCount rollover;
    rollover.reset(0xfffffffcu, 1000);
    assert(rollover.observe(2, 1010) && rollover.total() == 6);
    assert(!rollover.observe(1, 1020) && rollover.total() == 6 && !rollover.ready());
    assert(rollover.observe(3, 1030) && rollover.total() == 8);
    std::cout << "FG format boundaries, runtime caps and sustained dynamic transitions passed\n";
}
