#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xivrender::runtime {

// Native NGX model versions are independent of the Streamline plugin ABI.
struct Version {
    std::array<std::uint16_t, 4> parts{};
    bool known = false;
    bool operator==(const Version &) const = default;
    std::string ToString() const;
};

struct ModuleDescriptor {
    std::string name;
    std::filesystem::path path;
    bool exists = false;
    Version version;
};

enum class IssueCode {
    invalid_root,
    duplicate_module,
    wrong_directory,
    missing_module,
    unreadable_version,
    plugin_version_mismatch,
};

struct Issue {
    IssueCode code{};
    std::string module;
    std::string message;
};

struct BundleReport {
    std::filesystem::path root;
    std::vector<ModuleDescriptor> modules;
    std::vector<Issue> issues;
    bool core_coherent = false;
    bool fg_ready = false;
    bool reflex_ready = false;
    bool dlss_ready = false;
    bool nr_ready = false;
    bool streamline_dlss_ready = false;
    bool streamline_nr_ready = false;
    bool streamline_nr_candidate = false;
    bool streamline_nr_needs_probe = false;
    std::string summary;

    const ModuleDescriptor *Find(const std::string &name) const;
};

// Pure manifest validation: does not read files, load modules, or change settings.
// Every descriptor must name a known file directly inside the absolute root.
BundleReport ValidateDescriptors(const std::filesystem::path &root, std::vector<ModuleDescriptor> modules);

// Reads one explicit flat runtime directory. Never searches the NVIDIA cache,
// changes DLL search paths, or calls LoadLibrary/slInit. Readiness here describes
// the manifest only; hardware, driver, export, and feature checks come later.
BundleReport InspectBundle(const std::filesystem::path &root);

} // namespace xivrender::runtime
