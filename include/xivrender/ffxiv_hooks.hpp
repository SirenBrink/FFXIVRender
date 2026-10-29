#pragma once
#include "ffxiv_signatures.hpp"
#include <cstdint>
#include <span>
#include <string>
namespace xivrender::ffxiv {
// MinHook initialization belongs to the addon host. These functions own only
// their four native detours, never global MinHook state or other addons' hooks.
bool install_hooks(const Report &, std::span<const std::uint8_t> raw_file, std::string &error);
void remove_hooks();
void settings_changed();
// One-time activation refresh must survive unchanged saved quality/model values.
void request_feature_refresh();
// End the frontend-only scaling policy after all game rendering for this frame.
void finish_frame();
bool quality_hooks_active();
bool jitter_hooks_active();
void measured_size(unsigned render_width, unsigned render_height, unsigned display_width,
                   unsigned display_height);
} // namespace xivrender::ffxiv
