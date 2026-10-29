#include "xivrender/ffxiv_signatures.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace xivrender::ffxiv;
namespace {
template <class T> void put(std::vector<std::uint8_t> &b, std::size_t at, T value) {
    assert(at + sizeof(T) <= b.size());
    std::memcpy(b.data() + at, &value, sizeof(T));
}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> b(0x600);
    put<std::uint16_t>(b, 0, 0x5a4d);
    put<std::uint32_t>(b, 0x3c, 0x80);
    put<std::uint32_t>(b, 0x80, 0x4550);
    put<std::uint16_t>(b, 0x84, 0x8664);
    put<std::uint16_t>(b, 0x86, 2);
    put<std::uint16_t>(b, 0x94, 240);
    const auto o = 0x98;
    put<std::uint16_t>(b, o, 0x20b);
    put<std::uint64_t>(b, o + 24, 0x140000000);
    put<std::uint32_t>(b, o + 56, 0x4000);
    put<std::uint32_t>(b, o + 60, 0x200);
    put<std::uint32_t>(b, o + 108, 16);
    const auto s = o + 240;
    std::memcpy(b.data() + s, ".text", 5);
    put<std::uint32_t>(b, s + 8, 0x200);
    put<std::uint32_t>(b, s + 12, 0x1000);
    put<std::uint32_t>(b, s + 16, 0x200);
    put<std::uint32_t>(b, s + 20, 0x200);
    put<std::uint32_t>(b, s + 36, 0x60000020);
    std::memcpy(b.data() + s + 40, ".data", 5);
    put<std::uint32_t>(b, s + 48, 0x400);
    put<std::uint32_t>(b, s + 52, 0x2000);
    put<std::uint32_t>(b, s + 56, 0x200);
    put<std::uint32_t>(b, s + 60, 0x400);
    put<std::uint32_t>(b, s + 76, 0xc0000040);
    return b;
}
void patterns() {
    const std::array<std::uint8_t, 8> bytes{0x48, 0x8b, 0x05, 0x7f, 0x48, 0x8b, 0x05, 0x80};
    for (auto p : {"", " ", "?? ?", "4", "GG", "48,8B", "4?", "?4", "48 0x8B"})
        assert(!find_pattern(bytes, p).valid);
    auto result = find_pattern(bytes, "48 8b 05 ??");
    assert(result.valid && result.offsets == std::vector<std::size_t>({0, 4}));
    result = find_pattern(bytes, "7F 48");
    assert(result.valid && result.offsets == std::vector<std::size_t>({3}));
    assert(find_pattern(std::span(bytes).subspan(0, 3), "48 8B 05 7F").offsets.empty());
    assert(find_pattern({}, "48").valid && find_pattern({}, "48").offsets.empty());
    assert(find_pattern(bytes, "05 80").offsets == std::vector<std::size_t>({6}));
    for (std::size_t n = 0; n <= bytes.size(); ++n) {
        auto view = std::span(bytes).subspan(0, n);
        auto r = find_pattern(view, "48 8B 05 ??");
        for (auto at : r.offsets)
            assert(at + 4 <= n);
    }
    std::string huge;
    for (int i = 0; i < 4097; ++i)
        huge += "48 ";
    assert(!find_pattern(bytes, huge).valid);
}
void images() {
    const auto b = fixture();
    auto image = parse_pe(b);
    assert(image && image->image_size == 0x4000 && image->sections.size() == 2);
    assert(image->read(0x1000, 0x200).size() == 0x200);
    assert(image->read(0x11ff, 2).empty());
    assert(image->read(0x1200, 1).empty());
    assert(image->read(0xffffffff, 8).empty());
    assert(image->executable(0x1000, 0x200) && !image->executable(0x11ff, 2));
    assert(image->writable(0x2200, 8));
    assert(image->read(0x2200, 8).empty());
    assert(!image->writable(0x1000, 8));
    assert(!image->writable(0x23ff, 8));
    for (std::size_t n = 0; n < 0x200; ++n)
        assert(!parse_pe(std::span(b).subspan(0, n)));
    auto bad = b;
    put<std::uint32_t>(bad, 0x3c, 0xffffffff);
    assert(!parse_pe(bad));
    bad = b;
    put<std::uint16_t>(bad, 0x86, 97);
    assert(!parse_pe(bad));
    bad = b;
    put<std::uint16_t>(bad, 0x84, 0x14c);
    assert(!parse_pe(bad));
    bad = b;
    put<std::uint16_t>(bad, 0x98, 0x10b);
    assert(!parse_pe(bad));
    bad = b;
    put<std::uint32_t>(bad, 0x98 + 56, 0xffffffff);
    assert(!parse_pe(bad));
    bad = b;
    put<std::uint32_t>(bad, 0x188 + 40 + 12, 0x1100);
    assert(!parse_pe(bad));
    bad = b;
    put<std::uint32_t>(bad, 0x188 + 40 + 20, 0x300);
    assert(!parse_pe(bad));
    bad = b;
    put<std::uint32_t>(bad, 0x188 + 16, 0xffffffff);
    assert(!parse_pe(bad));
    const auto report = inspect_executable(b);
    assert(report.valid_pe && !report.supported && !report.quality && !report.jitter && !report.previews &&
           !report.errors.empty());
}
} // namespace
int main(int argc, char **argv) {
    patterns();
    images();
    std::cout << "PASS: bounded masked patterns, malformed inputs, ambiguity, PE ranges, permissions and "
                 "native fallback\n";
    if (argc == 2) {
        std::ifstream stream(std::filesystem::path(argv[1]), std::ios::binary);
        assert(stream);
        std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(stream), {});
        const auto report = inspect_executable(bytes);
        for (const auto &s : report.evidence)
            std::cout << s << '\n';
        for (const auto &s : report.errors)
            std::cerr << s << '\n';
        const auto &a = report.addresses;
        std::cout << std::hex << "settings=" << a.settings_global << " renderer=" << a.renderer_global
                  << " manager=" << a.render_manager_global << " device=" << a.device_global
                  << " query=" << a.query_size << " update=" << a.renderer_update
                  << " dispatcher=" << a.resize_dispatch << " getter=" << a.jitter_getter
                  << " packetReturn=" << a.jitter_packet_return << " Halton=" << a.halton_generator
                  << " camera=" << a.camera_update << " cameraReturn=" << a.halton_camera_return
                  << " preview=" << a.preview_accessor << std::dec << '\n';
        assert(report.supported);
        // A duplicate otherwise-valid selector must not choose the first match.
        const auto raw = parse_pe(bytes);
        assert(raw);
        const auto selector = find_pattern(raw->read(a.query_size, 128), "48 89 5C 24 08");
        assert(selector.valid);
        const auto settings = find_pattern(bytes, "4C 8B 05 ?? ?? ?? ?? 45 33 E4 41 80 78 54 02 74 06 45 38 "
                                                  "60 44 74 71 45 38 60 45 74 6B 41 0F B6 40 55 83 F8 06");
        assert(settings.offsets.size() == 1);
        auto ambiguous = bytes;
        const auto text = std::find_if(raw->sections.begin(), raw->sections.end(),
                                       [](const auto &s) { return (s.characteristics & 0x20000000) != 0; });
        assert(text != raw->sections.end() && text->raw_size >= 64);
        std::copy_n(bytes.data() + settings.offsets[0], 37,
                    ambiguous.data() + text->raw_offset + text->raw_size - 64);
        const auto duplicate = inspect_executable(ambiguous);
        assert(!duplicate.quality && !duplicate.supported);
        // Break the current packet->getter branch without touching any file.
        const auto image = parse_pe(bytes);
        assert(image);
        assert(a.game_main && a.frontend_reference && a.frontend_state_reference &&
               a.feature_descriptor_compare);
        // A frontend layout/reference drift withholds only menu scaling. The
        // independently verified native world SR and jitter paths survive.
        const auto state_operand = image->read(a.frontend_state_reference + 2, 4);
        assert(state_operand.size() == 4);
        auto changed_frontend = bytes;
        const auto state_offset = std::size_t(state_operand.data() - bytes.data());
        put<std::int32_t>(changed_frontend, state_offset, 0);
        const auto frontend_report = inspect_executable(changed_frontend);
        assert(frontend_report.quality && frontend_report.jitter && !frontend_report.addresses.game_main);
        // A changed native feature-cache field must withhold recreation, not
        // write an obsolete context offset just because other hooks survived.
        const auto descriptor_operand = image->read(a.feature_descriptor_compare + 9, 1);
        assert(descriptor_operand.size() == 1 && descriptor_operand[0] == 0xcc);
        auto changed_descriptor = bytes;
        changed_descriptor[std::size_t(descriptor_operand.data() - bytes.data())] = 0xcd;
        assert(!inspect_executable(changed_descriptor).quality);
        const auto original = image->read(a.jitter_packet_return - 4, 4);
        const auto offset = std::size_t(original.data() - bytes.data());
        auto damaged = bytes;
        std::fill_n(damaged.begin() + offset, 4, std::uint8_t{0x7f});
        assert(!inspect_executable(damaged).jitter);
    }
}
