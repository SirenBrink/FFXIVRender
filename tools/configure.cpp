#include "xivrender/settings.hpp"
#include <iostream>
int wmain(int argc, wchar_t **argv) {
    if (argc != 2 && argc != 3)
        return 2;
    std::string error;
    xivrender::Settings settings;
    if (argc == 3) {
        const auto mode = std::wstring_view(argv[2]);
        if (mode != L"--hdr-test" && mode != L"--fg-test" && mode != L"--fsr-mfg")
            return 2;
        auto loaded = xivrender::load_settings(argv[1]);
        if (!loaded.warnings.empty()) {
            std::cerr << "Existing settings need review before preparing the test\n";
            return 1;
        }
        settings = loaded.settings;
        if (mode == L"--fsr-mfg") {
            settings.fg.fsr_mfg = true;
        } else if (mode == L"--hdr-test") {
            settings.hdr.enabled = true;
            settings.hdr.output = xivrender::HdrOutput::ScRgb16Float;
            settings.dlss.hdr_input = true;
            settings.nr.hdr_input = true;
        } else {
            settings.fg.enabled = true;
            settings.fg.backend = xivrender::FrameGeneration::DlssG;
            settings.fg.factor = 2;
            settings.fg.dynamic = false;
            settings.fg.latency = xivrender::LatencyReduction::Reflex;
            settings.fg.reflex = true;
            settings.fg.xefg_unlock = false;
            if (settings.hdr.enabled)
                settings.hdr.output = xivrender::HdrOutput::Pq10;
        }
    }
    if (!xivrender::save_settings(argv[1], settings, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}
