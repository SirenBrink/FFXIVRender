#pragma once
#include "nr_images.hpp"
#include "settings.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace xivrender::nr {
struct FrameInput {
    SourceFrame images;
    const void *sr_feature = nullptr;
    std::uint64_t frame_id = 0;
    bool reset = false, hdr = false, motion_jittered = false;
};
struct PipelineStatus {
    bool active = false, paused = false, failed = false;
    unsigned width = 0, height = 0;
    std::string requested_runtime, active_runtime;
    std::string message = "NR is disabled", precision = "Selected runtime defaults";
    std::uint64_t creations = 0, evaluations = 0, applied = 0, skipped = 0, failures = 0, paced = 0;
};
// Single processing-thread owner. An enabled pipeline edits only successful
// native world-SR outputs. No caller-owned NGX parameters or temporal phases
// are modified. Normal frames use GPU waits. Exhausted command slots apply
// finite backpressure rather than intermittently omitting the NR effect.
class Pipeline {
  public:
    Pipeline();
    ~Pipeline();
    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;
    void Attach(ID3D11Device *native_device, const std::filesystem::path &addon_root);
    bool Process(const FrameInput &, const NeuralRenderingSettings &, bool focused);
    void Service(bool enabled);
    bool NeedsService() const;
    void Fault(const std::string &);
    void Retry();
    bool Shutdown(unsigned timeout_ms, std::string &error);
    PipelineStatus Status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xivrender::nr
