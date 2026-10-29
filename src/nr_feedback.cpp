#include "xivrender/nr_feedback.hpp"
#include <d3dcompiler.h>
#include <cstdint>
#include <limits>
namespace xivrender::nr {
using Microsoft::WRL::ComPtr;
namespace {
void barrier(ID3D12GraphicsCommandList *c, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to) {
    if (from == to)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    c->ResourceBarrier(1, &b);
}
constexpr char source[] = R"(
cbuffer Size:register(b0) { uint Width; uint Height; float Feedback; };
Texture2D<float4> Original:register(t0);
Texture2D<float4> Previous:register(t1);
RWTexture2D<float4> Input:register(u0);
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 if (id.x>=Width || id.y>=Height) return;
 float3 a=Original[id.xy].rgb, b=Previous[id.xy].rgb;
 Input[id.xy]=float4(all(isfinite(b))?max(lerp(a,b,Feedback),0):a,1);
}
)";
bool check(HRESULT hr, std::string &error) {
    if (SUCCEEDED(hr))
        return true;
    error = "NR feedback resource/shader initialization failed: " + std::to_string(hr);
    return false;
}
} // namespace
struct Feedback::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> scratch;
    ComPtr<ID3D12DescriptorHeap> clear_cpu;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    unsigned width = 0, height = 0, stride = 0;
    bool used = false;
};
Feedback::Feedback() : impl_(std::make_unique<Impl>()) {}
Feedback::~Feedback() = default;
bool Feedback::Initialize(ID3D12Device *d, unsigned w, unsigned h, std::string &error) {
    auto &s = *impl_;
    s.device = d;
    s.width = w;
    s.height = h;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC image{};
    image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    image.Width = w;
    image.Height = h;
    image.DepthOrArraySize = image.MipLevels = 1;
    image.SampleDesc.Count = 1;
    image.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    image.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!check(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &image, D3D12_RESOURCE_STATE_COMMON,
                                          nullptr, IID_PPV_ARGS(&s.scratch)),
               error))
        return false;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {2, ranges};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0, 0, 3};
    D3D12_ROOT_SIGNATURE_DESC desc{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized, diagnostics, code;
    if (!check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &diagnostics),
               error) ||
        !check(d->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                      IID_PPV_ARGS(&s.root)),
               error) ||
        !check(D3DCompile(source, sizeof(source), "NR feedback", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &diagnostics),
               error))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
    state.pRootSignature = s.root.Get();
    state.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (!check(d->CreateComputePipelineState(&state, IID_PPV_ARGS(&s.pipeline)), error))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC cpu_heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (!check(d->CreateDescriptorHeap(&cpu_heap, IID_PPV_ARGS(&s.clear_cpu)), error))
        return false;
    s.stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}
bool Feedback::Begin(ID3D12Resource *a, ID3D12Resource *b, unsigned passes,
                     ComPtr<ID3D12DescriptorHeap> &heap, std::string &error) {
    auto &s = *impl_;
    D3D12_DESCRIPTOR_HEAP_DESC d{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4 * (passes - 1),
                                 D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (!check(s.device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap)), error))
        return false;
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = srv.Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    s.device->CreateUnorderedAccessView(b, nullptr, &uav, s.clear_cpu->GetCPUDescriptorHandleForHeapStart());
    for (unsigned i = 1; i < passes; ++i) {
        s.device->CreateShaderResourceView(a, &srv, handle);
        handle.ptr += s.stride;
        s.device->CreateShaderResourceView(b, &srv, handle);
        handle.ptr += s.stride;
        s.device->CreateUnorderedAccessView(s.scratch.Get(), nullptr, &uav, handle);
        handle.ptr += s.stride;
        s.device->CreateUnorderedAccessView(b, nullptr, &uav, handle);
        handle.ptr += s.stride;
    }
    return true;
}
ID3D12Resource *Feedback::Mix(ID3D12GraphicsCommandList *c, ID3D12DescriptorHeap *heap, unsigned pass,
                              float feedback) {
    auto &s = *impl_;
    barrier(c, s.scratch.Get(),
            s.used ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.used = true;
    c->SetPipelineState(s.pipeline.Get());
    c->SetComputeRootSignature(s.root.Get());
    c->SetDescriptorHeaps(1, &heap);
    auto table = heap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += std::uint64_t(4 * (pass - 1)) * s.stride;
    c->SetComputeRootDescriptorTable(0, table);
    struct {
        unsigned width, height;
        float feedback;
    } constants{s.width, s.height, feedback};
    c->SetComputeRoot32BitConstants(1, 3, &constants, 0);
    c->Dispatch((s.width + 7) / 8, (s.height + 7) / 8, 1);
    barrier(c, s.scratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return s.scratch.Get();
}
void Feedback::ClearOutput(ID3D12GraphicsCommandList *c, ID3D12DescriptorHeap *heap, unsigned pass,
                           ID3D12Resource *output) {
    const auto &s = *impl_;
    const auto offset = std::uint64_t(4 * (pass - 1) + 3) * s.stride;
    const auto cpu = s.clear_cpu->GetCPUDescriptorHandleForHeapStart();
    auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += offset;
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    const float unwritten[]{invalid, invalid, invalid, 0};
    c->ClearUnorderedAccessViewFloat(gpu, cpu, output, unwritten, 0, nullptr);
    D3D12_RESOURCE_BARRIER ordered{};
    ordered.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ordered.UAV.pResource = output;
    c->ResourceBarrier(1, &ordered);
}
void Feedback::Finish(ID3D12GraphicsCommandList *c) {
    auto &s = *impl_;
    if (s.used)
        barrier(c, s.scratch.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
    s.used = false;
}
} // namespace xivrender::nr
