#pragma once
#include "d3d12_identity.hpp"
#include <d3d11.h>
namespace xivrender::d3d11_identity {
// Resolve only the documented explicit wrapper interface. Never compare GPUs
// by adapter LUID or infer native objects from private memory layouts.
inline Microsoft::WRL::ComPtr<ID3D11Device> native_device(ID3D11Device *device) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> current = device;
    std::array<ComPtr<IUnknown>, 8> seen;
    for (unsigned depth = 0; current && depth < seen.size(); ++depth) {
        ComPtr<IUnknown> canonical;
        if (FAILED(current.As(&canonical)))
            return {};
        for (unsigned prior = 0; prior < depth; ++prior)
            if (seen[prior] == canonical)
                return {};
        seen[depth] = canonical;
        ComPtr<IUnknown> original;
        const auto hr = current->QueryInterface(d3d12_identity::unwrapped_object,
                                                reinterpret_cast<void **>(original.GetAddressOf()));
        if (hr == E_NOINTERFACE)
            return current;
        if (FAILED(hr) || !original)
            return {};
        ComPtr<ID3D11Device> next;
        if (FAILED(original.As(&next)))
            return {};
        current = std::move(next);
    }
    return {};
}
inline Microsoft::WRL::ComPtr<ID3D11DeviceContext> unwrap_context(ID3D11DeviceContext *context) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11DeviceContext> current = context;
    std::array<ComPtr<IUnknown>, 8> seen;
    for (unsigned depth = 0; current && depth < seen.size(); ++depth) {
        ComPtr<IUnknown> canonical;
        if (FAILED(current.As(&canonical)))
            return {};
        for (unsigned prior = 0; prior < depth; ++prior)
            if (seen[prior] == canonical)
                return {};
        seen[depth] = canonical;
        ComPtr<IUnknown> original;
        const auto hr = current->QueryInterface(d3d12_identity::unwrapped_object,
                                                reinterpret_cast<void **>(original.GetAddressOf()));
        if (hr == E_NOINTERFACE)
            return current;
        if (FAILED(hr) || !original)
            return {};
        ComPtr<ID3D11DeviceContext> next;
        if (FAILED(original.As(&next)))
            return {};
        current = std::move(next);
    }
    return {};
}
inline Microsoft::WRL::ComPtr<ID3D11DeviceContext> native_context(ID3D11DeviceContext *context) {
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return {};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    device = native_device(device.Get());
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> native;
    if (device)
        device->GetImmediateContext(&native);
    return native;
}
} // namespace xivrender::d3d11_identity
