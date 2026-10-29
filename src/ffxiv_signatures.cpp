#include "xivrender/ffxiv_signatures.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace xivrender::ffxiv {
namespace {
constexpr std::uint32_t execute = 0x20000000, write = 0x80000000;
bool range(std::size_t offset, std::size_t length, std::size_t size) {
    return offset <= size && length <= size - offset;
}
template <class T> bool load(std::span<const std::uint8_t> data, std::size_t at, T &value) {
    if (!range(at, sizeof(T), data.size()))
        return false;
    std::memcpy(&value, data.data() + at, sizeof(T));
    return true;
}
std::string hex(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}
int digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
bool whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
std::optional<std::uint32_t> relative(const ImageView &image, std::uint32_t at, unsigned operand,
                                      unsigned length) {
    auto data = image.read(at, length);
    std::int32_t displacement = 0;
    if (!load(data, operand, displacement))
        return {};
    const auto target = std::int64_t(at) + length + displacement;
    if (target <= 0 || target >= image.image_size)
        return {};
    return std::uint32_t(target);
}
std::optional<std::uint32_t> unique(const ImageView &image, std::string_view pattern, std::string_view name,
                                    Report &report) {
    std::vector<std::uint32_t> hits;
    for (const auto &section : image.sections) {
        if (!(section.characteristics & execute))
            continue;
        auto data = image.read(section.rva, section.raw_size);
        auto result = find_pattern(data, pattern);
        if (!result.valid) {
            report.errors.push_back(std::string(name) + ": invalid built-in pattern");
            return {};
        }
        for (auto at : result.offsets)
            hits.push_back(section.rva + std::uint32_t(at));
    }
    if (hits.size() != 1) {
        report.errors.push_back(std::string(name) + ": expected one match, found " +
                                std::to_string(hits.size()));
        return {};
    }
    report.evidence.push_back(std::string(name) + " uniquely matched " + hex(hits[0]));
    return hits[0];
}
bool body(const ImageView &image, std::uint32_t at, std::string_view pattern) {
    const auto result = find_pattern(image.read(at, 128), pattern);
    return result.valid && !result.offsets.empty() && result.offsets[0] == 0;
}
std::uint32_t root(const ImageView &image, FunctionRange entry) {
    // MSVC divides large functions into chained runtime-function entries.
    for (unsigned depth = 0; depth < 16; ++depth) {
        auto header = image.read(entry.unwind, 4);
        if (header.size() != 4 || (header[0] & 7) != 1)
            return 0;
        if (!(header[0] & 0x20))
            return entry.begin;
        const auto offset = std::uint64_t(entry.unwind) + 4 + ((header[2] + 1) & ~1u) * 2;
        if (offset > std::numeric_limits<std::uint32_t>::max())
            return 0;
        auto chain = image.read(std::uint32_t(offset), 12);
        FunctionRange next;
        if (!load(chain, 0, next.begin) || !load(chain, 4, next.end) || !load(chain, 8, next.unwind) ||
            next.unwind == entry.unwind)
            return 0;
        entry = next;
    }
    return 0;
}
bool boundary(const std::vector<FunctionRange> &entries, std::uint32_t rva) {
    return std::any_of(entries.begin(), entries.end(), [&](const auto &e) { return e.begin == rva; });
}
bool root_calls(const ImageView &image, const std::vector<FunctionRange> &entries, std::uint32_t begin,
                std::uint32_t target) {
    for (const auto &e : entries) {
        if (root(image, e) != begin)
            continue;
        auto bytes = image.read(e.begin, e.end - e.begin);
        for (std::size_t i = 0; i + 5 <= bytes.size(); ++i)
            if (bytes[i] == 0xe8 && relative(image, e.begin + std::uint32_t(i), 1, 5) == target)
                return true;
    }
    return false;
}
} // namespace
PatternMatches find_pattern(std::span<const std::uint8_t> bytes, std::string_view pattern) {
    PatternMatches out;
    std::vector<int> mask;
    for (std::size_t i = 0; i < pattern.size();) {
        while (i < pattern.size() && whitespace(pattern[i]))
            ++i;
        if (i == pattern.size())
            break;
        const auto start = i;
        while (i < pattern.size() && !whitespace(pattern[i]))
            ++i;
        const auto token = pattern.substr(start, i - start);
        if (token == "?" || token == "??")
            mask.push_back(-1);
        else if (token.size() == 2 && digit(token[0]) >= 0 && digit(token[1]) >= 0)
            mask.push_back(digit(token[0]) * 16 + digit(token[1]));
        else {
            out.error = "invalid byte token";
            return out;
        }
        if (mask.size() > 4096) {
            out.error = "pattern exceeds 4096 bytes";
            return out;
        }
    }
    const auto anchor = std::find_if(mask.begin(), mask.end(), [](int v) { return v >= 0; });
    if (mask.empty() || anchor == mask.end()) {
        out.error = "empty or entirely wildcard pattern";
        return out;
    }
    out.valid = true;
    if (mask.size() > bytes.size())
        return out;
    const auto fixed = std::size_t(anchor - mask.begin());
    for (std::size_t at = 0; at <= bytes.size() - mask.size(); ++at) {
        if (bytes[at + fixed] != *anchor)
            continue;
        bool match = true;
        for (std::size_t i = 0; i < mask.size(); ++i)
            if (mask[i] >= 0 && bytes[at + i] != mask[i]) {
                match = false;
                break;
            }
        if (match)
            out.offsets.push_back(at);
    }
    return out;
}
std::span<const std::uint8_t> ImageView::read(std::uint32_t rva, std::size_t length) const {
    for (const auto &s : sections) {
        if (rva < s.rva)
            continue;
        const auto delta = std::uint64_t(rva) - s.rva;
        if (delta > s.raw_size || length > s.raw_size - delta)
            continue;
        const auto offset = std::uint64_t(s.raw_offset) + delta;
        if (offset > bytes.size() || length > bytes.size() - offset)
            return {};
        return bytes.subspan(std::size_t(offset), length);
    }
    return {};
}
bool ImageView::executable(std::uint32_t rva, std::size_t length) const {
    for (const auto &s : sections)
        if ((s.characteristics & execute) && rva >= s.rva &&
            range(std::uint64_t(rva) - s.rva, length, s.virtual_size))
            return true;
    return false;
}
bool ImageView::writable(std::uint32_t rva, std::size_t length) const {
    for (const auto &s : sections)
        if ((s.characteristics & write) && !(s.characteristics & execute) && rva >= s.rva &&
            range(std::uint64_t(rva) - s.rva, length, s.virtual_size))
            return true;
    return false;
}
std::vector<FunctionRange> ImageView::functions() const {
    std::vector<FunctionRange> result;
    if (exception_size % 12)
        return result;
    auto data = read(exception_rva, exception_size);
    if (data.size() != exception_size)
        return result;
    for (std::size_t i = 0; i + 12 <= data.size(); i += 12) {
        FunctionRange e;
        load(data, i, e.begin);
        load(data, i + 4, e.end);
        load(data, i + 8, e.unwind);
        if (e.begin >= e.end || !executable(e.begin, e.end - e.begin) || read(e.unwind, 4).size() != 4)
            return {};
        result.push_back(e);
    }
    return result;
}
std::optional<ImageView> parse_pe(std::span<const std::uint8_t> bytes, std::string *error) {
    auto fail = [&](const char *message) -> std::optional<ImageView> {
        if (error)
            *error = message;
        return {};
    };
    std::uint16_t mz = 0, machine = 0, count = 0, optional = 0, magic = 0;
    std::uint32_t pe = 0, signature = 0, headers = 0, directories = 0;
    if (!load(bytes, 0, mz) || mz != 0x5a4d || !load(bytes, 60, pe) || pe < 64 || pe > 0x100000)
        return fail("invalid DOS header");
    if (!load(bytes, pe, signature) || signature != 0x4550 || !load(bytes, pe + 4, machine) ||
        machine != 0x8664 || !load(bytes, pe + 6, count) || !count || count > 96 ||
        !load(bytes, pe + 20, optional) || optional < 240)
        return fail("invalid x64 PE header");
    const auto opt = std::size_t(pe) + 24;
    ImageView out;
    out.bytes = bytes;
    if (!range(opt, optional, bytes.size()) || !load(bytes, opt, magic) || magic != 0x20b ||
        !load(bytes, opt + 24, out.image_base) || !load(bytes, opt + 56, out.image_size) || !out.image_size ||
        out.image_size > 512u * 1024 * 1024 || !load(bytes, opt + 60, headers) || headers > bytes.size() ||
        !load(bytes, opt + 108, directories) || directories < 4)
        return fail("invalid PE32+ optional header");
    load(bytes, opt + 112 + 3 * 8, out.exception_rva);
    load(bytes, opt + 112 + 3 * 8 + 4, out.exception_size);
    const auto table = opt + optional;
    if (!range(table, std::size_t(count) * 40, bytes.size()) || table + std::size_t(count) * 40 > headers)
        return fail("truncated section table");
    for (unsigned i = 0; i < count; ++i) {
        const auto at = table + i * 40;
        Section s;
        const auto name = bytes.subspan(at, 8);
        s.name.assign(reinterpret_cast<const char *>(name.data()),
                      std::find(name.begin(), name.end(), 0) - name.begin());
        load(bytes, at + 8, s.virtual_size);
        load(bytes, at + 12, s.rva);
        load(bytes, at + 16, s.raw_size);
        load(bytes, at + 20, s.raw_offset);
        load(bytes, at + 36, s.characteristics);
        s.virtual_size = std::max(s.virtual_size, s.raw_size);
        if (!s.virtual_size || s.rva < headers || !range(s.rva, s.virtual_size, out.image_size) ||
            !range(s.raw_offset, s.raw_size, bytes.size()) || (s.raw_size && s.raw_offset < headers))
            return fail("invalid section range");
        for (const auto &old : out.sections) {
            if (std::uint64_t(s.rva) < std::uint64_t(old.rva) + old.virtual_size &&
                std::uint64_t(old.rva) < std::uint64_t(s.rva) + s.virtual_size)
                return fail("overlapping virtual sections");
            if (s.raw_size && old.raw_size &&
                std::uint64_t(s.raw_offset) < std::uint64_t(old.raw_offset) + old.raw_size &&
                std::uint64_t(old.raw_offset) < std::uint64_t(s.raw_offset) + s.raw_size)
                return fail("overlapping raw sections");
        }
        out.sections.push_back(std::move(s));
    }
    if (out.exception_size > 32u * 1024 * 1024 ||
        (out.exception_size && out.read(out.exception_rva, out.exception_size).size() != out.exception_size))
        return fail("invalid exception directory");
    return out;
}
Report inspect_executable(std::span<const std::uint8_t> file) {
    Report out;
    std::string error;
    auto parsed = parse_pe(file, &error);
    if (!parsed) {
        out.errors.push_back(error);
        return out;
    }
    out.valid_pe = true;
    const auto &image = *parsed;
    auto &a = out.addresses;
    const auto entries = image.functions();
    // Provenance: installed FFXIV 2026-10-20, verified again 2026-10-24:
    // SHA256 46a52ab1dbe80b9a8d6c4d4531bf66693904200c5135facacd053ef28a5de14a.
    // Displacements are wildcarded and decoded; RVAs below are never constants.
    const auto settings = unique(image,
                                 "4C 8B 05 ?? ?? ?? ?? 45 33 E4 41 80 78 54 02 74 06 45 38 60 44 74 71 45 38 "
                                 "60 45 74 6B 41 0F B6 40 55 83 F8 06",
                                 "native settings selector", out);
    const auto query =
        unique(image,
               "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 49 8B F9 "
               "49 8B F0 4C 8B F2 48 8B 98 20 42 00 00 48 85 DB 74 41 48 8B 05 ?? ?? ?? ?? 80 78 54 02",
               "native size query", out);
    const auto update = unique(image,
                               "4C 8B DC 57 48 81 EC D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 "
                               "60 F3 0F 10 81 10 07 00 00 0F 57 D2",
                               "renderer update", out);
    const auto dispatch = unique(image,
                                 "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8D 59 08 48 "
                                 "8B F1 48 8B CB FF 15 ?? ?? ?? ?? 8B 46 38 40 B5 01 48 8B 7E 30 85 C0 74 35",
                                 "resize dispatcher", out);
    const auto descriptor = unique(
        image,
        "41 B8 2C 00 00 00 48 8D 8F CC 00 00 00 48 8B D6 E8 ?? ?? ?? ?? 85 C0 74 44 48 8B CF E8 ?? ?? ?? ?? "
        "0F B6 87 81 01 00 00",
        "native feature descriptor cache", out);
    // Optional frontend family. A future layout change retains world DLSS
    // controls while withholding the menu scaling policy.
    const auto frontend =
        unique(image, "48 8D 0D ?? ?? ?? ?? 0F 28 F2 48 89 44 24 ??", "GameMain frontend state", out);
    const auto frontend_state =
        unique(image, "8B 05 ?? ?? ?? ?? 41 8B E8 83 C0 FE", "GameMain territory state reference", out);
    if (frontend && frontend_state) {
        const auto instance = relative(image, *frontend, 3, 7);
        const auto load_state = relative(image, *frontend_state, 2, 6);
        if (instance && image.writable(*instance, 0x4170) && load_state == *instance + 0x4100) {
            a.game_main = *instance;
            a.frontend_reference = *frontend;
            a.frontend_state_reference = *frontend_state;
        }
    }
    const auto accessor =
        unique(image, "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3", "CharaView accessor", out);
    const auto packet =
        unique(image, "E8 ?? ?? ?? ?? 0F 57 C0 41 BD C4 03 02", "queued world jitter packet", out);
    const auto camera_call =
        unique(image, "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 45 D0 48 8B 03 48 8D 57 50 48 89 44 24 38",
               "raster Halton call", out);
    if (settings) {
        const auto global = relative(image, *settings, 3, 7);
        if (global && image.writable(*global))
            a.settings_global = *global;
    }
    if (settings && descriptor) {
        std::uint32_t selector_root = 0, descriptor_root = 0;
        for (const auto &entry : entries) {
            if (entry.begin <= *settings && *settings < entry.end)
                selector_root = root(image, entry);
            if (entry.begin <= *descriptor && *descriptor < entry.end)
                descriptor_root = root(image, entry);
        }
        const auto release = relative(image, *descriptor + 28, 1, 5);
        if (selector_root && selector_root == descriptor_root && release &&
            body(image, *release,
                 "40 53 48 83 EC 20 0F B6 81 81 01 00 00 48 8B D9 A8 01 74 24 24 FD 88 81 81 01 00 00"))
            a.feature_descriptor_compare = *descriptor;
    }
    if (accessor) {
        const auto global = relative(image, *accessor, 3, 7);
        if (global && image.writable(*global)) {
            a.preview_accessor = *accessor;
            a.renderer_global = *global;
            out.previews = true;
        }
    }
    if (query) {
        const auto global = relative(image, *query + 21, 3, 7);
        const auto settings_again = relative(image, *query + 49, 3, 7);
        if (global && image.writable(*global) && a.settings_global && settings_again == a.settings_global &&
            boundary(entries, *query)) {
            a.query_size = *query;
            a.render_manager_global = *global;
        }
    }
    if (update && dispatch && boundary(entries, *update) && boundary(entries, *dispatch) &&
        root_calls(image, entries, *update, *dispatch)) {
        auto bytes = image.read(*update, 128);
        const auto device = find_pattern(bytes, "4C 8B 35 ?? ?? ?? ?? 41 0F 29 73 D8");
        if (device.valid && device.offsets.size() == 1) {
            const auto global = relative(image, *update + std::uint32_t(device.offsets[0]), 3, 7);
            if (global && image.writable(*global)) {
                a.renderer_update = *update;
                a.resize_dispatch = *dispatch;
                a.device_global = *global;
            }
        }
    }
    out.quality = a.settings_global && a.query_size && a.render_manager_global && a.renderer_update &&
                  a.resize_dispatch && a.device_global && a.feature_descriptor_compare;
    if (packet) {
        const auto target = relative(image, *packet, 1, 5);
        if (target && image.executable(*target, 8) && body(image, *target, "48 8B 81 78 02 00 00 C3")) {
            a.jitter_getter = *target;
            a.jitter_packet_return = *packet + 5;
        }
    }
    if (camera_call) {
        const auto generator = relative(image, *camera_call, 1, 5);
        const auto renderer = relative(image, *camera_call + 5, 3, 7);
        if (generator && renderer && renderer == a.renderer_global && image.executable(*generator, 16) &&
            boundary(entries, *generator) &&
            body(image, *generator, "48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 70")) {
            for (const auto &e : entries)
                if (e.begin <= *camera_call && *camera_call < e.end) {
                    const auto start = root(image, e);
                    if (start && body(image, start, "40 55 57 48 8D AC 24 68 FE FF FF") &&
                        relative(image, start + 52, 3, 7) == a.settings_global &&
                        body(image, start + 52, "48 8B 05 ?? ?? ?? ?? 32 D2 44 0F B6 40 54 41 80 F8 02")) {
                        a.camera_update = start;
                        break;
                    }
                }
            if (a.camera_update) {
                a.halton_generator = *generator;
                a.halton_camera_return = *camera_call + 5;
            }
        }
    }
    // FFXIVClientStructs Framework virtual slot 4. Resolve the vtable from
    // the constructor and validate the target against this PE's unwind table.
    const auto framework =
        unique(image, "48 8D 05 ?? ?? ?? ?? ?? ?? ?? 48 8B F1 49 89 6B", "Framework vtable", out);
    if (framework) {
        const auto table = relative(image, *framework, 3, 7);
        if (table) {
            auto data = image.read(*table + 4 * 8, 8);
            std::uint64_t pointer = 0;
            if (data.size() == 8)
                std::memcpy(&pointer, data.data(), 8);
            if (pointer >= image.image_base && pointer - image.image_base < image.image_size) {
                const auto rva = std::uint32_t(pointer - image.image_base);
                if (image.executable(rva, 64) && boundary(entries, rva)) {
                    a.simulation_tick = rva;
                    a.framework_vtable = *table;
                    out.evidence.push_back("Framework simulation tick resolved from verified vtable");
                }
            }
        }
    }
    const auto instance = unique(image, "48 8B 1D ?? ?? ?? ?? 8B 7C 24", "Framework instance", out);
    if (instance) {
        const auto global = relative(image, *instance, 3, 7);
        if (global && image.writable(*global, 8)) {
            a.framework_global = *global;
            a.framework_instance_reference = *instance;
            out.evidence.push_back("Framework instance global validated for live virtual-table ownership");
        }
    }
    out.jitter = a.jitter_getter && a.jitter_packet_return && a.halton_generator && a.halton_camera_return &&
                 a.settings_global && a.renderer_global && a.device_global;
    if (!out.quality)
        out.errors.push_back("native quality structural relationships are unverified; no hooks permitted");
    if (!out.jitter)
        out.errors.push_back("native jitter structural relationships are unverified; no hooks permitted");
    if (!out.previews)
        out.errors.push_back("preview accessor/global relationship is unverified; native previews retained");
    out.supported = out.quality && out.jitter && out.previews;
    return out;
}
} // namespace xivrender::ffxiv
