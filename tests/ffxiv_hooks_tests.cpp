// Exercise the production request transaction against the native descriptor
// cache contract recovered from FFXIV, without modifying an executable or GPU.
#include "../src/ffxiv_hooks.cpp"
#include <cassert>
#include <iostream>

namespace {
xivrender::Settings requested;
bool sr_ready = true;
std::vector<std::string> messages;
} // namespace
namespace xivrender {
Settings settings_snapshot() {
    return requested;
}
void log_message(const std::string &message) {
    messages.push_back(message);
}
std::uint64_t trace_epoch() {
    return ffxiv::state().requests.load();
}
} // namespace xivrender
namespace xivrender::native_dlss {
bool active() {
    return sr_ready;
}
} // namespace xivrender::native_dlss

namespace {
using namespace xivrender;
using namespace xivrender::ffxiv;

struct Fixture {
    std::array<unsigned char, 0x80> globals{};
    std::array<unsigned char, 0x4170> game_main{};
    std::array<unsigned char, 0x100> config{};
    std::array<unsigned char, 0x4300> manager{};
    std::array<unsigned char, 0x200> context{};
    std::array<unsigned char, 0x200> device{};
    std::array<unsigned char, 0x800> renderer{};
    std::uint64_t callbacks = 0x12345678;
    unsigned dispatches = 0, creates = 0, releases = 0;
    DlssModelPreset model = DlssModelPreset::Automatic;
    std::array<unsigned, 2> size{};
    unsigned char original_selector = 5;
    static inline Fixture *current = nullptr;

