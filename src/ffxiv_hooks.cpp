#include "xivrender/ffxiv_hooks.hpp"
#include "xivrender/app.hpp"
#include "xivrender/trace.hpp"
#include "xivrender/frame_generation.hpp"
#include "xivrender/fg_diagnostics.hpp"
#include "xivrender/fg_policy.hpp"
#include "xivrender/pointer_lease.hpp"
#include "xivrender/native_dlss.hpp"
#include "xivrender/hdr.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>
#include <sstream>
#include <iomanip>

namespace xivrender::ffxiv {
namespace {
using Tick = bool (*)(std::uintptr_t);
using Query = std::uintptr_t (*)(const unsigned *, unsigned *, unsigned *, unsigned *);
using Update = void (*)(std::uintptr_t, float);
using Dispatcher = unsigned char (*)(std::uintptr_t);
using Getter = void *(*)(std::uintptr_t);
using Generator = std::uintptr_t (*)(std::uintptr_t, float *, float *, unsigned, unsigned);
struct State {
    std::mutex mutex;
    std::mutex menu_mutex;
    std::mutex tick_mutex;
    std::atomic<bool> active{false};
    std::atomic<unsigned> inside{0}, requests{1}, forced_refresh{0};
    std::atomic<std::uint64_t> render{0}, display{0};
    std::uintptr_t base = 0;
    Addresses addresses;
    Query query = nullptr;
    Tick tick = nullptr;
    PointerLease tick_slot;
    std::atomic<std::uint64_t> tick_calls{0};
    std::uintptr_t tick_instance = 0;
    ULONGLONG next_tick_check = 0;
    Update update = nullptr;
    Dispatcher dispatcher = nullptr;
    Getter getter = nullptr;
    Generator generator = nullptr;
    std::vector<void *> targets;
    // Only the renderer thread accesses the request/restore transaction below.
    unsigned handled = 0, attempted = 0, attempts = 0;
    unsigned handled_refresh = 0, attempted_refresh = 0;
    ULONGLONG retry_at = 0;
    bool previous_enabled = false, selector_owned = false, previous_hdr = false;
    DlssQuality previous_quality = DlssQuality::Quality;
    DlssModelPreset previous_model = DlssModelPreset::Automatic;
    std::uintptr_t original_settings = 0;
    unsigned char original_selector = 0, owned_selector = 0;
    std::uintptr_t menu_config = 0;
    bool menu_gate_owned = false, menu_active = false;
    std::uint32_t menu_load_state = 0;
};
State &state() {
    static auto *s = new State;
    return *s;
}
struct Call {
    State &s;
    Call(State &value) : s(value) {
        s.inside.fetch_add(1, std::memory_order_acquire);
    }
    ~Call() {
        s.inside.fetch_sub(1, std::memory_order_release);
    }
};
bool read(std::uintptr_t at, void *output, std::size_t size) {
    SIZE_T copied = 0;
    return at >= 0x10000 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(at), output, size,
                             &copied) &&
           copied == size;
}
template <class T> bool read(std::uintptr_t at, T &output) {
    return read(at, &output, sizeof(output));
}
template <class T> bool write_value(std::uintptr_t at, const T &value) {
    SIZE_T written = 0;
    return at >= 0x10000 &&
           WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(at), &value, sizeof(value),
                              &written) &&
           written == sizeof(value);
}
bool settings(State &s, std::uintptr_t &address, std::array<unsigned char, 0x7c> &bytes) {
    return read(s.base + s.addresses.settings_global, address) && read(address, bytes.data(), bytes.size()) &&
           bytes[0x54] == 2;
}
struct FrontendState {
    unsigned char connected = 0, padding = 0;
    std::uint32_t load = 0, next = 0, current = 0;
};
bool frontend(State &s, FrontendState &out) {
    if (!s.addresses.game_main)
        return false;
    const auto at = s.base + s.addresses.game_main;
    return read(at + 0x40fe, out.connected) && read(at + 0x4100, out.load) && read(at + 0x4104, out.next) &&
           read(at + 0x4108, out.current) &&
           // Native AgentLobby's ready transition sets state 4. The public
           // GameMain layout documents only zone states 1/2/3; state 4 is still
           // frontend when disconnected and both territory IDs are zero.
           out.connected == 0 && (out.load == 0 || out.load == 4) && out.next == 0 && out.current == 0;
}
void release_menu_frame(State &s) {
    // Called with menu_mutex held. Preserve native writes and replaced settings
    // objects; only return a still-owned 1 to the original 0.
    if (s.menu_gate_owned) {
        std::uintptr_t config = 0;
        unsigned char gate = 0;
        if (read(s.base + s.addresses.settings_global, config) && config == s.menu_config &&
            read(config + 0x45, gate) && gate == 1)
            write_value(config + 0x45, static_cast<unsigned char>(0));
    }
    s.menu_gate_owned = false;
    s.menu_config = 0;
}
void begin_menu_frame(State &s) {
    std::lock_guard lock(s.menu_mutex);
    const auto requested = settings_snapshot();
    std::uintptr_t config = 0;
    std::array<unsigned char, 0x7c> bytes{};
    FrontendState scene;
    const bool needs_native_scene = fixed_dlss_resolution(requested.dlss);
    const bool active = s.active.load(std::memory_order_acquire) && native_dlss::active() &&
                        needs_native_scene && settings(s, config, bytes) && bytes[0x45] <= 1 &&
                        frontend(s, scene);
    if (s.menu_gate_owned && (!active || config != s.menu_config))
        release_menu_frame(s);
    const bool lobby_stage_changed = active && s.menu_active && scene.load != s.menu_load_state;
    if (active != s.menu_active || lobby_stage_changed) {
        s.menu_active = active;
        s.menu_load_state = scene.load;
        log_message(std::string("Frontend DLSS scaling ") +
                    (lobby_stage_changed ? "lobby stage changed" : (active ? "entered" : "left")) +
                    "; native gate=" + std::to_string(bytes[0x45]) + ", connected=" +
                    std::to_string(scene.connected) + ", load state=" + std::to_string(scene.load) +
                    ", next territory=" + std::to_string(scene.next) +
                    ", current territory=" + std::to_string(scene.current));
        // Reapply at lobby readiness and login/logout: native transitions may
        // recreate the graphics context or restore a native quality selector.
        if (requested.dlss.enabled && native_dlss::active() && config && bytes[0x54] == 2)
            request_feature_refresh();
    }
    if (!active)
        return;
    // Keep the same policy visible to sizing, native feature creation and
    // postprocessing. A query-only override would disagree with later stages.
    // Main Present restores it before the next frame's event/cinematic policy.
    if (bytes[0x45] == 0 && write_value(config + 0x45, static_cast<unsigned char>(1))) {
        s.menu_gate_owned = true;
        s.menu_config = config;
    }
}
float ratio(DlssQuality quality) {
    switch (quality) {
    case DlssQuality::Game:
        return 1; // Never used to override the native query.
    case DlssQuality::Dlaa:
        return 1;
    case DlssQuality::UltraQuality:
        return .77f;
    case DlssQuality::Quality:
        return 2.f / 3.f;
    case DlssQuality::Balanced:
        return 1.f / 1.7f;
    case DlssQuality::Performance:
        return .5f;
    case DlssQuality::UltraPerformance:
        return 1.f / 3.f;
    }
    return 1;
}
unsigned extent(unsigned output, float scale) {
    if (scale >= 1)
        return output;
    const auto lower = std::uint32_t(std::floor(double(output) * scale));
    return std::clamp(lower > 1 ? lower & ~1u : 1u, 1u, output);
}
std::uintptr_t observe_query(const unsigned *display, unsigned *optimal, unsigned *minimum,
                             unsigned *maximum) {
    auto &s = state();
    Call call(s);
    begin_menu_frame(s);
    const auto result = s.query(display, optimal, minimum, maximum);
    if (!s.active.load(std::memory_order_acquire) || (result & 0xff) == 0)
        return result;
    const auto requested = settings_snapshot();
    if (!fixed_dlss_resolution(requested.dlss) || !native_dlss::active())
        return result;
    std::uintptr_t config = 0;
    std::array<unsigned char, 0x7c> config_bytes{};
    if (!settings(s, config, config_bytes) ||
        (!config_bytes[0x45] && requested.dlss.quality != DlssQuality::Dlaa))
        return result;
    std::array<unsigned, 2> output{};
    if (!read(reinterpret_cast<std::uintptr_t>(display), output) || !output[0] || !output[1] ||
        output[0] > 16384 || output[1] > 16384)
        return result;
    const auto scale = ratio(requested.dlss.quality);
    const std::array<unsigned, 2> wanted{extent(output[0], scale), extent(output[1], scale)};
    // Preserve all three native answers if any guarded parameter write fails.
    std::array<unsigned, 2> old_optimal{}, old_minimum{}, old_maximum{};
    const auto opt = reinterpret_cast<std::uintptr_t>(optimal),
               low = reinterpret_cast<std::uintptr_t>(minimum),
               high = reinterpret_cast<std::uintptr_t>(maximum);
    if (!read(opt, old_optimal) || !read(low, old_minimum) || !read(high, old_maximum))
        return result;
    if (!write_value(opt, wanted) || !write_value(low, wanted) || !write_value(high, wanted)) {
        write_value(opt, old_optimal);
        write_value(low, old_minimum);
        write_value(high, old_maximum);
    }
    static TraceBudget trace;
    if (trace.take(trace_epoch(), 12)) {
        std::array<unsigned, 2> actual{};
        read(opt, actual);
        log_message("Size query request#" + std::to_string(trace_epoch()) +
                    ": quality=" + quality_name(requested.dlss.quality) + ", native_selector=" +
                    std::to_string(config_bytes[0x55]) + ", output=" + std::to_string(output[0]) + "x" +
                    std::to_string(output[1]) + ", original_optimal=" + std::to_string(old_optimal[0]) + "x" +
                    std::to_string(old_optimal[1]) + ", wanted=" + std::to_string(wanted[0]) + "x" +
                    std::to_string(wanted[1]) + ", returned=" + std::to_string(actual[0]) + "x" +
                    std::to_string(actual[1]));
    }
    return result;
}
void process_request(State &s, std::uintptr_t renderer) {
    if (!native_dlss::active())
        return;
    const auto request = s.requests.load(std::memory_order_acquire);
    const auto refresh = s.forced_refresh.load(std::memory_order_acquire);
    const bool requested_hdr = hdr::active();
    const bool hdr_changed = requested_hdr != s.previous_hdr;
    if (request == s.handled && refresh == s.handled_refresh && !hdr_changed)
        return;
    if (request != s.attempted || refresh != s.attempted_refresh) {
        s.attempted = request;
        s.attempted_refresh = refresh;
        s.attempts = 0;
        s.retry_at = 0;
    }
    if (GetTickCount64() < s.retry_at)
        return;
    const auto requested = settings_snapshot();
    const bool force = hdr_changed || (requested.dlss.enabled && refresh != s.handled_refresh);
    if (!s.attempts && !force && requested.dlss.enabled == s.previous_enabled &&
        requested.dlss.quality == s.previous_quality && requested.dlss.model_preset == s.previous_model) {
        s.handled = request;
        s.handled_refresh = refresh;
        return;
    }
    std::uintptr_t config = 0, manager = 0, context = 0, device = 0, callbacks = 0;
    std::array<unsigned char, 0x7c> bytes{};
    unsigned char flags = 0;
    if (!settings(s, config, bytes) ||
        (fixed_dlss_resolution(requested.dlss) && !bytes[0x45] && requested.dlss.quality != DlssQuality::Dlaa) ||
        !read(s.base + s.addresses.render_manager_global, manager) || !read(manager + 0x4220, context) ||
        !read(context + 0x181, flags) || !(flags & 1) || (flags & 0x30) ||
        !read(s.base + s.addresses.device_global, device) || !read(device + 0x30, callbacks) || !callbacks) {
        static TraceBudget trace;
        if (trace.take(trace_epoch(), 8)) {
            std::ostringstream line;
            line << "Native request#" << trace_epoch()
                 << " waiting: quality=" << quality_name(requested.dlss.quality)
                 << " model=" << model_name(requested.dlss.model_preset) << " mode=" << unsigned(bytes[0x54])
                 << " selector=" << unsigned(bytes[0x55]) << " native_scale_gate=" << unsigned(bytes[0x45])
                 << " context_flags=" << unsigned(flags) << " config=" << reinterpret_cast<void *>(config)
                 << " context=" << reinterpret_cast<void *>(context)
                 << " callbacks=" << reinterpret_cast<void *>(callbacks);
            log_message(line.str());
        }
        return;
    }
    if (s.original_settings != config) {
        s.original_settings = config;
        s.original_selector = bytes[0x55];
        s.selector_owned = false;
    }
    const auto next = static_cast<unsigned char>(static_cast<unsigned>(requested.dlss.quality) + 1);
    if (fixed_dlss_resolution(requested.dlss)) {
        // Each new ownership period starts from the latest native selection.
        if (!s.selector_owned)
            s.original_selector = bytes[0x55];
        if (next < 1 || next > 6 || !write_value(config + 0x55, next))
            return;
        s.owned_selector = next;
        s.selector_owned = true;
    } else if (s.selector_owned) {
        if (bytes[0x55] == s.owned_selector && !write_value(config + 0x55, s.original_selector))
            return;
        s.selector_owned = false;
    }
    const std::uint64_t invalidate = 0;
    if (!write_value(context + 0x150, invalidate))
        return;
    // The optimal-size table (+0x150) and the applied feature descriptor
    // (+0xCC) are distinct native caches. A model-only change leaves all
    // dimensions identical, so the native descriptor comparison otherwise
    // reuses the old model. Invalidate only its numeric display extent using
    // FFXIV's own uninitialized sentinel. The native query then releases and
    // recreates its feature through its normal path; we never own its handle.
    const bool recreate =
        force || requested.dlss.enabled != s.previous_enabled ||
        (requested.dlss.enabled && requested.dlss.model_preset != s.previous_model);
    if (recreate && !write_value(context + 0xcc, std::numeric_limits<std::uint64_t>::max()))
        return;
    const auto result = s.dispatcher(callbacks);
    ++s.attempts;
    if (!result && s.attempts < 3) {
        s.retry_at = GetTickCount64() + 250;
        return;
    }
    s.handled = request;
    // Acknowledge only the captured generation, never a concurrent new refresh.
    s.handled_refresh = refresh;
    s.previous_enabled = requested.dlss.enabled;
    s.previous_quality = requested.dlss.quality;
    s.previous_model = requested.dlss.model_preset;
    s.previous_hdr = requested_hdr;
    unsigned width[2]{};
    read(renderer + 0x428, width, sizeof(width));
    log_message("Native resize dispatched request#" + std::to_string(trace_epoch()) +
                ": quality=" + quality_name(requested.dlss.quality) +
                ", model=" + model_name(requested.dlss.model_preset) + ", selector=" + std::to_string(next) +
                ", result=" + std::to_string(result) + ", feature_refresh=" + std::to_string(recreate) +
                ", immediate renderer scene=" + std::to_string(width[0]) + "x" + std::to_string(width[1]) +
                "; evaluated frame will confirm application");
}
bool observe_tick(std::uintptr_t framework) {
    auto &s = state();
    Call call(s);
    s.tick_calls.fetch_add(1, std::memory_order_relaxed);
    if (s.active.load(std::memory_order_acquire)) {
        fg::simulation_begin();
        fg::note_trace(fg::TraceStage::FrameworkBodyBegin, 0, framework);
    }
    const auto result = [&] {
        fg::diagnostics::Scope phase("native.framework");
        return s.tick(framework);
    }();
    if (s.active.load(std::memory_order_acquire)) {
        fg::note_trace(fg::TraceStage::FrameworkBodyEnd, 0, framework, result ? 1 : 0);
        fg::simulation_end();
    }
    return result;
}
bool executable_pointer(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    if (address < 0x10000 || !VirtualQuery(reinterpret_cast<const void *>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const auto protect = info.Protect & 0xff;
    return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}
bool attach_tick_slot(State &s, bool installing = false) {
    std::lock_guard lock(s.tick_mutex);
    if (!installing && !s.active.load(std::memory_order_acquire))
        return false;
    const auto now = GetTickCount64();
    if (!installing && now < s.next_tick_check)
        return bool(s.tick_slot.slot);
    s.next_tick_check = now + 5000;
    if (!fg::presentation_requested(settings_snapshot().fg) || !s.addresses.framework_global ||
        !s.addresses.simulation_tick)
        return false;
    std::uintptr_t instance = 0, table = 0, target = 0;
    if (!read(s.base + s.addresses.framework_global, instance) || !read(instance, table) ||
        table % alignof(void *) || !read(table + 4 * sizeof(void *), target) || !executable_pointer(target)) {
        log_message("FG latency boundary waiting for readable native Framework instance and Tick slot");
        return false;
    }
    auto *slot = reinterpret_cast<void **>(table + 4 * sizeof(void *));
    if (s.tick_slot.slot) {
        if (slot != s.tick_slot.slot && target != reinterpret_cast<std::uintptr_t>(observe_tick)) {
            // Another owner may have copied/chained our slot. Do not wrap it
            // again and risk a cycle through that owner's saved original.
            log_message("FG Framework virtual-table changed after attachment; existing owners retained");
            return false;
        }
        if (!s.tick_calls.load(std::memory_order_relaxed)) {
            std::ostringstream line;
            line << "FG Framework Tick slot is bound but has no observed calls; instance="
                 << reinterpret_cast<void *>(instance)
                 << ", instance_vtable=" << reinterpret_cast<void *>(table)
                 << ", canonical_vtable=" << reinterpret_cast<void *>(s.base + s.addresses.framework_vtable)
                 << ", current_tick=" << reinterpret_cast<void *>(target);
            log_message(line.str());
        }
        return true;
    }
    if (target == reinterpret_cast<std::uintptr_t>(observe_tick))
        return false;
    const auto previous = s.tick;
    s.tick = reinterpret_cast<Tick>(target);
    if (!s.tick_slot.acquire(slot, reinterpret_cast<void *>(target),
                             reinterpret_cast<void *>(observe_tick))) {
        s.tick = previous;
        log_message(
            "FG latency boundary unavailable: the live Framework Tick slot changed during attachment");
        return false;
    }
    s.tick_instance = instance;
    std::ostringstream line;
    line << "FG latency markers attached to actual Framework instance; instance="
         << reinterpret_cast<void *>(instance) << ", instance_vtable=" << reinterpret_cast<void *>(table)
         << ", canonical_vtable=" << reinterpret_cast<void *>(s.base + s.addresses.framework_vtable)
         << ", original_tick=" << reinterpret_cast<void *>(target) << "; existing native detours retained";
    log_message(line.str());
    return true;
}
void release_tick_slot(State &s) {
    std::lock_guard lock(s.tick_mutex);
    s.tick_slot.release();
}
void observe_update(std::uintptr_t renderer, float elapsed) {
    auto &s = state();
    Call call(s);
    if (s.active.load(std::memory_order_acquire)) {
        fg::diagnostics::Scope prelude("native.renderer_prelude");
        {
            fg::diagnostics::SlowScope phase("native.tick_attachment");
            attach_tick_slot(s);
        }
        fg::simulation_end();
        fg::render_begin();
        {
            fg::diagnostics::SlowScope phase("native.frontend_policy");
            begin_menu_frame(s);
        }
        {
            fg::diagnostics::SlowScope phase("native.settings_request");
            process_request(s, renderer);
        }
    }
    fg::diagnostics::Scope phase("native.renderer_update");
    s.update(renderer, elapsed);
}
std::uintptr_t generate(std::uintptr_t opaque, float *x, float *y, unsigned frame, unsigned phases) {
    auto &s = state();
    Call call(s);
    if (!s.active.load(std::memory_order_acquire) ||
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != s.base + s.addresses.halton_camera_return ||
        !x || !y || !fixed_dlss_resolution(settings_snapshot().dlss))
        return s.generator(opaque, x, y, frame, phases);
    std::uintptr_t config = 0, renderer = 0, device = 0;
    std::array<unsigned char, 0x7c> bytes{};
    std::array<unsigned short, 3> native{};
    if (!settings(s, config, bytes) || !read(s.base + s.addresses.renderer_global, renderer) ||
        !read(renderer + 0x700, native) || !native[2])
        return s.generator(opaque, x, y, frame, phases);
    float scaling = 0, amount = 0;
    std::memcpy(&scaling, bytes.data() + 0x4c, 4);
    std::memcpy(&amount, bytes.data() + 0x78, 4);
    const auto relative_scale = (float(native[0]) / native[2]) * scaling;
    const auto strength = (amount - .001f) * relative_scale * relative_scale + .001f;
    if (!std::isfinite(strength) || strength < .125f || strength > 4)
        return s.generator(opaque, x, y, frame, phases);
    std::array<unsigned, 2> render{}, display{};
    const bool native_sizes = read(renderer + 0x428, render) &&
                              read(s.base + s.addresses.device_global, device) &&
                              read(device + 0x8c, display);
    if (!native_sizes) {
        const auto packed_render = s.render.load(std::memory_order_acquire),
                   packed_display = s.display.load(std::memory_order_acquire);
        render = {unsigned(packed_render >> 32), unsigned(packed_render)};
        display = {unsigned(packed_display >> 32), unsigned(packed_display)};
    }
    const auto original_phases = phases;
    if (phases == std::numeric_limits<unsigned>::max() && render[0] && render[1] && display[0] &&
        display[1] && render[0] <= display[0] && render[1] <= display[1] && display[0] <= 16384 &&
        display[1] <= 16384) {
        const auto area = (double(display[0]) / render[0]) * (double(display[1]) / render[1]);
        phases = unsigned(std::clamp(std::ceil(8 * area), 8., 4096.));
    }
    const auto result = s.generator(opaque, x, y, frame, phases);
    float hx = 0, hy = 0;
    if (read(reinterpret_cast<std::uintptr_t>(x), hx) && read(reinterpret_cast<std::uintptr_t>(y), hy) &&
        std::isfinite(hx) && std::isfinite(hy) && std::abs(hx) <= .501f && std::abs(hy) <= .501f &&
        std::abs(strength - 1) > 1e-6f) {
        hx /= strength;
        hy /= strength;
        write_value(reinterpret_cast<std::uintptr_t>(x), hx);
        write_value(reinterpret_cast<std::uintptr_t>(y), hy);
    }
    static TraceBudget trace;
    if (trace.take(trace_epoch())) {
        std::ostringstream line;
        line << std::setprecision(9) << "Raster jitter request#" << trace_epoch() << " frame=" << frame
             << " phase_argument=" << original_phases << " used_phases=" << phases << " strength=" << strength
             << " Halton_after_normalization=" << hx << ',' << hy << " render=" << render[0] << 'x'
             << render[1] << " display=" << display[0] << 'x' << display[1];
        log_message(line.str());
    }
    return result;
}
bool read_fg_camera(std::uintptr_t at, fg::Camera &c, std::string &reason) {
    // FFXIVClientStructs Render.Camera, validated at the main queued-SR
    // getter. A rejected native camera must not be replaced by synthetic data.
    c = {};
    reason.clear();
    std::array<float, 5> lens{};
    std::array<unsigned char, 5> flags{};
    if (!read(at + 0x10, c.view) || !read(at + 0x1a0, c.projection) || !read(at + 0x1e8, lens) ||
        !read(at + 0x200, flags)) {
        reason = "unreadable native fields";
        return false;
    }
    c.fov = lens[0];
    c.aspect = lens[2];
    c.near_plane = lens[3];
    c.far_plane = lens[4];
    c.inverted = flags[3] == 0;
    c.infinite = flags[4] == 0;
    // Native Render.Camera uses a 16-byte stride for each three-component
    // affine row. Its view/projection multiply and affine inverse consume
    // only XYZ; the fourth lanes are padding, including uninitialized bits.
    // Preserve the native row-vector basis and translation, and expand the
    // implicit homogeneous column for the providers' real float4x4 matrices.
    c.view[3] = c.view[7] = c.view[11] = 0;
    c.view[15] = 1;
    for (const auto value : c.view)
        if (!std::isfinite(value)) {
            reason = "nonfinite view matrix";
            return false;
        }
    for (const auto value : c.projection)
        if (!std::isfinite(value)) {
            reason = "nonfinite projection matrix";
            return false;
        }
    if (flags[0]) {
        reason = "orthographic native camera";
        return false;
    }
    if (!(c.near_plane > 0 && c.near_plane < 100 && c.fov > .05f && c.fov < 3.1f && c.aspect > .25f &&
          c.aspect < 8)) {
        reason = "invalid native lens";
        return false;
    }
    // FarPlane is unused by the game when FiniteFarPlane is false. Requiring
    // its stale/zero storage to exceed NearPlane incorrectly rejects a valid
    // infinite projection. Forward the actual projection convention instead.
    if (c.infinite)
        c.far_plane = std::numeric_limits<float>::infinity();
    else if (!std::isfinite(c.far_plane) || c.far_plane <= c.near_plane) {
        reason = "invalid finite far plane";
        return false;
    }
    // Validate the meaningful native basis instead of its padding. A proper
    // camera rotation must remain nonsingular, orthonormal and unreflected;
    // completing the implicit column must not admit arbitrary/misread data.
    const auto dot = [&](unsigned a, unsigned b) {
        double value = 0;
        for (unsigned column = 0; column < 3; ++column)
            value += double(c.view[a * 4 + column]) * c.view[b * 4 + column];
        return value;
    };
    for (unsigned row = 0; row < 3; ++row) {
        if (std::abs(dot(row, row) - 1) >= .02) {
            reason = "invalid native view basis";
            return false;
        }
        for (unsigned other = 0; other < row; ++other)
            if (std::abs(dot(row, other)) >= .02) {
                reason = "invalid native view basis";
                return false;
            }
    }
    const double determinant =
        double(c.view[0]) * (double(c.view[5]) * c.view[10] - double(c.view[6]) * c.view[9]) -
        double(c.view[1]) * (double(c.view[4]) * c.view[10] - double(c.view[6]) * c.view[8]) +
        double(c.view[2]) * (double(c.view[4]) * c.view[9] - double(c.view[5]) * c.view[8]);
    if (std::abs(determinant - 1) >= .02) {
        reason = "invalid native view orientation";
        return false;
    }
    if (std::abs(c.projection[15]) >= .001f || std::abs(std::abs(c.projection[11]) - 1) >= .01f ||
        c.projection[0] <= 0 || c.projection[5] <= 0) {
        reason = "invalid perspective projection";
        return false;
    }
    if (std::abs(c.projection[5] / c.projection[0] - c.aspect) >= .05f) {
        reason = "projection/lens aspect mismatch";
        return false;
    }
    c.valid = true;
    return true;
}
void *process_jitter_packet(State &s, std::uintptr_t camera, void *result, std::uintptr_t caller) {
    if (!s.active.load(std::memory_order_acquire) || caller != s.base + s.addresses.jitter_packet_return ||
        !result)
        return result;
    if (fg::output_requested()) {
        // Render.Camera layout from FFXIVClientStructs, reached only through
        // the validated main queued-SR callsite. Never infer camera matrices.
        fg::Camera c;
        std::string reason;
        read_fg_camera(camera, c, reason);
        static std::atomic<unsigned> samples{0};
        static std::atomic<ULONGLONG> next_failure{0};
        const auto now = GetTickCount64();
        auto next = next_failure.load();
        if (samples.fetch_add(1) < 4 ||
            (!c.valid && now >= next && next_failure.compare_exchange_strong(next, now + 10000))) {
            std::ostringstream line;
            line << "FG native camera: valid=" << c.valid
                 << ", reason=" << (reason.empty() ? "accepted" : reason) << ", lens=" << c.fov << ','
                 << c.aspect << ',' << c.near_plane << ',' << c.far_plane << ", inverted=" << c.inverted
                 << ", infinite=" << c.infinite << ", view_w=" << c.view[15]
                 << ", projection=" << c.projection[0] << ',' << c.projection[5] << ',' << c.projection[11]
                 << ',' << c.projection[15];
            line << ", view=[";
            for (unsigned i = 0; i < c.view.size(); ++i)
                line << (i ? "," : "") << c.view[i];
            line << "], projection=[";
            for (unsigned i = 0; i < c.projection.size(); ++i)
                line << (i ? "," : "") << c.projection[i];
            line << ']';
            log_message(line.str());
        }
        fg::camera(c);
    }
    // Native camera observation also serves FG/latency with vanilla DLSS DRS.
    // Only the optional DLSS controls own raster-jitter replacement below.
    if (!fixed_dlss_resolution(settings_snapshot().dlss))
        return result;
    std::array<float, 4> values{};
    std::array<unsigned, 2> size{};
    std::uintptr_t renderer = 0;
    if (!read(reinterpret_cast<std::uintptr_t>(result), values) ||
        !read(s.base + s.addresses.renderer_global, renderer) || !read(renderer + 0x428, size) || !size[0] ||
        !size[1] || size[0] > 16384 || size[1] > 16384)
        return result;
    for (const auto value : values)
        if (!std::isfinite(value))
            return result;
    if (std::abs(values[0]) > 4 || std::abs(values[1]) > 4)
        return result;
    float x = values[2] * size[0] * .5f, y = -values[3] * size[1] * .5f;
    if (values[0] == 0 && values[1] == 0)
        x = y = 0;
    const bool valid = std::isfinite(x) && std::isfinite(y) && std::abs(x) <= 2 && std::abs(y) <= 2 &&
                       std::abs(x * values[1] - y * values[0]) <= 1e-5f &&
                       x * values[0] + y * values[1] >= -1e-6f;
    static TraceBudget trace;
    if (trace.take(trace_epoch())) {
        std::ostringstream line;
        line << std::setprecision(9) << "Queued jitter request#" << trace_epoch()
             << " camera=" << reinterpret_cast<void *>(camera) << " native_pixels=" << values[0] << ','
             << values[1] << " raster_ndc=" << values[2] << ',' << values[3] << " aligned_pixels=" << x << ','
             << y << " alignment_accepted=" << valid << " render=" << size[0] << 'x' << size[1];
        log_message(line.str());
    }
    if (!valid)
        return result;
    thread_local float packet[2];
    packet[0] = x;
    packet[1] = y;
    return packet;
}
void *observe_getter(std::uintptr_t camera) {
    auto &s = state();
    Call call(s);
    auto *result = s.getter(camera);
    return process_jitter_packet(s, camera, result, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}
bool addresses_equal(const Addresses &a, const Addresses &b) {
    return a.settings_global == b.settings_global && a.renderer_global == b.renderer_global &&
           a.render_manager_global == b.render_manager_global && a.device_global == b.device_global &&
           a.query_size == b.query_size && a.renderer_update == b.renderer_update &&
           a.resize_dispatch == b.resize_dispatch && a.jitter_getter == b.jitter_getter &&
           a.feature_descriptor_compare == b.feature_descriptor_compare && a.game_main == b.game_main &&
           a.frontend_reference == b.frontend_reference &&
           a.frontend_state_reference == b.frontend_state_reference &&
           a.jitter_packet_return == b.jitter_packet_return && a.halton_generator == b.halton_generator &&
           a.halton_camera_return == b.halton_camera_return && a.simulation_tick == b.simulation_tick &&
           a.framework_vtable == b.framework_vtable && a.framework_global == b.framework_global &&
           a.framework_instance_reference == b.framework_instance_reference;
}
bool verify_live(const ImageView &file, std::uintptr_t base, std::uint32_t rva, std::size_t length) {
    const auto expected = file.read(rva, length);
    std::array<std::uint8_t, 128> actual{};
    return length <= actual.size() && expected.size() == length && read(base + rva, actual.data(), length) &&
           std::equal(expected.begin(), expected.end(), actual.begin());
}
} // namespace
bool install_hooks(const Report &report, std::span<const std::uint8_t> raw_file, std::string &error) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.active.load())
        return true;
    if (!s.targets.empty()) {
        error = "Previous native hooks are still owned; restart required";
        return false;
    }
    const auto verified = inspect_executable(raw_file);
    std::string parse_error;
    const auto file = parse_pe(raw_file, &parse_error);
    if (!file || !report.quality || !report.jitter || !verified.quality || !verified.jitter ||
        !addresses_equal(report.addresses, verified.addresses)) {
        error = "Native quality/jitter signatures are not independently verified";
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto &a = verified.addresses;
    for (const auto [rva, length] :
         std::array<std::pair<std::uint32_t, std::size_t>, 10>{{{a.query_size, 64},
                                                                {a.feature_descriptor_compare, 40},
                                                                {a.renderer_update, 96},
                                                                {a.resize_dispatch, 64},
                                                                {a.jitter_getter, 8},
                                                                {a.halton_generator, 32},
                                                                {a.jitter_packet_return - 5, 13},
                                                                {a.halton_camera_return - 5, 32},
                                                                {a.camera_update, 128},
                                                                {a.preview_accessor, 18}}})
        if (!verify_live(*file, base, rva, length)) {
            error = "Live native code differs from the inspected file (patch or another hook)";
            return false;
        }
    if (a.game_main && (!verify_live(*file, base, a.frontend_reference, 16) ||
                        !verify_live(*file, base, a.frontend_state_reference, 12))) {
        error = "Live frontend state references differ from the inspected executable";
        return false;
    }
    s.base = base;
    s.addresses = a;
    s.tick_calls = 0;
    s.tick_instance = 0;
    s.next_tick_check = 0;
    s.dispatcher = reinterpret_cast<Dispatcher>(base + a.resize_dispatch);
    const auto rollback = [&] {
        release_tick_slot(s);
        for (auto *target : s.targets)
            MH_DisableHook(target);
        const auto deadline = GetTickCount64() + 1000;
        while (s.inside.load(std::memory_order_acquire) && GetTickCount64() < deadline)
            Sleep(1);
        if (s.inside.load(std::memory_order_acquire)) {
            error += "; busy native trampolines retained until restart";
            return;
        }
        for (auto it = s.targets.rbegin(); it != s.targets.rend(); ++it)
            MH_RemoveHook(*it);
        s.targets.clear();
    };
    const auto add = [&](std::uint32_t rva, void *detour, void **original) {
        auto *target = reinterpret_cast<void *>(base + rva);
        const auto result = MH_CreateHook(target, detour, original);
        if (result != MH_OK) {
            error = std::string("Creating native hook failed: ") + MH_StatusToString(result);
            return false;
        }
        s.targets.push_back(target);
        return true;
    };
    if (!add(a.query_size, reinterpret_cast<void *>(observe_query), reinterpret_cast<void **>(&s.query)) ||
        !add(a.renderer_update, reinterpret_cast<void *>(observe_update),
             reinterpret_cast<void **>(&s.update)) ||
        !add(a.jitter_getter, reinterpret_cast<void *>(observe_getter),
             reinterpret_cast<void **>(&s.getter)) ||
        !add(a.halton_generator, reinterpret_cast<void *>(generate),
             reinterpret_cast<void **>(&s.generator))) {
        rollback();
        return false;
    }
    // Native hooks may be executing when ReShade shuts addons down. Pin our code
    // for this process; disabling the addon still removes our hooks, never busy code.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&install_hooks), &self)) {
        rollback();
        error = "Could not retain native callback code for safe process teardown";
        return false;
    }
    if (fg::presentation_requested(settings_snapshot().fg) && a.framework_instance_reference &&
        verify_live(*file, base, a.framework_instance_reference, 11))
        attach_tick_slot(s, true);
    for (auto *target : s.targets) {
        const auto result = MH_EnableHook(target);
        if (result != MH_OK) {
            rollback();
            error = std::string("Enabling native hook failed: ") + MH_StatusToString(result);
            return false;
        }
    }
    s.active.store(true, std::memory_order_release);
    s.requests.fetch_add(1, std::memory_order_release);
    log_message("Native quality and same-frame raster/NGX jitter hooks enabled");
    return true;
}
void remove_hooks() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.active.store(false, std::memory_order_release);
    release_tick_slot(s);
    finish_frame();
    for (auto *target : s.targets)
        MH_DisableHook(target);
    const auto deadline = GetTickCount64() + 1000;
    while (s.inside.load(std::memory_order_acquire) && GetTickCount64() < deadline)
        Sleep(1);
    if (s.inside.load(std::memory_order_acquire)) {
        log_message("Native hooks disabled; busy trampolines retained safely until process exit");
        return;
    }
    for (auto *target : s.targets)
        MH_RemoveHook(target);
    s.targets.clear();
}
void settings_changed() {
    state().requests.fetch_add(1, std::memory_order_release);
}
void finish_frame() {
    auto &s = state();
    std::lock_guard lock(s.menu_mutex);
    release_menu_frame(s);
}
void request_feature_refresh() {
    state().forced_refresh.fetch_add(1, std::memory_order_release);
    settings_changed();
}
bool quality_hooks_active() {
    return state().active.load(std::memory_order_acquire);
}
bool jitter_hooks_active() {
    return state().active.load(std::memory_order_acquire);
}
void measured_size(unsigned rw, unsigned rh, unsigned dw, unsigned dh) {
    auto &s = state();
    if (!rw || !rh || !dw || !dh || rw > 16384 || rh > 16384 || dw > 16384 || dh > 16384)
        return;
    s.render.store((std::uint64_t(rw) << 32) | rh, std::memory_order_release);
    s.display.store((std::uint64_t(dw) << 32) | dh, std::memory_order_release);
}
} // namespace xivrender::ffxiv
