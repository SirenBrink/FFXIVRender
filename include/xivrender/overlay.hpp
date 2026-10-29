#pragma once

namespace reshade::api {
struct effect_runtime;
}

namespace xivrender {
void draw_overlay(reshade::api::effect_runtime *runtime);
}
