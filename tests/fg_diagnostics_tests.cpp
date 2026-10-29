#include "xivrender/fg_diagnostics.hpp"
#include <windows.h>
#include <cassert>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>
#include <iostream>
using namespace xivrender::fg;
int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           (L"xivrender-diagnostics-" + std::to_wstring(GetCurrentProcessId()));
    diagnostics::start(directory);
    diagnostics::context(2, 17);
    diagnostics::event("sdk.warning", 42, 1, 2, 3, 4, "warning \"quoted\"\nC:\\runtime");
    diagnostics::event("status", 42, 0, 0, 0, 0, "normal status after the warning");
    std::vector<std::thread> producers;
    for (unsigned t = 0; t < 4; ++t) producers.emplace_back([t] {
        for (unsigned n = 0; n < 400; ++n)
            diagnostics::event("concurrent", t * 400 + n, t, n);
    });
    for (auto &thread : producers) thread.join();
    const std::string long_text(2000, 'x');
    diagnostics::event("long", 0, 0, 0, 0, 0, long_text.c_str());
    const auto split_utf8 = std::string(1533, 'x') + "\xe2\x82\xac";
    diagnostics::event("split_utf8", 0, 0, 0, 0, 0, split_utf8.c_str());
    diagnostics::event("utf8", 0, 0, 0, 0, 0, "\xe2\x82\xac");
    const auto output = diagnostics::path();
    {
        diagnostics::SlowScope stalled("test.stalled_stage");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    diagnostics::flush();
    diagnostics::shutdown();
    assert(!diagnostics::error());
    std::ifstream input(output, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    assert(text.find("warning \\\"quoted\\\"\\u000aC:\\\\runtime") != std::string::npos);
    assert(text.find("normal status after the warning") != std::string::npos);
    assert(text.find("\"kind\":\"phase.slow\"") != std::string::npos &&
           text.find("test.stalled_stage") != std::string::npos);
    assert(text.find("\"generation\":17,\"provider\":2") != std::string::npos);
    assert(text.find("\"truncated\":true") != std::string::npos);
    assert(text.find("\\ufffd") != std::string::npos);
    assert(text.find("\xe2\x82\xac") != std::string::npos);
    unsigned records = 0;
    for (size_t pos = 0; (pos = text.find("\"kind\":\"concurrent\"", pos)) != std::string::npos; ++pos)
        ++records;
    assert(records == 1600 && text.find("records_dropped") == std::string::npos);
    // Failed starts are visible and do not prevent a later valid session.
    const auto obstruction = directory / L"ordinary-file";
    { std::ofstream file(obstruction); file << "not a directory"; }
    diagnostics::start(obstruction / L"logs");
    assert(diagnostics::error());
    diagnostics::start(directory / L"recovery");
    diagnostics::event("recovered");
    diagnostics::shutdown();
    assert(!diagnostics::error());
    std::cout << "PASS: persistent warnings, concurrent records, escaping, bounded messages, flush and recovery\n";
}
