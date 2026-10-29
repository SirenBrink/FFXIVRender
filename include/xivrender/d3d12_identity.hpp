#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <utility>

namespace xivrender::d3d12_identity {
// ReShade 6.8 exposes the original COM object through this explicit QI.
// https://github.com/crosire/reshade/blob/v6.8.0/source/com_utils.hpp
// Only identity comparisons are normalized; callers retain their original
// rendering interfaces, descriptor behavior and addon event interception.
inline constexpr GUID unwrapped_object = {
    0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};

inline Microsoft::WRL::ComPtr<IUnknown> device_identity(IUnknown *object) {
    using Microsoft::WRL::ComPtr;
    if (!object)
        return {};
    ComPtr<ID3D12Device> current;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&current))) || !current)
        return {};
    std::array<ComPtr<IUnknown>, 8> seen;
    for (unsigned depth = 0; depth < seen.size(); ++depth) {
        ComPtr<IUnknown> canonical;
        if (FAILED(current.As(&canonical)) || !canonical)
            return {};
        for (unsigned prior = 0; prior < depth; ++prior)
            if (seen[prior].Get() == canonical.Get())
                return {}; // Reject cyclic or self-unwrapping implementations.
        seen[depth] = canonical;
        ComPtr<IUnknown> original;
        const auto result =
            current->QueryInterface(unwrapped_object, reinterpret_cast<void **>(original.GetAddressOf()));
        if (result == E_NOINTERFACE)
            return canonical; // Ordinary native COM identity, no extension.
        if (FAILED(result) || !original)
            return {};
        ComPtr<ID3D12Device> next;
        if (FAILED(original.As(&next)) || !next)
            return {};
        current = std::move(next);
    }
    return {}; // An unexpectedly deep wrapper chain is not trusted.
}

inline bool same_device(ID3D12Device *expected, ID3D12DeviceChild *child) {
    if (!expected || !child)
        return false;
    Microsoft::WRL::ComPtr<ID3D12Device> actual;
    if (FAILED(child->GetDevice(IID_PPV_ARGS(&actual))) || !actual)
        return false;
    const auto a = device_identity(expected);
    const auto b = device_identity(actual.Get());
    return a && b && a.Get() == b.Get();
}
} // namespace xivrender::d3d12_identity
