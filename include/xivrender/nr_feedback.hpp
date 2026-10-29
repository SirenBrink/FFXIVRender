#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <string>
namespace xivrender::nr {
// One scratch input for every later pass, shared safely on the ordered NR queue.
// Per-frame descriptor heaps must be retained until that frame's completion.
class Feedback {
  public:
    Feedback();
    ~Feedback();
    bool Initialize(ID3D12Device *, unsigned width, unsigned height, std::string &);
    bool Begin(ID3D12Resource *reference, ID3D12Resource *previous, unsigned passes,
               Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> &, std::string &);
    ID3D12Resource *Mix(ID3D12GraphicsCommandList *, ID3D12DescriptorHeap *, unsigned pass, float feedback);
    void ClearOutput(ID3D12GraphicsCommandList *, ID3D12DescriptorHeap *, unsigned pass, ID3D12Resource *);
    void Finish(ID3D12GraphicsCommandList *);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xivrender::nr
