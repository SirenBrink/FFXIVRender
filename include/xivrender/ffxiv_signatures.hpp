#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xivrender::ffxiv {
struct PatternMatches {
    bool valid = false;
    std::vector<std::size_t> offsets;
    std::string error;
};
// Hex bytes separated by whitespace; only whole-byte ? / ?? wildcards.
// Empty/all-wildcard/malformed patterns are rejected. Search never reads past bytes.
PatternMatches find_pattern(std::span<const std::uint8_t> bytes, std::string_view pattern);
struct Section {
    std::uint32_t rva = 0, virtual_size = 0, raw_offset = 0, raw_size = 0, characteristics = 0;
    std::string name;
};
struct FunctionRange {
    std::uint32_t begin = 0, end = 0, unwind = 0;
};
struct ImageView {
    std::span<const std::uint8_t> bytes;
    std::uint32_t image_size = 0, exception_rva = 0, exception_size = 0;
    std::uint64_t image_base = 0;
    std::vector<Section> sections;
    std::span<const std::uint8_t> read(std::uint32_t rva, std::size_t length) const;
    bool executable(std::uint32_t rva, std::size_t length = 1) const;
    bool writable(std::uint32_t rva, std::size_t length = 8) const;
    std::vector<FunctionRange> functions() const;
};
std::optional<ImageView> parse_pe(std::span<const std::uint8_t> bytes, std::string *error = nullptr);
struct Addresses {
    std::uint32_t settings_global = 0, renderer_global = 0, render_manager_global = 0, device_global = 0;
    std::uint32_t query_size = 0, renderer_update = 0, resize_dispatch = 0;
    std::uint32_t feature_descriptor_compare = 0;
    std::uint32_t game_main = 0, frontend_reference = 0, frontend_state_reference = 0;
    std::uint32_t jitter_getter = 0, jitter_packet_return = 0, halton_generator = 0, halton_camera_return = 0,
                  camera_update = 0;
    std::uint32_t preview_accessor = 0, simulation_tick = 0, framework_vtable = 0;
    std::uint32_t framework_global = 0, framework_instance_reference = 0;
};
struct Report {
    bool valid_pe = false, quality = false, jitter = false, previews = false, supported = false;
    Addresses addresses;
    std::vector<std::string> evidence;
    std::vector<std::string> errors;
};
// Reads an unmodified on-disk PE file. All outputs are RVAs, never process pointers.
// No fixed-RVA fallback. Independent feature flags permit native fallback when one
// signature family changes; supported requires every required family to validate.
Report inspect_executable(std::span<const std::uint8_t> file);
} // namespace xivrender::ffxiv
