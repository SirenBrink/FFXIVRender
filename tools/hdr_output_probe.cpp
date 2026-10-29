#include "xivrender/hdr.hpp"
#include "xivrender/frame_generation.hpp"
#include "xivrender/hdr_scene.hpp"
#include <d3dcompiler.h>
#include "xivrender/d3d12_identity.hpp"
#include <MinHook.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <atomic>
using Microsoft::WRL::ComPtr;
std::atomic<unsigned> deferred_callbacks{0};
// Test-only counterpart of the production init_command_list event. The real
// ReShade observer calls this export before deferred recording begins.
extern "C" __declspec(dllexport) BOOL XIVRenderProbeAttachHdrContext(IUnknown *object) {
    if (!xivrender::hdr::active() || !object)
        return FALSE;
    ComPtr<ID3D11DeviceContext> context;
    std::string error;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&context))) ||
        !xivrender::hdr::attach(context.Get(), error))
        return FALSE;
    if (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED)
        ++deferred_callbacks;
    return TRUE;
}
void require(bool ok, const char *text) {
    if (!ok)
        throw std::runtime_error(text);
}
ComPtr<ID3D11Texture2D> captured_scene(ID3D11Device *device, ID3D11DeviceContext *context,
                                       ID3D11Texture2D *source, UINT shader_id = 0x85e777ef,
                                       bool opaque_ui = false) {
    static constexpr char code[] =
        "Texture2D<float4> image:register(t0);"
        "void V(uint id:SV_VertexID,out float4 p:SV_Position){p=float4(id==2?3:-1,id==1?3:-1,0,1);}"
        "float4 P(float4 p:SV_Position):SV_Target{return image.Load(int3(p.xy,0));}";
    ComPtr<ID3DBlob> vs, ps;
    require(SUCCEEDED(
                D3DCompile(code, sizeof(code), nullptr, nullptr, nullptr, "V", "vs_5_0", 0, 0, &vs, nullptr)),
            "Probe VS compilation failed");
    require(SUCCEEDED(
                D3DCompile(code, sizeof(code), nullptr, nullptr, nullptr, "P", "ps_5_0", 0, 0, &ps, nullptr)),
            "Probe PS compilation failed");
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    require(
        SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex)),
        "Probe VS creation failed");
    require(
        SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel)),
        "Probe PS creation failed");
    // Synthetic stage identity only: this tests callback/context routing, not
    // that FFXIV's real shaders have been recognized in a game session.
    xivrender::hdr::scene::Tag(pixel.Get(), shader_id);
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    require(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &target)), "Probe scene target failed");
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    require(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)), "Probe scene RTV failed");
    require(SUCCEEDED(device->CreateShaderResourceView(source, nullptr, &srv)), "Probe scene SRV failed");
    context->ClearState();
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    ID3D11ShaderResourceView *input = srv.Get();
    context->PSSetShaderResources(0, 1, &input);
    ID3D11RenderTargetView *output = rtv.Get();
    context->OMSetRenderTargets(1, &output, nullptr);
    D3D11_VIEWPORT viewport{0, 0, 640, 360, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ComPtr<ID3D11BlendState> blend;
    if (shader_id != 0x85e777ef && !opaque_ui) {
        D3D11_BLEND_DESC b{};
        auto &r = b.RenderTarget[0];
        r.BlendEnable = true;
        r.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        r.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        r.BlendOp = r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha = D3D11_BLEND_ONE;
        r.DestBlendAlpha = D3D11_BLEND_ZERO;
        r.RenderTargetWriteMask = 15;
        require(SUCCEEDED(device->CreateBlendState(&b, &blend)), "Probe UI blend failed");
        context->OMSetBlendState(blend.Get(), nullptr, ~0u);
    }
    context->Draw(3, 0);
    context->ClearState();
    if (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED)
        return target;
    require(bool(xivrender::hdr::scene::Find(target.Get())),
            "Native scene draw was not captured through ReShade");
    return target;
}
void deferred_scene(ID3D11Device *device, ID3D11DeviceContext *immediate, ID3D11Texture2D *source) {
    using namespace xivrender::hdr;
    scene::EndFrame();
    ui::EndFrame();
    std::string error;
    const auto callbacks_before = deferred_callbacks.load();
    ComPtr<ID3D11DeviceContext> first, second;
    require(SUCCEEDED(device->CreateDeferredContext(0, &first)) &&
                SUCCEEDED(device->CreateDeferredContext(0, &second)),
            "Deferred context creation failed");
    ComPtr<IUnknown> native_device;
    if (SUCCEEDED(device->QueryInterface(xivrender::d3d12_identity::unwrapped_object,
                                         reinterpret_cast<void **>(native_device.GetAddressOf()))))
        require(deferred_callbacks.load() >= callbacks_before + 2,
                "ReShade deferred context creation did not reach native capture attachment");
    else
        require(attach(first.Get(), error) && attach(second.Get(), error), error.c_str());
    auto a = captured_scene(device, first.Get(), source);
    require(!scene::Find(a.Get()), "Unexecuted deferred draw was published");
    ComPtr<ID3D11CommandList> root, copy, clear;
    require(SUCCEEDED(first->FinishCommandList(FALSE, &root)), "Deferred scene finish failed");
    D3D11_TEXTURE2D_DESC desc{};
    a->GetDesc(&desc);
    ComPtr<ID3D11Texture2D> b;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &b)), "Deferred copy target failed");
    // Record an overwrite before this resource has ever acquired HDR storage.
    // It must still invalidate an alias created by an earlier execution later.
    ComPtr<ID3D11RenderTargetView> clear_view;
    require(SUCCEEDED(device->CreateRenderTargetView(b.Get(), nullptr, &clear_view)), "Clear view failed");
    const FLOAT zero[4]{};
    second->ClearRenderTargetView(clear_view.Get(), zero);
    require(SUCCEEDED(second->FinishCommandList(FALSE, &clear)), "Deferred clear finish failed");
    second->CopyResource(b.Get(), a.Get());
    require(SUCCEEDED(second->FinishCommandList(FALSE, &copy)), "Deferred copy finish failed");
    require(!scene::Find(a.Get()) && !scene::Find(b.Get()), "Finished but unexecuted work was published");
    immediate->ExecuteCommandList(copy.Get(), FALSE);
    require(!scene::Find(b.Get()), "Out-of-order deferred copy exposed an unexecuted scene");
    scene::EndFrame();
    immediate->ExecuteCommandList(root.Get(), FALSE);
    immediate->ExecuteCommandList(copy.Get(), FALSE);
    auto captured = scene::Find(b.Get());
    require(bool(scene::Find(a.Get())) && bool(captured), "Cross-context deferred scene/copy missing");
    // Verify GPU copy forwarding, not merely CPU alias bookkeeping.
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Deferred readback failed");
    immediate->CopyResource(staging.Get(), captured->reference.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Deferred map failed");
    const auto value = *static_cast<const std::uint16_t *>(mapped.pData);
    immediate->Unmap(staging.Get(), 0);
    require(value == 0x3800, "Deferred reference did not preserve the recorded SDR result");
    captured.reset();
    scene::EndFrame();
    immediate->ExecuteCommandList(copy.Get(), FALSE);
    require(!scene::Find(b.Get()), "Cached list reused a previous frame's scene validity");
    immediate->ExecuteCommandList(root.Get(), TRUE);
    immediate->ExecuteCommandList(copy.Get(), TRUE);
    require(bool(scene::Find(b.Get())), "Cached scene lists failed to replay");
    immediate->ExecuteCommandList(clear.Get(), FALSE);
    require(!scene::Find(b.Get()) && bool(scene::Find(a.Get())),
            "Deferred clear failed to invalidate only its target");
    auto discarded_target = captured_scene(device, first.Get(), source);
    ComPtr<ID3D11CommandList> discarded;
    require(SUCCEEDED(first->FinishCommandList(FALSE, &discarded)), "Discarded finish failed");
    discarded.Reset();
    require(!scene::Find(discarded_target.Get()), "Discarded command list published a scene");
    // UI replay shares one transmittance surface across contexts. Clearing at
    // record time would race/reclear it and make HUD brightness depend on order.
    ComPtr<ID3D11CommandList> ui_first, ui_second, ui_opaque;
    captured_scene(device, first.Get(), source, 3865947726u);
    require(SUCCEEDED(first->FinishCommandList(FALSE, &ui_first)), "First UI finish failed");
    captured_scene(device, second.Get(), source, 3865947726u);
    require(SUCCEEDED(second->FinishCommandList(FALSE, &ui_second)), "Second UI finish failed");
    captured_scene(device, second.Get(), source, 3865947726u, true);
    require(SUCCEEDED(second->FinishCommandList(FALSE, &ui_opaque)), "Opaque UI finish failed");
    const auto read_mask = [&] {
        auto mask = ui::Mask(device, 640, 360);
        require(bool(mask), "Deferred UI mask was not selected at execution");
        ComPtr<ID3D11Resource> resource;
        mask->GetResource(&resource);
        D3D11_TEXTURE2D_DESC md{};
        md.Width = 640;
        md.Height = 360;
        md.Format = DXGI_FORMAT_R8_UNORM;
        md.MipLevels = md.ArraySize = md.SampleDesc.Count = 1;
        md.Usage = D3D11_USAGE_STAGING;
        md.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> read;
        require(SUCCEEDED(device->CreateTexture2D(&md, nullptr, &read)), "UI readback creation failed");
        immediate->CopyResource(read.Get(), resource.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        require(SUCCEEDED(immediate->Map(read.Get(), 0, D3D11_MAP_READ, 0, &map)), "UI map failed");
        const auto alpha = *static_cast<const unsigned char *>(map.pData);
        immediate->Unmap(read.Get(), 0);
        return unsigned(alpha);
    };
    require(!ui::Mask(device, 640, 360), "UI mask published before execution");
    immediate->ExecuteCommandList(ui_first.Get(), FALSE);
    immediate->ExecuteCommandList(ui_second.Get(), FALSE);
    require(read_mask() >= 63 && read_mask() <= 65, "Two deferred translucent layers did not multiply");
    immediate->ExecuteCommandList(ui_first.Get(), FALSE);
    require(read_mask() >= 31 && read_mask() <= 33, "UI mask cleared more than once in one frame");
    ui::EndFrame();
    immediate->ExecuteCommandList(ui_first.Get(), FALSE);
    require(read_mask() >= 127 && read_mask() <= 129, "Cached UI mask did not reset on next frame");
    immediate->ExecuteCommandList(ui_opaque.Get(), FALSE);
    require(read_mask() == 0, "Opaque deferred UI leaked world HDR");
    ui::EndFrame();
    scene::EndFrame();
    std::cout << "PASS: deferred execution order, GPU copies, cached/discarded lists, clear invalidation and "
                 "UI transmittance\n"
              << std::flush;
}
int main(int argc, char **argv) {
    try {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"XIVRender HDR isolated probe";
        RegisterClassW(&wc);
        const bool visible = argc > 1 && std::string_view(argv[1]) == "--visible";
        HWND window = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"XIVRender HDR test", WS_POPUP,
            visible ? -32000 : 0, visible ? -32000 : 0, 3840, 2160, nullptr, nullptr, wc.hInstance, nullptr);
        if (visible)
            ShowWindow(window, SW_SHOWNOACTIVATE);
        require(window, "Probe window creation failed");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                            D3D11_SDK_VERSION, &device, nullptr, &context)),
                "Hardware D3D11 creation failed");
        require(MH_Initialize() == MH_OK, "MinHook init failed");
        xivrender::Settings settings;
        settings.hdr.enabled = true;
        xivrender::hdr::configure(settings, true);
        std::string error;
        require(xivrender::hdr::install_output(error), error.c_str());
        for (const unsigned count : {1u, 2u})
            for (const auto mode : {xivrender::HdrOutput::Pq10, xivrender::HdrOutput::ScRgb16Float}) {
                settings.hdr.output = mode;
                xivrender::hdr::configure(settings, true);
                settings.fg.reflex = false;
                xivrender::fg::configure(settings, {}, true);
                ComPtr<IDXGIDevice> dxgi;
                require(SUCCEEDED(device.As(&dxgi)), "DXGI device missing");
                ComPtr<IDXGIAdapter> adapter;
                require(SUCCEEDED(dxgi->GetAdapter(&adapter)), "Adapter missing");
                ComPtr<IDXGIFactory> factory;
                require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "Factory missing");
                DXGI_SWAP_CHAIN_DESC desc{};
                desc.BufferDesc.Width = count == 1 ? 0 : 3840;
                desc.BufferDesc.Height = count == 1 ? 0 : 2160;
                desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.BufferCount = count;
                desc.BufferDesc.RefreshRate = {60, 1};
                desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
                desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
                desc.OutputWindow = window;
                desc.Windowed = TRUE;
                desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
                ComPtr<IDXGISwapChain> chain;
                require(SUCCEEDED(factory->CreateSwapChain(device.Get(), &desc, &chain)),
                        "HDR proxy swapchain creation failed");
                DXGI_SWAP_CHAIN_DESC logical{};
                require(SUCCEEDED(chain->GetDesc(&logical)) &&
                            logical.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                            logical.SwapEffect == DXGI_SWAP_EFFECT_DISCARD && logical.BufferCount == count &&
                            logical.Flags == DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH,
                        "Native SDR chain contract changed");
                UINT size = 8, info[2]{};
                require(SUCCEEDED(chain->GetPrivateData(xivrender::hdr::output_info, &size, info)),
                        xivrender::hdr::status().c_str());
                require(info[0] == UINT(mode == xivrender::HdrOutput::Pq10 ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                                           : DXGI_FORMAT_R16G16B16A16_FLOAT),
                        "Actual HDR buffer format mismatch");
                require(info[1] == UINT(mode == xivrender::HdrOutput::Pq10
                                            ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                            : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709),
                        "HDR color space mismatch");
                xivrender::hdr::TransportInfo transport{};
                size = sizeof(transport);
                require(SUCCEEDED(chain->GetPrivateData(xivrender::hdr::transport_info, &size, &transport)),
                        "Private SDR transport metadata missing");
                const auto carrier_window = reinterpret_cast<HWND>(transport.window);
                require(carrier_window != window && IsWindow(carrier_window) &&
                            !IsWindowVisible(carrier_window) &&
                            transport.swap_effect == DXGI_SWAP_EFFECT_FLIP_DISCARD && transport.buffers >= 2,
                        "SDR compatibility and HDR output must own separate HWNDs");
                require(logical.OutputWindow == window && logical.BufferDesc.Width == 3840 &&
                            logical.BufferDesc.Height == 2160,
                        "Native window/automatic dimensions changed");
                // Reproduce the exact access-denied class of error if another
                // flip chain tries to acquire this occupied display HWND.
                ComPtr<IDXGIFactory> raw_factory = factory;
                ComPtr<ID3D11Device> raw_device = device;
                ComPtr<IUnknown> unwrapped;
                if (SUCCEEDED(factory->QueryInterface(xivrender::d3d12_identity::unwrapped_object,
                                                      reinterpret_cast<void **>(unwrapped.GetAddressOf()))))
                    require(SUCCEEDED(unwrapped.As(&raw_factory)), "Native factory identity missing");
                unwrapped.Reset();
                if (SUCCEEDED(device->QueryInterface(xivrender::d3d12_identity::unwrapped_object,
                                                     reinterpret_cast<void **>(unwrapped.GetAddressOf()))))
                    require(SUCCEEDED(unwrapped.As(&raw_device)), "Native device identity missing");
                auto conflicting = desc;
                conflicting.BufferCount = 2;
                conflicting.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                ComPtr<IDXGISwapChain> duplicate;
                const auto collision =
                    raw_factory->CreateSwapChain(raw_device.Get(), &conflicting, &duplicate);
                require((collision == E_ACCESSDENIED || collision == DXGI_ERROR_INVALID_CALL) && !duplicate,
                        "A second flip swapchain unexpectedly acquired the occupied display HWND");
                std::cout << "HWND ownership: collision HRESULT=0x" << std::hex << unsigned(collision)
                          << std::dec << "; private SDR HWND is distinct and hidden\n"
                          << std::flush;
                ComPtr<ID3D11Texture2D> inaccessible;
                require(FAILED(chain->GetBuffer(1, IID_PPV_ARGS(&inaccessible))) && !inaccessible,
                        "Discard chain incorrectly exposes buffer 1");
                ComPtr<ID3D11Texture2D> buffer;
                require(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer))), "SDR proxy buffer missing");
                D3D11_TEXTURE2D_DESC td{};
                buffer->GetDesc(&td);
                require(td.Format == DXGI_FORMAT_R8G8B8A8_UNORM && td.Width == 3840 && td.Height == 2160,
                        "Game/ReShade buffer is no longer full-size SDR");
                ComPtr<ID3D11RenderTargetView> rtv;
                require(SUCCEEDED(device->CreateRenderTargetView(buffer.Get(), nullptr, &rtv)),
                        "Native SDR RTV failed");
                // FFXIV evaluates through ReShade's wrapped immediate context,
                // whereas HDR swapchain warmup receives the native device.
                D3D11_TEXTURE2D_DESC input_desc{};
                input_desc.Width = 640;
                input_desc.Height = 360;
                input_desc.MipLevels = input_desc.ArraySize = input_desc.SampleDesc.Count = 1;
                input_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                input_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                ComPtr<ID3D11Texture2D> dlss_color;
                std::vector<std::uint16_t> color_data(640 * 360 * 4, 0x3800);
                D3D11_SUBRESOURCE_DATA data{color_data.data(), 640 * 8, 0};
                require(SUCCEEDED(device->CreateTexture2D(&input_desc, &data, &dlss_color)),
                        "Probe DLSS input creation failed");
                require(xivrender::hdr::attach(context.Get(), error), error.c_str());
                if (argc > 2 && std::string_view(argv[2]) == "--deferred")
                    deferred_scene(device.Get(), context.Get(), dlss_color.Get());
                dlss_color = captured_scene(device.Get(), context.Get(), dlss_color.Get());
                xivrender::hdr::DlssInput dlss_input;
                require(xivrender::hdr::prepare_dlss(context.Get(), dlss_color.Get(), 640, 360, 0, 0, 3840,
                                                     2160, dlss_input),
                        "HDR input preparation failed through wrapped context");
                // Simulate a successful HDR DLSS result and verify that the
                // SDR compatibility copy covers output resolution, not input.
                ComPtr<ID3D11UnorderedAccessView> hdr_out;
                require(
                    SUCCEEDED(device->CreateUnorderedAccessView(dlss_input.output.Get(), nullptr, &hdr_out)),
                    "Probe HDR output UAV failed");
                const float bright[]{2, 2, 2, 1};
                context->ClearUnorderedAccessViewFloat(hdr_out.Get(), bright);
                require(xivrender::hdr::begin_nr(context.Get()), "Wrapped HDR output isolation failed");
                D3D11_TEXTURE2D_DESC native_output_desc = input_desc;
                native_output_desc.Width = 3840;
                native_output_desc.Height = 2160;
                ComPtr<ID3D11Texture2D> native_output;
                require(SUCCEEDED(device->CreateTexture2D(&native_output_desc, nullptr, &native_output)),
                        "Probe native output creation failed");
                require(xivrender::hdr::finish_dlss(context.Get(), native_output.Get(), 3840, 2160, 0, 0),
                        "Wrapped HDR output conversion failed");
                native_output_desc.Usage = D3D11_USAGE_STAGING;
                native_output_desc.BindFlags = 0;
                native_output_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> readback;
                require(SUCCEEDED(device->CreateTexture2D(&native_output_desc, nullptr, &readback)),
                        "Probe full-resolution readback failed");
                context->CopyResource(readback.Get(), native_output.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                require(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
                        "Probe output map failed");
                for (const unsigned y : {0u, 2159u})
                    for (const unsigned x : {0u, 3839u}) {
                        const auto *pixel = reinterpret_cast<const std::uint16_t *>(
                            static_cast<const unsigned char *>(mapped.pData) + y * mapped.RowPitch + x * 8);
                        require(pixel[0] > 0x3800 && pixel[0] <= 0x3c00 && pixel[3] == 0x3c00,
                                "HDR conversion failed to fill a full-resolution output corner");
                    }
                context->Unmap(readback.Get(), 0);
                const float gray[]{.5f, .5f, .5f, 1};
                context->ClearRenderTargetView(rtv.Get(), gray);
                for (unsigned i = 0; i < 4; ++i)
                    require(SUCCEEDED(chain->Present(0, 0)), "HDR Present failed");
                context->ClearState();
                rtv.Reset();
                buffer.Reset();
                // Exercise count transitions in both directions, retaining FFXIV's flags.
                const auto hr = chain->ResizeBuffers(3 - count, 1360, 768, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                     DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
                require(SUCCEEDED(hr), "HDR resize failed");
                require(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer))), "Resized SDR buffer missing");
                buffer->GetDesc(&td);
                require(td.Width == 1360 && td.Height == 768 && td.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
                        "Resized SDR contract mismatch");
                buffer.Reset();
                require(SetWindowPos(window, nullptr, 0, 0, 1456, 816,
                                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE),
                        "Probe client resize failed");
                require(SUCCEEDED(chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN,
                                                       DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)),
                        "Automatic game-client resize failed");
                require(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer))),
                        "Automatic-size SDR buffer missing");
                buffer->GetDesc(&td);
                require(td.Width == 1456 && td.Height == 816, "Automatic resize used the private HWND size");
                buffer.Reset();
                std::cout << (mode == xivrender::HdrOutput::Pq10 ? "10-bit PQ" : "16-bit scRGB")
                          << ", native BufferCount=" << count
                          << ": real HDR output, SDR game buffers, native Present and resize passed\n"
                          << std::flush;
                chain.Reset();
                context->Flush();
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    DispatchMessageW(&message);
                require(!IsWindow(carrier_window), "Private SDR window survived chain teardown");
                SetWindowPos(window, nullptr, 0, 0, 3840, 2160, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        xivrender::hdr::remove_output();
        xivrender::hdr::shutdown();
        context->ClearState();
        context.Reset();
        device.Reset();
        DestroyWindow(window);
        std::cout << "PASS: actual HDR swapchain formats/color spaces, isolated native SDR contract, "
                     "present, resize and teardown\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
