#pragma once
#include <d3d11.h>
#include <string>
namespace xivrender::native_dlss {
void poll();
void shutdown();
bool active();
void attach_device(ID3D11Device *);
} // namespace xivrender::native_dlss
