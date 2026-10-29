#pragma once

#include "settings.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct ID3D12Fence;

namespace xivrender::nr {

enum class EnginePhase { Closed, Open, CreationRecorded, Creating, Ready, Failed, DeviceLost };

struct EngineConfig {
    std::filesystem::path bridge, model, data;
    unsigned width = 0, height = 0;
    unsigned passes = 1;
    std::array<NrPassProfile, 8> profiles{};
};

struct Evaluation {
    // All resources are on the configured device. Color/output are FP16 RGBA,
    // depth is R32_FLOAT and motion is R16G16_FLOAT, on the configured grid.
    // Caller transitions input resources to NON_PIXEL_SHADER_RESOURCE and the
    // output to UNORDERED_ACCESS before recording, then returns shared images
    // to COMMON before handing them back to D3D11.
    ID3D12Resource *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr;
    bool reset = false, depth_inverted = true;
    float motion_scale_x = 1.0f, motion_scale_y = 1.0f;
    float feedback = 1.0f;
};

struct EngineStatus {
    EnginePhase phase = EnginePhase::Closed;
    unsigned width = 0, height = 0;
    std::uint64_t creates = 0, evaluations = 0, failures = 0;
    std::uint32_t result = 1;
    std::string message;
    // The compatibility DLL selects its own kernels. No precision override is
    // implied, and an FP16-patched runtime must not be advertised as native FP8.
    std::string precision = "Selected runtime defaults (not overridden)";
};

// One private model generation with independent temporal features per pass. Calls are serialized
// by the frame coordinator. This class owns no queue, allocator or GPU wait.
// Each Record* invocation that calls the model MUST be followed by submission
// of that command list and MarkSubmitted, even when the model returns failure.
// HasPendingSubmission distinguishes a native call from a rejected argument.
// Creation must complete before the first evaluation; Ready only polls fences.
// All evaluations must use one ordered direct queue. Caller never advances NR
// twice for the same real engine frame. TryClose polls and refuses outstanding
// records/work instead of freeing in-flight GPU/model resources.
class Engine {
  public:
    Engine();
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    bool Open(ID3D12Device *, const EngineConfig &, std::string &error);
    // Check transport ownership before a native model call can record work.
    bool ValidateCompletionFence(ID3D12Fence *, std::string &error);
    bool RecordCreate(ID3D12GraphicsCommandList *, std::string &error);
    bool Ready();
    bool RecordEvaluate(ID3D12GraphicsCommandList *, const Evaluation &, std::string &error);
    bool HasPendingSubmission() const;
    bool MarkSubmitted(ID3D12Fence *, std::uint64_t value, std::string &error);
    bool TryClose(std::string &error);
    EngineStatus Status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xivrender::nr
