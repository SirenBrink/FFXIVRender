#pragma once
#include "settings.hpp"
#include <d3d11.h>
#include <memory>
#include <string>

namespace xivrender::nr {
// Finished, unjittered native SR color plus the exact native guides for that
// real frame. Offsets/extents describe active regions, never allocation sizes.
struct SourceFrame {
    ID3D11Texture2D *color = nullptr, *depth = nullptr, *motion = nullptr;
    unsigned output_width = 0, output_height = 0, output_x = 0, output_y = 0;
    unsigned guide_width = 0, guide_height = 0, depth_x = 0, depth_y = 0;
    unsigned motion_width = 0, motion_height = 0, motion_x = 0, motion_y = 0;
    float jitter_x = 0, jitter_y = 0;
    float motion_scale_x = 1, motion_scale_y = 1;
    bool depth_inverted = true;
    bool linear_hdr = false;
};

// Uses a private D3D11 context-state object and restores the caller's complete
// pipeline state. Game textures are never writable until Composite's final
// copy. NR alpha is ignored: the original SR alpha is always preserved.
class Images {
  public:
    Images();
    ~Images();
    Images(const Images &) = delete;
    Images &operator=(const Images &) = delete;
    bool Prepare(ID3D11Device *native_device, const SourceFrame &, unsigned model_width,
                 unsigned model_height, ID3D11UnorderedAccessView *color, ID3D11UnorderedAccessView *depth,
                 ID3D11UnorderedAccessView *motion, std::string &error,
                 const NeuralRenderingSettings *settings = nullptr);
    bool Composite(ID3D11Device *native_device, const SourceFrame &, unsigned model_width,
                   unsigned model_height, ID3D11ShaderResourceView *model_input,
                   ID3D11ShaderResourceView *model_output, std::string &error,
                   const NeuralRenderingSettings *settings = nullptr);
    void Reset();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xivrender::nr
