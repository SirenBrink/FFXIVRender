#pragma once
#include "settings.hpp"
#include <string>
#include <cstdint>
#include <functional>
namespace xivrender {
struct Status {
    std::string phase = "Starting", message, signatures, runtime;
    bool native_dlss_hooks = false, native_quality = false, conflict = false;
    unsigned render_width = 0, render_height = 0, display_width = 0, display_height = 0, jitter_phases = 0;
    float mip_bias = 0;
    bool evaluated_resolution = false;
    unsigned input_allocation_width = 0, input_allocation_height = 0;
    unsigned output_width = 0, output_height = 0;
    std::string resolution_source;
    int last_created_model = -1;
    std::uint64_t creates = 0, evaluations = 0, failures = 0, real_presents = 0;
    std::uint64_t last_evaluation_tick = 0;
};
Settings settings_snapshot();
void request_settings(const Settings &);
Status status_snapshot();
void update_status(const Status &);
Status modify_status(const std::function<void(Status &)> &);
void log_message(const std::string &);
bool save_requested_settings(std::string &error);
void request_rescan();
bool save_nr_slot(unsigned, const NeuralRenderingSettings &, std::string &error);
bool load_nr_slot(unsigned, NeuralRenderingSettings &, std::string &error);
} // namespace xivrender
