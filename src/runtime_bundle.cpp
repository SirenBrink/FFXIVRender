#include <xivrender/runtime_bundle.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winver.h>

#include <algorithm>
#include <cwctype>
#include <sstream>
#include <string_view>
#include <utility>

namespace xivrender::runtime {
namespace {
constexpr std::array<const char *, 10> names{
    "sl.interposer.dll", "sl.common.dll",  "sl.dlss_g.dll",  "sl.reflex.dll",   "sl.pcl.dll",
    "sl.dlss.dll",       "sl.dlss_nr.dll", "nvngx_dlss.dll", "nvngx_dlssg.dll", "nvngx_dlssnr.dll",
};

std::string Lower(std::string value) {
    for (char &c : value)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return value;
}

std::wstring PathKey(const std::filesystem::path &path) {
    auto value = path.lexically_normal().wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (value.size() > 3 && value.back() == L'\\')
        value.pop_back();
    for (wchar_t &c : value)
        c = static_cast<wchar_t>(std::towlower(c));
    return value;
}

Version ReadVersion(const std::filesystem::path &path) {
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!bytes || bytes > 16 * 1024 * 1024)
        return {};
    std::vector<std::uint8_t> data(bytes);
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, data.data()))
        return {};
    VS_FIXEDFILEINFO *fixed = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&fixed), &length) || !fixed ||
        length < sizeof(VS_FIXEDFILEINFO) || fixed->dwSignature != 0xfeef04bd)
        return {};
    return {{{static_cast<std::uint16_t>(HIWORD(fixed->dwFileVersionMS)),
              static_cast<std::uint16_t>(LOWORD(fixed->dwFileVersionMS)),
              static_cast<std::uint16_t>(HIWORD(fixed->dwFileVersionLS)),
              static_cast<std::uint16_t>(LOWORD(fixed->dwFileVersionLS))}},
            true};
}

void AddIssue(BundleReport &report, IssueCode code, const std::string &module, std::string message) {
    const bool already = std::any_of(report.issues.begin(), report.issues.end(), [&](const auto &issue) {
        return issue.code == code && issue.module == module;
    });
    if (!already)
        report.issues.push_back({code, module, std::move(message)});
}

bool Usable(BundleReport &report, const char *name, bool report_missing) {
    const auto *module = report.Find(name);
    if (!module || !module->exists) {
        if (report_missing)
            AddIssue(report, IssueCode::missing_module, name, std::string("Missing runtime file: ") + name);
        return false;
    }
    if (!module->version.known) {
        AddIssue(report, IssueCode::unreadable_version, name,
                 std::string("Cannot read a file version from ") + name + "; readiness is unverified");
        return false;
    }
    return true;
}

bool Coherent(BundleReport &report, std::initializer_list<const char *> group) {
    const auto *base = report.Find("sl.interposer.dll");
    bool okay = true;
    for (const auto *name : group) {
        if (!Usable(report, name, true)) {
            okay = false;
            continue;
        }
        const auto *module = report.Find(name);
        if (base && base->exists && base->version.known && module->version != base->version) {
            AddIssue(report, IssueCode::plugin_version_mismatch, name,
                     std::string(name) + " is " + module->version.ToString() + "; sl.interposer.dll is " +
                         base->version.ToString());
            okay = false;
        }
    }
    return okay;
}

bool OptionalPlugin(BundleReport &report, const char *name, bool base_ready) {
    const auto *plugin = report.Find(name);
    if (!plugin || !plugin->exists)
        return false;
    if (!Usable(report, name, false))
        return false;
    const auto *base = report.Find("sl.interposer.dll");
    if (!base || !base->exists || !base->version.known)
        return false;
    if (plugin->version != base->version) {
        AddIssue(report, IssueCode::plugin_version_mismatch, name,
                 std::string(name) + " is " + plugin->version.ToString() +
                     "; this optional plugin is not verified against Streamline " + base->version.ToString() +
                     ". Core FG readiness is unaffected");
        return false;
    }
    return base_ready;
}
} // namespace

std::string Version::ToString() const {
    if (!known)
        return "unknown";
    std::ostringstream text;
    text << parts[0] << '.' << parts[1] << '.' << parts[2] << '.' << parts[3];
    return text.str();
}

