#pragma once
#include <d3d11.h>
#include <string>
namespace xivrender::ngx {
// Preserve independent D3D12 NGX instances when FFXIV shuts down DX11.
// Passing a device records the native owner; nullptr retries hook installation.
bool guard_native_shutdown(ID3D11Device *, std::string &, HMODULE runtime = nullptr);
}
