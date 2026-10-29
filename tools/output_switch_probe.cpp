#include "xivrender/hdr.hpp"
#include "xivrender/frame_generation.hpp"
#include "xivrender/fg_diagnostics.hpp"
#include "xivrender/fg_policy.hpp"
#include <MinHook.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>
#include <filesystem>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
using namespace xivrender;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
int wmain(int argc, wchar_t **argv) {
    try {
        require(argc >= 2, "Usage: output_switch_probe <runtime> [--providers]");
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"XIVRender output transition validation";
        RegisterClassW(&wc);
        HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"",
            WS_POPUP, -32000, -32000, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
        require(window != nullptr, "Hidden test window failed");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                    D3D11_SDK_VERSION, &device, nullptr, &context)), "Hardware device failed");
        ComPtr<IDXGIFactory> factory;
        require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "Factory failed");
        Settings s;
        s.fg.enabled = s.fg.reflex = s.hdr.enabled = false;
        s.fg.backend = FrameGeneration::Off;
        fg::configure(s, std::filesystem::absolute(argv[1]), true);
        hdr::configure(s, true);
        require(MH_Initialize() == MH_OK, "MinHook failed");
        std::string error;
        require(hdr::install_output(error), error.c_str());
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc = {1280, 720, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        ComPtr<IDXGISwapChain> chain;
        require(SUCCEEDED(factory->CreateSwapChain(device.Get(), &desc, &chain)), "Cold SDR output failed");
        std::cout << "Compatibility: " << fg::status().compatibility << '\n';
        ComPtr<ID3D11Texture2D> buffer;
        require(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer))), "SDR carrier missing");
        ComPtr<ID3D11RenderTargetView> target;
        require(SUCCEEDED(device->CreateRenderTargetView(buffer.Get(), nullptr, &target)), "RTV failed");
        // Keep ReShade/game references alive throughout every format/provider
        // change. No ResizeBuffers or game feature restart is permitted here.
        for (unsigned cycle = 0; cycle < 2; ++cycle) {
            for (unsigned phase = 0; phase < 6; ++phase) {
                s.hdr.enabled = phase >= 1 && phase <= 3;
                s.hdr.output = phase == 2 ? HdrOutput::ScRgb16Float : HdrOutput::Pq10;
                s.fg.enabled = argc > 2 && phase >= 2 && phase <= 4;
                s.fg.backend = phase == 2 ? FrameGeneration::FsrFG
                               : phase == 3 ? FrameGeneration::XeFG : FrameGeneration::DlssG;
                s.fg.reflex = argc > 2 && phase == 5;
                fg::configure(s);
                hdr::configure(s);
                for (unsigned frame = 0; frame < 4; ++frame) {
                    fg::simulation_begin();
                    fg::simulation_end();
                    fg::render_begin();
                    const float gray[]{.25f, .5f, .75f, 1};
                    context->ClearRenderTargetView(target.Get(), gray);
                    require(SUCCEEDED(chain->Present(0, 0)), "Live output Present failed");
                }
                UINT bytes = 8, info[2]{};
                require(SUCCEEDED(chain->GetPrivateData(hdr::output_info, &bytes, info)), "Output metadata missing");
                const auto expected = fg::requested_output_format(s.hdr);
                const auto space = !s.hdr.enabled ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
                    : phase == 2 ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                require(info[0] == unsigned(expected) && info[1] == unsigned(space), "Actual output format/color space mismatch");
                require(hdr::active() == s.hdr.enabled, "Scene HDR state did not commit with output");
                ComPtr<ID3D11Texture2D> retained;
                require(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&retained))) && retained == buffer,
                        "Game/ReShade buffer identity changed");
                const auto state = fg::status();
                require(state.provider == (s.fg.enabled ? s.fg.backend : FrameGeneration::Off),
                        state.transition.c_str());
                std::cout << "PASS cycle=" << cycle << " phase=" << phase << " format=" << info[0]
                          << " space=" << info[1] << " provider=" << unsigned(state.provider) << '\n' << std::flush;
            }
        }
        context->ClearState();
        target.Reset(); buffer.Reset(); chain.Reset();
        hdr::shutdown(); hdr::remove_output();
        DestroyWindow(window);
        std::cout << "PASS: cold SDR, live HDR on/off/PQ/scRGB, stable game buffers and output teardown\n";
        fg::diagnostics::shutdown();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\nFG: " << fg::status().message << "\nHDR: " << hdr::status() << '\n';
        fg::diagnostics::event("probe.failed", 0, 0, 0, 0, 0, e.what());
        fg::diagnostics::shutdown();
        return 1;
    }
}