const ModuleDescriptor *BundleReport::Find(const std::string &name) const {
    const auto key = Lower(name);
    const auto it =
        std::find_if(modules.begin(), modules.end(), [&](const auto &item) { return item.name == key; });
    return it == modules.end() ? nullptr : &*it;
}

BundleReport ValidateDescriptors(const std::filesystem::path &root, std::vector<ModuleDescriptor> modules) {
    BundleReport report;
    report.root = root.lexically_normal();
    if (!root.is_absolute()) {
        AddIssue(report, IssueCode::invalid_root, "", "The runtime folder must be an absolute path");
        report.summary = "Runtime folder is not absolute; no runtime is eligible";
        return report;
    }

    for (auto &module : modules) {
        module.name = Lower(std::move(module.name));
        if (std::find(names.begin(), names.end(), module.name) == names.end())
            continue;
        const auto existing = std::find_if(report.modules.begin(), report.modules.end(),
                                           [&](const auto &item) { return item.name == module.name; });
        if (existing != report.modules.end()) {
            existing->exists = false;
            AddIssue(report, IssueCode::duplicate_module, module.name,
                     "Duplicate runtime descriptor; refusing ambiguous module " + module.name);
            continue;
        }
        if (!module.path.is_absolute() || PathKey(module.path.parent_path()) != PathKey(report.root) ||
            Lower(module.path.filename().string()) != module.name) {
            module.exists = false;
            AddIssue(report, IssueCode::wrong_directory, module.name,
                     "Runtime file is not the named module directly inside the selected folder: " +
                         module.name);
        }
        report.modules.push_back(std::move(module));
    }

    report.core_coherent = Coherent(
        report, {"sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll"});
    report.reflex_ready =
        Coherent(report, {"sl.interposer.dll", "sl.common.dll", "sl.reflex.dll", "sl.pcl.dll"});
    const bool native_fg = Usable(report, "nvngx_dlssg.dll", true);
    report.fg_ready = report.core_coherent && native_fg;
    report.dlss_ready = Usable(report, "nvngx_dlss.dll", false);
    report.nr_ready = Usable(report, "nvngx_dlssnr.dll", false);
    const bool base_ready =
        Usable(report, "sl.interposer.dll", false) && Usable(report, "sl.common.dll", false) &&
        report.Find("sl.interposer.dll")->version == report.Find("sl.common.dll")->version;
    report.streamline_dlss_ready = OptionalPlugin(report, "sl.dlss.dll", base_ready) && report.dlss_ready;
    // The temporary NR plugin may be older than the coherent core SDK. Keep it
    // as an explicit compatibility candidate; only an isolated API/device probe
    // can establish that this combination starts. Never replace core files for it.
    const auto *nr_plugin = report.Find("sl.dlss_nr.dll");
    const auto *interposer = report.Find("sl.interposer.dll");
    report.streamline_nr_candidate = base_ready && nr_plugin && nr_plugin->exists && nr_plugin->version.known;
    report.streamline_nr_needs_probe =
        report.streamline_nr_candidate && nr_plugin->version != interposer->version;
    report.streamline_nr_ready =
        report.streamline_nr_candidate && !report.streamline_nr_needs_probe && report.nr_ready;
    report.summary = report.fg_ready ? "FG runtime bundle is coherent (hardware and activation not checked)"
                                     : "FG runtime bundle is incomplete or inconsistent";
    report.summary += report.dlss_ready ? "; native DLSS available" : "; native DLSS unavailable";
    report.summary += report.nr_ready ? "; native NR available" : "; native NR unavailable";
    return report;
}

BundleReport InspectBundle(const std::filesystem::path &root) {
    if (!root.is_absolute())
        return ValidateDescriptors(root, {});
    std::error_code error;
    const auto normalized = std::filesystem::weakly_canonical(root, error);
    const auto selected = error ? root.lexically_normal() : normalized;
    std::vector<ModuleDescriptor> modules;
    modules.reserve(names.size());
    for (const auto *name : names) {
        ModuleDescriptor module{name, selected / name};
        error.clear();
        module.exists = std::filesystem::is_regular_file(module.path, error) && !error;
        if (module.exists)
            module.version = ReadVersion(module.path);
        modules.push_back(std::move(module));
    }
    return ValidateDescriptors(selected, std::move(modules));
}

} // namespace xivrender::runtime