    template <class Buffer, class Value> static void put(Buffer &buffer, size_t offset, const Value &value) {
        assert(offset + sizeof(value) <= buffer.size());
        std::memcpy(buffer.data() + offset, &value, sizeof(value));
    }
    template <class Value, class Buffer> static Value get(const Buffer &buffer, size_t offset) {
        assert(offset + sizeof(Value) <= buffer.size());
        Value value{};
        std::memcpy(&value, buffer.data() + offset, sizeof(value));
        return value;
    }
    static std::array<unsigned char, 0x2c> descriptor(unsigned char selector) {
        std::array<unsigned char, 0x2c> result{};
        const std::array<std::array<unsigned, 2>, 7> inputs{{{3840, 2160},
                                                             {3840, 2160},
                                                             {2956, 1662},
                                                             {2560, 1440},
                                                             {2258, 1270},
                                                             {1920, 1080},
                                                             {1280, 720}}};
        assert(selector < inputs.size());
        put(result, 0, std::array<unsigned, 2>{3840, 2160});
        put(result, 8, inputs[selector]);
        put(result, 0x20, unsigned(selector));
        return result;
    }
    static unsigned char dispatch(std::uintptr_t callback_owner) {
        auto &f = *current;
        assert(callback_owner == reinterpret_cast<std::uintptr_t>(&f.callbacks));
        ++f.dispatches;
        // The optimal-size table and the applied feature descriptor are two
        // distinct caches. Clearing +150 alone cannot force a same-size model.
        assert(get<std::uint64_t>(f.context, 0x150) == 0);
        const auto next = descriptor(f.config[0x45] ? f.config[0x55] : 1);
        put(f.context, 0xa0, next);
        if (std::memcmp(f.context.data() + 0xcc, next.data(), next.size()) != 0) {
            ++f.releases;
            ++f.creates;
            put(f.context, 0xcc, next);
            f.model = requested.dlss.enabled ? requested.dlss.model_preset : DlssModelPreset::Automatic;
        }
        f.size = get<std::array<unsigned, 2>>(next, 8);
        put(f.renderer, 0x428, f.size);
        put(f.context, 0x150, std::array<unsigned, 2>{3840, 2160});
        return 1;
    }
    Fixture() {
        current = this;
        requested = {};
        hdr::configure(requested, true);
        requested.dlss.quality = DlssQuality::Quality; // Fixed-quality fixture; production defaults to native DRS.
        // Tests opt into independent consumers explicitly. Default user
        // settings enable latency/depth, which now also need native frontend SR.
        requested.fg.reflex = false;
        requested.reshade.automatic_depth = false;
        requested.dlss.model_preset = DlssModelPreset::K;
        sr_ready = true;
        messages.clear();
        auto &s = state();
        s.active = true;
        s.inside = 0;
        s.requests = 1;
        s.forced_refresh = 0;
        s.render = 0;
        s.display = 0;
        s.base = reinterpret_cast<std::uintptr_t>(globals.data());
        s.addresses = {};
        s.addresses.settings_global = 0x10;
        s.addresses.render_manager_global = 0x20;
        s.addresses.device_global = 0x30;
        s.addresses.renderer_global = 0x40;
        s.dispatcher = dispatch;
        s.handled = s.attempted = s.attempts = 0;
        s.handled_refresh = s.attempted_refresh = 0;
        s.retry_at = 0;
        s.previous_enabled = false;
        s.previous_hdr = false;
        s.previous_quality = DlssQuality::Quality;
        s.previous_model = DlssModelPreset::Automatic;
        s.original_settings = 0;
        s.original_selector = s.owned_selector = 0;
        s.selector_owned = false;
        s.menu_config = 0;
        s.menu_gate_owned = s.menu_active = false;
        s.menu_load_state = 0;
        put(globals, 0x10, reinterpret_cast<std::uintptr_t>(config.data()));
        put(globals, 0x20, reinterpret_cast<std::uintptr_t>(manager.data()));
        put(globals, 0x30, reinterpret_cast<std::uintptr_t>(device.data()));
        put(globals, 0x40, reinterpret_cast<std::uintptr_t>(renderer.data()));
        put(manager, 0x4220, reinterpret_cast<std::uintptr_t>(context.data()));
        put(device, 0x30, reinterpret_cast<std::uintptr_t>(&callbacks));
        put(device, 0x8c, std::array<unsigned, 2>{3840, 2160});
        config[0x54] = 2;
        config[0x45] = 1;
        config[0x55] = original_selector;
        context[0x181] = 1;
        put(context, 0xcc, descriptor(original_selector));
        put(context, 0x150, std::array<unsigned, 2>{3840, 2160});
    }
    ~Fixture() {
        finish_frame();
        state().menu_active = false;
        current = nullptr;
    }
    void enable_frontend() {
        const auto offset = reinterpret_cast<std::uintptr_t>(game_main.data()) - state().base;
        assert(offset && offset <= UINT32_MAX);
        state().addresses.game_main = static_cast<std::uint32_t>(offset);
        config[0x45] = 0;
    }
    void tick() {
        process_request(state(), reinterpret_cast<std::uintptr_t>(renderer.data()));
    }
    void change(DlssModelPreset next) {
        requested.dlss.model_preset = next;
        settings_changed();
    }
};

void live_model_and_coalescing() {
    Fixture f;
    f.tick();
    assert(f.dispatches == 1 && f.creates == 1 && f.model == DlssModelPreset::K);
    assert((f.size == std::array<unsigned, 2>{2560, 1440}));
    const auto original_size = f.size;
    f.change(DlssModelPreset::L);
    f.tick();
    assert(f.dispatches == 2 && f.creates == 2 && f.model == DlssModelPreset::L);
    assert(f.size == original_size && f.config[0x55] == 3);
    // Changes between renderer updates coalesce to the final choice.
    f.change(DlssModelPreset::M);
    f.change(DlssModelPreset::K);
    f.tick();
    assert(f.dispatches == 3 && f.creates == 3 && f.model == DlssModelPreset::K);
    f.tick();
    assert(f.dispatches == 3 && f.creates == 3);
    // An unrelated saved setting must not destroy model history.
    requested.dlss.automatic_mip_bias = false;
    settings_changed();
    f.tick();
    assert(f.dispatches == 3 && f.creates == 3);
    request_feature_refresh();
    f.tick();
    assert(f.dispatches == 4 && f.creates == 4 && f.size == original_size);
    requested.dlss.enabled = false;
    settings_changed();
    f.tick();
    assert(f.config[0x55] == f.original_selector && !state().selector_owned);
    assert(f.model == DlssModelPreset::Automatic);
}

void native_drs_model_and_restore() {
    Fixture f;
    requested.dlss.quality = DlssQuality::Game;
    f.tick();
    assert(f.config[0x55] == f.original_selector && !state().selector_owned);
    assert(f.model == DlssModelPreset::K);
    f.change(DlssModelPreset::M);
    f.tick();
    assert(f.config[0x55] == f.original_selector && f.model == DlssModelPreset::M);
    // Native settings can change while the addon does not own resolution.
    f.config[0x55] = 4;
    requested.dlss.quality = DlssQuality::Quality;
    settings_changed();
    f.tick();
    assert(f.config[0x55] == 3 && state().selector_owned);
    requested.dlss.quality = DlssQuality::Game;
    settings_changed();
    f.tick();
    assert(f.config[0x55] == 4 && !state().selector_owned && f.model == DlssModelPreset::M);
    // Every new ownership period restores its own native starting value.
    f.config[0x55] = 6;
    requested.dlss.quality = DlssQuality::Balanced;
    settings_changed();
    f.tick();
    requested.dlss.quality = DlssQuality::Game;
    settings_changed();
    f.tick();
    assert(f.config[0x55] == 6 && !state().selector_owned);
    f.enable_frontend();
    requested.fg.enabled = requested.fg.reflex = requested.nr.enabled = true;
    requested.reshade.automatic_depth = true;
    const auto native_config = f.config;
    begin_menu_frame(state());
    assert(f.config == native_config && !state().menu_gate_owned);
}
void live_hdr_native_feature_recreation() {
    Fixture f;
    requested.dlss.enabled = false;
    requested.dlss.quality = DlssQuality::Game;
    f.tick();
    const auto baseline = f.creates;
    const auto config = f.config;
    hdr::commit_output(true);
    f.tick();
    assert(f.creates == baseline + 1 && f.config == config && state().previous_hdr);
    f.tick();
    assert(f.creates == baseline + 1);
    hdr::commit_output(false);
    f.tick();
    assert(f.creates == baseline + 2 && f.config == config && !state().previous_hdr);
}

void activation_and_readiness() {
    Fixture f;
    // Saved settings may equal the remembered tuple before SR interception.
    state().previous_enabled = requested.dlss.enabled;
    state().previous_quality = requested.dlss.quality;
    state().previous_model = requested.dlss.model_preset;
    state().handled = state().requests.load();
    f.config[0x55] = 3;
    Fixture::put(f.context, 0xcc, Fixture::descriptor(3));
    request_feature_refresh();
    sr_ready = false;
    f.tick();
    assert(!f.dispatches && state().handled_refresh == 0);
    sr_ready = true;
    // A pending native transition cannot be overwritten by the addon.
    f.context[0x181] = 0x11;
    f.tick();
    assert(!f.dispatches && state().handled_refresh == 0);
    f.context[0x181] = 1;
    f.tick();
    assert(f.dispatches == 1 && f.creates == 1);
    assert(state().handled_refresh == state().forced_refresh.load());
    f.change(DlssModelPreset::L);
    f.config[0x54] = 1; // Native FSR mode: no DLSS control or memory edits.
    const auto saved_context = f.context;
    const auto saved_config = f.config;
    f.tick();
    assert(f.dispatches == 1 && f.context == saved_context && f.config == saved_config);
    f.config[0x54] = 2;
    f.tick();
    assert(f.dispatches == 2 && f.creates == 2 && f.model == DlssModelPreset::L);
}

void external_selector_ownership() {
    Fixture f;
    f.tick();
    f.config[0x55] = 6; // A native or third-party choice after our last write.
    requested.dlss.enabled = false;
    settings_changed();
    f.tick();
    assert(f.config[0x55] == 6 && !state().selector_owned);
}

void disable_same_quality() {
    Fixture f;
    f.original_selector = 3;
    f.config[0x55] = 3;
    Fixture::put(f.context, 0xcc, Fixture::descriptor(3));
    f.tick();
    assert(f.creates == 1 && f.model == DlssModelPreset::K);
    const auto original_size = f.size;
    requested.dlss.enabled = false;
    settings_changed();
    f.tick();
    assert(f.creates == 2 && f.model == DlssModelPreset::Automatic);
    assert(f.config[0x55] == 3 && f.config[0x45] == 1 && f.size == original_size);
}

void disable_with_native_gate_closed() {
    Fixture f;
    f.tick();
    f.config[0x45] = 0;
    requested.dlss.enabled = false;
    settings_changed();
    f.tick();
    assert(f.creates == 2 && f.model == DlssModelPreset::Automatic);
    assert(f.config[0x55] == f.original_selector && f.config[0x45] == 0);
    assert((f.size == std::array<unsigned, 2>{3840, 2160}));
}
void frontend_rendering_and_cleanup() {
    Fixture f;
    f.enable_frontend();
    begin_menu_frame(state());
    assert(f.config[0x45] == 1 && state().menu_gate_owned);
    f.tick();
    assert(f.creates == 1 && (f.size == std::array<unsigned, 2>{2560, 1440}));
    // Late native quality/size/postprocess consumers must see the same policy.
    assert(f.config[0x45] == 1);
    finish_frame();
    assert(f.config[0x45] == 0 && !state().menu_gate_owned);
    begin_menu_frame(state());
    f.tick();
    assert(f.creates == 1); // Present restoration is not a new model request.
    finish_frame();
    f.change(DlssModelPreset::L);
    begin_menu_frame(state());
    f.tick();
    assert(f.creates == 2 && f.model == DlssModelPreset::L);
    finish_frame();
    // The native backend can change before Present; the owned gate still
    // needs restoration rather than leaking into FSR dynamic scaling.
    begin_menu_frame(state());
    f.config[0x54] = 1;
    finish_frame();
    assert(f.config[0x45] == 0);
}
void frontend_predicate_and_ownership() {
    Fixture f;
    f.enable_frontend();
    for (const auto offset : {0x4100u, 0x4104u, 0x4108u}) {
        Fixture::put(f.game_main, offset, std::uint32_t(0x100));
        begin_menu_frame(state());
        assert(f.config[0x45] == 0 && !state().menu_gate_owned);
        Fixture::put(f.game_main, offset, std::uint32_t(0));
    }
    f.game_main[0x40fe] = 1;
    begin_menu_frame(state());
    assert(f.config[0x45] == 0);
    f.game_main[0x40fe] = 0;
    const auto verified_instance = state().addresses.game_main;
    state().addresses.game_main = 0;
    begin_menu_frame(state());
    assert(f.config[0x45] == 0);
    state().addresses.game_main = verified_instance;
    sr_ready = false;
    begin_menu_frame(state());
    assert(f.config[0x45] == 0);
    sr_ready = true;
    requested.dlss.enabled = false;
    begin_menu_frame(state());
    assert(f.config[0x45] == 0);
    requested.dlss.enabled = true;
    f.config[0x54] = 1;
    begin_menu_frame(state());
    assert(f.config[0x45] == 0);
    f.config[0x54] = 2;
    begin_menu_frame(state());
    assert(f.config[0x45] == 1);
    f.config[0x45] = 0; // A native policy change while rendering.
    finish_frame();
    assert(f.config[0x45] == 0);
    f.config[0x45] = 1; // A native gate we never owned must remain untouched.
    begin_menu_frame(state());
    finish_frame();
    assert(f.config[0x45] == 1);
    f.config[0x45] = 0;
    begin_menu_frame(state());
    f.game_main[0x40fe] = 1;
    Fixture::put(f.game_main, 0x4100, std::uint32_t(4));
    Fixture::put(f.game_main, 0x4108, std::uint32_t(100));
    begin_menu_frame(state());
    assert(f.config[0x45] == 0 && !state().menu_active);
    // Replaced configuration objects must not be edited during cleanup.
    f.game_main = {};
    begin_menu_frame(state());
    assert(f.config[0x45] == 1);
    auto replacement = f.config;
    replacement[0x45] = 0;
    Fixture::put(f.globals, 0x10, reinterpret_cast<std::uintptr_t>(replacement.data()));
    finish_frame();
    assert(replacement[0x45] == 0 && f.config[0x45] == 1);
    Fixture::put(f.globals, 0x10, reinterpret_cast<std::uintptr_t>(f.config.data()));
}
void frontend_inputs_without_dlss_controls() {
    const auto check = [](unsigned consumer) {
        Fixture f;
        f.enable_frontend();
        requested.dlss.enabled = false;
        requested.fg.enabled = consumer == 0;
        requested.fg.reflex = consumer == 1;
        requested.nr.enabled = consumer == 2;
        requested.reshade.automatic_depth = consumer == 3;
        const auto original_config = f.config;
        const auto original_context = f.context;
        const auto refresh = state().forced_refresh.load();
        begin_menu_frame(state());
        assert(!state().menu_active && !state().menu_gate_owned && f.config == original_config);
        assert(f.context == original_context && state().forced_refresh.load() == refresh);
        // Independent consumers must not enable the native scaling gate.
        f.tick();
        assert(f.config == original_config && f.config[0x55] == f.original_selector);
        assert((f.size == std::array<unsigned, 2>{3840, 2160}));
        assert(f.model == DlssModelPreset::Automatic && !state().selector_owned);
        finish_frame();
        assert(f.config == original_config && !state().menu_gate_owned);
        // FSR is a native user selection, never a reason to force DLSS for
        // an independent consumer, even while it remains requested.
        f.config[0x54] = 1;
        const auto fsr_config = f.config;
        begin_menu_frame(state());
        assert(f.config == fsr_config && !state().menu_active && !state().menu_gate_owned);
    };
    for (unsigned consumer = 0; consumer < 4; ++consumer)
        check(consumer);
}
void frontend_login_logout_transition() {
    Fixture f;
    f.enable_frontend();
    begin_menu_frame(state());
    f.tick();
    finish_frame();
    assert(f.creates == 1);
    Fixture::put(f.game_main, 0x4100, std::uint32_t(2));
    Fixture::put(f.game_main, 0x4108, std::uint32_t(100));
    f.game_main[0x40fe] = 1;
    f.config[0x45] = 1;
    f.config[0x55] = 0; // Native graphics settings refreshed while logging in.
    begin_menu_frame(state());
    f.tick();
    assert(!state().menu_active && f.config[0x55] == 3 && f.creates == 2);
    assert((f.size == std::array<unsigned, 2>{2560, 1440}));
    finish_frame();
    assert(f.config[0x45] == 1); // The world gate is native-owned.
    f.game_main = {};
    f.config[0x45] = 0;
    begin_menu_frame(state());
    f.tick();
    assert(state().menu_active && f.creates == 3 && f.config[0x45] == 1);
    finish_frame();
    assert(f.config[0x45] == 0);
}
void frontend_cold_start_to_ready_lobby() {
    Fixture f;
    f.enable_frontend();
    begin_menu_frame(state());
    f.tick();
    finish_frame();
    assert(f.creates == 1 && f.config[0x45] == 0);
    // Actual failing run: native AgentLobby changes GameMain state 0 -> 4,
    // still disconnected with no current/next territory. A native selector
    // reset at that boundary must not silently replace the addon's choice.
    Fixture::put(f.game_main, 0x4100, std::uint32_t(4));
    f.config[0x55] = 0;
    begin_menu_frame(state());
    assert(state().menu_active && f.config[0x45] == 1);
    f.tick();
    assert(f.creates == 2 && f.config[0x55] == 3);
    assert((f.size == std::array<unsigned, 2>{2560, 1440}));
    finish_frame();
    assert(f.config[0x45] == 0);
    begin_menu_frame(state());
    f.tick();
    assert(f.creates == 2); // Stable lobby frames retain model history.
    requested.dlss.quality = DlssQuality::Performance;
    settings_changed();
    f.tick();
    assert((f.size == std::array<unsigned, 2>{1920, 1080}));
    finish_frame();
    f.change(DlssModelPreset::M);
    begin_menu_frame(state());
    f.tick();
    assert(f.model == DlssModelPreset::M);
    assert((f.size == std::array<unsigned, 2>{1920, 1080}));
    finish_frame();
    for (const auto load : {1u, 2u, 3u, 5u, 0x100u, 0x104u}) {
        Fixture::put(f.game_main, 0x4100, load);
        begin_menu_frame(state());
        assert(!state().menu_active && f.config[0x45] == 0);
    }
    Fixture::put(f.game_main, 0x4100, std::uint32_t(4));
    for (const auto offset : {0x4104u, 0x4108u}) {
        Fixture::put(f.game_main, offset, std::uint32_t(100));
        begin_menu_frame(state());
        assert(!state().menu_active && f.config[0x45] == 0);
        Fixture::put(f.game_main, offset, std::uint32_t(0));
    }
    f.game_main[0x40fe] = 1;
    begin_menu_frame(state());
    assert(!state().menu_active && f.config[0x45] == 0);
}
unsigned native_tick_calls = 0, existing_tick_calls = 0;
bool native_tick_fixture(std::uintptr_t) {
    ++native_tick_calls;
    return true;
}
bool existing_tick_fixture(std::uintptr_t framework) {
    ++existing_tick_calls;
    return native_tick_fixture(framework);
}
void actual_framework_slot_ownership() {
    Fixture f;
    auto &s = state();
    requested.fg.enabled = true;
    s.addresses.framework_global = 0x48;
    s.addresses.simulation_tick = 1;
    s.addresses.framework_vtable = 0x50;
    s.tick_calls = 0;
    s.next_tick_check = 0;
    std::array<void *, 8> clone{};
    Fixture::put(f.globals, 0x70, reinterpret_cast<std::uintptr_t>(native_tick_fixture));
    clone[4] = reinterpret_cast<void *>(existing_tick_fixture);
    std::array<unsigned char, 0x100> framework{};
    Fixture::put(framework, 0, reinterpret_cast<std::uintptr_t>(clone.data()));
    Fixture::put(f.globals, 0x48, std::uintptr_t(0));
    assert(!attach_tick_slot(s, true));
    Fixture::put(f.globals, 0x48, reinterpret_cast<std::uintptr_t>(framework.data()));
    assert(attach_tick_slot(s, true));
    assert(s.tick_slot.slot == clone.data() + 4);
    assert(Fixture::get<std::uintptr_t>(f.globals, 0x70) ==
           reinterpret_cast<std::uintptr_t>(native_tick_fixture));
    assert(clone[4] == reinterpret_cast<void *>(observe_tick));
    native_tick_calls = existing_tick_calls = 0;
    assert(reinterpret_cast<Tick>(clone[4])(reinterpret_cast<std::uintptr_t>(framework.data())));
    assert(s.tick_calls == 1 && native_tick_calls == 1 && existing_tick_calls == 1);
    release_tick_slot(s);
    assert(clone[4] == reinterpret_cast<void *>(existing_tick_fixture));
    clone[4] = reinterpret_cast<void *>(std::uintptr_t(1));
    assert(!attach_tick_slot(s, true));
    clone[4] = reinterpret_cast<void *>(existing_tick_fixture);
    assert(attach_tick_slot(s, true));
    clone[4] = reinterpret_cast<void *>(native_tick_fixture); // A later owner wins.
    release_tick_slot(s);
    assert(clone[4] == reinterpret_cast<void *>(native_tick_fixture));
    s.tick = nullptr;
    s.tick_instance = 0;
    s.next_tick_check = 0;
}
void native_camera_without_dlss_controls() {
    Fixture f;
    requested.dlss.enabled = false;
    std::array<unsigned char, 0x290> camera{};
    std::array<float, 16> view{}, projection{};
    view[0] = view[5] = view[10] = view[15] = 1;
    const float aspect = 16.f / 9, near_plane = .1f;
    projection[5] = 1 / std::tan(.5f);
    projection[0] = projection[5] / aspect;
    projection[11] = 1;
    projection[14] = near_plane;
    Fixture::put(camera, 0x10, view);
    Fixture::put(camera, 0x1a0, projection);
    Fixture::put(camera, 0x1e8, std::array<float, 5>{1, 1, aspect, near_plane, 0});
    Fixture::put(f.renderer, 0x428, std::array<unsigned, 2>{2560, 1440});
    state().addresses.jitter_packet_return = 0x60;
    const auto caller = state().base + state().addresses.jitter_packet_return;
    const auto address = reinterpret_cast<std::uintptr_t>(camera.data());
    std::array<float, 4> native_jitter{.5f, -.25f, .5f / 2560.f, .25f / 1440.f};
    const auto original_jitter = native_jitter;
    for (const bool interpolation : {true, false}) {
        requested.fg.enabled = interpolation;
        requested.fg.reflex = !interpolation;
        fg::configure(requested, {}, true);
        const auto before = fg::status();
        const auto config = f.config;
        assert(process_jitter_packet(state(), address, native_jitter.data(), caller) == native_jitter.data());
        const auto after = fg::status();
        assert(after.camera_samples == before.camera_samples + 1 &&
               after.valid_cameras == before.valid_cameras + 1);
        assert(native_jitter == original_jitter && f.config == config);
        // Auxiliary preview/getter users must not masquerade as the main camera.
        assert(process_jitter_packet(state(), address, native_jitter.data(), caller + 1) ==
               native_jitter.data());
        assert(fg::status().camera_samples == after.camera_samples);
    }
    // The controls still exclusively own raster alignment. Enabling them
    // changes the returned packet, while the native storage remains untouched.
    requested.dlss.enabled = true;
    requested.dlss.quality = DlssQuality::Game;
    assert(process_jitter_packet(state(), address, native_jitter.data(), caller) == native_jitter.data());
    assert(native_jitter == original_jitter);
    requested.dlss.quality = DlssQuality::Quality;
    const auto *aligned =
        static_cast<const float *>(process_jitter_packet(state(), address, native_jitter.data(), caller));
    assert(aligned != native_jitter.data() && std::abs(aligned[0] - .25f) < 1e-6f &&
           std::abs(aligned[1] + .125f) < 1e-6f && native_jitter == original_jitter);
    requested.dlss.enabled = false;
    requested.fg.enabled = requested.fg.reflex = false;
    fg::configure(requested, {}, true);
    const auto idle = fg::status();
    assert(process_jitter_packet(state(), address, native_jitter.data(), caller) == native_jitter.data());
    assert(fg::status().camera_samples == idle.camera_samples && native_jitter == original_jitter);
}
void native_fg_camera_validation() {
    std::array<unsigned char, 0x290> camera{};
    std::array<float, 16> view{}, projection{};
    view[0] = view[5] = view[10] = view[15] = 1;
    const float aspect = 16.f / 9, near_plane = .1f;
    projection[5] = 1 / std::tan(.5f);
    projection[0] = projection[5] / aspect;
    projection[11] = 1;
    projection[14] = near_plane;
    std::array<float, 5> lens{1, 1, aspect, near_plane, 0};
    std::array<unsigned char, 5> flags{};
    const auto store = [&] {
        Fixture::put(camera, 0x10, view);
        Fixture::put(camera, 0x1a0, projection);
        Fixture::put(camera, 0x1e8, lens);
        Fixture::put(camera, 0x200, flags);
    };
    const auto address = reinterpret_cast<std::uintptr_t>(camera.data());
    fg::Camera result;
    std::string reason;
    store();
    assert(read_fg_camera(address, result, reason));
    assert(result.valid && result.inverted && result.infinite && std::isinf(result.far_plane) &&
           reason.empty());
    // Infinite far storage is unused, including stale values smaller than near.
    lens[4] = -.5f;
    store();
    assert(read_fg_camera(address, result, reason));
    flags[4] = 1;
    store();
    assert(!read_fg_camera(address, result, reason));
    assert(reason == "invalid finite far plane" && !result.valid);
    lens[4] = 1000;
    projection[10] = near_plane / (near_plane - lens[4]);
    projection[14] = near_plane * lens[4] / (lens[4] - near_plane);
    store();
    assert(read_fg_camera(address, result, reason) && !result.infinite && result.far_plane == 1000);
    flags[0] = 1;
    store();
    assert(!read_fg_camera(address, result, reason) && reason == "orthographic native camera");
    flags[0] = 0;
    view[0] = 0;
    store();
    assert(!read_fg_camera(address, result, reason) && reason == "invalid native view basis");
    view[0] = 1;
    view[4] = .5f;
    store();
    assert(!read_fg_camera(address, result, reason) && reason == "invalid native view basis");
    view[4] = 0;
    view[0] = -1;
    store();
    assert(!read_fg_camera(address, result, reason) && reason == "invalid native view orientation");
    view[0] = 1;
    view[2] = std::numeric_limits<float>::quiet_NaN();
    store();
    assert(!read_fg_camera(address, result, reason) && reason == "nonfinite view matrix");
    view[2] = 0;
    view[12] = std::numeric_limits<float>::infinity();
    store();
    assert(!read_fg_camera(address, result, reason) && reason == "nonfinite view matrix");
    view[12] = 0;
    // The 0.4.2 runtime log records a proper rigid transform with stale bits
    // in its fourth lanes. Preserve all twelve meaningful values exactly.
    view = {0.999963f,    -0.000912597f, 0.00852301f, 9.3887e-44f,  1.16e-10f, 0.994316f, 0.106466f, 0,
            -0.00857173f, -0.106462f,    0.99428f,    4.59051e-41f, 190.648f,  -13.8992f, -34.1341f, 0};
    store();
    assert(read_fg_camera(address, result, reason));
    assert(result.view[3] == 0 && result.view[7] == 0 && result.view[11] == 0 && result.view[15] == 1);
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 3; ++column)
            assert(result.view[row * 4 + column] == view[row * 4 + column]);
    // Padding is not a float input, even when its bits decode as NaN or Inf.
    view[3] = view[7] = view[15] = std::numeric_limits<float>::quiet_NaN();
    view[11] = std::numeric_limits<float>::infinity();
    store();
    assert(read_fg_camera(address, result, reason));
    assert(result.view[3] == 0 && result.view[7] == 0 && result.view[11] == 0 && result.view[15] == 1);
    // Another recorded camera orientation makes a transpose/sign error
    // visible. Its native RH reverse-infinite projection has w = -view.z.
    view = {0, -.6f, .8f, 0, 0, .8f, .6f, 0, -1, 0, 0, 0, 0, -1.2f, -3.4f, 0};
    projection[10] = 0;
    projection[11] = -1;
    projection[14] = near_plane;
    flags[4] = 0;
    store();
    assert(read_fg_camera(address, result, reason));
    assert(result.projection == projection && result.projection[11] == -1);
    // Native camera origin (2,3,0) transforms to view origin using row
    // vectors, with translation in the fourth row, not the fourth column.
    for (unsigned column = 0; column < 3; ++column)
        assert(std::abs(2 * result.view[column] + 3 * result.view[4 + column] + result.view[12 + column]) <
               .000001f);
    projection[15] = 1;
    store();
    assert(!read_fg_camera(address, result, reason) && reason == "invalid perspective projection");
    projection[15] = 0;
    lens[2] = 2;
    store();
    assert(!read_fg_camera(address, result, reason) && reason == "projection/lens aspect mismatch");
    assert(!read_fg_camera(0, result, reason) && reason == "unreadable native fields");
}
} // namespace

int main() {
    live_hdr_native_feature_recreation();
    native_drs_model_and_restore();
    actual_framework_slot_ownership();
    native_fg_camera_validation();
    native_camera_without_dlss_controls();
    live_model_and_coalescing();
    activation_and_readiness();
    external_selector_ownership();
    disable_same_quality();
    disable_with_native_gate_closed();
    frontend_rendering_and_cleanup();
    frontend_predicate_and_ownership();
    frontend_inputs_without_dlss_controls();
    frontend_login_logout_transition();
    frontend_cold_start_to_ready_lobby();
    std::cout << "Native model requests, cache recreation, restoration and readiness checks passed\n";
}
