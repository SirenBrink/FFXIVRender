// SPDX-License-Identifier: GPL-3.0-or-later
// Binder/policy adapted from the OptiScaler SirenBrink fork.
#include <xivrender/mip_bias.hpp>
#include <MinHook.h>
#include <vector>

namespace xivrender::mip {
namespace {
using detail::Get;
using detail::Set;
using Microsoft::WRL::ComPtr;
struct State {
    std::recursive_mutex mutex;
    ComPtr<ID3D11DeviceContext> context;
    detail::Binder binder;
    std::array<Set, detail::Stages> setters{};
    std::array<Get, detail::Stages> getters{};
    std::vector<void *> targets;
    std::atomic<bool> active{false};
    std::atomic<unsigned> calls{0};
    HMODULE pinned_module = nullptr;
};
State &state() {
    static auto *value = new State;
    return *value;
}
struct Callback {
    std::atomic<unsigned> &calls;
    explicit Callback(std::atomic<unsigned> &value) : calls(value) {
        calls.fetch_add(1, std::memory_order_acq_rel);
    }
    ~Callback() {
        calls.fetch_sub(1, std::memory_order_release);
    }
};

template <unsigned Stage>
void STDMETHODCALLTYPE set_hook(ID3D11DeviceContext *context, UINT start, UINT count,
                                ID3D11SamplerState *const *samplers) {
    auto &s = state();
    Callback entered(s.calls);
    if (s.active.load(std::memory_order_acquire))
        s.binder.Bind(Stage, context, start, count, samplers);
    else
        s.setters[Stage](context, start, count, samplers);
}
template <unsigned Stage>
void STDMETHODCALLTYPE get_hook(ID3D11DeviceContext *context, UINT start, UINT count,
                                ID3D11SamplerState **samplers) {
    auto &s = state();
    Callback entered(s.calls);
    if (s.active.load(std::memory_order_acquire))
        s.binder.Read(Stage, context, start, count, samplers);
    else
        s.getters[Stage](context, start, count, samplers);
}
constexpr std::array<Set, detail::Stages> set_hooks{set_hook<0>, set_hook<1>, set_hook<2>,
                                                    set_hook<3>, set_hook<4>, set_hook<5>};
constexpr std::array<Get, detail::Stages> get_hooks{get_hook<0>, get_hook<1>, get_hook<2>,
                                                    get_hook<3>, get_hook<4>, get_hook<5>};
constexpr unsigned set_indices[]{10, 26, 32, 61, 65, 70}, get_indices[]{75, 85, 88, 99, 103, 108};

bool rollback(State &s) {
    s.active.store(false, std::memory_order_release);
    // Only targets created by this module are touched, never MH_ALL_HOOKS.
    for (auto *target : s.targets)
        MH_DisableHook(target);
    // Do not hold Binder's mutex while draining: a callback may already be
    // waiting for it, or still need the original trampoline after acquiring it.
    const auto deadline = GetTickCount64() + 250;
    while (s.calls.load(std::memory_order_acquire) && GetTickCount64() < deadline)
        Sleep(1);
    if (s.calls.load(std::memory_order_acquire))
        return false;
    // If draining timed out, preserve the context, sampler references and all
    // disabled trampolines. A later lifecycle call can finish the cleanup.
    if (s.context)
        s.binder.Clear(s.context.Get());
    for (auto *target : s.targets)
        MH_RemoveHook(target);
    s.targets.clear();
    s.context.Reset();
    return true;
}
} // namespace

bool attach(ID3D11DeviceContext *context, std::string &error) {
    error.clear();
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!context) {
        error = "Automatic texture bias requires a D3D11 context";
        return false;
    }
    if (s.active.load(std::memory_order_acquire)) {
        if (s.context.Get() == context)
            return true;
        error = "Automatic texture bias is already attached to another D3D11 context; detach it at the "
                "device lifecycle boundary first";
        return false;
    }
    if (!s.targets.empty() && !rollback(s)) {
        error = "Previous texture-bias callbacks are still draining; their context and disabled trampolines "
                "remain retained";
        return false;
    }
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        error = "Automatic texture bias requires the immediate D3D11 context";
        return false;
    }
    // A callback which was already entered must be able to return after addon
    // shutdown. Never let a host unload this module out from under that frame.
    if (!s.pinned_module &&
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&attach), &s.pinned_module)) {
        error = "Cannot retain texture-bias callback code safely for this process";
        return false;
    }
    auto **table = *reinterpret_cast<void ***>(context);
    s.context = context;
    for (unsigned stage = 0; stage < detail::Stages; ++stage) {
        auto *set = table[set_indices[stage]];
        auto *get = table[get_indices[stage]];
        void *original = nullptr;
        auto result = MH_CreateHook(set, reinterpret_cast<void *>(set_hooks[stage]), &original);
        if (result != MH_OK) {
            error = std::string("Creating texture-bias sampler hook failed: ") + MH_StatusToString(result);
            rollback(s);
            return false;
        }
        s.targets.push_back(set);
        s.setters[stage] = reinterpret_cast<Set>(original);
        original = nullptr;
        result = MH_CreateHook(get, reinterpret_cast<void *>(get_hooks[stage]), &original);
        if (result != MH_OK) {
            error =
                std::string("Creating texture-bias sampler-read hook failed: ") + MH_StatusToString(result);
            rollback(s);
            return false;
        }
        s.targets.push_back(get);
        s.getters[stage] = reinterpret_cast<Get>(original);
    }
    s.binder.Attach(context, s.setters, s.getters);
    // Make the complete trampoline table available before enabling any target.
    // If enabling fails, in-flight calls can still pass through these originals.
    for (auto *target : s.targets) {
        const auto result = MH_EnableHook(target);
        if (result != MH_OK) {
            error = std::string("Enabling texture-bias sampler hook failed: ") + MH_StatusToString(result);
            rollback(s);
            return false;
        }
    }
    s.active.store(true, std::memory_order_release);
    return true;
}

void update(bool enabled, unsigned renderWidth, unsigned displayWidth) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.active.load(std::memory_order_acquire))
        s.binder.Update(s.context.Get(), enabled, renderWidth, displayWidth);
}

void detach() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    rollback(s);
}

float current() {
    auto &s = state();
    return s.active.load(std::memory_order_acquire) ? s.binder.Target() : 0;
}
bool is_attached() {
    return state().active.load(std::memory_order_acquire);
}
unsigned long long sampler_creations() {
    return state().binder.Creations();
}

} // namespace xivrender::mip
