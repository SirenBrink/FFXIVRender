#pragma once
#include <windows.h>
#include <cstdint>
namespace xivrender {
// Own a single validated vtable slot without replacing another addon's native
// detour. The original function may already forward through Dalamud.
struct PointerLease {
    void **slot = nullptr;
    void *original = nullptr, *replacement = nullptr;
    bool acquire(void **target, void *expected, void *desired) {
        if (slot || !target || std::uintptr_t(target) % alignof(void *) || !expected || !desired)
            return false;
        DWORD protect = 0;
        if (!VirtualProtect(target, sizeof(void *), PAGE_READWRITE, &protect))
            return false;
        const auto prior = InterlockedCompareExchangePointer(target, desired, expected);
        DWORD ignored = 0;
        VirtualProtect(target, sizeof(void *), protect, &ignored);
        if (prior != expected)
            return false;
        slot = target;
        original = expected;
        replacement = desired;
        return true;
    }
    void release() {
        if (!slot)
            return;
        DWORD protect = 0;
        if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protect))
            return;
        InterlockedCompareExchangePointer(slot, original, replacement);
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void *), protect, &ignored);
        slot = nullptr;
    }
};
} // namespace xivrender
