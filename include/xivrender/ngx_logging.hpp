#pragma once
#include <windows.h>
#include <functional>
#include <string>
namespace xivrender::fg {
// Replace only this host's NGX init callback while binding Streamline. NGX's
// process-wide model cache can retain it after sl.common has been unloaded.
bool bind_streamline_ngx(const std::function<int()> &bind, int &result, std::string &error,
                        HMODULE test_runtime = nullptr);
}
