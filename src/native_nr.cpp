#include "xivrender/native_nr.hpp"
#include "xivrender/app.hpp"
#include <windows.h>
#include <mutex>
#include <atomic>

namespace xivrender::native_nr {
namespace {
struct Host {
    std::mutex mutex;
    nr::Pipeline pipeline;
    std::filesystem::path root;
    std::atomic<ID3D11Device *> native_device{nullptr};
    ULONGLONG last_log = 0, last_applied = 0;
    std::uint64_t logged_failures = 0;
    std::string last_runtime, last_message;
    const void *last_source = nullptr;
    unsigned source_width = 0, source_height = 0;
    DWORD render_thread = 0;
    bool disable_requested = false, retry_requested = false;
    std::string control_message;
    std::atomic<bool> needs_service{false};
};
Host &host() {
    static auto *value = new Host;
    return *value;
}
bool focused() {
    DWORD owner = 0;
    const auto window = GetForegroundWindow();
    return window && GetWindowThreadProcessId(window, &owner) && owner == GetCurrentProcessId();
}
} // namespace
void set_root(const std::filesystem::path &root) {
    auto &s = host();
    std::lock_guard lock(s.mutex);
    s.root = root;
}
void attach_device(ID3D11Device *device) {
    if (!device)
        return;
    auto &s = host();
    // Present rediscovers the same device on every frame. Attaching it again
    // must not wait for a render-thread NR evaluation or its GPU retirement.
    if (s.native_device.load(std::memory_order_acquire) == device)
        return;
    std::lock_guard lock(s.mutex);
    if (s.native_device.load(std::memory_order_relaxed) != device) {
        s.pipeline.Attach(device, s.root);
        s.native_device.store(device, std::memory_order_release);
    }
}
void after_upscale(const nr::FrameInput &input) {
    const auto cfg = settings_snapshot();
    const bool has_focus = focused();
    auto &s = host();
    std::lock_guard lock(s.mutex);
    try {
        const auto thread = GetCurrentThreadId();
        if (s.render_thread && s.render_thread != thread && s.pipeline.NeedsService()) {
            s.control_message = "NR is waiting for its render thread; clean DLSS frame retained";
            s.needs_service = true;
            return;
        }
        s.render_thread = thread;
        if (s.disable_requested || s.retry_requested) {
            // UI/Present callbacks queue intent only. All model calls and
            // immediate-context retirement remain beside native SR evaluation.
            // A quick off/on cycle must still retire the previous generation.
            s.pipeline.Retry();
            log_message("NR control applied on render thread=" + std::to_string(thread) +
                        ", enabled=" + std::to_string(cfg.nr.enabled) +
                        ", disable_requested=" + std::to_string(s.disable_requested) +
                        ", retry_requested=" + std::to_string(s.retry_requested));
            s.disable_requested = s.retry_requested = false;
        }
        s.control_message.clear();
        if (cfg.nr.enabled &&
            (input.sr_feature != s.last_source || input.images.guide_width != s.source_width ||
             input.images.guide_height != s.source_height)) {
            const auto &f = input.images;
            D3D11_TEXTURE2D_DESC color{}, depth{}, motion{};
            if (f.color)
                f.color->GetDesc(&color);
            if (f.depth)
                f.depth->GetDesc(&depth);
            if (f.motion)
                f.motion->GetDesc(&motion);
            log_message(
                "NR native frame: output=" + std::to_string(f.output_width) + "x" +
                std::to_string(f.output_height) + ", guides=" + std::to_string(f.guide_width) + "x" +
                std::to_string(f.guide_height) + ", motion=" + std::to_string(f.motion_width) + "x" +
                std::to_string(f.motion_height) + ", formats=" + std::to_string(unsigned(color.Format)) +
                "," + std::to_string(unsigned(depth.Format)) + "," + std::to_string(unsigned(motion.Format)) +
                ", MV scale=" + std::to_string(f.motion_scale_x) + "," + std::to_string(f.motion_scale_y) +
                ", HDR=" + std::to_string(input.hdr) +
                ", jittered MV=" + std::to_string(input.motion_jittered));
            s.last_source = input.sr_feature;
            s.source_width = f.guide_width;
            s.source_height = f.guide_height;
        }
        if (s.pipeline.Process(input, cfg.nr, has_focus))
            s.last_applied = GetTickCount64();
        s.needs_service = s.pipeline.NeedsService();
        const auto status = s.pipeline.Status();
        const auto now = GetTickCount64();
        const bool failed = status.failures != s.logged_failures;
        const bool changed = status.active_runtime != s.last_runtime || status.message != s.last_message;
        if (failed || (changed && now - s.last_log >= 1000) ||
            (cfg.nr.enabled && now - s.last_log >= 15000)) {
            s.last_log = now;
            s.last_runtime = status.active_runtime;
            s.last_message = status.message;
            s.logged_failures = status.failures;
            log_message(
                "NR: " + status.message + ", requested=" + status.requested_runtime +
                ", active=" + status.active_runtime + ", model=" + std::to_string(status.width) + "x" +
                std::to_string(status.height) + ", creates=" + std::to_string(status.creations) +
                ", evaluations=" + std::to_string(status.evaluations) + ", applied=" +
                std::to_string(status.applied) + ", skipped=" + std::to_string(status.skipped) +
                ", paced=" + std::to_string(status.paced) + ", failures=" + std::to_string(status.failures));
        }
    } catch (const std::exception &error) {
        s.pipeline.Fault(std::string("NR frame exception: ") + error.what());
        s.needs_service = s.pipeline.NeedsService();
        log_message(std::string("NR frame exception; clean native DLSS retained: ") + error.what());
        // Retain owners until an explicit retry/disable can retire them.
    } catch (...) {
        s.pipeline.Fault("NR frame exception");
        s.needs_service = s.pipeline.NeedsService();
        log_message("NR frame exception; clean native DLSS retained");
    }
}
bool needs_service() {
    return host().needs_service.load(std::memory_order_acquire);
}
void service() {
    // Present may run on the UI thread while the engine's immediate context
    // is in use on its render thread. It must not flush, release NR or switch
    // context state. after_upscale services pending retirement on that owner.
}
void enabled_changed(bool enabled) {
    auto &s = host();
    std::lock_guard lock(s.mutex);
    // Sticky until the render thread observes it, even if the user turns NR
    // back on before another successful native SR frame arrives.
    s.disable_requested |= !enabled;
    s.control_message = enabled ? "NR enable requested; waiting for native DLSS"
                                : "NR disable requested; waiting for render-thread cleanup";
    if (!enabled)
        s.last_source = nullptr;
    s.needs_service = true;
}
nr::PipelineStatus snapshot() {
    auto &s = host();
    std::lock_guard lock(s.mutex);
    auto status = s.pipeline.Status();
    if (!s.control_message.empty()) {
        status.active = false;
        status.message = s.control_message;
        return status;
    }
    if (status.active && GetTickCount64() - s.last_applied > 2000) {
        status.active = false;
        status.message = "NR is waiting for native DLSS frames";
    }
    return status;
}
void retry() {
    auto &s = host();
    std::lock_guard lock(s.mutex);
    s.retry_requested = true;
    s.control_message = "NR retry requested; waiting for render thread";
    s.needs_service = true;
    s.last_log = 0;
}
void shutdown(unsigned timeout_ms) {
    auto &s = host();
    std::lock_guard lock(s.mutex);
    std::string error;
    if (!s.pipeline.Shutdown(timeout_ms, error))
        log_message("NR teardown pending safe GPU retirement: " + error);
    else
        s.native_device.store(nullptr, std::memory_order_release);
    if (!s.pipeline.NeedsService()) {
        s.render_thread = 0;
        s.disable_requested = s.retry_requested = false;
        s.control_message.clear();
    }
    s.needs_service = s.pipeline.NeedsService();
}
} // namespace xivrender::native_nr
