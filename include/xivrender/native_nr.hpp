#pragma once
#include "nr_pipeline.hpp"
namespace xivrender::native_nr {
void set_root(const std::filesystem::path &);
void attach_device(ID3D11Device *native_device);
void after_upscale(const nr::FrameInput &);
bool needs_service();
// Present-side notification only; GPU/model work belongs to after_upscale.
void service();
// UI commands are deferred to the native SR render thread.
void enabled_changed(bool enabled);
nr::PipelineStatus snapshot();
void retry();
void shutdown(unsigned timeout_ms = 250);
} // namespace xivrender::native_nr
