#include "xivrender/ngx_logging.hpp"
#include "xivrender/fg_diagnostics.hpp"
#include <nvsdk_ngx_defs.h>
#include <d3d12.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <array>
#include <cstring>
unsigned foreign_calls = 0;
void NVSDK_CONV foreign(const char *, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) { ++foreign_calls; }
int wmain(int argc, wchar_t **argv) {
    assert(argc == 2);
    auto library = LoadLibraryW(argv[1]);
    assert(library);
    using Init = NVSDK_NGX_Result(NVSDK_CONV *)(const char *, NVSDK_NGX_EngineType, const char *, const wchar_t *,
                                             ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *);
    const auto init = reinterpret_cast<Init>(GetProcAddress(library, "NVSDK_NGX_D3D12_Init_ProjectID"));
    const auto emit = reinterpret_cast<void (*)()>(GetProcAddress(library, "Emit"));
    const auto reset = reinterpret_cast<void (*)()>(GetProcAddress(library, "Reset"));
    assert(init && emit && reset);
    std::array<unsigned char, 32> pristine{};
    std::memcpy(pristine.data(), reinterpret_cast<void *>(init), pristine.size());
    NVSDK_NGX_FeatureCommonInfo info{};
    info.LoggingInfo.LoggingCallback = &foreign;
    const auto invoke = [&](const char *id) {
        assert(init(id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "test", L".", nullptr, NVSDK_NGX_Version_API, &info) == NVSDK_NGX_Result_Success);
    };
    invoke("foreign"); emit(); assert(foreign_calls == 1); reset();
    const auto folder = std::filesystem::temp_directory_path() / (L"xivrender-ngx-log-" + std::to_wstring(GetCurrentProcessId()));
    xivrender::fg::diagnostics::start(folder);
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        std::string error;
        int result = -1;
        assert(xivrender::fg::bind_streamline_ngx([&] {
            invoke("be4ee394-a407-4b26-82c1-74250a367f68"); return 0;
        }, result, error, library));
        assert(result == 0 && error.empty());
        assert(!std::memcmp(pristine.data(), reinterpret_cast<void *>(init), pristine.size()));
        emit();
        assert(foreign_calls == 1); // No reference to the retired owner's logger.
        assert(info.LoggingInfo.LoggingCallback == &foreign); // Caller input is immutable.
    }
    // With the interceptor disabled, even the matching project is forwarded.
    reset(); invoke("be4ee394-a407-4b26-82c1-74250a367f68"); emit(); assert(foreign_calls == 2);
    reset();
    std::string error; int result = -1;
    assert(xivrender::fg::bind_streamline_ngx([&] { invoke("foreign"); return 24; }, result, error, library));
    emit(); assert(foreign_calls == 3 && result == 24);
    assert(!std::memcmp(pristine.data(), reinterpret_cast<void *>(init), pristine.size()));
    const auto file = xivrender::fg::diagnostics::path();
    xivrender::fg::diagnostics::shutdown();
    std::ifstream input(file);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    assert(text.find("cached callback during reinitialization") != std::string::npos);
    assert(text.find("cached callback after SDK owner retired") != std::string::npos);
    std::cout << "PASS: cached callback survives four SDK retirements; foreign init and original bytes preserved\n";
}
