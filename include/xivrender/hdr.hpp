#pragma once
#include "settings.hpp"
#include <d3d11.h>
#include <string>
#include <memory>
#include <wrl/client.h>
namespace xivrender::hdr {
void configure(const Settings &, bool startup = false);
bool active();
// Render-thread commit after the actual output transaction succeeds.
void commit_output(bool hdr);
void disable(const char *reason);
bool warmup(ID3D11DeviceContext *, unsigned width, unsigned height);
inline constexpr GUID output_info = {
    0x8648ae12, 0xe713, 0x4b92, {0x84, 0x44, 0xfa, 0xb4, 0x22, 0x59, 0xc5, 0x90}};
// Read-only transport metadata for integration validation; never a public
// second game swapchain or a presented intermediate.
inline constexpr GUID transport_info = {
    0x361db129, 0x8483, 0x4b70, {0xad, 0x3a, 0x5d, 0xe0, 0xe6, 0xca, 0x62, 0xd2}};
struct TransportInfo {
    std::uint64_t window;
    unsigned swap_effect, buffers;
};
HdrSettings options();
void report(const char *);
std::string status();
bool attach(ID3D11DeviceContext *, std::string &error);
void tag(ID3D11PixelShader *, const void *, size_t);
void begin_effects(ID3D11Texture2D *);
void finish_frame();
void shutdown();
struct DlssInput {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color, output, working;
    unsigned width = 0, height = 0;
};
bool prepare_dlss(ID3D11DeviceContext *, ID3D11Texture2D *, unsigned w, unsigned h, unsigned x, unsigned y,
                  unsigned output_width, unsigned output_height, DlssInput &);
bool begin_nr(ID3D11DeviceContext *);
bool finish_dlss(ID3D11DeviceContext *, ID3D11Texture2D *, unsigned w, unsigned h, unsigned x, unsigned y);
bool present(ID3D11DeviceContext *, ID3D11Texture2D *sdr, ID3D11RenderTargetView *hdr, unsigned w, unsigned h,
             bool pq);
// The swapchain transport preserves native SDR buffers and presents HDR only
// after ReShade effects and overlay. No second game Present or frame cap.
bool install_output(std::string &error);
void remove_output();
} // namespace xivrender::hdr
