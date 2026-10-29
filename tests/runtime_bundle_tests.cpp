#include <xivrender/runtime_bundle.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <vector>

using namespace xivrender::runtime;
namespace {
const std::filesystem::path root = L"C:\\XIVRender-runtime-fixture";
Version V(std::uint16_t a, std::uint16_t b, std::uint16_t c = 0, std::uint16_t d = 0) {
    return {{{a, b, c, d}}, true};
}
std::vector<ModuleDescriptor> Complete(Version core) {
    std::vector<ModuleDescriptor> modules;
    for (const char *name :
         {"sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll"})
        modules.push_back({name, root / name, true, core});
    // These deliberately differ from each other and from Streamline.
    modules.push_back({"nvngx_dlss.dll", root / "nvngx_dlss.dll", true, V(310, 9, 16)});
    modules.push_back({"nvngx_dlssg.dll", root / "nvngx_dlssg.dll", true, V(310, 9, 15)});
    modules.push_back({"nvngx_dlssnr.dll", root / "nvngx_dlssnr.dll", true, V(310, 8)});
    return modules;
}
ModuleDescriptor &Find(std::vector<ModuleDescriptor> &modules, const char *name) {
    return *std::find_if(modules.begin(), modules.end(), [&](const auto &m) { return m.name == name; });
}
} // namespace

int main(int argc, char **argv) {
    for (Version core : {V(2, 7, 32), V(2, 14, 16)}) {
        auto modules = Complete(core);
        auto report = ValidateDescriptors(root, modules);
        assert(report.core_coherent && report.fg_ready && report.reflex_ready && report.dlss_ready &&
               report.nr_ready);
        assert(report.issues.empty() && !report.streamline_dlss_ready && !report.streamline_nr_ready);
        assert(report.Find("SL.COMMON.DLL") && core.ToString() != "unknown");
        modules.push_back({"sl.dlss.dll", root / "sl.dlss.dll", true, core});
        modules.push_back({"sl.dlss_nr.dll", root / "sl.dlss_nr.dll", true, V(2, 8)});
        report = ValidateDescriptors(root, modules);
        assert(report.streamline_nr_candidate && report.streamline_nr_needs_probe);
        assert(report.fg_ready && report.streamline_dlss_ready && !report.streamline_nr_ready &&
               report.nr_ready);
    }
    auto modules = Complete(V(2, 14, 16));
    for (const char *missing : {"sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll",
                                "sl.pcl.dll", "nvngx_dlssg.dll"}) {
        auto broken = modules;
        Find(broken, missing).exists = false;
        assert(!ValidateDescriptors(root, broken).fg_ready);
    }
    for (const char *mismatched : {"sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll"}) {
        auto broken = modules;
        Find(broken, mismatched).version = V(2, 7, 32);
        assert(!ValidateDescriptors(root, broken).fg_ready);
    }
    auto invalid = modules;
    Find(invalid, "sl.common.dll").version = {};
    assert(!ValidateDescriptors(root, invalid).fg_ready);
    invalid = modules;
    Find(invalid, "sl.dlss_g.dll").path = L"C:\\ProgramData\\NVIDIA\\NGX\\models\\sl.dlss_g.dll";
    assert(!ValidateDescriptors(root, invalid).fg_ready);
    invalid = modules;
    invalid.push_back(modules.front());
    assert(!ValidateDescriptors(root, invalid).fg_ready);
    assert(!ValidateDescriptors("relative", modules).fg_ready);
    invalid = modules;
    Find(invalid, "sl.dlss_g.dll").exists = false;
    assert(ValidateDescriptors(root, invalid).reflex_ready);
    invalid = modules;
    Find(invalid, "nvngx_dlssg.dll").version = V(999, 1);
    assert(ValidateDescriptors(root, invalid).fg_ready);
    const auto absent = InspectBundle(root / L"missing");
    assert(!absent.fg_ready && !absent.dlss_ready && !absent.nr_ready);
    if (argc > 1) {
        const auto inspected = InspectBundle(std::filesystem::absolute(argv[1]));
        std::cout << inspected.summary << '\n';
        for (const auto &module : inspected.modules)
            std::cout << module.name << ": " << (module.exists ? module.version.ToString() : "missing")
                      << '\n';
    }
    std::cout << "PASS: old/current coherent bundles, optional NR independence, native model versions, "
                 "missing/mixed core, path scope and duplicate rejection\n";
}
