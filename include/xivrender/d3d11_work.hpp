#pragma once
#include <mutex>
namespace xivrender::d3d11_work {
// NR and FG both use the game's immediate context. Serialize complete private
// state transactions, rather than allowing individual D3D11 calls from the
// native SR thread and Present thread to interleave.
inline std::recursive_mutex &mutex() {
    static auto *value = new std::recursive_mutex;
    return *value;
}
struct Guard {
    std::unique_lock<std::recursive_mutex> lock{mutex()};
};
} // namespace xivrender::d3d11_work
