#include "xivrender/app.hpp"
#include "xivrender/native_nr.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include <cassert>
#include <iostream>
#include <future>
#include <thread>

namespace xivrender {
namespace {
Settings requested;
}
Settings settings_snapshot() {
    return requested;
}
void log_message(const std::string &) {}
} // namespace xivrender

int main() {
    using namespace xivrender;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, nullptr, nullptr)));
    native_nr::set_root(std::filesystem::current_path());
    native_nr::attach_device(device.Get());
    requested.nr.enabled = true;
    requested.nr.pause_on_focus_loss = false;
    // Rejection before creating any model/transport owners must still recover
    // through the user-facing enable toggle, without the retirement fast path.
    nr::FrameInput invalid;
    native_nr::after_upscale(invalid);
    assert(native_nr::snapshot().failed);
    assert(!native_nr::needs_service());
    requested.nr.enabled = false;
    native_nr::enabled_changed(false);
    auto status = native_nr::snapshot();
    assert(status.message.find("requested") != std::string::npos);
    assert(native_nr::needs_service());
    native_nr::service(); // Present/UI must not perform model or context work.
    assert(native_nr::snapshot().message == status.message);
    native_nr::after_upscale(invalid); // Native SR's owner delivers the request.
    status = native_nr::snapshot();
    assert(!status.failed && !status.active && status.message == "NR is disabled");
    assert(!native_nr::needs_service());
    requested.nr.enabled = true;
    native_nr::enabled_changed(true);
    assert(!native_nr::snapshot().failed);
    // Commands from a different UI thread must be coalesced without runtime
    // work. In particular off/on must not erase an unobserved disable request.
    std::thread ui([] {
        native_nr::enabled_changed(false);
        native_nr::enabled_changed(true);
        native_nr::retry();
        native_nr::service();
    });
    ui.join();
    assert(native_nr::snapshot().message.find("retry requested") != std::string::npos);
    requested.nr.enabled = false;
    native_nr::after_upscale(invalid);
    assert(!native_nr::snapshot().failed && !native_nr::needs_service());
    native_nr::shutdown();
    std::cout
        << "PASS: deferred UI controls, rapid off/on/retry and rejected-start recovery on render owner\n";
}
